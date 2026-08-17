# DESIGN-IVM-TEMPORAL — the remaining tier, and time-travel rollups

## RULINGS — Sean

- **Q4 RULED 2026-08-16: ship-and-stand-down for temporal mviews.**
  ("I'm liking your leaning as well.")  The stream carries the
  primary's rollups and rollup-history verbatim; apply-time stand-down
  extends to temporal-mview maintenance; temporal-mview storage gains
  a PK so it ships.  Plain mviews stay on their recompute path, which
  this ruling makes EXPLICIT policy rather than an accident of the
  session's no-PK skip.
- **Q1 RULED 2026-08-16: lift the refusal** (current-state mviews over
  temporal bases).
- **Q2 RULED 2026-08-16: TEMPORAL MATERIALIZED VIEW, EAGER-only in
  v1**; DEFERRED+TEMPORAL refused by name.
- **Q3 RULED 2026-08-16: AS OF in definitions refused permanently**,
  naming CREATE TABLE ... AS SELECT ... FOR SYSTEM_TIME AS OF as the
  fix.  Sean's reasoning recorded: it would open a set of complex
  interactions that would inherently be bug-prone.
- **Q5 PARTIAL: HAVING agreed** (read-side filter over fully-tracked
  groups).  DISTINCT: Sean asked for a POC of DISTINCT-OVER-JOINS
  before ruling — his motivation from practice: normalization does not
  always map uniqueness onto the root row, so recovering entity
  uniqueness through a join (COUNT(DISTINCT root) over a multiplying
  join) is a real consumer shape, not a corner.  POC-T4 below.
- **Build order after Q5 is decided** (Sean): Q1 -> Q2+Q4 -> Q5.

*Opened 2026-08-16 after the replication campaign closed, as the
decision packet for DOCKET #4's remainder: Tier-3 aggregates and the
temporal x mview composition (the refusal at mview.c:622 says "a
future feature, not an accident" — this is that future asking for its
rulings).  Nothing here is implemented.  POC obligations are listed
and OWED before any ruling is acted on; the leans are mine, the
rulings are Sean's.*

## What exists on each side (both complete, both tested)

- **IVM**: Tier 1 (COUNT/SUM/TOTAL/AVG, WHERE, single table) + Tier 2
  (MIN/MAX with the index advisory; inner joins with count(*) as
  liveness).  Eager and deferred.  `PRAGMA view_check` is the equality
  oracle.  Refused by name today: DISTINCT, HAVING, outer joins,
  subqueries, window functions, CTEs, and temporal bases.
- **TEMPORAL**: commit-seq axis, shadow-with-PK, AS OF, prune with
  watermark refusal, and (since yesterday) full replication of the
  history surface.

## The questions

### Q1 — May an mview read a temporal base's CURRENT state?

The refusal today is precautionary, not semantic: an mview maintains
from base-row deltas, and a temporal table's deltas are exactly its
base-row writes (the history capture is separate machinery listening
to the same writes).  A current-state mview over a temporal base needs
no new algebra — the two trigger families already coexist on one
table for replication's stand-down logic.

**Lean: yes, lift the refusal in v-next** — after POC-T1 (below)
proves the two synthesized trigger sets fire in a deterministic order
and view_check stays clean under the storm test on a temporal base.

### Q2 — TEMPORAL MATERIALIZED VIEW: history OF the rollup?

`CREATE TEMPORAL MATERIALIZED VIEW ... WITH SYSTEM VERSIONING
[MAINTENANCE EAGER]` — the mview's own states become versioned, and
`SELECT ... FROM mv FOR SYSTEM_TIME AS OF k` answers "what did the
rollup say at commit k?"  This is the composition with real product
pull (dashboards that can be audited; the Mosaic radar note's
"what did this look like when I cited it").

