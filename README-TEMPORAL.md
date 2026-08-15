# System-versioned temporal tables

*Fork feature, campaign of 2026-08-15 (DOCKET #6).  Design record:
`DESIGN-TEMPORAL.md` (eight questions, Sean's rulings dated, three POCs
before any question was drafted).  Phase record: `PLAN-TEMPORAL.md`.
Specification: `test/temporal1.test` written red before the feature
existed; the replay proof is `test/temporal2.test`, whose corruption
control stays in the suite forever.*

```sql
CREATE TEMPORAL TABLE acct(id INTEGER PRIMARY KEY, bal INTEGER)
  WITH SYSTEM VERSIONING;

UPDATE acct SET bal=200 WHERE id=1;          -- history captures itself

SELECT bal FROM acct FOR SYSTEM_TIME AS OF 3 WHERE id=1;
SELECT bal FROM acct FOR SYSTEM_TIME AS OF '2026-08-15 14:30:00.000'
 WHERE id=1;
```

## What system time IS here

**The commit sequence — not the clock.**  Every committing transaction
that touched a versioned table takes the next monotonic sequence
number; a commit log (`sqlite_hist_commits`) records the UTC wall-clock
beside each.  `AS OF <integer>` is exact; `AS OF '<utc text>'` resolves
through the log to the **last commit at or before** that instant — so a
temporal query only ever answers states that were genuinely current.
The hand-rolled trigger pattern every project reinvents cannot say
that: POC 1 measured intra-transaction intermediates persisted as
millisecond-wide "facts," and a 137ms stamp-to-commit window asserting
a state every live reader could refute.  Wall-clock here is a chart on
the sequence, not the axis — and commits faster than the chart's
millisecond resolution simply share an entry (the resolver's contract
is at-or-before, pinned in temporal2).

## The pieces

- **`sqlite_hist_<name>`** — the shadow: a real table (indexable,
  `.dump`-able, `integrity_check`ed) holding row images plus
  `(seq_from, seq_to)` intervals.  The interval columns are HIDDEN
  (the `ivm$` precedent); the table is read-only to users, writable by
  the engine's own capture programs, `writable_schema=ON` the declared
  repair exception.
- **Capture** — synthesized internal triggers (the mview machinery)
  doing last-write-wins merges under a per-transaction reserved
  sequence held C-side and cleared at commit/rollback.  Only the
  transaction's FINAL state of each row enters history.  The two SQL
  functions involved touch nothing but that C-side integer; every read
  and write of storage is visible trigger text.
- **`FOR SYSTEM_TIME AS OF expr`** — a table-reference suffix; joins
  may mix moments deliberately.  Rewritten at expansion onto the
  shadow with interval containment; the resolver runs once per query.
- **`PRAGMA history_list`** — (name, watermark, nhist) per versioned
  table, counts read live.
- **`sqlite_history_prune(tbl, upto)`** — raises the watermark and
  deletes closed intervals wholly before it.  Explicit, owned by its
  caller, `DIRECTONLY` (it writes; views and triggers cannot reach it).
  **`AS OF` earlier than the watermark REFUSES**: "history of t is
  pruned before seq N; the answer would be fabricated."  Nothing
  fabricated — the clause of the design that matters most.

## Discipline

- `DROP TABLE` on a versioned table refuses with the fix;
  `DROP TEMPORAL TABLE` takes table, shadow, and meta row in one
  intent.
- `ALTER ... ADD COLUMN` works and propagates to the shadow — absent
  history reads NULL, which is honest (the column did not exist).
  Every other shape change refuses: history carries the current shape
  (era-stamped history schema is recorded v2).
- Materialized views over versioned tables refuse in the conformance
  ladder (time-travel mviews are a future feature, not an accident).
- Version skew: temporal tables inherit degrade-at-load (DOCKET #9) —
  an older fork build opens the file, dead-marks the temporal objects,
  reads everything else.  This was #9's stated purpose; temporal is
  its first tenant.  (And graduating the fixture's fiction into a
  feature required re-planting degrade's future — see PLAN-TEMPORAL.)

## The proof

`test/temporal2.test`: per commit k, a fresh database replays the
workload through k — never reading history — and its live state must
equal `AS OF k`.  The corruption control corrupts one interior
interval, watches the proof go red, restores, and watches it recover.
An instrument that has never failed proves nothing; this one fails on
demand, forever.

## v1 boundaries (recorded, not hidden)

Temporal tables live in `main` (attached-db generalization recorded);
`FOR SYSTEM_TIME BETWEEN` waits; the commit-log UTC is stamped at the
transaction's FIRST temporal write, not the commit instant — the seq
axis is exact, the text-axis chart inherits a stamp-to-commit window
on long transactions (v2 can re-stamp at commit); savepoint rollback
past the reservation is a recorded caveat; retention POLICIES (Sean's
Q6 brainstorm) are v2 shapes.

## Isolation tactics (for maintainers)

Everything feature-specific lives in `src/temporal.c`.  The hooks are
one-to-few-line steers: `parse.y` (TEMPORAL keyword, table option, the
AS OF suffix), `build.c` (pairing validation + companions + stored-DDL
head + splice offset + refusals), `trigger.c` (lazy synthesis under
its own guard), `select.c` (one expander hook), `expr.c` (SrcItem dup
+ the engine-program DIRECTONLY exemption), `delete.c` (the read-only
wall's exemptions), `prepare.c` (dead-marking unregisters half-built
objects — a degrade refinement temporal earned), `pragma.c` +
`mkpragmatab.tcl` (history_list), `callback.c` (registry lifecycle).
The capture-trigger registry (`Schema.histHash`) follows `mviewHash`
exactly.
