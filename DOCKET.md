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

**Status: designed. Three POCs and eight rulings; implementation planned in
`PLAN-NESTED.md` (seven phases, tests written first). Not yet built.**

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

**Open question the syntax raises:** `sqlite3_column_count` now varies with the
projection. That is legal — it is per-statement — but `procgen` must emit
accessors per projection rather than per procedure, which is a change to its
output model and should be settled before the emitter is written.

---

## 3d. `mayAbort` assertion in stored procedures — *a real bug, inherited*

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
