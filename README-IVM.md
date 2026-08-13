# Materialized views with incremental maintenance

*Fork feature, campaign of 2026-08-12 (DOCKET #4).  Design record:
`DESIGN-IVM.md` (six questions, Sean's rulings dated).  Phase record:
`PLAN-IVM.md` (each phase annotated EXECUTED with its receipts).
Specification: `test/ivm1.test`, written red before any code existed;
its going green whole (0 of 24) was the campaign's definition of done.*

```sql
CREATE MATERIALIZED VIEW balances AS
  SELECT account, sum(amount) AS balance, count(*) AS entries
    FROM ledger GROUP BY account;
-- reads are ordinary table reads, always exact, indexable:
CREATE INDEX bal_ix ON balances(balance);
SELECT account FROM balances WHERE balance > 100;
```

## The object

A materialized view is a **real table under its own name**: schema row
`type='mview'`, a rootpage, `sql` holding the full defining statement.
Ordinary reads, `CREATE INDEX`, WAL, `.dump`, `integrity_check` — all
free, because nothing about the storage is special.  What is special
is enforced at the edges:

- **Columns re-derive from the stored SELECT at every schema load.**
  This is why `DROP TABLE`, `RENAME`, `RENAME COLUMN` and
  `DROP COLUMN` on a base table with a dependent view are refused,
  naming the view and the fix: a definition that stopped resolving
  would fail the next open of the whole database file.  `ADD COLUMN`
  stays legal because `SELECT *` definitions are refused (they would
  change shape when the base did).
- **User writes are refused with the reason.**  The one writer a view
  accepts is its own maintenance program.  `writable_schema=ON` and
  VACUUM's rebuild are the declared exceptions.
- **The Tier-1 conformance walk refuses everything it cannot
  maintain, by name, at CREATE**: min/max (Tier 2), DISTINCT
  (Tier 3), joins, subqueries, window functions, CTEs, LIMIT,
  ORDER BY, HAVING (see the dated amendment in DESIGN-IVM.md),
  expressions over aggregates, non-grouped bare columns, and
  non-deterministic functions — including `date('now')`, whose
  upstream flags claim CONSTANT because 'now'-detection is deferred
  to a runtime guard that a stored definition never gets; the walk
  analyzes date-function arguments itself.

Supported (Tier 1): `COUNT(*)`, `COUNT(x)`, `SUM(x)`, `TOTAL(x)`,
`AVG(x)`; `WHERE`; deterministic GROUP BY expressions and constants;
one ordinary base table.  **Tier 2 stage two (PLAN-IVM3): INNER
JOINS** — any number of ordinary base tables, each appearing once,
under any deterministic inner-join conditions.  Maintenance runs a
trigger set per base; each base's deltas re-run the definition with
that one table shadowed by the changed row, so the join probe uses
whatever index exists — and `view_check`'s advisory names the exact
`CREATE INDEX` per unserved probe column when one is missing.  A join
view carries no hidden bookkeeping: it must declare `count(*)` (that
column IS the group's liveness), `COUNT(x)`/`TOTAL`/`MIN`/`MAX`
maintain from their own outputs, and `SUM`/`AVG` refuse by name in
join views (their NULL-restoring counts are not derivable across a
join — `total()` is the workaround).  Self-joins and outer joins
refuse by name.  **Tier 2 stage one (PLAN-IVM2): `MIN(col)`
and `MAX(col)` over plain columns.**  Inserts maintain by comparison
(with the column's collation, captured at CREATE and spelled
explicitly in the generated SQL); deleting the stored extremum
re-runs the definition for that ONE group — a lazy rescan that only
evaluates when the departing value binary-matches the stored one, so
interior deletes cost nothing (measured at zero base scans in
`ivm2.test`).  With an index leading on the view's key columns the
rescan is a seek, and if that index is missing, `PRAGMA view_check`
says so: a `kind='advisory'` row carrying the runnable
`CREATE INDEX` statement, silent once the index exists.  For deferred
views, `view_refresh` repairs extremum columns per touched group.
`min`/`max` over expressions refuse by name (stage two).

## Maintenance

Hidden bookkeeping columns (`COLFLAG_HIDDEN`; invisible to `SELECT *`,
`table_info`, NATURAL joins; visible to `table_xinfo` and explicit
reads, which is the honest arrangement for engine state): `ivm$count`
(group liveness), per-SUM a non-null count (what lets SUM return to
NULL), per-AVG a sum and count (the visible average is their
quotient).  A UNIQUE key index `ivm$<view>$key` is created with the
view; dropping it costs speed, never correctness.

Three AFTER triggers per view are **synthesized as SQL text from the
stored definition and compiled by a private sub-parse at schema load**
— never persisted, not droppable, invisible to `.dump`.  The
construction rests on one identity: for a one-row delta, the
definition's own outputs ARE the bookkeeping deltas (the avg of one
row is the value), so the definition text drops in unchanged behind a
CTE named after the base table, which shadows it with the single
NEW/OLD row.  Application matches group keys with `IS`, never
`ON CONFLICT` — UNIQUE calls NULL keys distinct, GROUP BY calls them
one group.

`WITH MAINTENANCE EAGER` (default): deltas apply inside the writing
statement; reads are always exact; rollback undoes maintenance with
the write.  `WITH MAINTENANCE DEFERRED`: the same deltas append to
`sqlite_ivm_<view>_log` with a ±1 weight, and `PRAGMA view_refresh`
folds the log grouped — N deltas against one group cost one
application.  Reads of a stale deferred view never block and never
fold (a stale read's compiled program contains zero write opcodes —
tested, not asserted); they see the last refreshed state, and the
staleness is queryable everywhere the user already looks.

## The truth surfaces

- `PRAGMA view_check(name)` — the equality oracle: recomputes the
  definition and diffs against the stored rows **inside one
  statement** (one snapshot), reporting one row per disagreeing group
  (which side, rendered values) and ALWAYS a coverage summary —
  `'N groups compared, M differ'` — so an empty diff over zero groups
  reads as the finding it is.  On a stale deferred view the FIRST row
  is `kind='stale'` with the pending count and the exact refresh
  command.  No argument = check every view.
- `PRAGMA view_list` — (name, maintenance, pending, stale).  Eager
  views report 0/0, true by construction; deferred views report the
  log's actual row count.  Nothing here is ever a guess: before the
  capture mechanism existed, these columns were NULL, not zero.
- `PRAGMA view_refresh(name)` — fold now, atomically with the log
  truncation.  No argument = refresh every deferred view.  Eager
  views refresh as a no-op (nothing is ever pending).

## The costs, stated loudly

**A procedure that writes a maintained base table loses the body
cache.**  Its body now fires a trigger, and bodies that fire triggers
compile per statement (`PROC_CACHE_SUBPROG`).  This is the Q3 ruling's
documented tradeoff, accepted with eyes open, and it is *visible*:
`PRAGMA proc_check` names the reason on the affected procedure —
`compiles per prepared statement: the body fires a trigger or CALLs
another procedure`.  If this cost ever dominates, the recorded
alternatives are teaching the body cache to carry trigger subprograms,
or the session/preupdate capture layer (DESIGN-IVM Q3-B′), which
becomes self-funding the day replication work starts.

Other costs: writers pay per-row maintenance on eager views (the
xfer/truncate fast paths also disable themselves, which is a
correctness dependency); bulk-load-then-CREATE beats CREATE-then-bulk-
load; deferred views trade read freshness for write speed and the
refresh is whoever runs the pragma.

## Sharp edges

- **A database containing any materialized view is unreadable AT OPEN
  by binaries that do not understand it** — stock SQLite and older
  fork builds alike fail schema load with "malformed database schema",
  and the lockout covers every table in the file.  Found by dogfood
  the first morning (DOCKET #6, with three candidate postures awaiting
  a ruling).  Until then: a db with mviews is
  exactly-this-fork-version-or-nothing.

- The `ivm$` column-name prefix and the `sqlite_ivm_` table namespace
  are reserved (refused in definitions; protected by the sqlite_ rule).
- The delta log is user-readable and, like `sqlite_sequence`,
  technically user-writable.  Writing it corrupts pending state;
  `view_check` will say so.
- User triggers ON a materialized view fire on maintenance writes.
  Legal, occasionally useful, easy to surprise yourself with.
- `.dump` of a deferred view intentionally omits the log: restore
  repopulates from live base data, which absorbs whatever was pending.
- AVG maintenance recomputes sum/count in double precision.  For
  integer inputs this matches from-scratch AVG exactly; for
  floating-point inputs, accumulated rounding can in principle drift
  within double-precision noise.  `view_check` is the detector.
- TEMP materialized views are refused (v1), with the reason.

## Isolation tactics (for maintainers)

Everything feature-specific lives in `src/mview.c`.  The hooks in
upstream files are one-to-few-line steers, each guarded by `IsMView()`
or a fork-named Parse/Trigger flag: `build.c` (object-name type steer,
EndTable type strings, DROP refusals, unlink), `trigger.c` (lazy
synthesis before the no-triggers short-circuit; FinishTrigger hand-off;
maintenance-writer flag into the body sub-parse), `tokenize.c` (the
sub-parse hand-off survives RunParser's tail), `delete.c` (the write
refusal and its exemptions), `vacuum.c` (views rebuild as tables; log
DDL excluded, content copied; synthesis suppressed under
DBFLAG_Vacuum), `callback.c` (registry lifecycle with the schema),
`pragma.c` + `tool/mkpragmatab.tcl` (the three pragmas), `shell.c.in`
(dump/table whitelists).  The in-memory registry (`Schema.mviewHash`,
one `MViewInfo` per view) follows the `procHash` lifecycle exactly.
