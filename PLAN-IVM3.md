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

## Deliberately NOT in this campaign

Self-joins; outer joins; DISTINCT; HAVING; expression MIN/MAX;
cross-schema joins (the bases must share the view's schema — FixSelect
already enforces); the aux-schema unqualified-rescan edge recorded in
PLAN-IVM2 (pre-existing, unchanged here).
