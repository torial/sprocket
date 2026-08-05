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

## Typed clients from the schema

Because declared shapes are validated at CREATE time and exposed through
`PRAGMA proc_list` / `PRAGMA proc_info`, a database's whole request/response
contract is machine-readable **without executing anything**:

```
procgen.exe app.db > app_client.h     # tool/procgen.c
```

emits a self-contained header of `static` functions — `_prepare`, a typed
`_bind`, `_step`, per-result-set typed accessors, `_next_resultset`,
`_finalize`. Output is deterministic, so a diff means the contract moved.
`tool/procgen_test.c` compiles and calls the generated header and compares
every result against the same CALL issued by hand.

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
| arity | the k-th row-returning SELECT must have the width of segment k (names need not match: the declaration is the interface). A shape that nests expands to more than one segment -- see *Counting columns when a shape nests* below |
| branches | IF branches must make equal progress; the error names the divergent branch |
| loops | a loop body may not contain a row-returning SELECT (`SELECT ... INTO` is fine) |
| calls | a declared procedure may only CALL a `RETURNS NOTHING` procedure |
| `*` | `SELECT *` is rejected inside a declared procedure -- its arity is not knowable without name resolution |

Introspection (both read the in-memory catalog; no storage change):

```
PRAGMA [schema.]proc_list;          -- name, nparams, nresultsets (segments), declared
PRAGMA [schema.]proc_info(name);    -- resultset_index, position, name, decltype
                                    -- set 0 = parameters, 1..n = declared shapes
```

Multiple sets are delimited explicitly. `sqlite3_step()` reports
`SQLITE_DONE` at the end of each **segment** -- each top-level `SELECT` in the
body ends one -- and keeps reporting it until the client advances. For a
procedure that does not nest, one declared shape is one segment and the two
words are interchangeable:

```c
/* set 1 */
while( sqlite3_step(pStmt)==SQLITE_ROW ){ ... }
while( sqlite3_proc_next_resultset(pStmt)==SQLITE_OK ){
  /* metadata now describes the next set, which may be a different width */
  while( sqlite3_step(pStmt)==SQLITE_ROW ){ ... }
}
```

`sqlite3_reset()` returns the statement to the first set. A CALL of a
procedure with several declared shapes therefore no longer concatenates
them, which is a deliberate behavior change for declared procedures only.

### Counting columns when a shape nests

This is the one place the numbers surprise people, so it is worth stating
before you hit it. Given

```sql
RETURNS TABLE(
  id INTEGER,
  title TEXT,
  comments TABLE(post_id INTEGER, cid INTEGER, body TEXT) KEY(post_id = id)
)
```

the shape has **three columns**, two of which are **value columns** -- columns
that carry a value directly, as opposed to a nested table. Both numbers are
correct at once, and each is visible somewhere:

| Count | Here | Where you see it |
|---|---|---|
| columns | 3 | `PRAGMA proc_info`; `sqlite3_column_count()` |
| **value columns** | **2** | the width the parent `SELECT` must have |
| nested tables | 1 | one extra `SELECT` in the body; one extra segment on the wire |

`columns = value columns + nested tables`, always.

The consequence worth internalising: **the parent `SELECT` is narrower than the
declaration.** Each nested table is streamed by its own `SELECT` that follows,
in declaration order -- so widening the parent to three columns is an error,
not the fix:

```sql
BEGIN
  SELECT id, title FROM posts WHERE id = pid;                  -- 2 value columns
  SELECT post_id, cid, body FROM comments WHERE post_id = pid; -- the nested table
END;
```

Both halves of `KEY(child = parent)` are checked at CREATE: the child name
against the nested table's own columns, the parent name against the value
columns of the shape containing it. The correlation column is part of the
child's declared columns because the child result set genuinely carries it --
the declaration does not hide what the wire carries.

### Segments and declared shapes are different counts

Once a shape nests, the declaration and the wire diverge:

| Count | Here | Where |
|---|---|---|
| declared shapes | 1 | one `RETURNS TABLE` clause |
| **segments** | **2** | `PRAGMA proc_list.nresultsets`; what `sqlite3_proc_next_resultset()` advances through |

