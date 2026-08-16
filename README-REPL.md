# README-REPL — replication: shipping the fold

*Campaign 2026-08-15..16.  Design record: DESIGN-REPL.md (five rulings,
two POC rounds).  Plan and phase log: PLAN-REPL.md.*

## What replication is, here

Session changesets in commit-sequence order (Q1-C), cut into
**self-describing segments** — magic, format version, lineage (genesis
id), seq range, payload length, CRC — one segment per commit.  Segments
are the canonical substance (Q4-C): the daemon serves them live, the
same bytes archive on disk, and PITR is "apply through a target."  The
three UNGIT commitments of the Q4 ruling are load-bearing and each is
pinned by a check:

1. **Self-description** — everything an operator needs is in the bytes
   on disk, where they already look.
2. **Refuse, never skip** — gap, foreign lineage, checksum, truncation,
   divergence, missing schema: each refused BY NAME with the fix, each
   inert, each retained as a receipt.
3. **Declared sources** — archives and subscriptions are arguments,
   never watched directories.

## The pieces

- **`tool/sprocket_repl.c`** — the segment library (writer, applier,
  encoder, restore loop) and its 44-check selftest.  Standalone:
  `sprocket_repl --selftest`, and PITR:
  `sprocket_repl --restore DIR DB [--upto-seq N | --upto-utc TS]`.
- **`sprocketd --archive DIR`** — the daemon cuts one segment per
  commit into the declared archive (atomically, before the write's
  caller wakes) and serves `SUBSCRIBE` beside `CALL`: the subscriber
  always reads seq N from the archive file, waiting on the cut signal
  when N does not exist yet — catch-up and live-tail are ONE loop, and
  the wire carries the same bytes as the disk.  At startup the daemon
  verifies it can serve the whole lineage (no holes, right genesis,
  database not behind its own archive) — refusals happen when declared,
  not at 2am.
- **`PRAGMA replica_status`** — the truth surface over the
  `sprocket_repl_meta` receipts: (role, last_seq, last_utc, lag_source,
  last_error).  `lag_source` names the BASIS of the freshness claim;
  a database with no replication state says so in words (an empty
  result would be indistinguishable from an unrecognized pragma).

## The temporal axis replicates

POC 2's chain (recorded in DESIGN-REPL) ended with the whole temporal
surface riding ordinary changesets: base rows, every version interval,
and the commit clock — UTC shipped as VALUES, so the replica's chart
equals the primary's.  What it took, all in the fork's own hands:

- shadows declare `PRIMARY KEY(base-key, seq_from)` (their natural
  version key; also indexes AS OF);
- the session module carries explicitly-hidden columns (h==1) in
  changesets — generated columns stay excluded;
- `sqlite3changeset_apply` runs as a MAINTENANCE context
  (`DBFLAG_TemporalMaint`): replica shadows accept the stream, and the
  replica's own capture triggers stand down so history arrives
  verbatim instead of being re-stamped;
- the applier pre-scans the manifest and REFUSES missing or misshapen
  tables (the engine's own apply silently skips them — measured).

**The blessed pin (Q5)**: `test/repl2.test` runs temporal2's replay
proof against a replica built from per-commit changesets — corruption
control included, wired into the session permutation so it cannot rot.

## A bug found on the way (and refused at the door)

System versioning keyed PK-less rowid tables on rowid coincidence;
after the first UPDATE the shadow's rowids outrun the base's and
history closes the WRONG versions (measured: two open intervals for
one key; AS OF returning a state no reader ever saw).  A temporal
table now **requires a PRIMARY KEY** — refused at CREATE with the fix
named — and rowid tables with declared non-INTEGER PKs key on the PK
columns.  `temporal1.test` section 11 pins both.

## Schema travels out of band (v1)

Changesets carry rows, not DDL.  A replica or restore target declares
its schema first; the applier's refusal names this.  DDL replication is
a recorded v2 question.  Tables without a PRIMARY KEY do not replicate
(the session module's documented limitation); temporal tables always
have one now.

## Maintainer notes / least proud

- The daemon's segment cut runs on the queue thread between COMMIT and
  the callers' wake — one file write per batch on the hot path.  Fine
  at current scale; a slow disk stretches commit latency.  Recorded
  rather than optimized.
- `--upto-utc` refuses to pass a segment carrying no commit clock (a
  commit that touched no temporal table) instead of guessing; restore
  by seq remains.  This is honest but means time-based PITR needs a
  temporal-enabled primary.
- The R3-era "debug veryquick" gate was found to have run the release
  binary (nmake timestamp no-op); the R5 gate chain touches sqlite3.c
  before each build and re-covered both regimes over the union of
  changes.  Instrument scar recorded here so nobody trusts an
  'up-to-date' nmake in a regime chain again.
- sessionconflict.test (upstream) leaves its TCL log handler registered;
  with the fork's chattier (honest) log channel this poisoned every
  later file in the session permutation.  Fixed at the file's end, the
  way session3/sessionat already do.
