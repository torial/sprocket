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

**✅ TypeScript emitter DONE 2026-08-14 (PLAN-TSGEN) — and with it #2's
done-means is met in FULL: all four emitters (c, zebra, python, ts),
round-trip gates, deterministic regeneration.**  The N-API addon
(tool/napi) statically links the fork; INTEGER crosses the JS boundary
as number within 2^53-1 and BigInt beyond, pinned both directions.

**TypeScript emitter — substrate decided 2026-08-13 (Sean: N-API).**
Node has no built-in FFI, so the TS client has two legitimate substrates:
an **N-API addon** (stable ABI across Node majors, plain C, statically
links the fork — the local client, Python-ctypes' equivalent) and the
**wire** (the browser client, waits on transport).  One TS emitter, two
runtimes behind one generated type surface.  Sequenced after the queue
engine piece; the addon's cost is a per-platform prebuild matrix.

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

**Still open:** the TypeScript emitter and wiring `.procgen` into the shell.
The Zebra emitter is done (2026-08-06, `--lang zebra`).

### ✅ Python emitter done 2026-08-12 — `--lang python`

The "only the emit differs" premise above was FALSE for runtime languages:
stock SQLite cannot execute CALL and DB-API bindings cannot reach the
segment APIs, so a Python client needs a runtime binding to the fork.  The
emitter therefore generates a SELF-CONTAINED client: typed dataclasses plus
a ctypes runtime over the fork's own `sqlite3.dll` (the proc APIs ride the
auto-generated export list for free), with `connect(db_path, dll_path)`
taking the DLL path explicitly — loading whatever is on PATH would be the
ambient-state failure.  Same RETURNING policy as Zebra (explicit list at
depth 1, `RETURNING *` past it), bucket-based stitch, python-keyword
shield, raw column names as dict keys with sanitized dataclass fields.
Verified end-to-end by `tool/procgen_pytest.py` against
`tool/procgen_pynest_fixture.sql` (both committed): flat+params, depth-1,
depth-2 with an empty leaf, a comparator negative control, and a loud
refusal path.  Regeneration byte-identical; Zebra and C output unchanged.

**TypeScript — deliberately still open, blocked on a runtime choice, not
on emission:** Node has no fork binding; the candidates are (a)
`better-sqlite3` recompiled against the fork's amalgamation plus an
extension surface for the proc APIs (real work, native toolchain per
consumer), (b) a WASM build of the fork (heavy, but zero native deps), or
(c) N-API glue generated alongside the client (procgen emitting its own
binding, the most self-contained and the most code).  Pick when a TS
consumer exists; the Python emitter is the template for whichever runtime
wins.

**Effort: small. Differentiation: high.**

## 3. Nested result shapes — *MetaKit's idea, and an open design question*

**Status: BUILT — and this line was stale long enough to mislead a
ledger (caught 2026-08-13).** PLAN-NESTED phases 1–6 DONE by 2026-08-05;
depth ≥ 2 landed via PLAN-DEPTH (COMPLETE 2026-08-08, procdeep 19/19,
Mosaic depth-2 typed client reproducing the case study); projection
executed via PLAN-PROJECTION (2026-08-11); 3i/3f/3e followed 2026-08-12.
`procgen` recurses depth-N and emits C, Zebra, and Python.  Still open
in this family: 3h (interleaved segments — napkin only, the one design
that gets both laziness and one round trip) and the balanced-
misattribution checksum (recorded as a separate feature).  The original
design record follows.

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

### POC 1 (2026-08-03) — does nesting earn its place at all?

`tool/proc_nestpoc.c`. Before choosing syntax, measure whether the feature is
needed: SQL already has three ways to return a parent with its children, and
Tack implements all three. Measured on 10 parents x 20 children, bytes counted
through the **real wire codec**, values counted exactly — no timings.

| strategy | bytes | values | |
|---|---|---|---|
| S1 join-flatten | 29,861 | 1200 | parent columns repeat per child row |
| **S3 JSON fold** | **12,501** | 50 | children as JSON text |
| S4 two result sets | 15,511 | 640 | no repetition; needs correlation |

**The result argues against the obvious motivation.** S4 beats join-flatten by
48%, which is the number one would quote — but **JSON fold is smaller still, by
24%**. So "fewer bytes" cannot be the case for nested shapes. Something already
in the box does better.

**Control, and the honest half:** with *one* child per parent there is nothing
to deduplicate and S4 must lose. It does — 1721 bytes against join-flatten's
1593. A feature that looked good on every input would mean the measurement was
wrong.

### What POC 1 changed about the argument

If bytes are not the case, what is? **Type fidelity — and it is not a matter of
degree.**

```
SELECT json_group_array(blob_col) FROM att;
Error: JSON cannot hold BLOB values
```

JSON fold does not merely encode a BLOB awkwardly; it **refuses**. A result set
carries the same column as `SQLITE_BLOB` untouched. The same flattening applies
less dramatically to the integer/real distinction, which JSON has no way to
preserve.

So the thesis for DOCKET #3 is now:

> Nested shapes are not a compression feature. They are the only way to return
> a parent with typed, streamable children — JSON fold is smaller but lossy and
> outright rejects binary; join-flatten is faithful but quadratic in the parent.

That is a narrower and more defensible claim than the one this entry started
with, and it should be the sentence any syntax is judged against.

**Still to prototype:** whether declared-metadata correlation (candidate A)
actually lets a client reassemble more cheaply than it can today, and what the
syntax costs. POC 1 says the feature is worth *a* design; it does not yet say
which.

### Rulings — Sean, 2026-08-03, after POC 1

**R1. The nested thing is a real column. `column_count` reports 3.**
This selects candidates B/C over A: **one** result set whose third value holds
the children, not two sequential sets. Sean's reasoning, and it corrected mine:
the invariant is *"an unmodified client still works,"* **not** *"column count
matches the flat form."* A third column carrying JSON satisfies it completely —
a flat client reads three columns, the third being text, and nothing breaks; an
aware client reads the same column through a structured accessor and gets full
fidelity. That is **graceful degradation**, where candidate A's "declared as a
column but physically a second set" was a small lie in a contract language.

*Background, recorded because it was the genuinely confusing part:* today's two
declared shapes are not two things side by side. They are one cursor before and
after `sqlite3_proc_next_resultset()` — a tape with two segments, with the
statement's own column metadata rewritten between them. Candidate A's
`comments TABLE(...)` was only a *name for the second segment*.

**R2. Nested-table syntax, not `FOR EACH`.**
`RETURNS TABLE(id, title, comments TABLE(cid, body))` reads more intuitively
than declaring a related set separately — and under R1 it is now also literally
true, since the nested thing really is a column.

**R3. Validation stays lightweight.**
The engine records **cardinality** — how many child rows belong to each parent —
and nothing more. That catches the failure that actually happens (children lost
or duplicated) for one integer per parent. A monotonicity assertion on the
correlation key in debug builds is the natural companion and is nearly free.

Deliberately **not** done by the engine: enforcing `ORDER BY`, validating that
the key is a real foreign key, or any referential-integrity check. Those are
expensive and they belong to the reassembler.

**R4. Notation is punted, deliberately.**
MetaKit's succinctness is still attractive but the broader consequences are
unclear, and notation only becomes expensive once it is in a persisted schema.
Punting therefore means *not shipping syntax yet*, which is where POC 2 leaves
us anyway.

**R5. Division of labour — the database is a compiled API.**
Sean's framing: *"the database w/ sprocs / remote calls has a sort of compiled
api... the reassembler does the quality work, not the DB necessarily."*

- The engine **declares**. It does not enforce ordering, does not reassemble,
  does not validate keys.
- **`procgen` emits the reassembler**, so the ordering assumption and the
  cardinality check live in generated, per-language, schema-versioned code.
- The engine's whole obligation shrinks to: record the relationship, record the
  cardinality, keep both introspectable.

This collapses several open questions at once and makes the engine change much
smaller than it was heading toward. It also moves the reference reassembler
onto **#2's** side of the line rather than #3's.

**R6. Binary children in the JSON fallback: base64.**
SQLite ships both `ext/misc/base64.c` and `ext/misc/base85.c` (Brasfield's own
variant, whose alphabet already excludes the double quote).