`nresultsets` counts **segments** — so it is exactly the number of sets a
client will step through, and it agrees with the API that shares its name.
For every procedure that does not nest, shapes and segments are equal and the
value is unchanged from previous releases.

`PRAGMA proc_info`'s `resultset_index` counts segments too, so all three agree.
A nested table's own columns appear as the segment following its parent, in
declaration order:

```
resultset_index  position  name       decltype
0                0         pid        INTEGER     <- parameters
1                0         id         INTEGER
1                1         title      TEXT
1                2         comments               <- nested: EMPTY decltype
2                0         post_id    INTEGER     <- the nested table's columns
2                1         cid        INTEGER
```

**An empty `decltype` is how a client recognises a nested column**, since
`CREATE` requires a type on every scalar one. Declaration order is what links
segment N back to the column it belongs to — nothing else does.

One property is intentionally **not** enforced:

- **Column types.** Declared types are authoritative for CALL metadata but
  are not checked against the body. In SQLite a declared type expresses
  affinity and intent rather than a constraint; this follows that model.

## What it costs — measured, 2026-08-04

Deterministic VDBE step counts, 200 parents x 5 children, data-local:

| | VDBE steps | vs N+1 |
|---|---|---|
| N+1 (one child query per parent — the naive ORM pattern) | 9,805 | — |
| **nested procedure, segment path** | **6,815** | **30% less** |
| nested procedure, folded JSON column | 17,408 | **1.8× more** |

**The typed path pays. The flat path does not.** The folded JSON column costs
*more* than the N+1 it appears to replace. It exists so an unmodified client
works at all: a **compatibility feature**, not an optimisation, and a client that
can be taught anything should project it away with `RETURNING` and read the
segments.

### The whole fan-out curve, 1000 children held constant

| shape | N+1 | segments | fold |
|---|---|---|---|
| 200 × 5 | 9,805 | **6,815** | 17,408 |
| 100 × 10 | 8,405 | **6,415** | 15,708 |
| 50 × 20 | 7,705 | **6,215** | 14,858 |
| 20 × 50 | 7,285 | **6,095** | 14,348 |
| 10 × 100 | 7,145 | **6,055** | 14,178 |
| 5 × 200 | 7,075 | **6,035** | 14,093 |

**Segments win at every shape** — the gap narrows from 3,000 to 1,040 as fan-out
widens, but never closes. There is no crossover where N+1 becomes the better
choice.

All three curves flatten toward a floor set by the 1000 child rows every
approach must deliver; what separates them is per-parent overhead, roughly 14
steps for N+1, 4 for segments, 17 for the fold.

**And the fold's cost is mostly NOT the correlated subquery.** An earlier note
here said the fold "is an N+1 with JSON on top." The scan says otherwise: at
5 parents there are only five subqueries, and the fold still costs about twice
N+1. Its overhead above the floor stays near 8,000 regardless of parent count,
because **`json_object`/`json_group_array` construction scales with the number of
CHILD rows, not with the number of parents.** Widening the fan-out does not help
it. That is the honest mechanism, and it was measured rather than reasoned.

**What this does not measure: round trips.** Locally, N+1 costs 200 statement
executions in the same process. Across a connection it costs 200 *round trips*
against one, and no VDBE counter can see that. These numbers are the data-local
case — the one the fork was built for — and they understate both procedure
paths everywhere else.

### What happens when the correlation is not indexed

The table above indexed `comments(post_id)`. Removing it — the exact case the
phase 6 advisory warns about — separates the two approaches by a different order
of magnitude:

| shape | index | N+1 | segments |
|---|---|---|---|
| 200 × 5 | yes | 9,805 | 6,815 |
| 200 × 5 | **no** | **606,206** | **12,816** |
| 20 × 50 | yes | 7,285 | 6,095 |
| 20 × 50 | **no** | **64,226** | **12,096** |

