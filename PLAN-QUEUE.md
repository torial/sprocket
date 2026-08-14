# PLAN-QUEUE — the write queue's engine piece: queued-write database mode

*Written 2026-08-13, the same evening as the design conversation it
implements (DESIGN-NETWORK §1a).  Sean's requirement, verbatim in spirit:
"we probably do want a database mode that is independent of connection —
if there are no interactive sessions and a queue session begins, someone
can't force it to be interactive until the queue connection(s) close."*

## Scope, cut deliberately small

This campaign builds ONLY the engine piece of the queue sketch: the
**queued-write database mode** and its truth surface.  The queue itself —
drain loop, waiters, priorities, the server that owns it — is application
architecture (DESIGN-NETWORK spine 1) and is NOT this campaign.  The mode
must exist first because it is the one part transport policy cannot fake:
an out-of-band CLI writes straight to the file, and only the engine can
refuse it.

## The contract

- `PRAGMA queue_writer=ON` declares this connection a queue writer on this
  database.  While ANY connection holds the declaration, a write
  transaction by any NON-declared connection is refused with the reason
  and the fix.  Reads are never affected.
- The mode releases when the last declaring connection closes or turns
  the pragma OFF.  A crashed queue writer must release it too — the
  mechanism must make abandonment impossible, not merely unlikely.
- `PRAGMA queue_writer` (no argument) reports this connection's own
  declaration; `PRAGMA queue_mode` reports whether the database is under
  queue discipline right now (0/1) — queryable by exactly the connection
  being refused.
- Healthy inertness: a database no one declares on behaves byte-identically
  to today.  The suites are that pin.
- Refusal message, pinned: `database is in queued-write mode; writes go
  through the queue owner (see PRAGMA queue_mode), or wait for the queue
  connection to close`.

## Decisions inside the sketch (Sean may veto)

- **WAL/wal2 only.**  The cross-connection substrate is the shm region,
  which exists only in WAL modes.  `queue_writer=ON` on a rollback-mode
  database refuses with the reason and the fix (`journal_mode=wal2`).
  This is honest scoping, not a gap: the deployment this serves
  (DESIGN-NETWORK) is WAL by prescription.