| | expansion | JSON | availability |
|---|---|---|---|
| base64 | 1.333x | safe | **every stdlib** |
| base85 | 1.25x, plus `\` escaping ~1.265x | needs `\` escaped | bespoke decoder per language |

base64 wins on the shape of the path rather than the ratio: **the JSON fallback
is the degraded route for unaware clients** — aware clients get the BLOB
natively and pay nothing. The encoding therefore only ever matters where
compatibility matters most and bytes matter least. Trading a stdlib one-liner
in every language for 5% on the path you hope nobody takes is the wrong side of
that trade.

### POC 2 (2026-08-03) — built. Q2 came back **yes**, decisively.

`tool/proc_nestpoc2.c`. 50 parents x 20 children, exact counts, no timings.

**Q1 — what materialisation costs, and whether laziness recovers it**

| strategy | child rows read | JSON bytes built | peak buffered |
|---|---|---|---|
| eager JSON column | 1000 | 46,493 | 20 |
| lazy column (client reads 1 in 5) | 200 | 9,291 | 20 |
| ordered merge (two sets) | 1000 | **0** | 20 |

Laziness recovers exactly the proportion the client never asks for, and the
merge materialises **nothing**. All three hold only one parent's children at a
time, so none of them is a memory problem.

**This prices R1 rather than contradicting it.** The nested-column design costs
46 KB of JSON that the two-set design does not — *for a flat client*. With a
lazy column an aware client reads the structured accessor and never triggers
the JSON at all, so the materialisation cost falls entirely on the degraded
path. That is exactly where R1 puts it, and POC 2 confirms the arithmetic
works out.

**Q2 — does declaring the correlation buy anything over the convention?**

The convention is "both sets ordered by the correlation key". Break it and the
ordered merge returns:

```
correctly ordered : 1000 pairs
wrongly ordered   :   20 pairs
```

**It does not error. It silently returns 2% of the data and reports success.**

So the convention is *not* self-enforcing, and the answer to Q2 is yes: what
declaration buys is **detection**. This is the vacuous-instrument pattern in
data form — a wrong answer that looks exactly like a right one.

**Consequence for Tack, worth acting on independently of this docket item:**
its S2 ordered-merge relies on this convention with only a *debug-build*
assertion. In a release build, a procedure author who writes the wrong
`ORDER BY` silently loses almost all of the child rows. That is a live bug
class in the ORM design, not a hypothetical.

**Q3 — does the cardinality check catch it?**

```
half of parent 7's children deleted     : 1 parent mismatched
parent 9's children duplicated          : 2 parents mismatched
counts restored                         : 0 parents mismatched
```

Yes, both directions, and **localised to the offending parent** rather than
merely reporting a total that is wrong. The last line is the control: a check
that never goes quiet is an alarm, not a check.

### R7 — the correlation column should be index-advised (Sean, 2026-08-03)

Option 1.5 has the engine *impose* the child ordering. If the correlation
column has no index behind it, that ordering costs a full sort on every CALL —
and the author never sees it, because they never wrote the `ORDER BY`. Making
the engine responsible for the ordering makes it responsible for saying when
the ordering is expensive.

**Do not reimplement index reasoning. The planner already knows.**

```
no index :  SCAN c
            USE TEMP B-TREE FOR ORDER BY