**Segments degrade gracefully; N+1 degrades catastrophically.** Without the
index, segments pay a single sort — roughly **2×**, bounded and one-off. N+1
pays a full scan of the child table *per parent row* — **62×** at 200 parents,
and growing with the parent count rather than the data.

That is the real argument for pushing the correlation into the engine. Indexed,
segments win by 1.4×, which is worth having. Unindexed, they win by **47×**,
because the engine's plan is O(n log n) once where the client's is O(parents ×
children).

It also explains why the phase 6 advisory is **advisory and never an error**:
even unindexed, the procedure path is *fine*. A 2× sort is a nuisance worth
mentioning. It is the naive client, not the engine, that falls off a cliff.

## Known limitations (deliberate, v1)

- **A nested table streams as its own segment, not yet as a column** (branch
  `nested-shapes`). `CALL` works: the parent segment reports its value columns
  and each nested table follows as the next segment, ordered by the correlation
  key. What is missing is the flat client's single wide row — so a stock
  `sqlite3` CLI sees the parent columns and stops, rather than seeing the
  nested data as JSON. Segment-aware clients are complete; the invariant is
  not met until that column lands.
- No `OUT`/`INOUT` parameters (result sets cover most cases; planned later).
- Per-column types of declared shapes are advisory (see above).
- `sqlite3_proc_next_resultset()` is declared in `sqlite3.h` but is **not**
  registered in the loadable-extension API table (`sqlite3_api_routines` in
  `sqlite3ext.h` / `sqlite3Apis` in `loadext.c`), so run-time-loaded
  extensions cannot call it. Statically-linked embedders — who are the
  audience for multiple result sets — are unaffected.

  **This is a decision, not an oversight.** Two reasons, recorded so nobody
  "fixes" it by reflex:

  1. *Upstream precedent runs the same way.* Conditionally-compiled API
     families are generally kept out of that table: `sqlite3_preupdate_*`,
     `sqlite3_snapshot_*`, and `sqlite3_stmt_scanstatus*` are all absent.
     `carray_bind` is the lone counter-example, added with an
     `#ifdef`→`0` placeholder to hold its slot.
  2. *Appending to the table is a permanent ABI commitment, and this is a
     fork.* `sqlite3_api_routines` is a flat, order-dependent struct;
     extensions bind by slot offset. Our tail is `/* Version 3.52.0 and
     later */`. If we append a slot, we take the offset upstream will use
     for **its** next API — and then an extension compiled against upstream
     `sqlite3ext.h` that calls that future function, loaded into a library
     built from this fork, jumps into `proc_next_resultset` instead. Wrong
     signature, no diagnostic. That is the one class of divergence a fork
     cannot walk back later.

  If it is ever wanted, the `carray_bind` shape is the correct one
  (`#ifndef SQLITE_OMIT_PROCEDURE` → function, `#else` → `0`, appended at
  the very end and never reordered), and the divergence above should be
  stated loudly at the same time.
- Multi-row `SELECT INTO` takes the first row silently (Sean's decision;
  MySQL-strict mode would be a small phase 4+ addition).
- An uncorrelated **FROM-clause subquery** inside a WHILE/LOOP body
  materializes once per CALL, not per iteration (its caching is not
  expression-controlled). WHERE/SET/PSM-expression subqueries in loops are
  handled correctly (`EP_VarSelect` marking).
- `sqlite3_changes()` after CALL follows trigger-like accounting; exact
  semantics not yet pinned as a contract.
