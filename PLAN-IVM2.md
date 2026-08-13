# PLAN-IVM2 — Tier 2 stage one: MIN/MAX and the index advisory

*Written 2026-08-12, on Sean's "Let's proceed w/ Tier 2."  Parent design:
DESIGN-IVM.md Q2 (Tier 2 = "MIN/MAX with the advisory, inner equi-joins",
Sean's ruling); this campaign is the MIN/MAX half — joins are stage two,
deliberately not here.  Build mechanics unchanged from HANDOFF-IVM.
Before believing any phase status: `git log --oneline` is the record.*

## The shape being built, in four sentences

`MIN(col)` / `MAX(col)` become legal in materialized-view definitions:
inserts maintain by comparison, deletes rescan the ONE touched group
only when the deleted value IS (binary) the stored extremum — a lazy,
CASE-guarded rescan that re-runs the definition filtered to the group,
which WHERE-pushdown turns into an index seek when the base has a
(key…, arg) index.  The R7-style advisory tells the user exactly which
index that is: `PRAGMA view_check` gains `kind='advisory'` rows
carrying the runnable `CREATE INDEX` statement, present while the index
is missing, silent once it exists, never an error.  Deferred views
containing an extremum refresh by touched-group recompute
(delete-and-reinsert from the definition over the log's distinct keys),
because a fold with no inverse has no pure-fold application.  The
equality oracle needs NO changes — recompute-and-diff already judges
extremum columns, which is the point of having built it first.

## Decisions inside the Q2 ruling (recorded 2026-08-12; Sean may veto)

- **Plain-column arguments only** (this stage): the aggregate's
  collation is then the column's, knowable at CREATE, captured into the
  registry, and emitted as explicit `COLLATE` on every generated
  comparison.  `min(<expression>)` refuses by name with the fix.
- **Binary-IS rescan trigger**: rescan iff the deleted value `IS` the
  stored extremum.  Correct under any collation by this argument: in a
  consistent state every group value ≥(collation) the stored extremum;
  if the deleted value differs binary from the stored value, the row
  that produced the stored value still exists, so the extremum stands.
  Ties under exotic collations rescan harmlessly; non-qualifying
  deletes cost zero (the CASE's rescan subquery is never evaluated).
- **No hidden columns for extremums**: the algebra (see the shelf's
  tiers essay) says no bounded state can survive adversarial deletes —
  the base itself is the required state, and the rescan reads it.
- **Deferred + extremum = touched-group recompute** at refresh: DELETE
  the touched groups, re-INSERT them from the definition filtered to
  the log's distinct keys (birth, death, and every aggregate handled
  uniformly), then clear the log.  Views without extremum columns keep
  the pure fold.
- **The advisory surfaces in `view_check`** (the 2am surface, already
  where the user looks), kind='advisory', detail = the runnable
  `CREATE INDEX` text, ordered after 'summary'.  Detection: no base
  index whose leading columns cover the view's key columns.

## Phases (each ends runnable, gates green, DEBUG=3 at least once)

- **Q0 — the spec, red.**  `test/ivm2.test` written before code, run
  once to see the red, committed red.  Sections: 1.x acceptance +
  refusals (expression arg pinned), 2.x eager INSERT/DELETE/UPDATE
  including qualifying-delete rescan, duplicate-extremum ties, NOCASE
  collation leg, all-NULL groups; 3.x the scan-count economics (a
  tally-style pin proving non-qualifying deletes do NOT rescan);
  4.x deferred touched-group recompute; 5.x the advisory (red leg on
  the indexless fixture, silent leg once indexed, never-an-error);
  6.x storm with extremums, view_check empty.
- **P1 — conformance + registry.**  MVIEW_COL_MIN/MAX kinds; plain
  column check; per-column collation capture; CREATE accepts and
  populates (population needs nothing new — the definition computes).
- **P2 — eager maintenance.**  Insert-side comparison combine with
  explicit COLLATE; delete-side CASE-guarded lazy rescan; update =
  both sides; ivm1's Tier-1 behavior untouched.
- **P3 — deferred.**  Touched-group recompute in view_refresh for
  extremum-bearing views.
- **P4 — the advisory.**  Detection walk + view_check rows.  This is
  DOCKET/PLAN-IVM P5's instrument, built now that it has something
  true to say.
- **P5 — docs.**  README-IVM, DESIGN-IVM Tier-2 note, DOCKET pointer,
  wiki.

## Controls

- ivm1 stays 0/24 untouched at every phase (Tier 1 must not move).
- The rescan's laziness is MEASURED (3.x pin), not asserted.
- The advisory has both legs: seen firing (no index) and seen silent
  (index present) — an advisory only ever seen silent is unwatched.
- Full sweep both regimes per phase; test files are inputs to running
  instruments.
- The tiers essay's predictions are the campaign's outside witness:
  where implementation contradicts the algebra, the ESSAY gets the
  correction, kethiv/qere.

## EXECUTED 2026-08-12 (same night as Q0; the spec drained 30 -> 0)

All phases landed in one arc — the campaign's machinery was already
built by IVM P1–P4, and Tier 2 turned out to be new CASES in existing
generators plus one new surface:

- The walk classifies MIN/MAX over plain columns, capturing the
  argument's collation and base-column name into the registry (the
  registry allocation grew two pointer arrays + a string pool).
- Eager: the add side is a comparison CASE with the captured COLLATE
  spelled explicitly; the subtract side is the binary-IS lazy rescan,
  a scalar subquery inside a CASE arm so non-qualifying deletes never
  evaluate it — MEASURED at zero FULLSCAN_STEPs by ivm2 3.1 (and the
  IS key-match seeks the view's own index, also visible in that zero).
  The rescan's definition re-run is UNSHADOWED by construction: the
  CTE that shadows the base lives inside the delta subquery's scope,
  and the SET expressions are outside it.
- Deferred: the fold skips extremum columns; view_refresh gained a
  repair pass — one definition re-run per TOUCHED group, before the
  log truncation because "touched" is read from it.  The delta log's
  extremum columns carry the captured COLLATE so the fold's seeding
  min/max compares as the aggregate does.
- The advisory (PLAN-IVM P5's instrument, now with something true to
  say): compile-time detection in view_check — no base index leading
  with the key columns — emitting kind='advisory' with the runnable
  CREATE INDEX (keys then argument columns).  ivm2 5.2 executes the
  carried statement VERBATIM and watches the advisory go silent; the
  schema cookie expires the compiled check, so silence is immediate.
- ivm1's 1.2 re-pinned at the moved boundary (expression arguments),
  with its history in the comment — the design changed first, which
  is the one sanctioned way a spec expectation moves.
- A reopen section (7.x) was added after first green: synthesis and
  collation capture happen at schema load, and only a fresh
  connection proves they re-derive.

The tiers essay's predictions held without exception: no hidden
columns for extremums (the base is the unbounded state, the rescan
reads it), and the information-theoretic bound showed up as code
shape — the rescan subquery IS the Ω(n) memory, paid only on the
deletes that touch the extremum.

Gates: ivm2 0/38, ivm1 0/24, full sweep + ivm2 in the roster, both
regimes.

## Deliberately NOT in this campaign

Inner equi-joins (Tier 2 stage two); `min`/`max` over expressions;
DISTINCT anything; HAVING; auto index creation (the advisory carries
the statement, the user runs it — the Q1-addendum ruling).
