# PLAN-IVM3 — Tier 2 stage two: inner joins

*Written 2026-08-13, on Sean's "Let's proceed w/ equi-joins."  Parent
design: DESIGN-IVM.md Q2 Tier 2 (Sean's ruling); stage one (MIN/MAX +
advisory) landed 2026-08-12 as PLAN-IVM2.  Build mechanics unchanged.
The record is `git log`, not this file's claims.*

## The shape being built, in five sentences

Materialized-view definitions may now join: `FROM A JOIN B ON …`
(comma joins included), any number of tables, each an ordinary base
table appearing once.  Maintenance synthesizes a trigger SET PER BASE:
each base's triggers shadow THAT table with the one-row NEW/OLD CTE and
let every other table resolve to the real thing, so the join probe
happens inside the delta select with whatever index serves it — the
textbook Δ(A⋈B) = ΔA⋈B ∪ A⋈ΔB, serialized per row by the trigger
machinery, each delta seeing the other side current.  The registry
grows from one base to a base LIST (flags, drop/alter refusals,
synthesis, and teardown all loop it).  The advisory gains the design's
own example: per base, the OTHER side's equality-condition columns get
a runnable CREATE INDEX row in view_check when unindexed (the
join-probe advisory); the key/extremum advisory stays single-base-only
rather than guess across tables.  Deferred capture, the fold, the
extremum repair pass, view_check, population, and the log are all
SOURCE-AGNOSTIC and need nothing.

## Decisions inside the Q2 ruling (2026-08-13; Sean may veto)

