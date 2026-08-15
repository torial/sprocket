# PLAN-TEMPORAL — system-versioned tables (DOCKET #6)

*Written 2026-08-15, the day of Sean's eight rulings (DESIGN-TEMPORAL).
The record is `git log`, not this file's claims.  Spec-first: the
campaign's definition of done is `test/temporal1.test` going green
whole, plus the replay-proof harness of Q8.*

## The shape, restated from the rulings

Commit-sequence time (Q1-B): every committing transaction that touched
a versioned table takes the next monotonic seq; a commit log maps seq
to UTC wall-clock.  Shadow history per table (Q2-A):
`sqlite_hist_<name>` holding the row image plus `(seq_from, seq_to)`.
Engine capture at commit (Q3-B): per-transaction last-write buffers
applied to the shadow inside the committing transaction.  SQL:2011
surface (Q4): `CREATE TEMPORAL TABLE ... WITH SYSTEM VERSIONING`,
`FOR SYSTEM_TIME AS OF <seq-or-utc-text>`.  ADD COLUMN only (Q5).
Explicit prune with a watermark that REFUSES pre-watermark AS OF (Q6).
Interactions as declared (Q7).  The POC-2b workload-replay proof with
its corruption control (Q8).

## A consequence found while writing the spec

**Graduating fiction requires planting new fiction.**  degrade1.test's
future-syntax fixture is literally `CREATE TEMPORAL TABLE tt ... WITH
SYSTEM VERSIONING` — the moment the grammar accepts it, that fixture
stops being the future and degrade1's pins break (the planted row has
a bogus rootpage, so it would fail differently, not die at "TABLE").
Phase P3 re-plants degrade1's future with syntax that stays invented
(`WITH QUANTUM VERSIONING`) in the same commit that makes the old
future real.  The degrade suite's job is meeting files from tomorrow;
tomorrow moving closer is the feature working.

## P1 architecture, decided at first contact (Sean may veto)

Q3-B's "buffer applied at commit" is re-spelled with identical
semantics and a simpler machine: **eager in-transaction shadow writes
under a reserved sequence.**

- A connection field (`db->pendingHistSeq`) is 0 outside any
  transaction.  The first write to any versioned table reserves
  `max(seq)+1`, inserts the `sqlite_hist_commits` row (seq, UTC-now),
  and holds the value until COMMIT or ROLLBACK clears it C-side --
  no SQL runs at commit, which is the whole difficulty dissolved.
- Capture is synthesized internal triggers (the IVM machinery, and
  legitimately this time: POC 1 ruled out triggers for *wall-clock
  stamping*, not for capture per se).  Each write merges: close the
  prior open interval (`seq_to = pending`), delete any same-txn
  pending row for the key, insert the new open image.  Last-write-wins
  is incremental, so intermediate states never survive the
  transaction -- disease 1 stays cured.
- Sequence order IS commit order: reservation happens under the write
  lock, and the single writer serializes both.  Disease 2's seq-axis
  cure stands.
- **The one honest wrinkle, flagged for Sean**: the commit-log UTC is
  stamped at the transaction's FIRST temporal write, not the commit
  instant.  The seq axis (Q1-B's truth) is exact; the text-axis chart
  inherits a stamp-to-commit window for long transactions.  v2 can
  re-stamp at commit if a hook materializes; the design's "chart, not
  manifold" framing is why this is tolerable rather than disease 2
  returning.
- Savepoint edge, recorded: ROLLBACK TO a savepoint predating the
  reservation removes the commits row while `pendingHistSeq` survives;
  the capture path re-verifies the row exists before trusting the
  reservation.

## Q0 corrections from first contact (2026-08-15)

- The spec's 1.0 listing predates the watermark storage decision: a
  `sqlite_hist_meta` table exists (WITHOUT ROWID, so no autoindex row),
  and the pin now includes it.
- The meta row is NOT created eagerly at CREATE: a nested INSERT
  compiles before the nested CREATE it depends on ever runs.  An
  absent row reads as watermark 0 -- honest, since nothing was pruned.
- Implementation scars P1 collected, for the record: the stored DDL
  head is RECONSTRUCTED by EndTable and must keep its TEMPORAL; table
  options merge selectively into tabFlags; the parse flag must be
  consumed before the same statement's nested plain CREATEs; the
  synthesis hook needs its own guard (not mview's); trigger bodies may
  not qualify table names; and DIRECTONLY functions in synthesized
  bodies need the engine-program exemption (bMViewMaintProg) so USER
  triggers stay walled out while the engine talks to itself.

## Phases

- **Q0 — the spec, red**: `test/temporal1.test` — DDL + shadow
  existence; ONE history row per changed row per COMMIT (POC 1 disease
  1 pinned absent); seqs advance per commit, not per statement;
  AS OF seq across full lifecycle (before-creation empty, post-delete
  absent, re-insert distinct); AS OF UTC text resolving through the
  commit log (pinned via the log's own values, not wall-clock guesses);
  DROP refusal + DROP TEMPORAL; ADD COLUMN history-NULL semantics +
  other ALTERs refusing; prune watermark refusal both sides; mview-
  over-temporal refusal; a small replay leg.  First run: syntax errors
  everywhere — the feature does not exist.
- **P1 — the axis**: commit sequence + commit log storage
  (`sqlite_hist_commits`? one per db), assigned at commit for
  transactions touching versioned tables; UTC capture.
- **P2 — capture**: per-txn last-write buffer, shadow apply at commit,
  rollback discipline (buffer dies with the txn).
- **P3 — DDL + skew**: CREATE TEMPORAL TABLE grammar, schema
  round-trip, shadow auto-creation, refusals (DROP/ALTER family);
  degrade1 re-planted the same commit.
- **P4 — the readout**: FOR SYSTEM_TIME AS OF rewrite onto the shadow
  (seq form, then text form via the log).
- **P5 — prune + watermark.**
- **P6 — the proof + gates**: POC-2b harness as a permanent suite;
  sweeps + veryquick BOTH regimes before any inertness claim (the
  degrade lesson, written where it will be obeyed); Linux run.
- **P7 — docs**: README-TEMPORAL, DOCKET #6, README-IVM cross-refs.

## Controls

- POC 1's two diseases pinned as engine tests: no intra-transaction
  state in history; no AS OF answer naming a state no committed
  snapshot contained.
- The replay proof's corruption control stays in the suite forever —
  the instrument is never trusted un-red.
- Healthy-file inertness: a database with no temporal tables pays
  nothing and changes nothing; the 43-suite roster is that pin.

## Deliberately NOT in v1

FOR SYSTEM_TIME BETWEEN; retention policies (Sean's brainstorm,
recorded in DESIGN-TEMPORAL Q6); era-stamped history schema (v2 of
Q5); temporal materialized views; replication of the commit log (the
essay's thesis waits for the replication campaign).
