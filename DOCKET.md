# Docket — where this fork could go

*Candidate directions, ranked, with the reason each earns its place and what
"done" would mean. Written 2026-08-01. Committed so it survives context loss.*

Active work is `PLAN-TRANSPORT.md`. This file is what comes after, or instead.

**Companion roadmap.** Zebra's ORM ("Tack") is designed on top of this fork —
`ZEBRA_ORM_ARCHITECTURE.md` in the language project. Its §14 works each item
below from the ORM's side, so the two roadmaps are planned together. Three
couplings worth knowing before picking anything up here:

- **Authorization (#1) gates the ORM too.** Tack makes procedures a
  network-facing application surface, so the proc boundary becomes its security
  boundary. It is currently unguarded on both sides.
- **Typed client generation (#2) is the same mechanism as Tack's build-time
  pre-pass**, pointed at different languages. `PRAGMA proc_info` is the shared
  interface; do not build the introspection twice.
- **The shape-cache handshake has a stronger form for a generated client.**
  Tack knows every result shape at *build* time, so it never needs a describe
  at all — and a cookie mismatch means the binary was compiled against a
  different schema than the server runs, which is a deployment error to fail on
  rather than a cache miss to recover from.

---

## 1. Authorization for procedures — *we created this hole*

**This entry was substantially wrong and is corrected 2026-08-03.** It claimed
the procedure boundary was "unguarded." It was not. Measured before building
anything:

- **Body statements are already authorized.** A changed authorizer is honoured:
  `sqlite3_set_authorizer()` expires prepared statements, they re-prepare, and
  the CALL is denied with the same message an inline statement would give — on
  a cached connection and on a fresh one. (A suspected hole in the
  compiled-body cache was tested for specifically and does not exist.)
- **The authorizer already knew which procedure a statement came from**, via
  `zAuthContext` — the 6th callback argument. Policy like "deny reads of
  `secret` when the context is `leaky`" was expressible all along.
- **Table-level denial reaches through a body**, verified: `leaky()` denied,
  `p()` allowed, same connection.

So enforcement existed; only *granularity* was missing.

### ✅ Done 2026-08-03 — `SQLITE_CALL`, action code 34

The invocation as a distinct authorizable event: arg3 = procedure name,
arg5 = the database the procedure lives in (resolved from its schema, so
`CALL p()` and `CALL main.p()` present identically to a policy). Checked after
resolution and arity but **before** the body is compiled or fetched from cache
— deliberately, so a future trusted-body mode still passes this gate.

Now expressible and previously not: refuse `CALL transfer_funds` even when the
caller may touch every table the body uses; gate mutation-only procedures,
which have no result shape to filter on; and block laundering — denying an
inner procedure blocks an outer one that wraps it. `proc5.test`, 15 tests.

### ✅ Done 2026-08-03 — the security level

`SECURITY INVOKER` (default, unchanged behaviour) / `SECURITY DEFINER` (body
compiled with the authorizer detached). Reported in `PRAGMA proc_list`'s new
`security` column; persists in the stored CREATE text; the three new keywords
are non-reserved. `proc5.test` covers it, including the control that a plain
`SELECT` on the same table *is* denied — without which the DEFINER result would
prove nothing — and that `SQLITE_CALL` still gates a DEFINER procedure, which
it must, since the body is unchecked and that becomes the only gate.

Design notes for whoever revisits this:

- Suppression is `db->xAuth` saved, cleared, restored around the body compile.
  Narrow by construction: the outer CALL is already authorized by then.
- `SECURITY DEFINER` was chosen over inventing a word because it is instantly
  recognisable from Postgres/MySQL, even though SQLite has no user model to
  hang a "definer" identity on — the semantics people expect (body runs with
  authority the caller lacks) are exactly what it does.
- `SQL SECURITY DEFINER`, the full standard spelling, was rejected: `sql` is a
  column name in `sqlite_schema`, and making it a keyword invites collisions.

### Original framing — the security level (Sean's ruling, 2026-08-03)

A per-procedure level, a *"guest"/"everyone"* mode, under which a procedure may
run its body **without** authorizing each statement — SQL's `SECURITY DEFINER`
in spirit, though SQLite has no user model to hang a "definer" identity on, so
the framing is a trust level rather than an identity.

**Explicitly not the default.** Created procedures keep today's behaviour:
bodies are always authorized. Elevation is opt-in at CREATE time.

`SQLITE_CALL` was the prerequisite — if a trusted body goes unchecked, the call
gate is the *only* gate, so it had to exist first. Open design questions:
the keyword and syntax; whether the level is visible in `PRAGMA proc_info`
(it should be — a client generator needs to know); whether a trusted procedure
may CALL an untrusted one, or the reverse; and how it interacts with the
existing conformance rule that a shape-declaring procedure may only CALL a
`RETURNS NOTHING` one.

**Priority — corrected.** An earlier draft of this entry said the security
level was "the remaining piece before anything listens on a socket." That
overstates it. `SQLITE_CALL` supplies the *restrictive* direction, which is the
safety-critical one; the security level supplies *elevation*, which is a
capability rather than a hole. A socket can listen safely without it. It is
wanted, not blocking.

## 2. Typed client generation — *cheapest thing with the largest multiplier*

Declared shapes are static, validated at `CREATE` time, and already
introspectable via `PRAGMA proc_info`. So the full request/response contract of
a database is machine-readable **without executing anything**. Emit client stubs
— C, TypeScript, Python, and Zebra — straight from the schema.

This is what turns "SQLite has stored procedures" into "SQLite has a typed RPC
layer," and it compounds with every phase of the transport plan.

**Done means:** `sqlite3 db.sqlite ".procgen ts"` emits typed callables; a
round-trip test that generates, compiles, and calls; regeneration is
deterministic (byte-identical for an unchanged schema).

### ✅ C generator done 2026-08-03 — `tool/procgen.c`

`procgen.exe app.db > app_client.h` emits a self-contained header of `static`
functions: `_prepare` / typed `_bind` / `_step` / per-set typed accessors
(`greet_rs1_msg`, `two_rs2_b`) / `_next_resultset` / `_reset` / `_finalize`.

Design notes worth keeping:

- **Accessors, not materialised structs.** Keeps SQLite's zero-copy semantics
  and raises no question about who owns a string.
- **Two type maps, not one.** `sqlite3_column_text()` returns
  `const unsigned char *` but a caller binding a parameter holds a
  `const char *`. Emitting the accessor type on the bind side made every call
  site cast — exactly the friction a generated client exists to remove. Caught
  by reading the emitted signature, not by a test.
- **Deterministic by construction** — procedures ordered by name (`proc_list`'s
  natural order is hash order and would churn), columns by
  `(resultset_index, position)`.
- Identifiers sanitised, so a column named `order by` cannot emit
  uncompilable source.

**Verified end to end** (`tool/procgen_test.c`, 17 checks): the generated
header is *compiled* and *called*, and every result is compared against the
same CALL issued through plain `sqlite3_prepare()` on the same database. That
comparison is the positive control — the generated client cannot pass by
agreeing with itself. Regeneration is byte-identical.

**Still open:** TypeScript, Python and Zebra emitters (the introspection and
ordering logic is shared; only the emit differs), and wiring it into the shell
as `.procgen` rather than a standalone tool.

**Effort: small. Differentiation: high.**

## 3. Nested result shapes — *MetaKit's idea, and an open design question*

**Status: not decided. This entry exists to be prototyped, not implemented.**

### The problem it solves

`Tack`'s stitch layer (`ZEBRA_ORM_ARCHITECTURE.md` §6) carries **four separate
strategies** — join-flatten, ordered merge-join, JSON fold, multi-set proc —
and every one of them exists for a single reason: **SQL cannot return a tree.**
They are all machinery for flattening parent-with-children into rectangles and
reassembling it on the client.

MetaKit (Jean-Claude Wippler, Equi4, c. 1996–2005) simply returned the tree. A
cell could contain an entire subtable, declared in a notation of admirable
succinctness:

```
posts[id:I,title:S,comments[cid:I,body:S]]
```

Columnar storage was its famous idea and DuckDB has long since done that
better. **Nested views are the idea nobody picked up**, and they are the one
that would pay here.

### The invariant — non-negotiable

> **A `CALL` must continue to look exactly like a `SELECT` to any client that
> has not opted in.** `sqlite3_column_count/_name/_decltype` keep working;
> `sqlite3_step()` keeps yielding flat rows. Nothing that compiles today may
> behave differently tomorrow.

This property is why declared shapes were worth building at all — it is what
makes every existing driver, GUI, and language binding work against procedures
for free. **No design that trades it away should be adopted, however elegant.**

### Candidate designs, to be prototyped rather than argued about

**A. Nesting as declared metadata over flat result sets — *current favourite*.**
`RETURNS TABLE(id, title, comments TABLE(cid, body))` compiles to exactly the
multi-set form we already have: set 1 parents, set 2 children carrying the
parent key. The rows on the wire and through the C API are **unchanged**. What
is new is only that `PRAGMA proc_info` records the parent/child relationship
and the correlation key.

Why this looks strongest: it adds nothing to the data model, so the invariant
is preserved *by construction* rather than by a compatibility shim. And it
converts Tack §6's S4 proc-author contract — "emit sets in declared order, each
ordered by the correlation key," currently a documented convention with a debug
assertion — into something the engine **declares and can enforce**. The
hardest part of the stitch layer stops being convention.

**B. A nested value, via subtype-tagged JSONB.** SQLite already has
`sqlite3_result_subtype` / `sqlite3_value_subtype`, which the JSON functions
use. A nested column could be JSONB carrying a subtype tag: legacy clients see
a blob or text, aware clients decode structure. Uses existing machinery, adds
no type. Weaker fidelity than A, but it is the only candidate that gives a
genuine single-value nesting.

**C. A true nested value type.** MetaKit-faithful: a `sqlite3_value` that *is*
a result set. Highest fidelity, and it breaks the invariant outright —
`sqlite3_column_type()` has nothing to return. Recorded so the option is
explicit; almost certainly wrong for SQLite's philosophy.

**D. Opt-in statement mode.** Nested by request (`sqlite3_proc_config(stmt,
…)`), flat otherwise. Preserves the invariant but doubles the engine's paths,
which is the cost that keeps compounding.

A and B are complementary rather than rival: A handles parent/child collections,
B handles a genuinely scalar-shaped nested value. It may be right to build A
and never need B.

### How to decide — Sean's steer, 2026-08-02

Explicitly **do not rush this**, on the Andrew Kelley model: keep searching for
the right representation rather than shipping the first workable one, and
accept slow version numbers as the price. **Build multiple POCs and compare
them on real shapes** before choosing. Prototypes are cheap here because the
wire codec, the declared-shape checker and `proc_info` all already exist.

A POC should demonstrate, at minimum:

1. The invariant holds — an unmodified client (the `sqlite3` CLI is the honest
   test) sees today's behaviour exactly.
2. A nested shape survives `CREATE PROCEDURE` conformance checking.
3. `PRAGMA proc_info` expresses the nesting.
4. The wire protocol carries it, and shape-free mode still works.
5. Tack could collapse S1–S4 into one decoder against it.

### Succinctness

MetaKit's schema string is worth borrowing as **notation** regardless of which
design wins. `posts[id:I,title:S,comments[cid:I,body:S]]` nests without
ceremony, is human-writable and machine-parseable, and is a far better
candidate than anything we would invent — relevant to nested declarations,
to `PRAGMA proc_info` output, and to typed client generation (#2).

*(Sean used MetaKit's types to spec data designs in his notebooks twenty-plus
years ago, which is where this came from.)*

### Caveat recorded

MetaKit is dormant and its headline idea (columnar) is superseded. The goal
here is **to push SQLite as far as it will go**, not to reimplement MetaKit —
its other features are likely not part of this journey. Nesting earns its place
on merit, not lineage.

## 4. Incremental view maintenance — *the big one, and the one I most want*

Materialized views that update as writes land rather than being recomputed.

This is the missing structural piece of the architecture in `DESIGN-NETWORK.md`.
The recommendation there is append-only ledger plus periodic rollups — which
only works if rollups maintain themselves. Today that is hand-written triggers,
every time, bespoke per schema, and wrong in the same ways each time.

It is also a genuinely hard and beautiful problem: propagating deltas through
joins and aggregates, deciding what is maintainable at all (aggregates with
inverses like SUM/COUNT are easy; MAX under deletion is not), and choosing
eager versus deferred maintenance.

**Done means:** `CREATE MATERIALIZED VIEW … WITH INCREMENTAL MAINTENANCE`; a
documented, enforced subset of maintainable expressions with a clear error
outside it; property tests asserting the view equals its from-scratch
recomputation after arbitrary write sequences — that equality is the whole
correctness contract and is beautifully testable.

**Effort: large. Value: this is the one that would make the fork worth using
for reasons unrelated to procedures.**

## 5. `wal2` and `BEGIN CONCURRENT` — *two decisions, not one*

**Corrected 2026-08-01 after actually looking.** This entry previously treated
them as a single choice. They are not, and the difference is large.

Both live in the **official** SQLite repository (`github.com/sqlite/sqlite`
mirrors Hipp's Fossil at sqlite.org/src), among ~1,718 branches. Checked
currency:

| Branch | Last merged with trunk |
|---|---|
| `wal2` | **2026-07-13** — current |
| `begin-concurrent` | **2026-07-13** — current |
| `begin-concurrent-wal2` *(combined)* | **2019-01-11** |
| `begin-concurrent-pnu-wal2` | 2023-02-02 |
| `begin-concurrent-report-wal2` | 2021-11-17 |

For reference this fork branched from trunk **2026-07-04** at 3.53.3, so each
feature branch is nine days *ahead* of our fork point on trunk. There are
periodic version pins (`wal2-3.51`, `begin-concurrent-3.51`) but none at 3.53.

**The finding: the SQLite team keeps each feature current and has not kept the
combination current since 2019.** Taking both is therefore not "carry two
upstream branches" — it is doing integration work upstream stopped doing seven
years ago. Do not plan on the pair.

### 5a. `wal2` — ✅ **PORTED 2026-08-02** — 0 errors out of 393,363

Ported (not merged) onto the 3.53.4 base; see the commit for why and how. wal2
suites: 538 tests, 0 errors. Verified this fork still reads rollback-mode and
classic-`wal` databases unchanged — only wal2 files are one-way, and that is
reversible via `PRAGMA journal_mode=delete`.

Two facts to carry: `sqlite3_wal_hook`'s argument means *uncheckpointed pages
across both wal files* in wal2 mode (0 when the other file is done), and there
are two files to ship for replication rather than one. wal2 bounds WAL growth;
it does **not** add concurrent writers.

*Original rationale, kept for the record:*

Fixes checkpoint starvation: under continuous read traffic the WAL never
resets and grows without bound until latency collapses. `DESIGN-NETWORK.md`
already calls that "your production incident" — it is a problem this fork would
actually hit, whereas multi-writer is one it would only hit at scale we have
not reached.

**Trial merge run 2026-08-01** (scratch branch, aborted, no trace left). The
result changes the *method*, not the verdict.

`wal2`'s own feature delta — measured against its trunk ancestor, not against
our merge base — is **9 files, +1402/−477**, and 1784 of those changed lines are
in `wal.c` alone. Touched: `wal.c`, `wal.h`, `pager.c`, `pager.h`, `btree.c`,
`pragma.c`, `vdbe.c`, `vdbeaux.c`, `test_tclsh.c`. Its overlap with our files is
three, and tiny: `pragma.c` (3 lines), `vdbe.c` (25), `vdbeaux.c` (3).

**But `git merge origin/wal2` is the wrong instrument.** This fork is based on
the *release branch* `branch-3.53` (merge base `92a6c5c`), while `wal2` tracks
*trunk*. Merging the branch therefore drags in a month of unrelated trunk
evolution — 61 files, +5299/−3389 — on top of the feature. The trial produced
**23 conflicts**:

| Kind | Count | Notes |
|---|---|---|
| Fossil metadata | 4 | `VERSION`, `manifest*` — trivial |
| files we modify | **1** | `src/shell.c.in` only |
| trunk drift in files we do not own | 18 | `btree.c`, `expr.c`, `func.c`, `os_win.c`, `pager.c`, `printf.c`, `ext/*`, `test/*` |

**Zero conflicts in the stored-procedure implementation.** `proc.c`, `parse.y`,
`vdbe.c`, `vdbeapi.c`, `vdbeaux.c`, `sqliteInt.h`, `pragma.c`, `resolve.c`,
`trigger.c`, `attach.c`, `prepare.c`, `main.c`, `complete.c`, `sqlite.h.in` and
`test1.c` all merged clean. The conflicts are release-branch-versus-trunk drift,
not wal2-versus-procedures.

Decisive detail: **`wal.c` and `wal.h` are identical between our 3.53.3 base and
wal2's trunk ancestor.** Only `btree.c` (+70) and `pager.c` (+102) have drifted.
So wal2's main payload applies to our base unchanged.

**Recommended method — port the feature, do not merge the branch.** Apply the
9-file delta onto 3.53.3; `wal.c`/`wal.h` land clean and only `btree.c` and
`pager.c` need hand reconciliation (~172 lines of drift between them). Isolated,
testable against our existing 392,950-test gate, and it does not destabilise a
fork that currently passes it.

**The alternative worth deciding deliberately rather than drifting into:** catch
up to trunk first, then merge `wal2` cleanly. Bigger — those 22 drift conflicts
are the price — but paid once, and it leaves every future upstream merge cheap.
Related: upstream has shipped **3.53.4** since we forked, so a small catch-up on
our own release line is available and low-risk regardless of which path we take.

### 5b. `BEGIN CONCURRENT` — decide at transport Phase 5

Multiple optimistic writers with page-level conflict detection. Also current
and mergeable, but it changes what group commit should look like, so it belongs
to the Phase 5 design conversation rather than to this one.

## 6. System-versioned temporal tables — *SQLite has no story here at all*

SQL:2011 `AS OF` / `FOR SYSTEM_TIME`. Every row carries a validity interval;
updates close the old version and open a new one; queries can ask what the
table looked like at a past instant.

SQLite has nothing, and the workaround (shadow history tables plus triggers) is
universal, hand-rolled, and subtly wrong in most implementations. Pairs
naturally with the append-only ledger pattern and with #3.

**Done means:** `AS OF` query support, automatic versioning on write, a
retention/pruning policy, and a proof that a point-in-time query equals a
from-scratch replay to that point.

## 7. Fan-out shard virtual table — *the one sharding piece that belongs in the engine*

A vtab presenting N shard files as one logical table with constraint pushdown,
so cross-shard reporting queries do not have to be written by hand. Everything
else about sharding is application architecture and explicitly does not belong
in this fork (`DESIGN-NETWORK.md`); this is the exception.

## 8. `OUT` / `INOUT` parameters — *listed in README-PROCS as planned*

Result sets cover most cases, which is why this was deferred. Worth revisiting
only if the typed-client work (#2) makes the absence awkward at the boundary.

---

## Ordering I would actually recommend

**Authorization → typed clients → incremental views**, with the transport plan
running alongside, and **nested result shapes (#3) prototyped in parallel
rather than scheduled** — it is a design question that wants several POCs and
no deadline, and it makes typed clients substantially more useful whenever it
lands. Authorization because we opened the hole and it gates
anything network-facing; typed clients because they are cheap and multiply the
value of everything else; incremental views because that is the one that
changes what the fork *is*.

`BEGIN CONCURRENT` needs a decision before transport Phase 5 regardless of
where it sits in this list.