The mechanics fall out of what is already built: eager maintenance
writes the mview inside the writer's transaction; a temporal mview's
shadow captures those writes under the SAME reserved commit seq as
the base writes that caused them — one commit, one seq, everywhere.
The subtlety: maintenance writes are engine-context
(bMViewMaintProg), and temporal capture triggers must FIRE for them
on the mview while user writes stay refused — today's exemption
matrix (delete.c) already distinguishes these paths.

**Lean: build it, EAGER-only in v1** (a deferred temporal mview would
version fold-times rather than commit-times — a chart that lies about
when).  Refuse `DEFERRED` + `TEMPORAL` together, by name.

### Q3 — AS OF inside mview definitions?

`CREATE MATERIALIZED VIEW m AS SELECT ... FROM t FOR SYSTEM_TIME AS
OF 5` — a frozen snapshot as a table.  It never changes, so it is not
maintenance at all; it is `CREATE TABLE AS` wearing mview clothes.

**Lean: refuse, permanently, naming `CREATE TABLE ... AS SELECT ...
FOR SYSTEM_TIME AS OF` as the fix.**  An mview that cannot change has
no business in the maintenance registry.

### Q4 — Replication of temporal mviews

Today's stand-down matrix: temporal capture stands down during
changeset apply (history ships as values); mview maintenance does NOT
stand down (its storage does not ship; replicas recompute).  A
TEMPORAL mview is both at once: its maintenance must RUN on the
replica (recompute the rollup) but its history capture must NOT
re-stamp... except the recomputed rollup writes are the replica's own,
while the history rows ALSO arrive in the changeset (the temporal
mview's shadow has a PK, so it ships).

**Lean: on the replica, a temporal mview's maintenance runs and its
capture stands down; its shipped shadow rows are authoritative.**
The view_check oracle plus repl2's pattern (aim the replay proof at a
replica) make this testable exactly like everything else.  POC-T2
must measure the double-write risk before this is trusted.

### Q5 — Tier 3 proper: DISTINCT and HAVING

- **DISTINCT**: needs per-value reference counts — a hidden
  `ivm$dcount` per distinct key.  Well-understood algebra, real
  bookkeeping cost, and the join lesson (hidden columns collapse
  under joins) says DISTINCT + joins compose only with the count(*)
  liveness discipline.  **Lean: single-table DISTINCT in Tier 3;
  DISTINCT-over-joins refused by name until a consumer asks.**
- **HAVING**: moved to Tier 2 by amendment but not built.  A group
  failing HAVING must still be TRACKED (it can pass later), so the
  storage carries all groups and HAVING filters at read.  That is a
  WHERE-on-read, cheap and honest.  **Lean: build in Tier 3 as a
  read-side filter over fully-tracked groups.**
- Outer joins, subqueries, window functions, CTEs: **stay refused**,
  each by name (no consumer, real algebra cost).

## POC obligations — PAID 2026-08-16 (results, and what they changed)

- **POC-T1 (blocks Q1) — CLEAN.**  In a throwaway build with only the
  refusal lifted, a REAL mview over a REAL temporal base survived a
  1500-op storm (inserts/updates/deletes/REPLACEs): mview
  oracle-equal at every step, history invariant (exactly one open
  interval per live key) intact throughout, 780 commits.  Write order
  is deterministic: base -> commit log -> history -> mview.  **The Q1
  refusal was purely precautionary; lifting it is low-risk.**  Cost
  characteristic: both trigger families serialize per row on the
  write path — a temporal+mview base pays both captures per write.
- **POC-T2 (blocks Q4) — the fork is REAL and today's applier picks
  neither side.**  User/maintenance triggers DO fire during
  changeset apply (measured: one firing per applied row), so a
  replica recomputes rollups locally — AND the shipped rollup rows
  collide with that recompute (2 conflicts measured).  A permissive
  OMIT handler happens to converge (recompute is deterministic and
  local capture stands down, so shipped history applies cleanly), but
  the REAL applier refuses on conflict — a temporal mview would read
  as false divergence today.  Also measured: plain mview storage has
  NO PK, so it never ships — today's replication of plain mviews is
  recompute-only BY ACCIDENT of the session no-PK skip.  **Q4 must
  choose one writer explicitly**; see tradeoffs below.
