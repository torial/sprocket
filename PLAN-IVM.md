# PLAN-IVM — incremental view maintenance, the campaign

*Written 2026-08-12, immediately after Sean's rulings (DESIGN-IVM.md,
RULINGS section — read it first; this file turns decisions into phases).
Cold-start brief: assumes no memory. Build mechanics in HANDOFF-3h.md.
Before believing any phase status here, `git log --oneline` and the
suite counts are the record — this plan's own ancestor (PLAN-PROJECTION)
was drafted a week stale, and the lesson is IN that file.*

## The shape being built, in five sentences

`CREATE MATERIALIZED VIEW name [WITH MAINTENANCE EAGER|DEFERRED] AS
SELECT <Tier-1 aggregate query>` creates a REAL TABLE under the view's
own name (schema type `mview`, rootpage and all — the sqlite_sequence
class of object, so ordinary reads, ordinary indexes, WAL crash story
all arrive free), populated at CREATE by running the definition.
Maintenance triggers are SYNTHESIZED IN MEMORY at schema load from the
stored definition — never persisted as schema objects — and either
apply grouped deltas inline (EAGER, the default) or append to a delta
log folded by an explicit `PRAGMA view_refresh` (DEFERRED; staleness
always queryable; never folded on read). `PRAGMA view_check(name)`
recomputes the definition in the same snapshot and diffs, reporting
mismatched ROWS plus a mandatory coverage summary. Everything outside
Tier 1 is refused at CREATE, by name, with the fix.

## Decisions inherited (do not re-litigate; receipts in DESIGN-IVM.md)

Q1 per-view choice, EAGER default, deferred = explicit-refresh-only.
Q2 Tier 1 now (COUNT/SUM/TOTAL/AVG, WHERE/HAVING, deterministic exprs,
single table), Tier 2 anticipated. Q3 trigger capture, ONE capture path
with two application schedules, proc-cache tradeoff documented
prominently. Q4 self-named shadow table. Q5 view_check ships with the
feature. Q6 follows Q3. Index detection ADVISES (naming the exact
CREATE INDEX); creation never automatic.

## Localisation pass (verify before building — names must be confirmed)

- `CREATE` head branching in parse.y (how PROCEDURE joined it) — where
  `MATERIALIZED` attaches without a new keyword; *see lemon say* zero
  conflicts in parse.out, do not trust exit 0.
- The internal-name creation path (`sqlite_sequence` / stat tables):
  how the engine creates `sqlite_`-class objects past the reserved-name
  refusal — the mview's own name is user-chosen, but its schema TYPE
  and write-protection need the same class of allowance.
- Trigger attachment: `pSchema->trigHash` + table linkage — confirm an
  in-memory Trigger never persisted still fires on every DML path and
  still disables the xfer/truncate fast paths (that disabling is a
  CORRECTNESS dependency here).
- Schema-load ordering: the mview's base table must exist when the
  definition re-parses (creation order guarantees it forward; confirm
  for .dump round-trip).
- `.dump` / shell whitelists (the sharp-edges note: types are
  whitelisted) and `sqlite3_complete()` — MV DDL has no `BEGIN...END`,
  expect no change, confirm.

## Phases (each ends runnable, gates green, DEBUG=3 at least once)

- **P0 — the spec, red.** `test/ivm1.test` written before any code, in
  the proc3c tradition: the file going green IS the feature. Committed
  red, and RUN once to see the red.
- **P1 — object without maintenance.** Grammar (zero new keywords —
  `MATERIALIZED`/`MAINTENANCE` as checked identifiers), catalog row,
  Tier-1 conformance with refusals-by-name, table-under-own-name with
  population at CREATE, reads, `CREATE INDEX` on it, user writes
  refused with the reason, DROP, .dump emits DDL (restore repopulates).
  Gate: creation/read/refusal tests green; every OTHER suite untouched;
  and the absence of maintenance is a STATED fact, not a gap — P2
  proves it visibly.
  **EXECUTED 2026-08-12.** ivm1 1.x/2.x/8.x green, 13/24 red = exactly
  the P2–P4 sections; full sweep (proc family + alter/vacuum/view/
  pragma) 0 errors in BOTH regimes, baselines matching HANDOFF-IVM
  (procfault 2734 rel / 3192 dbg); parse.out read, zero conflicts.
  Beyond the checklist, three hazards closed that the plan had not
  named: (1) columns re-derive from the stored SELECT at every schema
  load, so DROP/RENAME/DROP-COLUMN of a base table with a dependent
  mview is REFUSED (registry: `Schema.mviewHash`, procHash lifecycle) —
  a dangling definition would fail the whole file's next open;
  (2) `SELECT *` definitions refused (shape drift between stored rows
  and re-derived columns); (3) VACUUM rebuilds mviews as tables (DDL
  replays before the content copy; rowids preserved; the write refusal
  exempts `DBFLAG_Vacuum`).  Also: `.dump`/`.tables`/`.schema`/
  `PRAGMA table_list` all speak 'mview'.  Verified beyond ivm1 by a
  reload/persistence/VACUUM/dependency-refusal scratch matrix (close,
  reopen, re-derive, alter/drop legs both directions).