- **Authorization (updated 2026-08-03).** Body statements authorize under the
  procedure's auth context like trigger steps — and that context *is* the
  procedure name in the authorizer's 6th argument, so policy of the form
  "deny reads of `secret` when the context is `leaky`" was always expressible.
  What was missing, and is now added, is **`SQLITE_CALL` (action code 34)**:
  the invocation itself as a distinct authorizable event, with the procedure
  name as arg3 and its database as arg5.

  That is the gate a procedure needs to be a privilege *boundary* rather than
  a macro — an application can refuse `CALL transfer_funds` even when the
  caller may touch every table the body uses. It also gates mutation-only
  procedures, which have no result shape to filter on. Tests: `proc5.test`.

  **Security level (added 2026-08-03).**

  ```sql
  CREATE PROCEDURE p() RETURNS TABLE(x INTEGER) SECURITY DEFINER
  BEGIN SELECT x FROM restricted; END;
  ```

  `SECURITY INVOKER` is the default and is exactly the previous behaviour:
  every body statement is authorized against the caller's policy. `SECURITY
  DEFINER` compiles the body with the authorizer detached, so the procedure may
  touch what its caller may not — which is what makes a procedure a privilege
  *boundary* rather than a macro.

  This is safe only because `SQLITE_CALL` fires *before* the body is compiled
  or fetched from cache, so an unchecked body is reachable exclusively through
  a gate the application opened deliberately. The two features are one design;
  do not adopt either half alone.

  The level is reported by `PRAGMA proc_list` in a `security` column (spelled
  `INVOKER`/`DEFINER`, not a bit — a client generator has to reproduce the
  clause), and it survives schema reload because it lives in the stored CREATE
  text. `SECURITY`, `DEFINER` and `INVOKER` are **non-reserved**, so schemas
  already using them as identifiers keep working.

  Compatibility note: a schema containing a `SECURITY` clause cannot be read by
  a build of this fork that predates the feature — it is a parse error on
  schema load, not corruption. Stock SQLite cannot read `CREATE PROCEDURE` at
  all, so nothing changes there.
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
| `src/vdbe.c` | `OP_DropProc`, `OP_ProcSetEnd` (result-set boundary) |
| `src/vdbeapi.c` | `sqlite3_proc_next_resultset`; boundary handling in `sqlite3Step`/`sqlite3_step` |
| `src/test1.c` | `sqlite3_proc_next_resultset` TCL binding |
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
- `proc4.test` (declared shapes, incl. the result-set boundary API) 47/47
- Full `veryquick`: **0 errors out of 392,956**, no leaks (vanilla baseline:
  0/392,771). `dbstatus` and `memsubsys2` — the memory-measurement suites
  that caught a refcount leak in this fork once before — 26/26 each.
- Re-verified 2026-08-01 after the `SQLITE_API` header fix: **0 errors out of
  392,950**. The total differs by 6 from the run above; `veryquick`'s count is
  not perfectly stable across environments, so treat the *error* count as the
  contract and the total as informational.
- **Upstream 3.53.4 merged 2026-08-02: 0 errors out of 392,985.** Zero
  conflicts. `resolve.c` and `shell.c.in` auto-merged, and `resolve.c` is the
  one that mattered — it carries the procedure parameter-resolution branch and
  the `pNC = pTopNC` fix noted above. Verified present afterwards, not assumed.

### Running the suite on Windows (hard-won)

`testfixture.exe` links Tcl dynamically. If `tcl86t.dll` is not on `PATH` the
binary **exits 0 having printed nothing**, which reads exactly like a clean
run — a green that means the tests never started. Likewise the repository
directory must be on `PATH` (`jimsh0.exe` and `testfixture.exe` are invoked
unqualified and Windows may not search the working directory).

    set PATH=C:\Projects\sqlite;C:\Tcl\bin;%PATH%

**Build BOTH binaries.** The `shellB-*` tests exec the **CLI**, not the
fixture, so `nmake testfixture.exe` alone leaves a stale `sqlite3.exe` behind.
After the 3.53.4 merge that produced 3 failures in `shellB-intck01.sql` which
read exactly like a regression and were nothing of the kind: 3.53.4 ships a
*new* intck test, and the old shell had no `.intck` support for it. One command
settled it — `sqlite3.exe --version` said 3.53.3 while the fixture said 3.53.4.

    nmake /f Makefile.msc sqlite3.exe
    nmake /f Makefile.msc testfixture.exe

Always make both binaries prove what they are before believing a result:

    sqlite3.exe --version           # must match the tree you think you built
    testfixture.exe <script>        # any trivial script that prints
    testfixture.exe test\veryquick.test

A real `veryquick` run emits roughly **400,000 lines** and takes minutes. If
the output is small or the run is fast, it did not happen.

