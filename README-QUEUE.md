# The queued-write database mode

*Fork feature, campaign of 2026-08-14 (PLAN-QUEUE.md; design record
DESIGN-NETWORK.md §1a).  Specification: `test/queue1.test`, written red
before any code.*

```sql
-- the queue owner's connection:
PRAGMA journal_mode=wal2;
PRAGMA queue_writer=ON;

-- every other connection, while the owner lives:
INSERT INTO t VALUES(1);
-- Error: database is in queued-write mode; writes go through the
-- queue owner (see PRAGMA queue_mode), or wait for the queue
-- connection to close
```

## What it is

A **database-level** write discipline for the group-commit queue
architecture (DESIGN-NETWORK spine 1): one process owns all writes and
batches them; everyone else reads.  The mode is what transport policy
cannot fake — an out-of-band CLI writes straight to the file, and only
the engine can refuse it.

- `PRAGMA queue_writer=ON` declares this connection a queue writer.
  While ANY declarant lives, write transactions from non-declared
  connections are refused with the reason and the fix.  **Reads are
  never affected.**
- The declaration lasts exactly as long as the connection: `=OFF`,
  `db close`, process exit, and **process crash** all release it.
  Release-on-crash is inherited from OS file-lock semantics rather
  than implemented, so abandonment is impossible rather than unlikely.
- `PRAGMA queue_writer` (no argument) reports this connection's own
  declaration; `PRAGMA queue_mode` reports whether the database is
  under the discipline right now — queryable by exactly the connection
  being refused.
- Multiple declarants may coexist (the mode outlives any one of them).
- Requires WAL or wal2 (`queued-write mode requires WAL; set PRAGMA
  journal_mode=wal2 first`) — the mode's substrate is the WAL shared
  memory, which rollback mode does not have.
- Corollary: a **brand-new empty database** cannot be declared on —
  the WAL exists only after the first write, so `queue_writer=ON`
  refuses until the schema (or any write) has materialized the file.
  Create first, then declare; a queue owns a database, and an empty
  file is not one yet.  (`proc_queue.c`'s checked declaration caught
  exactly this ordering.)

## What it costs

A database **nobody declares on** pays one mapped-memory byte read per
write transaction — effectively nothing, and the suites pin that
nothing else changed.  On a degraded check (a crashed declarant's
stale hint), one shm lock probe repairs the hint and subsequent writes
are back to the byte read.

## Sharp edges

- The refusal is `SQLITE_BUSY`, deliberately: the condition clears
  when the owner closes, and a `busy_timeout` will wait for exactly
  that.  A caller with a long timeout waits it out before erroring.
- Two connections racing a stale-hint probe can each see the other's
  probe and refuse one write spuriously; the race is momentary and
  resolves toward refusal, never toward a barge-in.
- `writable_schema=ON` does **not** bypass this gate (unlike
  degrade-at-load's, DOCKET #9): a living owner needs no repair
  exception, and an expert override here would just reintroduce the
  barge-in the mode exists to prevent.
- The gate covers SQL write transactions.  `sqlite3_backup` writing
  INTO a queue-held database bypasses it (backup opens its write
  transaction below the gated opcode); the backup APIs are the queue
  owner's tools, not a general side door, but this is a known gap.

## Mechanism (for maintainers)

The declaration is a SHARED OS lock on shm-lock "slot 12" — byte 132
of the wal-index, the first byte of `WalCkptInfo.notUsed0`, reserved
space no build reads or writes.  Byte 133 is the fast-path hint.  Slot
8 (byte 128) is the OS layers' dead-man switch and bytes 129–131 are
live data — the only lockable bytes overlaying reserved space are
132–135, and P1 measured that before any code was written.  The gate
sits in OP_Transaction beside degrade-at-load's, and a faulted shm
read during the check propagates as the I/O error it is (`walseh1`
caught the first cut fabricating a verdict).  Everything lives in
`wal.c` behind four `sqlite3WalQueue*` entry points, with pager
passthroughs; the OS layers' only change is admitting slots 12–15.