index    :  SCAN c USING INDEX i
```

Since 1.5 is the thing imposing the ordering, it can observe whether the
ordering came free — `sqlite3WhereIsOrdered()` reports exactly that, already
computed, no heuristics about which index might apply, and correct for cases a
column-lookup would get wrong (expression indexes, partial indexes, a child
source that is a view or a join rather than a table).

**Precedent for the channel already exists.** `SQLITE_WARNING_AUTOINDEX`
(`src/where.c:1056`, logged through `sqlite3_log`) is the same class of message:
non-fatal, advisory, "the planner had to build something you could have
provided." Follow it rather than inventing a warning mechanism.

**Surface it twice, per R5's division of labour:**

1. **Runtime backstop** — log once at prepare, in the AUTOINDEX style. Catches
   the case where an index is dropped after deployment.
2. **Build time, which is where it is actionable** — make the fact
   introspectable so `procgen` reports it while generating the client. A
   developer can add an index when they are looking at the schema; they cannot
   when they are reading a production log.

**Constraints on the advice itself:**

- **Never an error.** A ten-row child table does not need an index, and the
  author may well know better than the check.
- **Say why, not just what.** "The declared correlation `comments.post_id`
  requires an ordering that no index supplies" is actionable; "add an index"
  is nagging.
- **Only knowable at prepare, not at CREATE.** The body is conformance-checked
  at CREATE but not fully planned there. `procgen` already opens the database
  and reads `proc_info`; preparing each CALL to collect this is a small
  extension of what it does.

**Refined 2026-08-04, on implementing it.** The advisory is emitted **at
compile**, not at every prepare — a body served from the procedure cache does
not re-advise. That is sufficient for the stated purpose: an index dropped after
deployment changes the schema, which expires statements and forces a recompile,
so the advisory reappears exactly when the situation it warns about arises.
Requiring a log on every prepare would re-advise for a repeated identical call,
which is noise rather than signal. `test/proc6adv.test` had originally demanded
that, and the *spec* was corrected rather than the engine.

**Detection method, with its limit.** The generated code for the child `SELECT`
is scanned for `OP_SorterOpen`: an ordering an index satisfied emits no sorter.
`sqlite3WhereIsOrdered()` would be more precise but is only reachable during
where-loop generation, not after `sqlite3Select()` returns. The consequence is
that a sorter the child query needed **anyway** — its own `GROUP BY`, say — is
indistinguishable, so this can over-report. It never under-reports, which is the
right direction for an advisory.

**Honest limit:** performance advice ages badly. This says the ordering costs a
sort, which is a fact; whether that matters is a judgement the tool cannot
make. Report the fact, let the developer judge.

### POC 3 (2026-08-03) — how cardinality should travel. **Answer: per-parent.**

`tool/proc_nestpoc3.c`. The temptation is to choose by cost. The right way is
to choose by **what each option can see**, so this is a detection matrix: five
ways the child stream goes wrong, three ways of carrying cardinality.

| failure mode | total | per-parent | constant |
|---|---|---|---|
| *(control: correct data)* | quiet | quiet | quiet |
| ordering broken | CAUGHT | CAUGHT | CAUGHT |
| children lost | CAUGHT | CAUGHT | CAUGHT |
| children duplicated | CAUGHT | CAUGHT | CAUGHT |
| misattributed, one child moved | **missed** | CAUGHT | CAUGHT |
| misattributed, balanced swap | missed | **missed** | missed |
| *(legitimate variable counts)* | quiet | quiet | **FALSE ALARM** |

Cost through the real codec: total **9 bytes**, per-parent **450 bytes** for 50
parents (~9 per parent), constant **0**.

**CONSTANT is disqualified**, and not on cost. It false-alarms on legitimate
variable cardinality — which is the normal case for real data, since posts do
not all have the same number of comments. Its apparent 4-of-5 detection rate is
an artefact of alarming at nearly everything.

**PER-PARENT is the choice.** It catches exactly one thing a total does not:
*unbalanced misattribution*, the realistic shape of an off-by-one in a
correlation key. That costs ~9 bytes per parent row — about 3% of a response
that is already ~15 KB at this size — which is cheap for a real bug class.

**The limit, recorded so nobody assumes cardinality means correctness:**
a *balanced* swap (two equal-sized parents exchanged) is invisible to **every**
count-based check at any granularity and any cost. Counting is simply the wrong
instrument for content. Catching that needs a checksum over the correlation
keys, which is a different feature and not proposed.

### The subtlety that matters more than the choice

A cardinality check only detects if the count is computed **independently of
the child stream it is checking**. If the engine derives per-parent counts by
running the same child query, a mislabeled correlation key produces consistent
— and consistently wrong — counts, and the check passes while the data is
wrong. That is the positive-control problem in another costume: a check drawn
from the same source as the data cannot validate it.

So the honest scope of this feature is narrower than "cardinality catches
errors":

> **Cardinality checks transport integrity, not query correctness.**
> Truncation, framing bugs, a dropped frame, a reassembler that loses a run —
> all caught, because the declared count is genuinely independent of what
> arrives. A logic error *inside the procedure body* is not caught, because
> the count and the rows come from the same place.

That is still worth having: POC 2 showed the transport-shaped failure loses 98%
of rows silently. But it should be documented as integrity, not validation.

### Where POC 2 leaves the design

1. The feature earns its place on **type fidelity** (POC 1) and on **detection**
   (POC 2 Q2). Neither is about bytes.
2. The engine's obligation is confirmed small and matches R5: record the
   relationship, record the cardinality, keep both introspectable. It does not
   need to enforce ordering — the reassembler compares counts and that is
   sufficient to catch the silent failure.
3. The lazy nested column is the right shape for R1, and its cost lands on the
   flat-client path by construction.

**Still open, and now the only things that are:** the syntax (R4 punted it),
and whether the cardinality is carried per-parent in the result stream or
declared once in the shape. The second is a real design question — per-parent
counts cost a value per row, a declared constant cannot express variable
cardinality.

### Original scope, for the record

Build the **lazy nested column** and answer three things:

1. What does materialisation actually cost? A JSON fallback means a parent's
   children must be materialised before that parent row can be handed out —
   unless the column is lazy and materialises only when a flat client reads it.
   Measure both.
2. Does declared correlation buy anything over the convention Tack already
   uses? Tack knows the FK graph at build time and its S2 ordered-merge already
   reassembles from two sets. **POC 2 must be allowed to answer "no"** — in
   which case the outcome is documentation, not grammar.
3. Confirm the cardinality check (R3) catches lost and duplicated children, by
   breaking it on purpose.

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

### #2 addendum, 2026-08-04 — the Zebra emitter is the priority, and its output shape is specified

`procgen` emits C today. The obvious next emitters were TypeScript/Python/Zebra
in no particular order. **Zebra is now first**, because the fork's named
consumer is Zebra's ORM, and its required output shape is already known from the
Zebra side rather than guessed:

- **native output must be `List(T)` of structs**, not accessors a consumer
  adapts. Adapters are where the silent-loss class lives.
- Zebra's data access is positional binds via list literals, `?`-optionals on
  every handle, nil-checked before use.
- Zebra has no `/=`, no `&`, and only just gained `~`. Less C-shaped than
  instinct suggests.

Source: [[correspondence_fable-opus5]] entry 2. Not inferred here.

---

## 3b. The rowid spine — *late materialisation, and MetaKit's trick returning*

**Sean's, 2026-08-03, while reviewing the phase 5 blocker.** Let the parent
segment carry only the row id; do not load the parent's other fields, or any
children, until asked. The id keys into both.

This is **late materialisation** — fetch the qualifying identifiers first, then
fetch only what is actually projected. It is what column stores do, and it is
recognisably MetaKit's own move, which is a pleasing place for it to reappear
given #3 started from MetaKit.

**Where it genuinely helps.** Option 1 of the phase 3+5 unit buffers the parent
segment, and its memory argument rests on "parents are the small side" — an
assumption about `posts` vs `comments`, not a guarantee. A spine of ids makes
that **true by construction** rather than by luck. That is a real improvement to
the design being built, not an alternative to it, and it is the cheapest way to
remove the one soft spot in option 1.

**Where it breaks, precisely.** Deferring the parent's *other columns* requires
re-addressing a row by id, and a stored procedure's parent `SELECT` is arbitrary
SQL, not a table scan — `SELECT id, count(*) ... GROUP BY` has no row to go back
to. Column stores can do this because a row is a physical address; here it is a
computed result.

So it splits cleanly:

- **Spine + buffered parent columns + lazy children** — this *is* option 1,
  strengthened. Adopt.
- **Spine + lazy everything** — sound only when the parent `SELECT` is a
  projection of a single table with a rowid and no aggregation. That is a
  planner-detectable precondition and a legitimate *optimisation* to apply when
  it holds, not a v1 mechanism.

**Bonus worth noting:** the spine gives phase 4 its cardinality for free. The id
list is the exact parent count, and per-parent child counts hang off it without
a second pass.

---

## 3c. Level-of-detail projection — *bigger than nesting, and it subsumes laziness*

**Sean's, 2026-08-03.** A mechanism saying what level of information is wanted:
top level only, or posts + comments, or posts + authors.

This is **projection over the declared shape** — a column list generalised to
nested tables. It is worth separating from #3 because it is the larger idea.

**Why it beats laziness rather than duplicating it.** The boundary fold's
laziness is *discovered*: the engine finds out nobody read column 3 after the
parent row was delivered. Projection is *declared up front*, so the engine can
decline to run — or even to compile — the child `SELECT` at all. It also fixes
the case laziness handles worst: with two nested tables, skipping `comments` to
reach `authors` still costs a scan of `comments` under streaming, and costs
nothing under projection.

**The fork worth deciding before building.** Projection can be *runtime*
(the call names what it wants; `sqlite3_column_count` then varies per statement,
which is legal but multiplies the metadata cases) or *compile-time* (procgen
emits `pwc_flat()` and `pwc_with_comments()` variants, which costs nothing at
run time but multiplies procedures). R5 already puts the client in charge of
reassembly and the client knows its needs at generation time, which argues for
compile-time — but runtime is what makes an *unmodified* client able to ask,
and the invariant is about unmodified clients.

**What it unlocks.** Depth ≥ 2 is currently out of scope partly because a deep
tree is an unbounded fetch. With projection you never fetch a whole tree, only
selected edges — which makes the ancestor-path problem worth revisiting rather
than permanently closed.

---

### 3c design, written up 2026-08-04 — now the next thing, not future work

Phase 5b made this urgent rather than desirable. The generated fold lives in the
parent `SELECT`, so **every** client pays for it: stepping the parent scans the
children even when nothing reads the column, and a segment-aware client that
then reads the child segment scans them **twice**. Measured at 4 and 8 where
laziness gave 0 and 4 (`proc6-8.5/8.6`). That is a regression for the typed
client, which is the one the feature exists to serve.

**Where the fold has to move.** It is currently baked into the body at CREATE
(`procCheckAndLower`), which is why it is unconditional. Runtime suppression is
not available — SQLite evaluates every result column of a row it produces. So
the fold must move to **CALL-compile time**, where the requested projection is
known and can be compiled in or out. The body stays canonical and unfolded;
`codeProcBody` generates fold columns per request.

**Consequence to plan for:** the proc cache is keyed by procedure. It must
become keyed by *(procedure, projection)*, or two clients asking for different
projections will silently share one compiled body — a wrong-answer bug of
exactly the kind this feature keeps producing.

**The surface.** The projection changes codegen, so it must be known at prepare,
which means it belongs in the statement text rather than in a C setter. Proposal,
costing **zero new keywords** — `RETURNING` is an existing token and already
means "these are the columns I want back" in SQLite's own vocabulary:

```sql
CALL post_with_comments(1);                          -- all fold columns (default)
CALL post_with_comments(1) RETURNING id, title;      -- no fold columns generated
```

**Semantics, deliberately narrow for v1:** the clause controls *only which
generated fold columns are produced*. Segments are unaffected — they are the
body's own `SELECT`s and always stream. So:

- Omitting the clause folds everything. **The invariant is the default**, which
  is what keeps an unmodified client working.
- A typed client writes `RETURNING <value columns>` and gets no subquery at all.
  The double scan disappears and `proc6-8.5/8.6` return to 0 and 4.
- Naming some nested tables and not others works per-table, which is Sean's
  "posts + comments but not posts + authors" case.

**Deliberately NOT in v1:** dropping the child *segment* for an unprojected
nested table. It would save the scan entirely, but it changes how many sets a
client steps through, so `nresultsets` becomes projection-dependent and every
segment-aware consumer has to agree about which sets exist. Worth doing, worth
doing separately.

**The open question this section used to carry — accessors per projection —
was settled 2026-08-11; the campaign record is `PLAN-PROJECTION.md`.** The
settlement, briefly: in the Zebra emitter (the consumer that exists) the
question dissolves, because the PROJECTION IS CHOSEN BY THE GENERATOR, not
the caller — the emitted client issues one query per procedure with
`RETURNING <value columns>` fixed at generation time, and its surface is the
typed struct stitched from segments, so `sqlite3_column_count` variance never
reaches a consumer. The C emitter does not emit RETURNING and honestly keeps
accessors per procedure. If a future emitter offers CALLER-chosen
projections, accessors go per projection then — deferred until such a
consumer exists. Everything else in this section is implemented: P0–P2
(grammar, canonical body, projection codegen) landed 2026-08-04
(`9b8f0539..0704b28e`, `proc3c`), the cache re-key landed 2026-08-11
(`proc3c` 6.x/7.x, `sqlite3_proc_cache_list`), and the localisation pass's
three bug finds are recorded in the plan.

---

## 3i. RETURNING at depth — projection over nested shapes

**Written 2026-08-12, from a consumer hitting the wall rather than from
symmetry.** `witnesses_full` (Mosaic: witnesses → claims → confidences,
depth 2) is the projection feature's own motivating client one level deeper
than the feature reaches: RETURNING is refused past one level of nesting, so
procgen's Zebra emitter leaves the deep client paying JSON fold construction
at **every** level for columns it stitches from segments and never reads.
The 3c campaign's "done means" sentence promised this proc a projection it
cannot legally write — caught 2026-08-11, recorded in PLAN-PROJECTION.md.

**Why depth-1 RETURNING refuses depth.** The projection list names root
columns, but at depth the *child segments themselves* carry generated folds
(a claims row carries the confidences fold), and a root-scoped list has no
way to name them. Silently accepting the list and leaving the inner folds
would be the accept-and-ignore failure this branch keeps refusing.

**Three designs considered:**

- **A. Root-scope list at depth** — allow the depth-1 semantics on deep
  procs; inner segments keep their folds. Rejected: for the one consumer
  that exists this is half the win (the claims-segment fold is exactly as
  wasted as the root one), and it silently under-delivers what the spelling
  appears to promise.
- **B. Nested projection syntax** — `RETURNING wid, claims(subject, cid)`.
  Full generality: per-column choice at every level, grammar mirroring the
  declaration. Rejected *for now*: no consumer needs per-column choice at
  depth (the typed client's need is uniform), and it costs a recursive
  grammar, recursive validation, and a tree-shaped cache-key
  canonicalization. This is the someday-general form; nothing in C
  forecloses it.
- **C. `RETURNING *` — all value columns, no generated folds, at every
  level.** The consumer's actual request has one shape: "the declared value
  columns; I stitch children from segments; generate no folds anywhere."
  One spelling serves it exactly. **Chosen.**

**C's semantics, deliberately narrow:**

- `RETURNING *` is accepted at ANY depth (including depth 1, where it means
  "keep every value column, drop every fold" — expressible today only by
  listing all value columns).
- Fold columns are declined at every level: the root SELECT's and each
  child segment's. Segments themselves are untouched, as in 3c —
  `nresultsets` does not change, the invariant stays opt-in.
- The named-list form keeps its depth-1-only rule and its refusal past one
  level; the refusal message now names `RETURNING *` as the fix (UNGIT:
  refusals name the fix).
- Cache key: `*` canonicalizes to its own signature, distinct from the
  default body and from any named list.
- `WITH COUNTS` composes unchanged (counts are parent-arm columns, not
  folds). INTERLEAVED continues to refuse RETURNING in both spellings —
  it computes no folds, nothing to decline (3h, closed).
- procgen: deep nested procs emit `RETURNING *`; depth-1 emission may
  switch to `*` for uniformity or keep explicit lists (decide by whichever
  keeps regeneration byte-identical for existing clients).

**Done means:** a depth-2 fixture under `RETURNING *` shows zero child
scans on the parent walk at BOTH levels (the proc3c 3.x measurement, one
level deeper), segment content byte-identical to the default's segments,
the planted cache legs extended to the `*` signature, and procgen's deep
emission verified end-to-end against `witnesses_full` on the live Mosaic
database.

### ✅ IMPLEMENTED 2026-08-12 — and the control measured something extra

Landed as designed (option C): `"*"` sentinel IdList, nested-aware
`sqlite3ProcProjKeeps(pProj, zName, bNested)`, star flowing down the
child-descriptor recursion, `"*"` cache signature (deliberately not
deduped with the expanded list — the comment in `procProjSignature` says
why), named-list depth refusal now names the fix, procgen deep emission.
Zero lemon conflicts, checked in `parse.out` rather than assumed.
`proc3c` 8.x: full walk at depth-2 drops from measured `{20 9}` scans to
`{4 3}` — each level scanned exactly once, by its own segment.

**The positive control found a real defect, diagnosed exactly 2026-08-12
(EXPLAIN receipts in the session log):** `procAddFoldColumn` hands the
inner recursion a RAW DUP of the child's key expression
(`procFoldInnerExpr(..., sqlite3ExprDup(pOld->a[v].pExpr))` — the comment
beside it argues capture-safety, which is true, and misses evaluation
semantics).  The dup'd expression becomes the RHS of the grandchild
correlation and lands INSIDE the grandchild scan loop: opcode listing
shows `tally` at one site per json value (fine) and one site inside the
replies Rewind loop — **O(children × grandchildren) evaluations of the
key expression per parent row**.  Measured: 2 json sites + 2 comments ×
3 replies = 8/parent = the pinned 16.  For a NON-DETERMINISTIC key
expression this is also a wrongness bug: the correlation evaluates a
different value than the json displays.

**Fix design (deliberately not implemented in the overnight session —
it is a restructure, not a patch):** the root fold must WRAP its child
like the inner levels already do (`FROM (child AS _f*) AS a1 WHERE
a1._f0 = parent.key`, inner levels correlating on `a1._fN`), with the
flattener blocked via the `LIMIT -1` idiom (procInterArm's trick) when
the child carries a further nested fold and any referenced child
expression is not a plain column — an expression key can never use an
index, so blocking flattening there costs nothing.  The same guard
belongs in `procFoldInnerExpr`'s own wrap for level N→N+1.  proc3c 8.1
announces the fix: `{20 9}` drops to `{12 9}`.

### ✅ RESTRUCTURE LANDED 2026-08-12 (evening) — and the pin dropped PAST the prediction

Implemented as designed, with one economy the design missed:
`procAddFoldColumn` now simply CALLS `procFoldInnerExpr` at level 1
with the parent-alias key reference — the root's broken construction
and the inner levels' correct one differed only in that the root
inlined what the inner levels wrapped, so the fix deleted ~90 lines
rather than adding a parallel path.  One guard site covers every
level.  proc3c 8.1 announced at `{8 9}`, not the predicted `{12 9}`:
the restructure also removed the twice-per-parent child evaluation the
morning note had recorded as a separate suspected inefficiency — 8 is
the naive model's own floor (2 parents × one full child scan, every
projected expression evaluated once per scanned row).

**The A/B that mattered:** with ONLY the LIMIT-1 guard disabled, the
pin snapped back to `{20 9}` exactly — the flattener flattens the wrap
(non-deterministic projected functions included) and reconstructs the
original defect byte-for-byte.  The guard is load-bearing, not
hardening; without it the restructure is a no-op that LOOKS landed.
proc3c 8.1d now watches the adversarial case (a function DECLARED
deterministic that counts its calls) so the regression announces
itself in both flag regimes.

---

## 3d. `mayAbort` assertion in stored procedures — *a real bug, inherited*

### ✅ FIXED 2026-08-12 on `stored-procs` (`ada5adc0`), merged here

The hypothesis below (cache replay) was **wrong** — the assert fires on the
first fresh compile. Root cause: `codeProcProgram` claimed
`sqlite3MayAbort()` blanket for every write step, and the claim is false
for a body whose writes cannot abort; simply dropping it fails the debug
auditor the other way (a body SELECT calling any function counts as
abortable). The claim is now **computed from the compiled opcodes**
(`procProgramMayAbort`, mirroring `sqlite3VdbeAssertMayAbort`'s own test),
so the CALL claims exactly what its body can do. With it, the ENTIRE proc
family runs green under `DEBUG=3` for the first time.

### The second bug behind the wall — CORRECTED twice, kethiv and qere

The overnight fix recorded the `sqlite_sequence` lock in `autoIncBegin`
and called the hole "latent upstream for triggers." **Both halves were
wrong, found the same morning by building a STOCK 3.53 debug fixture and
watching the claimed repro survive.** Upstream is fine: the outer DML
statement's own end-of-codegen `sqlite3AutoincrementEnd()` call sweeps a
trigger body's deferred autoinc registrations and records the lock in
time. What the survival exposed instead was the REAL fork bug, worse than
any assert: **a CALL statement never made that sweep at all, so a body
INSERT into an AUTOINCREMENT table read the counter and NEVER wrote it
back** — `sqlite_sequence` stayed empty and a deleted max rowid was
silently REUSED through a procedure, the one thing AUTOINCREMENT exists
to prevent, shipped since procedures could first insert. The debug lock
assert was this bug's shadow: the read cursor existed, the write side
that would have recorded the lock never did. Fix: `sqlite3CallProc`'s
toplevel codegen calls `sqlite3AutoincrementEnd()` exactly as every DML
statement does; the `autoIncBegin` band-aid is reverted to upstream
parity. `proc3` 5.3b–5.3d pin the contract (sequence persisted, deleted
max never reused). No upstream report is warranted — the draft died at
its own verification step, which is the step existing for that.

**Found 2026-08-04 by building `DEBUG=3`.** Every build of this fork's
procedure work had been `-O2` with `NDEBUG`, so no `assert()` had ever run.

```
proc1-2.3 ... Assertion failed:
  !pParse->isMultiWrite || sqlite3VdbeAssertMayAbort(v, pParse->mayAbort)
