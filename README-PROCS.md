# Stored Procedures for SQLite (fork)

This fork adds stored procedures to SQLite: schema-resident, pre-compiled,
multi-statement routines with a procedural language, invoked with `CALL`.
Upstream SQLite has declined this feature for two decades; this fork exists for
the case their advice ("put the logic in the application") does not cover —
multiple programs, in different languages, sharing one database file and
needing the logic to live with the data.

Forked from SQLite **3.53.3** (`version-3.53.3`, check-in `d4c0e51e4aeb`).
Design rationale and internals: [`DESIGN-PROCS.md`](DESIGN-PROCS.md).
Architecture decisions were made by Sean McKay; design and implementation by
Claude (Fable 5), 2026-07-04.

## What you get

```sql
CREATE PROCEDURE pay_down(acct INTEGER, amt REAL)
BEGIN
  DECLARE bal REAL;
  SELECT balance FROM accounts WHERE id = acct INTO bal;
  IF bal IS NULL THEN
    RAISE(ABORT, 'no such account');
  ELSEIF bal < amt THEN
    RAISE(ABORT, 'insufficient funds');
  END IF;
  UPDATE accounts SET balance = bal - amt WHERE id = acct;
  SELECT id, balance FROM accounts WHERE id = acct;  -- rows stream to caller
END;

CALL pay_down(42, 19.99);
```

- **DDL**: `CREATE [TEMP] PROCEDURE [IF NOT EXISTS] name(param [type], ...)`,
  `DROP PROCEDURE [IF EXISTS]`. Procedures persist as `type='proc'` rows in
  `sqlite_schema`, reload on open, work in ATTACH-ed databases, appear in
  `.schema` and `.dump`.
- **CALL** is a row-producing statement: rows from bare `SELECT`s in the body
  stream through ordinary `sqlite3_step()`/`sqlite3_column_*()`. No new C API.
  A procedure with no bare SELECTs behaves like an UPDATE (one step, `DONE`).
- **Language**: `DECLARE var [type] [DEFAULT expr]`, `SET var = expr`,
  `IF/ELSEIF/ELSE`, `WHILE ... DO ... END WHILE`, `LOOP ... END LOOP`/`LEAVE`,
  `SELECT ... INTO vars` (first row wins; all-NULL when empty), `RETURN`,
  `RAISE(ABORT|FAIL|ROLLBACK, msg)` / `RAISE(IGNORE)`, plus any
  INSERT/UPDATE/DELETE/SELECT, and `CALL` of other procedures (recursion
  allowed, bounded by `SQLITE_LIMIT_TRIGGER_DEPTH` at runtime).
- **Declared result shapes** (optional): a procedure may state what a CALL
  returns, which makes the result statically knowable to tooling.

  ```sql
  CREATE PROCEDURE post_with_comments(pid INTEGER)
    RETURNS TABLE(id INTEGER, title TEXT, created_at TEXT)
    RETURNS TABLE(cid INTEGER, post_id INTEGER, body TEXT)
  BEGIN
    SELECT id, title, created_at FROM posts WHERE id = pid;
    SELECT id, post_id, body FROM comments WHERE post_id = pid;
  END;
  ```

  `RETURNS NOTHING` declares a mutation-only procedure. The body is checked
  against the declaration **at CREATE time**, so a nonconforming procedure
  cannot be installed, and `prepare("CALL ...")` reports the first declared
  shape through `sqlite3_column_count/_name/_decltype` -- a CALL looks like
  a SELECT to existing tooling. Declared names win over the body's spelling.
  Procedures without a RETURNS clause behave exactly as before.
- **Atomicity**: a failure anywhere in the body undoes the whole CALL
  (statement-journal semantics), inside or outside explicit transactions.
- **Pre-compilation**: the body compiles once per prepared CALL into a VDBE
  SubProgram (the machinery triggers use); `step`/`reset` cycles never
  re-parse. On top of that, a per-connection cache shares the compiled body
  across statements: re-preparing a CALL skips body compilation entirely
  (measured ~1.35x faster prepares on an 8-statement body; the win scales
  with body size). Cached bodies must be self-contained -- non-TEMP, no
  AUTOINCREMENT tables, no triggers fired by body DML, no nested CALL;
  anything else transparently uses the per-statement tier. The cache
  invalidates on schema change (cookie), function/collation registration,
  DETACH, and close.