- **Deterministic INNER conditions of any shape are accepted.**  The
  shadow construction is correct for arbitrary inner-join conditions
  (an inner join distributes over the union of one side's rows); the
  "equi" in equi-join is a COST property, so equalities feed the
  advisory rather than gate the walk.
- **Self-joins refuse by name** (the shadow would replace BOTH
  occurrences; the honest delta needs inclusion–exclusion — later).
  ivm1-1.4's fixture is a self-join; its pin moves to the new message,
  design-first.
- **Outer joins refuse by name** (Tier 3 per DESIGN-IVM: membership
  changes flip null-extended rows).
- Every FROM item must be an ordinary base table (views, vtabs,
  subqueries, other mviews: refused per item, existing messages).

## AMENDMENT (2026-08-13, mid-campaign; kethiv in Q0's spec, qere here)

The first build caught a real algebraic error in this plan's silent
assumption.  Tier 1's delta derivations rest on the ONE-ROW identity:
a delta contributes ivm$count=1, and a SUM's non-null count-delta is
`value IS NOT NULL`.  Under a join, ONE BASE ROW IS MANY JOIN ROWS,
and the identity collapses — measured as a group surviving its own
death (liveness decremented 1 when two join rows departed).

The definition's own aggregates over the delta group are still exactly
right (the delta select re-runs the definition, so its count(*) IS the
group's join-row delta).  What is not derivable from the outputs is
the HIDDEN bookkeeping — and the honest repair is that join views have
no hidden columns at all:

- **count(*) is REQUIRED in a join definition** (refused by name with
  the fix if absent): it is the group's liveness, and under a join it
  must be declared because it cannot be derived.
- COUNT(x), TOTAL(x), MIN/MAX(col): maintain from their own outputs,
  which are correct group-deltas by construction.
- **SUM and AVG refuse by name in join views**: their NULL-restoring
  counts (SUM's count-of-non-null, AVG's sum+count) are hidden parts
  the text cannot re-derive.  total() is the named workaround; a later
  stage can lift this by detecting declared sibling count(x)/sum(x)
  columns (sqlite3ExprCompare over the resolved trees makes that
  checkable at CREATE).
- The join delta select is therefore the MINIMAL form — the shadowed
  definition verbatim, no appended derivations — and the liveness
  column referenced by the death step, the fold, and view_list's
  arithmetic is the declared count(*) column rather than ivm$count.

Single-table views keep their hidden columns and their one-row
identities unchanged; every nBase==1 path is untouched.

Two further Q0 spec corrections from the first live runs, same
authority:

- The storm and reopen sections counted a DEFERRED view's honest
  staleness diffs as failures — lazyact exists from 6.x on and its log
  rightly accumulates the storm.  The legs now run no-arg
  `PRAGMA view_refresh` first, which is also the better test.
- The probe-economics pins measured absolute FULLSCAN_STEP counts,
  which include temp-structure steps (the shadow CTE's co-routine, the
  GROUP BY b-tree) — noise at the 0-vs-4 granularity.  EXPLAIN QUERY
  PLAN shows the join probe itself is already optimal (SCAN CONSTANT
  ROW, then SEARCH by rowid).  The pins now measure the O(1) claim
  directly: inflate the PROBED table with join-inert rows and require
  the per-op step count NOT to grow for an indexed probe, and to grow
  for an unindexed one, and to stop growing once the advisory's index
  exists.

## Phases

- **Q0 — the spec, red.**  `test/ivm3.test`: acceptance (2-table and
  3-table), refusals (self-join, LEFT JOIN, non-deterministic ON),
  eager maintenance from BOTH sides (child churn; parent delete
  killing groups; join-key updates moving rows between groups;
  parent-key update re-keying a group), extremum-in-join rescan,
  MEASURED probe economics (child-insert probes parent PK at zero
  fullscans; parent-delete probes unindexed child above zero; the
  advisory's carried statement executed verbatim drops it to zero),
  deferred join view converging, a two-table storm, reopen.
- **P1 — registry.**  MViewInfo: base LIST, per-base trigger arrays,
  join-probe column capture (per base, the plain columns it
  contributes to top-level equality conjuncts against other tables).
  All consumers loop: TF_MViewBase on every base, FindDependent,
  UnlinkAndDelete, teardown.
- **P2 — walk.**  Multi-item FROM acceptance (per-item ordinary-table
  checks), self/outer refusals, condition determinism (inner-join ON
  terms live in pWhere post-prep, so the existing WHERE walk covers
  them), equi collection.
- **P3 — synthesis.**  The per-base loop; trigger names carry the base
  ordinal.  Nothing else changes: the delta builder already takes "the
  changed table" as a parameter.
- **P4 — advisory v2.**  Join-probe advisories per base; key/extremum
  advisory guarded to single-base views.
- **P5 — docs.**  README-IVM, DESIGN-IVM landed-note, wiki; ivm1-1.4
  re-pin travels with the walk change (P2).

## Controls

- ivm1 0/24 (with the 1.4 re-pin) and ivm2 0/38 at every phase.
- Probe economics measured via FULLSCAN_STEP, both directions, and the
  advisory seen firing AND seen silenced by its own carried statement.
- Sweep both regimes; the roster gains ivm3.

## EXECUTED 2026-08-13 (the spec drained 25 → 0)

The registry grew its base list, per-base trigger arrays, and the
join-probe capture; the walk gained the per-item FROM loop with the
three refusals plus the amendment's two (SUM/AVG in joins, the
count(*) requirement); synthesis became a per-base lazy loop (each
base's set builds on ITS first DML — a table never written never pays);
and the delta select for join views collapsed to the MINIMAL form, the
shadowed definition verbatim, because a join view's outputs are already
its group deltas.  The advisory's join mode emits one row per unserved
probe column, IPK-aware (a rowid probe is never advised).

The first live run caught the campaign's real bug — the one-row
identity collapse recorded in the amendment above — measured as a
group surviving its own death, and the fix DELETED code: join views
carry no hidden columns, no delta derivations, no fold extras; the
liveness column is the declared count(*), threaded by name through the
death steps and the fold.  EXPLAIN QUERY PLAN confirmed the shadow
CTE join-probes optimally with no hints (SCAN CONSTANT ROW, then
SEARCH by rowid), and the economics pins were rebuilt to measure the
actual O(1) claim: an indexed probe's cost does not grow when the
probed table is inflated 20x with join-inert rows; an unindexed
probe's does; the advisory's own statement stops the growth.

Gates: ivm3 0/33; ivm1 0/24 (1.4 re-pinned at the self-join boundary,
history in the comment); ivm2 0/38; full sweep with ivm3 in the
roster, both regimes.

## Deliberately NOT in this campaign

Self-joins; outer joins; DISTINCT; HAVING; expression MIN/MAX;
cross-schema joins (the bases must share the view's schema — FixSelect
already enforces); the aux-schema unqualified-rescan edge recorded in
PLAN-IVM2 (pre-existing, unchanged here).