```

Reproduces in `proc1`, `proc4`, `proc5` and `psm1` — the suites that predate
nested shapes. **Confirmed pre-existing** by checking out `stored-procs` at
`a0dabed7`, building it in debug, and reproducing at the same test. It is not a
nested-shapes regression.

The statement is marked multi-write without the matching abort flag. The
suspect region is the proc cache's replay of toplevel bookkeeping — it sets
`sqlite3MultiWrite()` and `sqlite3MayAbort()` from separate cached flags
(`PROCCACHE_WRITES`, `PROCCACHE_MAYABORT`), so a body that reaches one without
the other would produce exactly this. **That is a hypothesis; localise before
fixing**, the way the lookaside leak was localised rather than guessed.

**Belongs on `stored-procs`, not `nested-shapes`.** It affects the shipped
procedure feature regardless of nesting, and fixing it on a feature branch would
bury a general fix inside an unrelated merge.

**Standing lesson:** a release-only test regime cannot see any `assert()`, and
SQLite is unusually assert-dense. `DEBUG=3` belongs in the routine, not in the
occasional deep check.

---

## 3h. Interleaved segments — the napkin

**Napkin planning 2026-08-05, at Sean's request, after the coroutine probe
cleared the SubProgram-frame risk.** Not a decision; the shape of one.

Interleaving means emitting parent row P, then P's children, then the next
parent — one client cursor, bounded-memory streaming, no `.collect()`. The
engine can drive it (`multiSelectOrderBy` merges two coroutines today). The
open question was never the engine. **It is what a row's shape changing per
step does to the C API.**

### The constraint that decides the design

**Today an unaware client is safe by construction, and that is not an
accident.** A client that never calls `sqlite3_proc_next_resultset()` sees
segment 0 and stops — `sqlite3_step()` returns `SQLITE_DONE` at the set
boundary (`bAtSetEnd`). So a legacy client consuming a nesting procedure gets a
well-formed single result set of constant shape. Everything past segment 0 is
reached only by asking.

**Interleaving destroys that property unconditionally.** An unaware client would
receive parent rows and child rows in one sequence with shifting arity, names
and types.

**Therefore interleaving must be an opt-in per CALL**, exactly as `WITH COUNTS`
is. Unaware clients never request it and observe today's behavior byte for byte;
aware clients (procgen-generated) request it and handle per-row dispatch. This
is not a concession — it is the same pattern the feature already uses twice, and
it means the risky path cannot be entered by accident.

### How a row says which segment it is

- **A new accessor reading `Vdbe.iProcSet`** — *preferred.* That field already
  exists and already tracks the current set; interleaving updates it per row
  instead of per boundary. No wire cost, no extra column, a pure read, and it
  composes cleanly: `step()` → `current_segment()` → dispatch.
- **A discriminator column** — works, and the `nHiddenCol` machinery already
  carries trailing columns the client is not shown. But it costs a column on
  every row to carry information the statement already holds. Redundant.
- **Infer from arity** — rejected outright. Two segments of equal width are
  indistinguishable, and the failure is silent (below).

### The failure mode is silent, which raises the bar

Measured, not assumed: `columnMem()` returns `columnNullValue()` and sets
`SQLITE_RANGE` for an out-of-range index. So a client holding a stale, wider
column count and reading a narrower row **gets NULLs — not a crash, not
garbage.** It reads as "this row has no body," not "you misread the shape."

That is the taxonomy's worst case: a wrong answer wearing a plausible one's
clothes. It argues for opt-in plus an explicit discriminator, and against
anything a client could get subtly wrong while appearing to work.

### Rejected: the denormalized join

Repeating parent columns on every child row is shape-stable, streams, and needs
no API change at all — the classic answer, and worth stating why it is not ours:
**two sibling nested tables cross-product.** Posts with comments *and* tags
yields |comments| × |tags| rows per post. That explosion is the reason segments
exist; adopting denormalization would re-introduce exactly what the feature was
built to remove. It remains reasonable for the single-nested-table case, and is
not worth a second code path.

### RESOLVED 2026-08-05 — `WITH COUNTS` does not fight interleaving

**Settled before proceeding, at Sean's direction: we are past the point where
iterating on a wire format is cheap, so this gets decided rather than tried.**

**The napkin invented a problem that does not exist.** It assumed the count
must be derived from the child stream, so that emitting the parent first would
force the driver to drain that parent's children into memory.

`procAddCountColumn` does nothing of the kind. It duplicates the child SELECT,
strips its ORDER BY, replaces the projection with `count(*)`, and ANDs in
`child_key = sqlite_proc_parent.parent_key` — **a correlated scalar subquery on
the parent's own wrapped SELECT.** The count is computed by the parent's query
and never touches the child coroutine.

**Proven by an existing passing test, not by reading the codegen.** `proc4c`'s
`counts` helper steps a `WITH COUNTS` call through **segment 0 only** and reads
`sqlite3_proc_child_count()` on each parent row, never calling
`sqlite3_proc_next_resultset()`. 13 tests, 0 errors. If the count depended on
the child stream those numbers could not be right, so the independence is
already pinned by the suite that shipped.

**So under interleaving the count is strictly better than it is today.** It
arrives *before* the children rather than alongside them, which means an aware
client can pre-size its collection *and* verify at the group boundary as the
children end. No buffering, no trailing marker, no mode exclusion, and no
change to `procAddCountColumn`.

**Two corrections to the napkin, recorded rather than quietly dropped:**

- The buffering it proposed is unnecessary.
- It called that buffering "bounded (max fan-out, not data size)", which was
  misleading even on its own terms. For skewed data the largest fan-out
  approaches the dataset — one hot parent with a million children *is* the
  whole table — so "bounded by max fan-out" is not a memory guarantee. Had the
  premise been right, the fix would have been worse than advertised.

**The honest limit is unchanged and inherited from POC 3.** The count and the
rows still derive from the *same predicate*, so they cannot disagree about what
the predicate means. This catches rows lost in transit, not a wrong correlation.
Interleaving lengthens "transit" slightly — two independent evaluations within
one snapshot rather than one stream — but does not turn the check into an
independent oracle, and nothing here should be read as claiming it does.

### Open, and honestly unresolved

- **`sqlite3_proc_next_resultset()` has no meaning** when there are no segment
  boundaries. **DECIDED: `SQLITE_MISUSE` in interleaved mode.** Returning
  `SQLITE_DONE` would be indistinguishable from "no further set", which is the
  answer a correct client gets today — so it would let a client that wrongly
  believes it is in segmented mode run to completion silently. Misuse is the
  only return that cannot be mistaken for success.
- **The ordering requirement returns.** A merging design *does* need each level
  ordered by its parent key — the ancestor-path constraint retracted earlier
  today. The retraction is correct for the shipped protocol and does not
  transfer to this one. Any depth-N work must not inherit the retraction blindly.

### MEASURED 2026-08-06 — the two-coroutine merge already runs inside a proc

**Risk 1 is retired, and it cost no code.** A compound `SELECT ... UNION ALL
... ORDER BY` in a proc body routes through `multiSelectOrderBy`, which is
precisely the two-coroutine key merge the interleaving driver needs.

```
1  Program           1  2  1  program   Call: merged
2      InitCoroutine 1  17 3            SUBR: next-A
14       Yield       1  0  0
17     InitCoroutine 2  52 18           SUBR: next-B
29       Yield       2  0  0
32     Noop                              SUBR: out-A
35   Noop                                SUBR: out-B
```

Two concurrent coroutines with independent program counters, both **inside the
`Program` frame**, each with its own output subroutine.

**The output is the positive control, not the opcodes.** With `t1 = 1,3,5` and
`t2 = 2,4,6`, a real merge yields `1,2,3,4,5,6` and mere concatenation yields
`1,3,5,2,4,6`. Observed: `1,2,3,4,5,6`. The opcodes prove the mechanism was
compiled; the interleaved data proves it *ran and merged*.

So every mechanical premise of interleaving is now demonstrated in this fork:
resumable independent PCs inside a SubProgram frame, two of them concurrent, and
a key-comparison merge driving alternating output from separately-ordered
sources. What remains is wiring proc's own parent/child statements into that
shape — not discovering whether the shape exists.

*Instrument note: a naive `grep -c InitCoroutine` returned 3 against a program
containing 2, because the probe's own `.print` banner contained the word. The
opcode-only count is 2. Counting matches of a term that appears in your own
labels inflates toward the answer you are hoping for.*

### Risks 2 and 3 — MEASURED 2026-08-06, `test/procinter.test`

Promoted from the reasoning below to evidence. Eight tests, no leaks.

A shape with **two sibling nested tables of different widths** gives three
segments of arity **4 → 2 → 3**, so the widest-set name array is exercised
narrowing *and* widening rather than only growing.

- **Risk 2 — repeated `ApplyProcSet` holds.** Arity and every column name are
  correct at each segment across both transitions, and two independent walks of
  the same statement return byte-identical metadata (1.2) — a reused buffer or
  a stale pointer would drift on the second pass. No leaks. Per-*row* remains
  untested, but per-row differs from per-segment only in frequency: the call
  does not allocate.
- **Risk 3 — out-of-range is empty, not stale.** On segment 1 (2 columns),
  index 3 — valid on segment 0 — yields empty rather than the segment-0 value
  that occupied that register. Better than feared: registers are not leaking
  the wider segment's data. Still **silent**, since nothing surfaces to a client
  that does not check `sqlite3_errcode()`. 2.1 is the positive control: the same
  accessor returns non-empty at indices the segment does have, on the same row,
  so 2.0 cannot pass merely because the accessor is broken or no row was
  produced.

**The draft of that test demonstrated the hazard it was written to pin.** It
asked for the width *after* `sqlite3_finalize()` and got **4** — the previous
segment's width, read out of freed memory. Not a crash, and a number plausible
enough to read as an answer. The corrected version reads everything before
finalize and says so in a comment, because the next person to edit it will be
tempted the same way.

### Superseded reasoning, kept for the record — source-only, NOT measured

Recorded at a lower confidence than the above, and labelled so no one later
mistakes reading for evidence.

- **Per-row `ApplyProcSet` looks safe by construction.** It assigns
  `nResColumn`/`nHiddenCol` and copies names into an array *deliberately sized
  for the widest set so advancing never reallocates*. No allocation on the path,
  so calling it per row rather than per boundary should differ only in
  frequency. Wanted: a measured run, since "written for mid-statement change"
  was not "written for per-row change".
- **Cached `sqlite3_column_name()` pointers go stale.** Names are copied with
  `SQLITE_TRANSIENT` into statement-owned memory that `ApplyProcSet` overwrites.
  This is already true at segment boundaries today; interleaving makes it true
  per row, which stretches the documented contract (valid until the statement is
  finalized or automatically reprepared). Mitigated by the mode being opt-in,
  but generated clients must read names per row or cache per segment index.

### IMPLEMENTED 2026-08-06 -- the rewrite, and what its tests caught

`CALL p() WITH INTERLEAVED` works: one discriminated result set (`_segment,
_key, payload...`), engine-merged in a single pass, `procinter` 6.x pinning
output, names, tie order, author within-group ORDER BY survival, and every
refusal.  procfault grew 1064 -> 1442 walking the new constructors under OOM.

Three bugs found by the tests before the commit, in escalating severity:
a constructed ORDER BY ordinal not recognized by the compound resolver (the
phase-3 constructed-token lesson, third occurrence); a pProj double free in
the composition refusal (owner is CallProcProject -- crashed as a MISSING
summary line, not an error); and one SHIPPED bug latent since 08-05: the
author-ORDER-BY merge path in procLowerChild returned before procRecordFold
and before the LIMIT/compound guards, so an author-ordered child had no fold
recipe at all -- phantom column on the flat path, counts wrongly refused,
advisory blind, LIMIT unrefused.  The fold path had simply never been tested
with an author-ordered child.

Still open here: COUNTS+INTERLEAVED and RETURNING+INTERLEAVED compositions
(refused, not built); the zebra-sprocket seam upgrade (its _fetch_segments can
become one WITH INTERLEAVED query with no client change); and the CALL-vs-
EXPLAIN error-surfacing asymmetry observed on the pre-fix ordinal -- the same
resolution failure was loud under EXPLAIN and silent at CALL, which deserves
an entry of its own if it reproduces on a deliberately broken input.

### CLOSED 2026-08-08 -- RETURNING x INTERLEAVED is refused BY DESIGN, permanently

RETURNING exists to decline fold columns a flat client would otherwise pay
for.  INTERLEAVED computes no folds, so there is nothing to project away:
the composition has no purpose, and building it would be surface without
meaning.  The refusal message stands as the permanent behavior.  (bugbook
BUG-5, closed wontfix.)  COUNTS x INTERLEAVED remains genuinely buildable --
the count is a parent-arm column and would arrive before the children -- and
waits for a consumer to exist (bugbook BUG-4).

### The implementation fork, probed 2026-08-06 — rewrite vs driver

Planning the codegen exposed two materially different implementations, so the
choice is recorded here and put to Sean rather than made silently.

**A. The tree rewrite.** `WITH INTERLEAVED` compiles the shape into a compound
`UNION ALL` ordered by the correlation key -- one result set, uniform
NULL-padded arity, a leading `_segment` column:

```sql
SELECT id, 0 AS _segment, id, title FROM (parent...)
UNION ALL
SELECT post_id, 1, post_id, cid FROM (child...)
ORDER BY 1
```

Probed against a live fixture: the output IS the interleaved stream (P1, its
children, P2, ..., childless parent present), the planner chooses
`MERGE (UNION ALL)` -- two coroutines, parent by ordered scan, child by its key
index, single pass -- and with the key as the only ORDER BY term there is NO
temp b-tree: ties between arms favor the left arm, so a parent precedes its
own children for free.  (Left-arm-on-ties is implementation behavior, not
contract; building on it means pinning it with a test in this fork.)

What A dissolves: every per-row metadata risk measured above -- one result set
means no per-row ApplyProcSet, constant column_count, no stale name pointers,
and `next_resultset` returning DONE honestly rather than MISUSE by decree.
The zebra-sprocket seam keeps emitting real per-segment names by renaming
positionally (it can read PRAGMA proc_info at runtime), so generated clients
do not change.  Server economics are the segment economics: one pass, index
driven, streaming in bounded memory.

What A refuses: two nested tables that correlate on DIFFERENT parent columns
cannot share one merge order -- `KEY(a_id = a)` and `KEY(b_id = b)` have no
common sort.  Refused at CALL with a clear error; same-key siblings (both
fixtures, Mosaic) work.  And interleaved output is ordered by the correlation
key, not the author's parent order -- a real, documented semantic of the
opt-in.

**B. The hand-written driver.** Parent coroutine; per parent row, run each
child SELECT correlated to the current key (the fold recipe minus the JSON),
bumping iProcSet per row.  Keeps per-segment metadata on the wire, arbitrary
sibling keys, and the author's parent order.  Costs: server-side N+1 -- the
economics this feature's own benchmark exists to beat -- plus all the per-row
metadata hazards return, plus substantially more new VDBE codegen.

**Recommendation: A.** The shared-parent-key restriction is honest and
refusable; B buys generality for the rarer shape at N+1 cost and more
machinery.  A also converges the layers: the seam's client shape becomes the
engine's native protocol.

### Measure before building

- Two body statements compiled with `SRT_Coroutine` and merged — the probe
  covered one coroutine, not two.
- Whether `ApplyProcSet` is safe to call per row. It is *already* written for
  mid-statement shape change (the name array is sized for the widest set so
  advancing never reallocates), which is encouraging, but per-row was not its
  design point.
- What `sqlite3_column_name()` returns mid-interleave for a client that cached
  the pointer. `SQLITE_TRANSIENT` copies suggest cached pointers go stale.

---

## 3g. The depth-2 refusal does not prevent depth 2 — it makes it unguarded

**Found 2026-08-05 by trying it against Mosaic, the first schema not designed
for this feature.** Its model has a genuine depth-2 shape, stated in the schema:
*"`addresses` is an ARRAY: a floating interval is an interval with multiple
attested addresses."* So witness → intervals → addresses.

Declared the obvious way, the engine refuses:

```
Parse error: nested tables may not themselves nest: addresses
```

**But the author gets it anyway, in four lines**, by writing the inner level
with the same `json_group_array` the engine uses for the outer one:

```sql
SELECT i.wid, i.iid, i.book,
       (SELECT json_group_array(json_object('tradition', a.tradition, 'rng', a.rng))
          FROM address a WHERE a.iid = i.iid)
  FROM interval i;
