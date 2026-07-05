# Stored Procedures for SQLite — Design

**Fork baseline:** SQLite 3.53.3 (tag `version-3.53.3`, check-in `d4c0e51e4aeb`)
**Branch:** `stored-procs`
**Status:** design approved by Sean 2026-07-04. Phase 1 (DDL/schema) and phase 2 (CALL
execution, STREAM verified live) implemented; `veryquick` green at phase 1
(0/392,805). Phase 3 (PSM control flow) next.

**Phase 3 (PSM language) implemented 2026-07-04.** Semantics settled during
implementation:

- `SELECT ... INTO` is **first-row-wins** (all-NULL when the query returns no rows) —
  Sean's decision 2026-07-04; MySQL-style multi-row strictness is a phase 4+ option.
  Compiled as a row-value subquery (`sqlite3CodeSubselect`), which supplies the
  LIMIT-capping, NULL-init, and column-count checking for free.
- **`OP_Once` staleness**: uncorrelated subqueries normally compile under `OP_Once`,
  caching their result for a whole frame invocation — wrong inside WHILE/LOOP bodies.
  All PSM-managed expressions, and every DML subquery inside a loop, are marked
  `EP_VarSelect` to force per-iteration evaluation. Known remaining gap: an
  uncorrelated **FROM-clause subquery** inside a loop body still materializes once
  per CALL (its caching is not controlled by an Expr flag); rare pattern, documented
  limitation.
- `RAISE` is a PSM *statement* compiled directly in proc.c; the expression form of
  RAISE inside a SELECT remains trigger-only (upstream check untouched).
- Variables are hoisted to one flat scope per body; declared types are currently
  documentation (values keep dynamic types). Parameters are assignable locals.
- `LOOP` is represented as a WHILE step with a NULL condition.

**Deviations from the original design, discovered during implementation:**
- Body validation (including result-shape checks) happens at first *prepare* of a
  CALL, not at CREATE PROCEDURE time — consistent with SQLite's trigger philosophy
  (bodies may reference tables created later). §5's "enforced at CREATE" is dropped.
- Proc-to-proc CALL landed in phase 2, not phase 3: the `proc_cmd` grammar family
  (= `trigger_cmd` + CALL) exists from the start, and result shapes merge across
  CALL boundaries at compile time.
- The resolver hook (§4) also landed in phase 2 — parameters are variables, and the
  body can't reference them without it.

## 1. Goals and non-goals

**Goals**

- `CREATE PROCEDURE` / `DROP PROCEDURE` as first-class schema objects, persisted in
  `sqlite_schema`, surviving reopen, honoring ATTACH-ed databases like triggers do.
- `CALL name(args...)` as a *row-producing* statement: rows stream to the caller through the
  ordinary `sqlite3_step()` / `sqlite3_column_*()` interface. No new C API required for
  callers.
