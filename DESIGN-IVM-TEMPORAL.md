# DESIGN-IVM-TEMPORAL — the remaining tier, and time-travel rollups

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

## POC obligations (before rulings are requested to be acted on)

- **POC-T1**: temporal base + hand-built maintenance-shaped triggers —
  do the two synthesized families fire in stable order?  Does the
  storm test + view_check stay clean?  (Blocks Q1.)
- **POC-T2**: a temporal mview mock (temporal table maintained by a
  user trigger from a base) run through capture -> segment -> apply —
  measure whether replica recompute + shipped shadow rows double-write
  or agree.  (Blocks Q4.)
- **POC-T3**: DISTINCT refcount delta under the one-row trigger CTE —
  the Tier-1 identity survived joins only by deletion; check the
  dcount identity early.  (Blocks Q5's DISTINCT lean.)

## Not in scope

Read-side folding (ruled never), promotion of mviews into the
replication protocol (they recompute), era-stamped history schema
(temporal v2, separate thread).
