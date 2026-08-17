# PLAN-IVMT — temporal mviews, DISTINCT/HAVING, and the composition

*Written 2026-08-16, all five DESIGN-IVM-TEMPORAL questions ruled
(Sean), four POCs paid.  Spec-first: test/ivmt1.test is the campaign
spec, committed red on purpose — if an expectation is wrong, this
plan and the design doc change FIRST.  Build order ruled: Q1 ->
Q2+Q4 -> Q5.*

## The shape being built, in five sentences

A materialized view may read a temporal base (the refusal was
precautionary; POC-T1's storm was clean).  `CREATE TEMPORAL
MATERIALIZED VIEW ... WITH SYSTEM VERSIONING [WITH MAINTENANCE EAGER]`
versions the ROLLUP itself: the storage table gains the temporal
shadow, maintenance writes are captured under the same commit seq as
the base writes that caused them, and `FOR SYSTEM_TIME AS OF k` on the
view answers "what did the rollup say at commit k" through the
existing AS OF machinery (the storage is just a table).  DEFERRED +
TEMPORAL refuses by name (a deferred temporal mview would version
fold-times — a chart that lies about when).  Replication ships the
temporal mview's storage and history verbatim and the replica's
maintenance STANDS DOWN for it (Q4: ship-and-stand-down), while plain
mviews keep their recompute path as explicit policy.  Tier 3 adds
COUNT(DISTINCT x) — single-table and over joins — via per-value
refcount side tables, and HAVING via fully-tracked groups with the
filter applied when maintaining the visible table.

## Phases

- **P1 (Q1) — lift the temporal-base refusal.**  Remove the walk
  refusal; POC-T1's storm becomes the test (ivmt1 §1); REPLACE
  displacement over the composition pinned (rides the 2026-08-16
  displacement fix).  Gate: ivmt1 §1 green, all ivm/temporal suites
  untouched, both regimes.
- **P2 (Q2) — TEMPORAL MATERIALIZED VIEW, eager-only.**
  1. Grammar: TEMPORAL prefix on CREATE MATERIALIZED VIEW; WITH
     SYSTEM VERSIONING pairing enforced both ways (the temporal-table
     precedent); WITH MAINTENANCE DEFERRED + TEMPORAL refused by name.
  2. Storage: the view's table gains PRIMARY KEY(group cols) — an
     invariant worth having anyway — and TF_Temporal, so
     sqlite3TemporalEndTable builds its shadow with PK(group cols,
     seq_from).  Global aggregates (no GROUP BY): single-row storage;
     shadow keys on seq_from alone via an empty key predicate; if the
     empty-key predicate fights, v1 refuses global-aggregate temporal
     mviews by name and the plan records why.
  3. Capture: maintenance writes to the storage fire its temporal
     triggers (POC-T2 proved trigger-chain firing is not gated by
     recursive_triggers; the displacement path is already exempt).
     One commit -> one seq, base and rollup history stamped together.
  4. AS OF on the view: expander rewrite works on any TF_Temporal
     table — expected to compose with zero new code; ivmt1 pins it.
  5. view_check gains nothing new (the oracle recomputes current
     state); temporal2's replay-proof SHAPE aimed at the rollup is the
     new instrument: rollup AS OF k == from-scratch rollup of replay
     through k (ivmt1 §3).
- **P3 (Q4) — ship-and-stand-down.**
  1. The temporal mview's storage has a PK now, so session capture
     ships it and its shadow rows verbatim.
  2. Apply-side: synthesized maintenance triggers whose TARGET view is
     temporal stand down under DBFLAG_TemporalMaint (new Trigger flag
     set at synthesis: bTemporalMView).  Plain-mview maintenance keeps
     firing during apply (recompute path, now explicit policy).
  3. The pin: repl2's pattern with a temporal mview — replica rollup
     AND rollup history value-equal to the primary's, corruption
     control included, in the session permutation.
- **P4 (Q5) — DISTINCT and HAVING.**
  1. COUNT(DISTINCT x): conformance walk accepts it (single table and
     inner joins); per-aggregate hidden side table
     sqlite_ivm_d_<view>_<n>(val PRIMARY KEY, cnt) counting JOIN ROWS
     per value (POC-T4's construction); maintenance feeds the delta
     join rows the IVM3 machinery already computes; the visible column
     reads count(*) of the side table.  SUM/AVG(DISTINCT) refuse by
     name.  view_check verifies side-table refcounts against recompute
     (the T4 refbad query becomes engine surface).
  2. HAVING: accepted with Tier-1/2 expressions over the tracked
     aggregates; storage tracks ALL groups in a hidden side table when
     HAVING is present; the visible table carries only passing groups,
     maintained transactionally with the side table.  view_check
     compares both layers.
- **P5 — gates, docs, DOCKET; the INDEX NEED ANALYZER designed** (its
  own mini-design: input surfaces — procs, mview definitions,
  arbitrary SQL; output — one row per unserved probe with the runnable
  CREATE INDEX; the IVM3 advisory generalized).  Sean rules its
  design before it is built.

## Controls (stated before running)

- ivmt1.test is written RED before P1 code; sections map to phases.
- POC-T1's storm shape (mixed ops incl. REPLACE, oracle every step,
  history invariant every step) is the standing storm harness.
- The temporal-mview replay proof (rollup AS OF k == from-scratch
  rollup at k) must be seen red via the corruption control before it
  is trusted.
- Replication pin runs on session-enabled builds both platforms.
- Full gates per phase: veryquick both regimes + session suite +
  Linux sweep (the r5-r7 chain pattern; touch sqlite3.c before each
  regime build — the nmake up-to-date scar).

## Deliberately NOT in this campaign

Deferred temporal mviews (refused by name); SUM/AVG(DISTINCT) (refused
by name); outer joins, subqueries, window functions, CTEs (stay
refused); read-side folding (ruled never, standing); mviews over
temporal mviews (recorded as a question for when someone asks).