- **POC-T3 (blocks Q5 DISTINCT) — identity HOLDS; found a live bug on
  the way.**  Single-table DISTINCT refcounts survived a 2000-op
  storm with NULLs exactly (refcounts equal true multiplicities
  throughout); cost ~43 opcodes per maintained insert.  The sharp
  edge: REPLACE under recursive_triggers=OFF broke the refcounts —
  and the SAME hole existed in SHIPPED Tier-1 and SHIPPED temporal
  capture (UNIQUE-displaced rows left ghost open intervals; AS OF
  returned states no reader saw).  **Fixed in the engine the same
  day**: displaced rows always reach synthesized capture; user
  delete triggers stay gated on the pragma exactly as upstream
  (insert.c/trigger.c; pinned by ivm4.test and temporal1 section 12).

## POC-T4 (2026-08-16, at Sean's request) — DISTINCT-over-joins

Sean's consumer shape from practice: normalization does not always map
uniqueness onto the root row; COUNT(DISTINCT root) over a multiplying
join recovers entity uniqueness.  The construction probed: IVM3's
per-base delta triggers computing the join-row delta for the one
changed row, feeding T3's refcounts — dcounts(value, cnt) counts JOIN
ROWS per distinct value, one HIDDEN SIDE TABLE per DISTINCT aggregate
(per-value bookkeeping, which is exactly why it survives the join
lesson that killed per-row hidden columns).

**Verdict: the identity HOLDS over joins.**  2500-op storm — inserts,
deletes, child-rebind updates (i.oid), counted-value updates (o.cust),
REPLACEs, NULLs, dangling children — zero mismatches, refcounts equal
to true multiplicities throughout, and the survival case exact (a
value outliving one of its parents via another).  Costs: child-insert
~52 opcodes, parent-delete ~68, counted-value update ~143 (the
heaviest: it moves count-of-children refs between values).  The
parent-side deltas scan the child table by join key unless indexed —
IVM3's probe advisory (view_check emitting the runnable CREATE INDEX)
applies unchanged.  Composition note: the engine version feeds IVM3's
shadowed-definition delta rows (already proven exact for count(*))
into the refcounts, so multi-table joins and arbitrary INNER
conditions inherit that machinery rather than new algebra.
SUM(DISTINCT)/AVG(DISTINCT): not probed, refuse by name in v1.

## Q4 tradeoffs, made concrete by POC-T2

- **Ship-and-stand-down** (extend the apply-time stand-down to mview
  maintenance; shipped rows authoritative): one writer, byte-faithful
  rollups and rollup-history, no recompute cost at apply.  Cost:
  segments carry rollup rows (size), and the temporal-mview storage
  must gain a PK so it ships at all.
- **Recompute-and-filter** (never ship maintained tables; replicas
  recompute during apply): smaller segments, and it matches what
  plain mviews already do implicitly.  Cost: apply pays maintenance
  per row; correctness leans on maintenance determinism (holds for
  Tier 1/2 by construction); the temporal mview's HISTORY would then
  be re-stamped locally — the seq axis matches (same commits) but it
  must be proven, not assumed.
- **My lean after measurement: ship-and-stand-down.**  It extends the
  R5 principle (the stream carries the primary's truth verbatim) and
  the blessed-pin instrument can verify it end to end; the implicit
  recompute path for PLAIN mviews can stay as-is (it works and ships
  nothing extra), with the stand-down scoped to TEMPORAL mviews.

## Not in scope

Read-side folding (ruled never), promotion of mviews into the
replication protocol (they recompute), era-stamped history schema
(temporal v2, separate thread).