```

One `CALL`, correct answer, readable declaration, properly nested JSON out.

**And every guarantee is gone.** This is the uncomfortable part, and it is worse
than the refusal being galling — it is *seductive*:

| | declared nesting | hand-rolled inner level |
|---|---|---|
| correlation checked at CREATE | yes | **no** |
| ordering imposed by the engine | yes | **author writes it, or omits it** |
| conformance checked against the body | yes | **no** |
| `WITH COUNTS` integrity | yes | **no** |
| introspectable by `procgen` | yes | **no — it is a `TEXT` column** |
| reachable on the segment path | yes | **no — fold only** |

A typo in `a.iid = i.iid` silently returns wrong groupings. **That is exactly
the class of failure this feature exists to prevent, reintroduced by hand one
level down, with no signal that the safety net has ended.**

**So the refusal is not protecting anyone.** It redirects depth 2 into a form
that looks identical from outside and is unguarded. Authors will take that route
because it is four lines and it works.

**Options, none obviously right:**

- **Support depth ≥ 2 properly.** The recorded blockers stand: ordering by the
  full ancestor path, and every ancestor key must be exposed — a `replies` table
  holding only `comment_id` cannot correlate to a post. Real work, not a syntax
  fix.
  **— RETRACTED 2026-08-05. Those blockers describe a protocol this code does
  not implement.** `procLowerChild` orders each child segment by *its own key
  only*; the parent segment is not ordered at all. Reassembly is therefore
  already key-based grouping rather than a lockstep merge, so a grandchild
  segment ordered by its own key attaches to children by the same rule children
  attach to parents — no ancestor path, and `comment_id` alone is sufficient
  because a reply correlates to a *comment*, never to a post. The wire format is
  untouched as well: `VdbeProcSet.nHidden` is per-segment, so a child row
  carries counts for its own nested tables and `sqlite3_proc_child_count()`
  reads them unchanged. The real cost is recursion in four one-level walks
  (`sqlite3ProcSegmentCount`, `procBuildEmits`, `procApplyFolds`,
  `sqlite3VdbeSetProcShapes`). Sean opened this option 2026-08-05; it is the
  chosen direction, scheduled after phase 7.
- **Notice and advise.** The engine already inspects child `SELECT`s (phase 3,
  phase 6). A `json_group_array` in one is detectable, and an advisory —
  *"this column nests by hand; its correlation is not checked"* — costs little
  and removes the silence. Cheapest honest option.
- **Refuse harder.** Reject hand-rolled aggregates in a child `SELECT`. Wrong:
  it forbids legitimate aggregation and would not be enforceable anyway.

**IMPLEMENTED 2026-08-05 — the advisory.** A `json_group_array` anywhere in a
child `SELECT`, including inside a correlated subquery, is detected by an
expression walk at CREATE and logged at prepare on the same channel as the R7
index advisory:

```
SQLITE_WARNING: procedure handrolled: nested table kids builds a further level
by hand; that inner correlation is not checked, not ordered by the engine, and
not covered by WITH COUNTS
```

Both directions pinned in `proc6adv` 4.1/4.2 — it fires for a hand-rolled level
and stays silent for an ordinary nested table, without which it would be a
function that always logs rather than a detector. Never an error; 4.3 asserts
the call still returns its rows.

**Original recommendation, for the record:** The value of the declared form is that it
cannot be got wrong; the value of the hand-rolled form is that it exists. What
the author is currently denied is *knowing which one they are in*.

**It was hit a second time the same afternoon, by a route this entry did not
anticipate.** Above, depth 2 was found in a schema *designed* that way. Hours
later the same wall was hit by *schema evolution*: widening one scalar field
into a set turned `witness → claims` into `witness → claims → confidences`
without anyone intending a nested shape at all. That episode is written up as a
narrative in `C:\Projects\mosaic\sprocket\CASE-STUDY-schema-change.md`.

The lesson worth carrying back here: **the population that hits this limit is
larger than "schemas with a genuinely nested model."** It includes every schema
that later widens a scalar into a collection — a routine, well-motivated change
whose depth cost is invisible at decision time. That argues the first option
above (support depth ≥ 2 properly) deserves more weight than it was given.

---

## 3f. The segment total — the one check per-parent counts cannot make

### ✅ IMPLEMENTED 2026-08-12

As proposed below, with the sibling call: `WITH COUNTS` now also carries a
per-nested-table TOTAL — the child SELECT's row count WITHOUT the
correlation, an uncorrelated scalar the optimizer evaluates once — in the
hidden layout `[counts..., totals...]`, read by `sqlite3_proc_child_total()`.
`procnull` 1.4 pins the orphan detection on the parent row (count 2,
total 3, before the child segment is ever walked), 1.6 pins quiet on clean
data, 1.5 pins the -1 legs; `proc4c` 5.x pins per-table indexing of both
halves on a two-table shape and that the visible surface does not move.

**Found 2026-08-05 by adversarial review, pinned in `test/procnull.test`.**

A child row whose correlation key is **NULL** attaches to no parent. SQL equality
with NULL is NULL, not true, so the row is absent from the fold, absent from the
segment grouping, and absent from every count. Measured:

| | |
|---|---|
| child rows in the table | **3** |
| rows appearing under a parent | **2** |
| `sqlite3_proc_child_count()` for that parent | **2** — agrees with the fold |
| sum of per-parent counts vs. child segment row count | **2 vs 3** |

**`WITH COUNTS` is structurally blind to this**, and for the reason POC 3
already recorded: the count derives from *the same predicate as the data*, so
the two cannot disagree. Per-parent cardinality catches rows lost **in
transit**. It cannot catch rows that never belonged to a parent at all.

**The check that does catch it is a total** — sum of per-parent counts against
the number of rows the child segment actually produced. That comparison crosses
the predicate: one side is grouped by the correlation, the other is not.

**Proposal:** `WITH COUNTS` also carries a per-segment total, exposed as
`sqlite3_proc_child_count(stmt, -1)` or a sibling call, so a reassembler can
assert `sum(parents) == total` without counting rows itself.

**Not a bug in the fold.** A NULL key genuinely correlates to nothing, and
inventing a parent for it would be worse. This is about *detectability*: today
the loss is silent, and it should be loud enough that a client can choose.

**Also worth documenting either way:** authors should consider `NOT NULL` on a
correlation column, and the R7 index advisory is the natural place to mention it
— it already inspects the correlation.

---

## 3e. Declared idempotency — *the engine knows; nobody can ask*

### ✅ IMPLEMENTED 2026-08-12 — `writes` on `PRAGMA proc_list`

Derived by walking the body's TriggerStep list (`sqlite3ProcBodyWrites`):
INSERT/UPDATE/DELETE and any unknown step kind report writes; SELECT
(incl. INTO), DECLARE/SET/LEAVE/RETURN/RAISE do not; IF/WHILE recurse;
CALL resolves the callee and walks it, with an unresolvable callee, a
visited-set overflow, or any future step kind all counting as writes —
conservative in exactly the direction the entry below argues.  Computed
fresh per pragma row rather than stored, so it cannot go stale against
the schema it describes.  Recursion (self or mutual) is not misread as a
write.  `proc4` 7.x pins both directions.  Deliberately NOT named
idempotent, per the honest limit below.

**Raised 2026-08-05 while checking whether Tack's design accounted for the wire
transport. It does (`ZEBRA_ORM_ARCHITECTURE.md` §14.6) — but it predates this,
and getting it wrong duplicates writes.**

**The problem.** The transport makes the request a `CALL`. Every RPC client that
has ever existed eventually asks: *the connection dropped before I saw a
response — may I resend?* For a read, yes. For a write, resending may apply it
twice. A client cannot tell which a procedure is, so it must either never retry
(and surface every blip as a failure) or always retry (and risk duplicates).

**The engine already knows.** `procCachePopulate` sets `PROCCACHE_WRITES` from
`DbMaskTest(pParse->writeMask, iDb)`. It is computed, stored, and unreachable —
`PRAGMA proc_list` exposes `name, nparams, nresultsets, declared, security` and
nothing about whether the body writes.

**Proposal.** A `writes` column on `proc_list`, so a generated client learns at
build time whether a `CALL` is retry-safe, and a remote binding can retry reads
automatically while refusing to retry writes.

**Derive it statically, not from the cache.** `PROCCACHE_WRITES` is set when the
body is *compiled*, and `proc_list` reads the schema-resident `Proc`, which has
never been compiled. Walking the body's `TriggerStep` list for `TK_INSERT` /
`TK_UPDATE` / `TK_DELETE` (and for a `CALL` of a procedure that writes) answers
it at CREATE time, where the answer belongs. The conformance walk already
traverses exactly that list.

**Be conservative in the right direction.** Unknown must mean *writes*. A
procedure wrongly marked read-only produces duplicate writes on a retry — silent,
data-corrupting, and discovered late. One wrongly marked as writing produces an
unnecessary error on a dropped connection. Misses are recoverable; false
assurances are not.

**Honest limit.** "Does not write" is not the same as "idempotent." A read-only
procedure calling a non-deterministic function, or one whose result feeds a
later decision, is safe to *resend* but not necessarily safe to *treat as
unchanged*. This buys retry-safety for reads, not general idempotency, and the
column should be named `writes` rather than `idempotent` so it cannot be
over-read.

---

## 4. Incremental view maintenance — *the big one, and the one I most want*

### Design questions to settle BEFORE code (drafted 2026-08-12, for Sean)

1. **Eager or deferred?** (all six expanded with scenarios in DESIGN-IVM.md) Maintain on every write inside the writer's
   transaction (reads always fresh, writers pay), or mark-dirty and fold in
   on read/checkpoint (writers cheap, first read pays)? The fork's
   read-heavy consumers (Graze, Mosaic) argue deferred; the equality
   contract is easier to state eager.
2. **The maintainable subset, drawn where?** SUM/COUNT/AVG have inverses;
   MIN/MAX under deletion need the base rows; DISTINCT needs counts; joins
   need which-side-changed bookkeeping. Propose: v1 = single-table
   GROUP BY with invertible aggregates + refuse the rest BY NAME at CREATE
   (the UNGIT shape), v2 = inner joins. Is MIN/MAX worth its cost tier?
3. **Delta capture: triggers underneath, or a write-path hook?** Hidden
   auto-triggers reuse shipped machinery but make the view's cost visible
   in trigger-land; a pager/vdbe hook is cleaner and far more invasive.
4. **Where does the materialization live?** A real shadow table with a
   rootpage (checkpointable, crash story = WAL's) vs in-memory with
   rebuild-on-open (simpler, cold-start cost).
5. **The correctness contract as a PRAGMA?** `PRAGMA view_check(name)`
   recomputing from scratch and diffing — the equality oracle as a
   first-class surface from day one, not a test-only harness.
6. **Interaction with procedures:** may a proc body write a table that
   feeds a maintained view (trigger-depth interactions), and may a proc
   SELECT from one mid-transaction under eager maintenance?

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

**DONE 2026-08-12.** Sean ruled all six questions (DESIGN-IVM.md, RULINGS,
including the mid-campaign hidden-columns ruling); the campaign executed as
`PLAN-IVM.md` P1–P4, one commit per phase (ab17af45, 7547f60c, 8048a13d,
b5ca293b), and `test/ivm1.test` — the spec, committed red on purpose — is
green whole, 0 of 24.  The syntax that shipped is `CREATE MATERIALIZED VIEW
… [WITH MAINTENANCE EAGER|DEFERRED]`.  Everything this entry asked for
exists: the enforced subset refusing by name (Tier 1; HAVING moved to
Tier 2 by dated amendment), the equality oracle as a first-class surface
(`PRAGMA view_check`, diff rows + mandatory coverage summary, stale-first
on deferred), and the storm test folding to oracle-clean.  User docs:
`README-IVM.md`, including the Q3 proc-cache tradeoff in its own loud
section.  Deliberately not in v1 (each refused by name, ranked for the
follow-on): MIN/MAX with the index advisory, inner joins, DISTINCT,
HAVING, TEMP-schema views, read-side folding (never).

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

**Pre-registered position, 2026-08-13** (design conversation with Sean;
sketch at DESIGN-NETWORK.md §1a): the group-commit write queue likely
makes BC permanently unnecessary for this fork.  Three legs: (1) BC
isolates poorly — its page-read tracking is cross-cutting through
btree/pager/wal with no narrow seam, and both BC and wal2 modify
`wal.c`, so carrying the pair means hand-owning a merge upstream
abandoned in 2019; (2) eager IVM write-amplifies onto shared view and
index pages, turning BC's page-level optimism into a retry storm on
exactly this fork's databases; (3) the queue delivers write batching
with zero engine change, and BC only pays when multiple OS processes
need long concurrent write transactions on one file — a demand no
current consumer has.  Decision stays open until Phase 5, but the
default is now NO unless that demand materializes.

## 6. System-versioned temporal tables — *SQLite has no story here at all*

**✅ BUILT 2026-08-15 — designed and shipped in two days, POC-first
(DESIGN-TEMPORAL: eight rulings, Sean; PLAN-TEMPORAL: seven phases).**
The done-means below is met in full: `FOR SYSTEM_TIME AS OF` (commit
sequence exact; UTC text resolved through the commit log to
last-at-or-before), automatic capture at commit via synthesized
internal triggers under a C-side reserved sequence (POC 1's two
clock-borne diseases pinned ABSENT by passing tests), retention via
`sqlite_history_prune` + a watermark whose far side REFUSES, and the
replay proof: per-commit from-scratch workload replay equals AS OF at
every checkpoint, with a corruption control kept red-capable forever
(temporal2).  Receipts: temporal1 0/32, temporal2 0/8, sweeps 44/44
both regimes, veryquick 0/393,987 release and 0/394,964 DEBUG.
Version skew inherited from #9 as designed — degrade's fixture
graduated from fiction to feature and its future was re-planted.
Along the way temporal EARNED two refinements elsewhere: dead objects
now unregister their half-built corpses (a gencol1 leg had been
passing by writing through one), and ALTER's splice arithmetic learned
non-"CREATE TABLE " heads.  Recorded v1 boundaries in
README-TEMPORAL.md; era-stamped history schema, BETWEEN, retention
policies, and temporal mviews are the named v2 shapes.

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

**EXAMINED AND CLOSED 2026-08-15, the revisit trigger having armed (#2
complete, four emitters).**  The absence is not awkward: a write-proc
returning values ends with a SELECT, and every generated client hands
back a typed one-row list — one indexing step from what OUT params
would give.  If that step ever bothers a real consumer, the remedy is
EMITTER SUGAR (single-row single-column shapes returning scalars),
not an engine feature: no grammar, no ABI, no wire change.  Reopens
only on a concrete consumer complaint.

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

## 9. Materialized views and the version-skew lockout -- *found by dogfood, 2026-08-13*

*(Filed as a second #6 by mistake on 2026-08-13 -- the temporal-tables
entry already held that number, which is fitting, because the two
entries turn out to be the same question; renumbered to 9 same day.)*

**The finding, exact:** a database containing ANY materialized view is
unreadable AT OPEN by any binary that does not understand the view's tier --
including stock SQLite, where `CREATE MATERIALIZED VIEW` is a syntax error and
schema load fails with "malformed database schema"; including the fork's OWN
older binaries, where a Tier-2 join view fails the conformance walk the same
way.  The lockout is whole-file: `SELECT count(*) FROM claim` on an untouched
table refuses, because the schema never finishes loading.  Found minutes into
the first Mosaic dogfood, when the CLI (one campaign stale) refused the
database the testfixture had just written.  The failure MESSAGE held up
honestly -- it named the view and the reason -- but the blast radius is every
table in the file.

**Why it matters more than a stale-binary slip:** version skew is the
DEPLOYMENT NORM here, not an accident.  Zebra vendors its own engine copy;
Graze ships whatever it pinned; a fork upgrade reaches binaries one at a time.
Under the current design, creating one materialized view in a shared database
flips that file to exactly-this-fork-version-or-nothing.

**Three routes (decision wanted before mviews touch any shared db):**

1. **Accept and declare.**  Fork-only file format, documented loudly
   (README-IVM sharp edges).  Cheapest; honest; matches FTS5's posture except
   that FTS5 degrades at QUERY time, not open.
2. **Degrade at load.**  Tolerate an unparseable/unconformant mview row:
   mark the object dead, load the rest of the schema, refuse queries that
   touch the dead view by name (and its maintenance stays off; view_check
   reports it).  Newer-fork files then open under older forks minus the new
   tier.  Stock sqlite still refuses (it cannot parse the DDL at all).
3. **Store as stock-readable.**  Schema row says `CREATE TABLE` (the visible
   columns; stock reads the materialization as a plain, silently-stale
   table), and the mview-ness -- definition text, maintenance mode -- lives
   in an `sqlite_ivm_meta` table the fork consults at load.  Full
   stock-readability of the DATA; the price is P1's elegant
   single-schema-row design, .dump semantics rework, and a stale-table
   hazard handed to stock readers with no staleness surface to consult.

**RULED 2026-08-13: Posture 2 (degrade-at-load), Sean.**  The deciding
argument was posture 3's conditional benefit: stock-readability is a
FILE property granted per-feature, and every deployment file already
carries procs -- the set of files that would actually gain stock access
is empty.  Degrade-at-load protects the fork's own lineage, which is
where the skew genuinely bites.  Campaign: PLAN-DEGRADE.md.

Lean, not a ruling: (2) is the engineering sweet spot (graceful within the
fork's own lineage, where the skew actually bites today); (3) is the one to
pick only if stock-readability of mview DATA is a real requirement for the
Zebra/Graze seam -- worth asking the consumers before paying for it.

**BUILT 2026-08-13, same day as the ruling** (PLAN-DEGRADE.md;
spec `test/degrade1.test`, 0/20).  As shipped: any object-level init
failure at FULL schema load dead-marks (name, type, retained reason)
instead of failing the file; `PRAGMA dead_list` is the surface; dead
names refuse with the reason and the fix; the file is read-only while
degraded (v1), gated at OP_Transaction per-database, with
`writable_schema=ON` as the declared repair exception.  Deaths cascade
honestly (a dead mview's key index dies with it, reasons chained).
Two first-contact corrections worth remembering: degrade must NOT
apply to the incremental re-parse after a live CREATE/ALTER (upstream
view-29 pins the immediate error, which is the better surface), and
the write gate had to move from compile-time to runtime because vtab
cookie bookkeeping books write intents that are not writes.  Recorded,
not built: the v2 degraded-write marker protocol; dead-object DROP.
Temporal tables (#6), when built, inherit this loader for free --
older fork builds will open files carrying them, minus the feature.

## 10. Replication — *shipping the fold* 

**✅ BUILT 2026-08-16 — R0 through R7 in one campaign (DESIGN-REPL:
five rulings, Sean; PLAN-REPL: eight phases), POC-first twice.**
Session changesets in commit order (Q1-C), cut into self-describing
segments (magic/version/genesis/seq-range/CRC) — the Q4-C ruling's
three UNGIT commitments each pinned by a check: self-description,
refuse-never-skip (gap, lineage, checksum, truncation, divergence,
missing schema — by name, inert, receipt retained), declared sources.
Surfaces: `tool/sprocket_repl.c` (writer/applier/encoder/PITR, 44
checks), `sprocketd --archive` + `SUBSCRIBE` (catch-up and live-tail
are ONE loop over the archive files; wire bytes == disk bytes,
byte-compared), `PRAGMA replica_status` (freshness never without its
basis; no-replication-state SAID, not silent), and
`--restore [--upto-seq | --upto-utc]` where the time target reads the
commit clock shipped inside the segments.  The crown: the TEMPORAL
axis replicates verbatim — shadow PKs, hidden-column capture,
apply-as-maintenance with the replica's own capture standing down —
and test/repl2.test aims temporal2's replay proof at a REPLICA,
corruption control kept, wired into the session permutation.  Found
on the way and refused at the door: PK-less temporal tables fabricated
history via rowid coincidence; a temporal table now requires a
PRIMARY KEY.  Receipts: veryquick 0/394,007 release + 0/394,984 debug,
session suite 0/19,256 Windows + 0/17,893 Linux, daemon 57/57.
Recorded, not built (v2): promotion/failover, DDL replication,
engine-resident writer, compression, browser/HTTP delivery.

## Radar (not work items yet)

**Showcase surface (Sean, 2026-08-16):** think about how Mosaic can
showcase the fork's features — or whether another app is the right
vehicle. The bar is NATURAL use, not a simplified demo: procs/shapes
serving real request patterns, temporal answering a question a reader
actually asks (what did this text/annotation look like when I cited
it?), replication backing a real read-replica or PITR story, the
daemon fronting a real workload. Candidate fits to explore when taken
up: Mosaic's annotation/apparatus layers are naturally versioned
(temporal), its reading surfaces are naturally read-replicas
(replication), and its API shapes are naturally nested (procs +
TSGEN). Prerequisite honesty: sprocketd is Windows-only today; a real
deployment story likely needs the Linux socket shim first.
