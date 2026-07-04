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
- **Atomicity**: a failure anywhere in the body undoes the whole CALL
  (statement-journal semantics), inside or outside explicit transactions.
- **Pre-compilation**: the body compiles once per prepared CALL into a VDBE
  SubProgram (the machinery triggers use); `step`/`reset` cycles never
  re-parse. A per-connection cache shared across statements is the planned
  phase 5.

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

## Known limitations (deliberate, v1)

- No `OUT`/`INOUT` parameters (result sets cover most cases; planned later).
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
| `src/parse.y`, `tool/mkkeywordhash.c` | grammar (`proc_cmd` = `trigger_cmd` + CALL + PSM), fallback keywords |
| `src/sqliteInt.h` | `Proc`/`ProcParamList`/`ProcPrg`; `Schema.procHash`; `TriggerStep.zVar/pThen/pElse`; `Parse` fields |
| `src/callback.c`, `src/prepare.c` | schema hash lifecycle; `ProcPrg` cleanup |
| `src/resolve.c` | variable resolution after column lookup fails (must set `pNC=pTopNC` — see git history) |
| `src/trigger.c`, `src/attach.c` | step destructor/fixer recurse into PSM child lists |
| `src/vdbe.c` | `OP_DropProc` |
| `src/vdbeaux.c` | `sqlite3VdbeTransferColumnNames` (CALL column names) |
| `src/complete.c` | `CREATE PROCEDURE` uses the trigger ";END;" states |
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

- `proc1.test` 33/33, `proc2.test` 31/31, `psm1.test` 35/35, no memory leaks
- Full `veryquick`: **0 errors out of 392,870** (vanilla baseline: 0/392,771)
