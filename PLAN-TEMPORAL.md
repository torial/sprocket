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