- A PSM-style procedural body from v1 (Sean's scope decision): `DECLARE`, `SET`,
  `IF/ELSEIF/ELSE`, `WHILE`, `LOOP`/`LEAVE`, `SELECT ... INTO`, `RAISE(...)`, `RETURN`,
  nested `CALL`.
- **Pre-compilation**: the body compiles once to a VDBE `SubProgram` and is reused. A
  prepared `CALL` never re-parses or re-codegens the body across `sqlite3_step`/`reset`
  cycles; a later phase shares the compiled body across statements (§6).

**Non-goals (v1)**

- `OUT`/`INOUT` parameters (phase 5+; result sets cover the common cases).
- Multiple *differently-shaped* result sets per procedure (rejected: incompatible with
  `sqlite3_column_count()` being fixed at prepare time).
- On-disk bytecode. VDBE opcodes are deliberately unstable across SQLite versions; the
  `CREATE PROCEDURE` text in `sqlite_schema` stays canonical, bytecode is always a cache.
- DDL inside procedure bodies (schema-cookie churn inside a cached subprogram invalidates
  the caller mid-flight; revisit later).
- Exception handlers (`DECLARE ... HANDLER`). `RAISE` semantics only for v1.

## 2. Surface syntax

```sql
CREATE [TEMP] PROCEDURE [IF NOT EXISTS] [db.]name ( [pname [type] [, ...]] )
BEGIN
    proc_stmt; [proc_stmt; ...]
END;

DROP PROCEDURE [IF EXISTS] [db.]name;

CALL [db.]name ( [expr, ...] );
```

`proc_stmt` is one of:

| Statement | Notes |
|---|---|
| `DECLARE var [type] [DEFAULT expr]` | affinity from `type` as usual for SQLite; NULL default |
| `SET var = expr` | expr may be a scalar subquery |
| `IF expr THEN ... [ELSEIF expr THEN ...] [ELSE ...] END IF` | |
| `WHILE expr DO ... END WHILE` | |
| `LOOP ... END LOOP` / `LEAVE` | `LEAVE` exits innermost loop; labels deferred |
| `SELECT ... INTO var [, var...] ...` | errors if >1 row (strict, unlike MySQL's silent pick) |
| bare `SELECT ...` | emits rows to the caller (§5) |
| `INSERT` / `UPDATE` / `DELETE` (incl. upsert) | same restrictions as trigger steps re: qualified names in TEMP procs |
| `RAISE(ABORT\|FAIL\|ROLLBACK, msg)` / `RAISE(IGNORE)` | reuses trigger RAISE machinery |
| `RETURN` | halt procedure successfully |
| `CALL other(...)` | depth-limited (§7) |

All new keywords (`PROCEDURE`, `CALL`, `DECLARE`, `WHILE`, `LOOP`, `LEAVE`, `ELSEIF`,
`RETURN`, `THEN` is already a keyword) join the `%fallback ID` list in `parse.y`, so they
remain **non-reserved** — existing databases with columns named `call` keep working.
`mkkeywordhash.c` gets the new entries; the keyword-hash is regenerated at build time.

## 3. Storage and schema plumbing

Mirrors triggers end to end:

- New row in `sqlite_schema`: `type='proc'`, `name=<proc name>`, `tbl_name=''`,
  `rootpage=0`, `sql=<full CREATE PROCEDURE text>` (token-capture from `createkw` through
  final `END`, exactly like `sqlite3FinishTrigger` does with its `pAll` token).
- In-memory: `struct Proc` registered in a new `Schema.procHash` (parallel to `trigHash`).
  Reparsed on schema load by the `sqlite3InitCallback` path; `db->init.busy` distinguishes
  load-from-schema from user DDL, same as triggers.
- `DROP PROCEDURE` deletes the schema row + hash entry, bumps the schema cookie (expiring
  every prepared statement that could reference the proc — this is the correctness backbone
  for all caching).
- TEMP procedures live in the TEMP schema and are visible cross-database per the usual
  TEMP-object rules.

## 4. Body representation and compilation

Two layers, mirroring `Trigger` / `TriggerStep`:

- **Parse product:** `struct Proc` holds a `ProcStep` list. `ProcStep` extends the
  `TriggerStep` idea: `op` ∈ {TK_INSERT, TK_UPDATE, TK_DELETE, TK_SELECT, and new PSM ops
  for DECLARE/SET/IF/WHILE/LOOP/LEAVE/RETURN/CALL/RAISE}. Control-flow steps own child
  `ProcStep` lists (then/else branches, loop bodies) — a tree, not a flat list.
- **Codegen:** `codeProcBody()` (new file `src/proc.c`) builds a `SubProgram` with its own
  `Parse` context, exactly the `codeRowTrigger()` pattern: sub-`Parse`, `sqlite3GetVdbe`,
  walk the step tree, `OP_Halt`, `sqlite3VdbeTakeOpArray`. DML/SELECT steps reuse
  `sqlite3Insert/Update/DeleteFrom/Select` unchanged. PSM steps compile to plain jump
  structures (`sqlite3ExprIfFalse` + labels — the same primitives CASE and trigger WHEN
  already use).

**Variables and parameters.** Procedure-local variables (params + DECLAREd) are
subprogram-local memory cells, allocated from the sub-`Parse`'s `nMem`. Name resolution: a
`ProcVarCtx` (name → cell) hangs off the sub-`Parse`; `resolve.c` gets one additional
lookup step (behind `pParse->pProcVarCtx`, so zero effect on non-proc parses) that resolves
a matching identifier to `TK_REGISTER`. Resolution order: proc variable shadows column?
**No — columns win inside statements with a FROM clause; bare identifiers outside queries
are variables.** (Matches MySQL's practical rule; documented gotcha, revisit if painful.)

Parameter passing: `CALL` evaluates arguments into a contiguous register block in the
*caller's* frame and issues `OP_Program` with P1 = that base register. The body's prologue
copies each parameter into its local cell via `OP_Param` (offset *i*) — the exact mechanism
triggers use for `NEW.x`/`OLD.x`. Offsets are frame-relative constants, so the compiled
body is position-independent and cacheable across call sites.

## 5. CALL as a row-producing statement

Bare `SELECT`s in the body compile with a normal row-output destination — `OP_ResultRow`
from *inside the subprogram frame*. Verified against 3.53.3 source: `OP_ResultRow` stores
`p->pResultRow` and yields `SQLITE_ROW`; `OP_Program` context-switches the whole Vdbe
(frame stack), so resumption is frame-agnostic. Nothing in the engine forbids this; it is
merely unexercised (trigger SELECTs use `SRT_Discard`, RETURNING routes through an
ephemeral table because *row triggers fire mid-row-operation* — a constraint CALL doesn't
have).

- `Vdbe.nResColumn` for a prepared `CALL` is set at prepare time from the compiled body's
  emitting SELECTs. All emitting SELECTs in one procedure must agree on column count —
  enforced at `CREATE PROCEDURE` time (post-`*` expansion) and re-checked at prepare.
  Column names come from the first emitting SELECT.
- A procedure with no bare SELECTs is a command: `CALL` returns `SQLITE_DONE` on first
  step, like an UPDATE.

**Isolation seam (named per global standards):** row emission goes through a small
strategy function `procEmitStrategy()` in `proc.c` choosing between STREAM (OP_ResultRow
in frame — the plan) and SPOOL (RETURNING-style ephemeral table drained by the caller —
the proven-safe fallback). If streaming hits an engine assumption we can't satisfy
(candidates: statement-journal interaction when a body mixes writes *before* emits,
`sqlite3_stmt_busy` edge cases), we flip one function, not the architecture.

## 6. Pre-compilation and caching (the point of the exercise)

Three tiers, landed in this order:

1. **Per-prepared-statement (phase 2, trigger-parity):** the `SubProgram` is compiled once
   at `sqlite3_prepare()` of the `CALL` and owned by that Vdbe (`sqlite3VdbeLinkSubProgram`).
   `step`/`reset`/`step` loops never recompile. This alone equals the efficiency of a
   hand-managed prepared statement, with the logic living in the database.
2. **Per-connection shared cache (phase 5 — implemented, revised):** a
   `ProcCacheEntry` list on the **connection** (`sqlite3.pProcCache`), *not* on the
   `Proc` as originally sketched — schema teardown frees Proc objects through a zeroed
   stand-in db handle (`xdb` pattern), which must never free lookaside memory, and
   compiled bodies are lookaside-eligible. Keeping every cache byte owned by the real
   handle makes all frees safe. `SubProgram` is now refcounted (`nRef`); owners are the
   compiling Vdbe's intrusive list, cache entries, and consuming Vdbes (via a
   non-intrusive `apSharedProg` array — the intrusive list cannot be shared). Hits
   require iDb + Schema pointer identity + schema-cookie match + name; flushes happen
   in `sqlite3ExpirePreparedStatements` (function/collation registration, schema
   resets), on DETACH, and at close. A hit replays the toplevel bookkeeping the
   compile would have done: `nMaxArg`, write-transaction/journal flags, column names.

   **Cacheability is conservative** — only self-contained bodies: not TEMP (multi-db
   references defeat single-cookie guarding), not shared-cache mode, no AUTOINCREMENT
   (autoinc opcodes address toplevel-frame registers by number, which differ across
   statements), and no embedded sub-programs (trigger programs and nested CALLs are
   statement-owned; sharing them needs cycle-aware refcounting — mutually recursive
   cached procs would form uncollectable reference cycles). Excluded bodies silently
   use tier 1.

   Measured (bench-proccache.tcl, 8-statement body, 3000 fresh prepares): cached
   21-32 µs/prepare vs 26-43 µs uncached across runs — the body-compile cost is
   eliminated (the win scales with body size), and a cached CALL prepares faster
   than the equivalent inline SQL, which pays codegen every time.

   **Hard-won invariant:** `sqlite3_db_status()` measures statement memory by
   running the real teardown path with `db->pnBytesFreed` set and all frees
   faked. Teardown must not mutate state that survives the call;
   `sqlite3SubProgramUnref` therefore counts-without-decrementing during
   measurement passes. (Violating this leaked one op array per trigger per
   measured statement — dbstatus/memsubsys2 caught it.)
3. **Cross-connection (explicitly deferred, maybe never):** shared-schema builds only;
   not worth the mutex surface for v1.

## 7. Transactions, errors, recursion

- **Atomicity:** `CALL` is one statement. The whole body runs inside the caller's
  transaction with a statement journal when needed (the OP_Program path already provides
  this for multi-statement trigger bodies — same rules: abort unwinds the statement, not
  the enclosing transaction, unless `RAISE(ROLLBACK,...)`).
- **No COMMIT/ROLLBACK statements inside bodies** (v1): SQLite's autocommit model makes
  in-proc transaction control a semantic minefield (MySQL allows it; PostgreSQL functions
  don't). `RAISE` covers error signaling.
- **Recursion:** nested `CALL` reuses the trigger-depth walk (`pOuterParse` chain) against
  `SQLITE_LIMIT_TRIGGER_DEPTH`; self-recursion compiles (the recursive `OP_Program`
  invocation reuses the in-progress `SubProgram`, as recursive triggers already do).
- **Authorizer:** new `SQLITE_CREATE_PROCEDURE` / `SQLITE_DROP_PROCEDURE` / `SQLITE_CALL`
  action codes; body statements re-authorize under the proc's auth context like trigger
  steps do (`zAuthContext`).

## 8. File/touch-point isolation

New logic concentrates in **`src/proc.c`** (schema object lifecycle, step tree, codegen,
CALL) — the isolation layer. Existing files touched, kept minimal:

| File | Change |
|---|---|
| `src/parse.y` | grammar rules; `%fallback` additions; `proc_stmt` nonterminals |
| `tool/mkkeywordhash.c` | new keywords |
| `src/sqliteInt.h` | `Proc`/`ProcStep`/`ProcVarCtx` decls, `Schema.procHash`, prototypes |
| `src/prepare.c` | accept `type='proc'` rows in `sqlite3InitCallback` |
| `src/build.c` | `sqlite3SchemaClear` frees procHash; DROP plumbing hook |
| `src/resolve.c` | one guarded lookup branch for proc variables |
| `src/vdbe.c` | `OP_DropProc` opcode; possibly relax one assert for STREAM |
| `src/complete.c` | `sqlite3_complete()`: `CREATE PROCEDURE` reuses the trigger ";END;" states (found in phase 1: without this, shells split multi-line bodies at the first interior semicolon) |
| `src/shell.c.in` | `.dump` type whitelist gains `'proc'` (`.schema` needed no change) |
| `Makefile.msc`, `main.mk`, `Makefile.in`, `tool/mksqlite3c.tcl` | add `proc.c` |

`SQLITE_OMIT_PROCEDURE` compile guard from day one, both as upstream etiquette and as a
bisection tool: `-DSQLITE_OMIT_PROCEDURE` must produce a binary that passes the stock test
suite bit-for-bit in behavior.

## 9. Testing

- `test/proc1.test` — DDL lifecycle: create/drop/if-exists/reopen/ATTACH/temp,
  sqlite_schema round-trip, name collisions, `.schema` output.
- `test/proc2.test` — CALL execution: params, result streaming, DONE-only procs, writes,
  atomicity/RAISE, recursion depth, schema-change invalidation.
- `test/psm1.test` — language: DECLARE/SET/IF/WHILE/LOOP/LEAVE/INTO strictness/RETURN,
  shadowing rules, affinity.
- Full `veryquick` suite green with procs compiled in **and** with `SQLITE_OMIT_PROCEDURE`.
- Perf evidence for the caching claim: repeat-CALL microbench vs. equivalent app-side
  prepared statements and vs. a re-prepared script (phase 5 exit criterion).

## 10. Known risks / open questions

1. **STREAM viability** — strongest candidate for a surprise; SPOOL fallback is load-bearing
   insurance (§5). Decide by spike in phase 2, before the grammar work bakes in assumptions.
2. **Lemon conflicts** — `SELECT ... INTO` postfix and `IF ... THEN` inside the statement
   grammar may fight existing rules; mitigation: proc-body statements are their own
   nonterminal family (like `trigger_cmd`), so ambiguity is scoped.
3. **Non-reserved keyword edge cases** — `CALL foo(1)` vs. function-call expression
   statements? SQLite has no expression-statements, so `CALL` at statement start is
   unambiguous.
4. **`type='proc'` in sqlite_schema** — external tools that `SELECT ... WHERE type IN
   ('table','view',...)` will simply not show procs (fine); tools that *error* on unknown
   types will break (their bug, but document it).
5. **Variable/column shadowing rule** (§4) — simple but has a gotcha inside correlated
   subqueries; needs explicit tests.