- **Enforcement point is the OP_Transaction write gate** — the choke point
  degrade-at-load (DOCKET #9) proved: fires exactly when a statement opens
  a write transaction, per-database, never on compile-time bookkeeping.
- **`writable_schema=ON` does NOT bypass this gate** (unlike degrade's).
  Degrade's exemption exists so a file frozen in every build can be
  repaired; queue mode has a living owner whose closing releases it, so
  there is nothing an expert override rescues — it only reintroduces the
  barge-in Sean ruled out.

## The open mechanism question — P1 answers it by measurement, not taste

The shm lock table has exactly `SQLITE_SHM_NLOCK` (8) slots, all
assigned (write, ckpt, recover, 5 read marks).  Candidates:

1. **Extend the lock region** (slot 8).  Offsets are compile-time
   constants; an older fork build sharing the LIVE file neither takes nor
   respects the new slot.  Skew consequence: enforcement holds only among
   queue-aware builds.  That may be acceptable — it is degrade's own
   posture (protection starts at the build that carries it) — but it must
   be DECLARED, and the os layer's assumptions about the lock-byte range
   must be verified on win32 and unix both.
2. **A flag word in wal-index header space.**  Same skew posture, plus
   torn-write and recovery-time questions the lock slot doesn't have —
   but no os-layer surface.
3. **A sidecar lock file** (`db-queue`).  No shm coupling at all, works in
   rollback mode too; costs a new file the ecosystem must learn, and
   liveness (crash cleanup) has to be built instead of inherited from the
   os advisory-lock semantics that make 1 self-releasing.

Leaning 1 — crash-release is inherited from the same mechanism that makes
a dead writer's WAL_WRITE_LOCK evaporate, which is the property the
contract calls load-bearing.  P1 measures the os-layer reality before
committing; if 1 survives contact, 2 and 3 are recorded as rejected with
this paragraph as the reason.

### P1 MEASURED 2026-08-13 — slot 12 plus a hint byte; slot 8 is a trap

- **"Slot 8" is taken**: `WIN_SHM_DMS`/`UNIX_SHM_DMS` — the dead-man
  switch — is byte `SHM_BASE+SQLITE_SHM_NLOCK` = 128 on both os layers.
  Extending the slot range naively would have landed the queue lock on
  the liveness byte.  Found by grep, not by crash.
- **wal2 does NOT use extra slots** — its own header comment: the wal2
  read-locks "use four of the six read-locking slots used by legacy wal
  mode."  The `<<(SQLITE_SHM_NLOCK+i)` shift that looked like slot
  extension is SEH cleanup *bookkeeping* (shared locks in the low byte
  of a mask, exclusive in the high), not lock addressing.
- **Bytes 129–131 are live data** (`nBackfillAttempted`'s tail);
  **bytes 132–135 are `WalCkptInfo.notUsed0`** — reserved, never read
  or written by any build, and inside the byte range the os lock calls
  can address.  Same reserved-space arrangement `aLock[]` itself uses.
- **Ruling: the QUEUE lock is byte 132 ("slot 12"); byte 133 is the
  hint.**  Declarants hold SHARED on slot 12 for connection lifetime
  (os releases file locks on handle close and process death — crash-
  release inherited, as hoped).  The write gate's fast path reads hint
  byte 133 from the mapped shm — one memory read; healthy files pay
  essentially nothing.  Only when the hint is set does the gate probe
  slot 12 with a try-EXCLUSIVE for the truth; a probe that finds the
  lock free clears the stale hint (crashed declarant) and proceeds.
- **Costs found with it:** the os-layer asserts (`os_win.c:4401`'s
  `ofst+n<=SQLITE_SHM_NLOCK`, `os_unix.c:4718`, and os_win's
  WAL-protocol sanity block) must learn the new slot; our lock calls
  bypass `walLockExclusive`'s SEH `lockMask` bookkeeping, so a
  `SQLITE_USE_SEH` build would not auto-release the queue lock after a
  structured-exception fault — either add the mask handling or refuse
  the pragma under SEH builds; decide in P2, not by default.
- Options 2 (wal-index flag word alone: no crash-release) and
  3 (sidecar file: liveness hand-built, new ecosystem surface) are
  REJECTED for the reasons the leaning paragraph predicted.

## Phases

*ALL EXECUTED 2026-08-14, one session from ruling to green.  Receipts:
queue1 0/20; walseh1 0/260; sweep 42/42 both regimes; veryquick clean
in BOTH regimes (release 0/393,879; DEBUG=3 0/394,857); and the
overnight first: a Linux build (WSL Ubuntu) sweeping the full 42-suite
roster 42/42, so the os_unix.c slot edits are measured rather than
mechanically-consistent-and-hoped.  The mode is dogfooded into
tool/proc_queue.c (14 checks), which promptly found the fresh-empty-
file edge now recorded in README-QUEUE.md.  Two corrections the gates bought:*

- *The first cut's SEH exception path FABRICATED a "mode active"
  verdict when the shm read faulted — a spurious queue refusal where
  the truth was an I/O error.  walseh1's fault injection caught it
  (three legs, the refusal message verbatim in the transcript).  The
  API is now rc-plus-out-param the whole way down, and a faulted check
  propagates exactly what BeginTrans would.  A fallback VALUE on a path
  feeding a decision — the instrument sin, in engine code.*
- *veryquick also surfaced the DEGRADE campaign's unpaid gate (15
  corrupt-family re-pins + the empty-reason fix), recorded in
  PLAN-DEGRADE's post-campaign corrections.*

- **Q0 — the spec, red.**  `test/queue1.test`, written before any code:
  declaration + refusal + release + reporting pragmas + rollback-mode
  refusal + healthy inertness + two-connection crash-release (close
  without OFF releases).  First run shows the pragmas failing to exist.
- **P1 — the substrate.**  Answer the mechanism question with a measured
  probe; implement the declaration and the cross-connection visibility.
- **P2 — the gate + surfaces.**  OP_Transaction check; the two pragmas;
  pinned messages.
- **P3 — gates + docs.**  Full sweep both regimes (inertness); DESIGN-
  NETWORK §1a annotated BUILT for this piece; DOCKET cross-reference.

## Deliberately NOT in this campaign

The queue itself (drain loop, waiters, batching, priorities); the server;
the wire transport; the TypeScript emitter (waits on a wire to speak
through); any change to rollback-mode behavior.