- **P2 — the oracle.** `PRAGMA view_check` (rows + coverage summary,
  one snapshot) and `PRAGMA view_list` (name, maintenance, pending,
  stale). Gate: clean immediately after CREATE, and **RED against P1's
  unmaintained writes — the oracle's own seen-red leg exists by
  construction before the feature it checks.**
  **EXECUTED 2026-08-12.** ivm1 10/24: all of 3.x green; the remaining
  red is exactly 5.x/6.x/7.x.  The seen-red leg was run by hand and
  watched: after unmaintained base writes the oracle reported three
  diff rows naming group and side, summary '3 groups compared, 2
  differ' — the arithmetic itself checked (alice both-sides, carol
  missing, bob matching).  An EMPTY view reports '0 groups compared'
  — coverage stated, never a clean bill.  Implementation: one
  generated WITH fresh/stored/gone/extra compound compiled via nested
  parse into the pragma's own program, so both sides share the
  statement's snapshot; EXCEPT gives NULL-safe aggregate comparison.
  view_check works no-arg (checks everything, proc_check's lesson);
  unknown names are refused by name.  view_list spells unmeasured
  pending/stale as NULL — a zero there would be a fabrication until
  capture exists (P3/P4 flip them to measured values).  MViewInfo now
  carries the definition text (captured at the AS token) and
  per-column key flags, both written by the single init-path
  registration.  Suites: full sweep + pragma family, 0 errors, both
  regimes.
- **P3 — eager maintenance.** Synthesized triggers; per-statement
  grouped application; group liveness via the mandatory COUNT; UPDATE
  as two-sided delta; WHERE filtering both sides; AVG as SUM+COUNT.
  Gate: view_check EMPTY under a seeded randomized write storm (the
  acceptance test); proc interplay pinned — a body write maintains the
  view, the cache exclusion appears in `proc_check` (the documented
  tradeoff, tested not just written), own-transaction reads exact.

  **P3 EXECUTION NOTES (design worked 2026-08-12; Sean ruled hidden
  columns — "akin to our materialized views being a real table").**
  - *Storage:* visible result columns + hidden (`COLFLAG_HIDDEN` +
    `TF_HasHidden`; `IsHiddenColumn` already honors the flag in this
    build — star expansion, NATURAL/USING, table_info all skip, and
    ONLY mview creation ever sets it on an ordinary table; the
    `__hidden__` name magic stays off).  Layout, in order after the
    visible columns: `ivm$count` (COUNT(*) liveness, always); per SUM
    column i an `ivm$n$i` (count of non-null arg, to restore NULL);
    per AVG column j an `ivm$s$j` + `ivm$n$j` (visible avg = s/n,
    NULL at n=0, CAST REAL division).  COUNT/TOTAL need nothing.
  - *Key index:* auto UNIQUE index on the visible key columns, named
    `sqlite_autoindex_<view>_N` via `sqlite3CreateIndex(pTblName=0)`
    — the table-constraint precedent, so NULL-sql schema row, orphan
    reload, .dump and VACUUM exclusion all arrive free.  Population
    precedes it in the CREATE program, so `sqlite3RefillIndex` is
    emitted after population.  Key-less views: no index; their single
    group row exists from population (aggregate over empty = one row)
    and never dies.
  - *Delta application is per-row and that is a FEATURE:* for a
    one-row delta the definition's own outputs ARE the hidden deltas
    (delta avg over one row = the value; n-delta = value IS NOT NULL;
    ivm$count-delta = 1), so trigger bodies synthesize from the
    STORED TEXT with zero expression surgery.
  - *Trigger shape (per mview, on the base):* AFTER INSERT / DELETE /
    UPDATE.  The delta select is the definition text UNCHANGED,
    prefixed `WITH "<base>"(<cols>) AS (VALUES(NEW…))` — the CTE
    SHADOWS the base table (verified empirically, with UPDATE-FROM
    and IS-matching, in newcte.tcl 2026-08-12).  Application is
    UPDATE-combine-FROM-delta with `IS` key matching, THEN
    INSERT-if-absent (NOT EXISTS) — never ON CONFLICT, because UNIQUE
    treats NULL keys as distinct while GROUP BY treats them as one
    group.  DELETE side subtracts, then a keyed
    `DELETE … WHERE ivm$count<=0` (skipped for key-less views).
    UPDATE = OLD-side then NEW-side.
  - *Synthesis mechanics:* full CREATE TRIGGER text, sub-parsed
    (declare_vtab-style Parse) under a fork flag that makes
    FinishTrigger build-but-not-persist (no trigHash, no schema row,
    no codegen); attached to the base's pTrigger list ONCE, LAZILY,
    from sqlite3TriggerList (schema mutex held there), keyed on a
    TF-flag set at registration; owned by MViewInfo, freed by the
    registry teardown (which must also unlink from the base's list).
    Lazy synthesis means xfer/truncate self-disable automatically and
    the proc body-cache exclusion + proc_check naming arrive with no
    new code (ivm1 7.1).
  - *VACUUM cannot double-maintain:* registration happens only on the
    schema-RELOAD path; vacuum executes DDL directly, so the vacuum
    db has an empty registry, no flags, no triggers — the content
    copy is clean.  Restore-from-dump maintains correctly by
    construction regardless of statement order.
  - *HAVING moves to Tier 2* (DESIGN-IVM amended, dated): a group
    failing HAVING must be absent from the table yet keep
    accumulating, and with row-as-storage there is nowhere to
    accumulate.  Refused at CREATE with the reason; the Tier-2 path
    is a per-group base rescan on absent-group deltas (the MIN/MAX
    cost class, index advisory and all).
  - *view_list:* eager views flip pending/stale from NULL to 0/0 —
    measured-by-construction once triggers exist.  Deferred views
    stay NULL until P4.
  - *Order of work:* registration/walk (kinds, TF flag, HAVING) →
    CREATE (hidden cols, augmented population from a PRE-RESOLUTION
    dup — the resolved tree cannot be re-prepped — auto index +
    refill) → synthesis → view_list → gates.

  **EXECUTED 2026-08-12.** ivm1 3/24: 5.x (including the acceptance
  storm) and 7.x green; the remaining red is exactly 6.x (P4).  The
  key index rides an engine-emitted `OP_SqlExec CREATE UNIQUE INDEX`
  placed after EndTable's schema reload, so an ordinary CREATE INDEX
  builds and fills it — no register plumbing.  Three bugs found and
  fixed on the way, each caught by an instrument: (1)
  `sqlite3TriggersExist` short-circuits on `pTrigger==0` before ever
  reaching the lazy-synthesis hook — the hook moved above the
  short-circuit; (2) `sqlite3RunParser`'s own tail FREES
  `pParse->pNewTrigger`, so the sub-parse hand-off returned freed
  memory that read as garbage trigger-ops (found when printing the
  fields segfaulted; the earlier symptom, mask=0, was that garbage
  being compared); (3) every sub-parse leaves an EMPTY but allocated
  Vdbe from statement-completion codegen — unfinalized it pinned the
  whole connection as a zombie at close, which is why BOTH leak
  components (77KB connection + 33KB triggers) traced to one line;
  `sqlite3_next_stmt` showing three empty-SQL statements was the
  tell.  The maintenance-write exemption is a flag carried by the
  synthesized Trigger into its body's sub-parse
  (`Trigger.bMViewMaint` → `Parse.bMViewMaintProg`) so USER triggers
  writing a view still refuse.  VACUUM: synthesis is suppressed under
  DBFLAG_Vacuum and the view content is copied with an EXPLICIT
  column list built from table_xinfo, because `SELECT *` skips hidden
  columns — the generic copy would have silently dropped the
  bookkeeping.  Verified by a 11-leg matrix (SUM-to-zero vs all-NULL
  distinguished; AVG NULL semantics; group-key moves; whole-group
  death; reopen re-synthesis; VACUUM + post-vacuum maintenance; proc
  interplay with the named cache exclusion; view_list eager 0/0
  measured; transactional rollback), the oracle judging every leg at
  0 differ, and a MEMDEBUG build reporting all allocations freed.
- **P4 — deferred.** Same triggers, log application
  (`sqlite_ivm_<name>_log` weighted rows), `PRAGMA view_refresh`,
  pending/stale surfaces; view_check on a stale view says STALE before
  anything else. Gate: storm + refresh converges to empty view_check;
  reads never block on folding (proven, not asserted).
- **P5 — the index advisory.** Creation-time detection via the R7
  instrument; the advisory carries the runnable `CREATE INDEX ...`
  statement. Gate: fires on the indexless fixture, silent once the
  index exists, never an error.
- **P6 — docs.** README-IVM.md with the Q3 proc-cache tradeoff in its
  own loud section; DOCKET #4 closed with pointers; wiki.

## Controls (stated before running, per the house instrument discipline)

- The storm states its expected magnitudes first (N ops → deterministic
  final recompute) and the oracle's summary row proves coverage — an
  empty diff over zero compared groups is a FINDING, not a pass.
- The oracle is seen red (P2-vs-P1) before it is ever trusted green.
- Refusals are tested for the NAMED construct, both legs (refused thing
  refused, allowed thing allowed).
- veryquick full run at campaign end; suite-family sweep both regimes
  per phase.
- Test files are inputs to running instruments: no edits to them while
  a suite runs (priced twice on 2026-08-12; see continuity).

## Deliberately NOT in this campaign

MIN/MAX (Tier 2, with the advisory), joins, DISTINCT, outer anything,
TEMP-schema mviews (refused v1 with the reason), auto index creation,
B′/preupdate capture (waits for replication), any read-side folding.