## Semantics worth knowing

- **Name resolution**: inside body statements, columns shadow
  parameters/variables. A bare identifier is a column wherever a column by
  that name is in scope, otherwise a variable. (Same practical rule as MySQL;
  name your parameters distinctly from columns.)
- Keywords added (`PROCEDURE CALL DECLARE ELSEIF LEAVE LOOP RETURN WHILE`) are
  **non-reserved** — existing schemas using them as identifiers keep working.
- Declared parameter/variable types are currently documentation; values keep
  dynamic types. Parameters are assignable locals.
- All row-producing SELECTs in one procedure (including those in procedures it
  CALLs) must produce the same column count — checked at prepare time.
- Variables are one flat scope per body (DECLAREs are hoisted; no block
  scoping).
- DDL inside bodies is not supported. `BEGIN/COMMIT/ROLLBACK` statements
  inside bodies are not supported (use `RAISE(ROLLBACK, ...)`).

## Declared shapes: rules and introspection

Conformance is checked once, at CREATE PROCEDURE time, on the body's
control-flow graph -- there is **no cost at CALL time**:

| Rule | What is enforced |
|---|---|
| count | every complete path streams exactly the declared sequence; `RAISE` may abort mid-sequence |
| arity | the k-th row-returning SELECT must have the width of shape k (names need not match: the declaration is the interface) |
| branches | IF branches must make equal progress; the error names the divergent branch |
| loops | a loop body may not contain a row-returning SELECT (`SELECT ... INTO` is fine) |
| calls | a declared procedure may only CALL a `RETURNS NOTHING` procedure |
| `*` | `SELECT *` is rejected inside a declared procedure -- its arity is not knowable without name resolution |

Introspection (both read the in-memory catalog; no storage change):

```
PRAGMA [schema.]proc_list;          -- name, nparams, nresultsets, declared
PRAGMA [schema.]proc_info(name);    -- resultset_index, position, name, decltype
                                    -- set 0 = parameters, 1..n = declared shapes
```

Two properties are intentionally **not** enforced:

- **Column types.** Declared types are authoritative for CALL metadata but
  are not checked against the body. In SQLite a declared type expresses
  affinity and intent rather than a constraint; this follows that model.
- **Differing set widths.** Every streamed set is delivered through one
  prepared statement, which has a single column count, so all declared
  shapes must currently agree in arity. This is reported at CREATE time
  with an explanation. Lifting it requires an explicit result-set boundary
  API (`sqlite3_proc_next_resultset`), which is designed but not built.

## Known limitations (deliberate, v1)

- No `OUT`/`INOUT` parameters (result sets cover most cases; planned later).
- Declared shapes must all have the same number of columns, and per-column
  types are advisory (see the section above).
- Multi-row `SELECT INTO` takes the first row silently (Sean's decision;
  MySQL-strict mode would be a small phase 4+ addition).
- An uncorrelated **FROM-clause subquery** inside a WHILE/LOOP body
  materializes once per CALL, not per iteration (its caching is not
  expression-controlled). WHERE/SET/PSM-expression subqueries in loops are
  handled correctly (`EP_VarSelect` marking).
- `sqlite3_changes()` after CALL follows trigger-like accounting; exact
  semantics not yet pinned as a contract.
- No authorizer action codes for procedures yet; body statements authorize
  under the procedure's auth context like trigger steps.
- The expression form of RAISE inside a SELECT remains trigger-only; the
  statement form covers procedures.

## Implementation shape (for maintainers)

All procedure logic lives in **`src/proc.c`** — the isolation layer. Touches
elsewhere are small and greppable by `SQLITE_OMIT_PROCEDURE`:

| Where | What |
|---|---|
| `src/parse.y`, `tool/mkkeywordhash.c` | grammar (`proc_cmd` = `trigger_cmd` + CALL + PSM, `procreturns` clauses), fallback keywords incl. `RETURNS` |
| `src/pragma.c`, `tool/mkpragmatab.tcl` | `proc_list` / `proc_info` |
| `src/sqliteInt.h` | `Proc`/`ProcParamList`/`ProcShape`/`ProcPrg`; `Schema.procHash`; `TriggerStep.zVar/pThen/pElse`; `Parse` fields |
| `src/callback.c`, `src/prepare.c` | schema hash lifecycle; `ProcPrg` cleanup |
| `src/resolve.c` | variable resolution after column lookup fails (must set `pNC=pTopNC` — see git history) |
| `src/trigger.c`, `src/attach.c` | step destructor/fixer recurse into PSM child lists |
| `src/vdbe.c` | `OP_DropProc` |
| `src/vdbeaux.c` | `sqlite3VdbeTransferColumnNames` (CALL column names) |
| `src/complete.c` | `CREATE PROCEDURE` uses the trigger ";END;" states |
| `src/vdbe.h`, `src/vdbeInt.h` | `SubProgram.nRef`; `Vdbe.apSharedProg` (non-intrusive refs to cached bodies) |
| `src/main.c`, `src/attach.c` | body-cache flush at close and on DETACH |
| `src/shell.c.in` | `.dump` includes `'proc'` |

Bodies are `TriggerStep` trees, so the trigger DbFixer and codegen
infrastructure is reused wholesale; PSM control flow compiles to plain jump
structures via `sqlite3ExprIfFalse` + labels. `SQLITE_OMIT_PROCEDURE` (implied
by `SQLITE_OMIT_TRIGGER`) removes the feature; pass it to both the C compiler
and lemon.

**Subtle invariants** (violate these and you get the bugs we already fixed):

1. Any resolver extension that "finds" a name must set `pNC` before reaching
   `lookupname_end`, or the nRef bookkeeping walks off the NameContext list.
2. Expressions evaluated repeatedly within one frame invocation (loops!) must
   defeat `OP_Once` subquery caching — mark subqueries `EP_VarSelect`.
3. Frame memory cells start `MEM_Undefined`, not NULL — anything read before
   written needs explicit prologue initialization.
4. Proc bodies have no `pTriggerTab`, so statement-journal protection must be
   requested explicitly (`sqlite3MultiWrite`/`sqlite3MayAbort`) for write steps.
5. `SrcList` names inside stored bodies arrive with `fg.fixedSchema` set by the
   DbFixer; read the schema from `u4.pSchema`, not `u4.zDatabase`.
6. Every byte of the connection-level body cache is allocated and freed
   against the real `sqlite3*`. Schema teardown uses a zeroed stand-in handle
   whose frees bypass lookaside -- caching anything on schema-owned objects
   (e.g. on the Proc itself) would corrupt the heap.
7. A cache hit must replay the toplevel bookkeeping its compile would have
   done: `nMaxArg`, write-transaction and statement-journal flags, result
   column names. Missing the write flags means body writes run without a
   write transaction.
8. The declared-shape metadata override in `sqlite3CallProc` must run
   *after* all three body-acquisition paths (in-statement reuse, cache
   hit, fresh compile). Each of those sets result-column names its own
   way; the override exists to supersede all of them.
9. Statement teardown (`sqlite3VdbeClearObject` and everything it calls)
   doubles as the `sqlite3_db_status()` **memory-measurement pass**: with
   `db->pnBytesFreed` set, all "frees" merely count bytes and nothing is
   released. Teardown code must therefore never mutate state that outlives
   the call — *reference counts included*. `sqlite3SubProgramUnref`
   counts-without-decrementing in that mode; violating this leaked one op
   array per trigger per measured statement (found via dbstatus/memsubsys2,
   cost several hours — see git history).

## Build (Windows/MSVC)

```
vcvars64 && nmake /f Makefile.msc sqlite3.exe TCLSH_CMD=<a tclsh>
nmake /f Makefile.msc testfixture.exe TCLDIR=<tcl install with headers/libs>
testfixture test/proc1.test ; test/proc2.test ; test/psm1.test
testfixture test/veryquick.test    # full regression, ~393k assertions
```

If builds fail with "'foo.exe' is not recognized" on hardened Windows
(`NoDefaultCurrentDirectoryInExe`), put the build directory on `PATH` first;
the makefiles invoke freshly built tools by bare name.

## Test status at last commit

- `proc1.test` 33/33, `proc2.test` 31/31, `proc3.test` (cache) 29/29,
  `psm1.test` 35/35, no memory leaks
- `proc4.test` (declared shapes) — **written but not yet executed**: it was
  authored on a machine without Tcl headers, so `testfixture.exe` could not
  be built there. Its cases were all verified by hand against `sqlite3.exe`
  first; run it (and `veryquick`) before trusting the shape feature.
- Full `veryquick`: **0 errors out of 392,870** (vanilla baseline: 0/392,771)
