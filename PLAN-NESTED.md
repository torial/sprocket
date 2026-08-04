# PLAN: nested result shapes

*Written 2026-08-03, after three POCs and eight rulings. Committed so it
survives context loss — this file plus `DOCKET.md` §3 is the whole briefing.*

## Why there is a plan rather than a commit

The design is settled; the feature is not startable in pieces. Grammar without
the runtime would let `CREATE PROCEDURE` accept a nested declaration that
`CALL` then fails to honour — a shape-declaring feature that lies about the
shape. So it lands as a sequence where **every phase leaves the tree green and
the feature either absent or complete**, never half-present.

## What was decided, and by what evidence

| | |
|---|---|
| Feature earns its place on **type fidelity**, not bytes | POC 1: JSON fold is *smaller*, but `json_group_array` **refuses** BLOBs |
| Value of declaring the correlation is **detection** | POC 2: a wrong `ORDER BY` silently returns **2%** of rows and reports success |
| Cardinality travels **per-parent** | POC 3: total-only misses unbalanced misattribution; constant false-alarms on real data |
| Scope is **transport integrity, not query correctness** | POC 3: a check drawn from the same source as the data cannot validate it |
| Syntax **A2**; growth path E | costs **zero** new keywords — `TABLE` and `KEY` already exist |
| Body form **1½** — engine imposes the ordering | authors cannot mis-order what they do not write |
| Engine declares; **procgen reassembles** | R5 |

```sql
CREATE PROCEDURE post_with_comments(pid INTEGER)
  RETURNS TABLE(
    id INTEGER,
    title TEXT,
    comments TABLE(post_id INTEGER, cid INTEGER, body TEXT) KEY(post_id = id)
  )
BEGIN
  SELECT id, title FROM posts WHERE id = pid;
  SELECT post_id, cid, body FROM comments WHERE post_id = pid;
END;
```

*Corrected 2026-08-03, during phase 2.* The draft above declared the child as
`(cid, body)` while its `SELECT` emitted three columns. **The correlation
column is part of the child's declared shape**, because it is a column the
child result set actually carries on the wire and the declaration may not hide
what the wire carries. Both halves of `KEY` are now checked at CREATE: the
child half against the nested column list (phase 1), the parent half against
the scalar columns of the shape that contains it (phase 2).

## The invariant, restated as a test

> **An unmodified client still works.** `sqlite3_column_count()` reports 3;
> the third column is readable as text (JSON, with BLOBs base64-encoded);
> nothing that compiles today behaves differently.

**Every phase below re-runs this against the stock `sqlite3` CLI**, which is
the client we cannot lie to. If it ever needs a patched shell to pass, the
phase is wrong.

## Standing rules

The five instrument rules from `PLAN-TRANSPORT.md` apply unchanged, plus the
sharper form phase 5 of that plan produced:

> A count is no better than a stopwatch when the count is itself
> timing-dependent.

Build recipe, PATH requirements and the stale-binary trap: see
`PLAN-TRANSPORT.md`. Build **both** `sqlite3.exe` and `testfixture.exe`.

---

## Phase 1 — grammar and storage — **DONE** (`nested-shapes`)

**Deliverable.** Parse `name TABLE(cols) KEY(child = parent)` inside
`RETURNS TABLE(...)`. Store the nested shape and the correlation pair on the
`Proc`. Persist by the existing route — the CREATE text lives in
`sqlite_schema` and is re-parsed on open, so persistence is free if the
grammar round-trips.

**Test, written first.**
- `CREATE` then reopen; the nesting and both correlation columns survive.
- Malformed clauses are rejected with pointed errors: missing `KEY`, a key
  naming a column that is not in the child list, a nested table inside a
  nested table (depth 2 is out of scope — reject it explicitly rather than
  accepting it and behaving oddly).
- **Non-reserved check:** a table named `key` with a column named `table`
  still works. The fork's other keywords carry this guarantee and this one
  must too.

**How this test can fail.** Feed a declaration whose `KEY` names a
non-existent column; it must be an error at CREATE, not at CALL.

---

## Phase 2 — conformance — **DONE** (`nested-shapes`)

**How it landed.** The declared shapes are flattened, once, into the sequence
of result sets the body must emit: a shape with *k* nested tables expands to
the parent followed by *k* children, in declaration order. The control-flow
walk then reasons about a single integer cursor exactly as it did before
nesting existed — `IF` branch symmetry, loops and `RETURN` needed no change —
and a procedure that nests nothing produces the same array the checker used
before, so its diagnostics are unchanged to the byte. `test/proc6.test`
section 5 pins that.

Two rules the plan had not anticipated, both found by writing the flattening:

- **The parent half of `KEY` must name a scalar column of its own shape.**
  This cannot be checked in the grammar action, which sees only the columns
  declared *before* the nested one; by conformance time the shape is complete,
  so `RETURNS TABLE(comments TABLE(...) KEY(post_id = id), id INTEGER)` is
  accepted (proc6-3.2).
- **A shape may not consist only of nested tables** — it would have no column
  to correlate on. Reported before the key check, which would otherwise blame
  the missing parent column and send the author after the wrong mistake.

**`CALL` is refused, and this was measured rather than assumed.** Phase 1's
arity check had been the only thing making a nested `CALL` unreachable; phase 2
removed it. With the guard absent, `CALL pwc(1)` returned `1 first 0.0` —
`sqlite3_column_count` reported the **3 declared** columns while the parent
`SELECT` wrote **2** result registers, so the third column read a register the
body never assigned and handed the client a fabricated value.

Reporting the scalar count instead would be quieter and no more honest: it
drops the nested table with no indication it was declared, which is a
shape-declaring feature lying about the shape. So `CALL` of a nested procedure
errors at prepare until phase 5 can materialise the column. Introspection
(`PRAGMA proc_info`) still works, which is what lets `procgen` run ahead of the
runtime. `proc6` section 6 pins both the refusal and — as its control — that a
procedure declaring a shape and nesting nothing still calls.

### Where the fold happens — **decided: at the boundary API**

`PRAGMA proc_list.nresultsets` reports **declared shapes** (1 for `pwc`) while
the body emits **2** result sets and `sqlite3_proc_next_resultset()` advances
per emission. Two different quantities, one name — and the invariant wants the
flat client to see one set of three columns while `procgen` wants the segments
*unfolded*, to avoid paying for JSON it will throw away.

So the nested column is synthesized by the **statement API** for clients that
never call `next_resultset()`. The VDBE keeps streaming both segments.

**Decided on a measurement, not on taste.** The argument for folding inside the
VDBE is that column count 3 would then be structural, with no synthesized
column and no metadata/register mismatch — the mismatch that produced the
`0.0`. The argument against it is that folding is *inherently eager*: a parent
row cannot be delivered until every one of its children has been buffered, so
phase 5's laziness test would be false by construction and would have to be
weakened to match the design. That is the wrong direction — it trades the
sharpest instrument in this plan for a tidier implementation.

That argument only holds if the existing two-segment path really is lazy, so it
was measured rather than reasoned about (`proc6` section 7):

| | |
|---|---|
| step set 1 only, then finalize | **0** child rows scanned |
| advance to set 2 and drain *(control)* | 2 |
| advance to set 2, **one** step | **1** |

Lazy per **row**, not merely per segment. The control uses the same statement
text and the same UDF as the measurement, so the only variable is whether
`next_resultset()` was called — the zero cannot be a function the body never
invoked. A flat client also already stops at set 1 on its own, since
`sqlite3_step()` returns `SQLITE_DONE` at set end, so half the invariant works
today with no new code.

**And it was confirmed for the case it governs, not merely a similar one.**
Section 7 declares *two shapes*, so its set boundary could have been an
artifact of the declaration rather than of the body — while `pwc` declares
*one* shape whose child segment the declaration never mentions. If the step
loop ran past that boundary, a flat client would scan the children whether or
not it read column 3, and this rationale would not apply to nesting at all.
Measured with `procShapesNest` temporarily bypassed in a scratch build:

| | rows delivered | children scanned |
|---|---|---|
| `seg` — two declared shapes | 1 | 0 |
| `pwc` — one shape, nested table | 1 | 0 |

Identical. The set boundary is produced by the **body's** structure — each
top-level `SELECT` ends a set — not by the number of declared shapes, so
laziness carries over unchanged. This cannot be a permanent test while `CALL`
is refused; **phase 5 must re-pin it** as one the moment the guard is lifted.

**The cost this accepts, explicitly.** Metadata will say 3 while the result row
holds 2 — the same configuration that produced the fabricated value, now
deliberate and handled. Every reader (`sqlite3_column_*`, `sqlite3_data_count`,
the shell's printer, blob lifetimes) must agree about the synthesized index, so
phase 5 routes it through a *single* accessor rather than special-casing each
entry point. And reading the column means stepping the child segment mid-row,
then restoring set-1 metadata so a later `next_resultset()` still behaves.

**Consequence for phase 4.** The cardinality check now sits at a layer that did
not impose the ordering it validates — closer to POC 3's warning about a check
drawn from the same source as the data. Phase 4 must carry the count from where
the ordering was imposed rather than recomputing it at the boundary.

**Resolved:** `nresultsets` now counts **segments**, so it agrees with
`sqlite3_proc_next_resultset()` and is exactly the number of sets a client will
step through. Shapes and segments are equal for every procedure that does not
nest, so no existing value changed — which is why this was worth doing now
rather than after the feature ships.

**Left open, and it belongs to phase 7.** `PRAGMA proc_info`'s
`resultset_index` still enumerates *declared shapes*, and a nested table's own
columns are not listed anywhere. `procgen` cannot generate a typed child
accessor without them. Adding them raises a question worth deciding rather than
defaulting: whether the child's columns become their own `resultset_index`
(making `proc_info` count segments too, at the cost of an implicit
declaration-order link back to the parent column) or gain an explicit
parent-column field (a wider result, which the existing `proc4` cases pin).

### As originally planned

**Deliverable.** The body must emit the parent `SELECT` then the child
`SELECT`, checked at CREATE time against the declaration, on the existing
control-flow-graph machinery.

**Test, written first.** A body with the two SELECTs transposed is rejected;
one missing the child SELECT is rejected; one whose child SELECT has the wrong
arity is rejected. Each error names the offending statement.

**How this test can fail.** A conforming body must be *accepted* — the control
that stops this becoming a check that rejects everything.

---

## Blocker found on starting phase 5: segments cannot interleave

*Found 2026-08-03, before writing any phase 5 code.*

The boundary fold assumed the statement API could materialise column 3 for a
parent row by draining that parent's children on demand. **It cannot**, and the
reason is structural rather than subtle: the body is two statements, so the
child `SELECT`'s opcodes do not begin until the parent `SELECT`'s loop has
ended. The two streams are sequential in time, never live together. To serve
column 3 of parent row 1 the API would have to advance past every remaining
parent row — and there is no rewind.

This does not sink the decision to fold at the boundary, but it does mean the
fold needs a buffer somewhere, and where that buffer sits is a real choice:

1. **Buffer the parent segment, stream the children.** Parents are the small
   side (posts, not comments). With the ordering imposed, children arrive
   grouped, so at most one parent's children are held at a time. Memory is
   parents + one group. Laziness survives: untouched column 3 means the child
   segment is never stepped at all.
2. **Compile the child `SELECT` as a coroutine** (`OP_InitCoroutine`/`OP_Yield`)
   so the parent loop can pull child rows on demand. This is an *engine* fold —
   but a lazy one, because a coroutine only runs when yielded to, which is
   precisely the objection that ruled engine-fold out. Elegant, and a much
   larger change to `codeProcProgram`.
3. **Re-execute the child query per parent.** N+1 executions, simplest to write,
   changes the body's execution model and abandons the single-scan property.

**All three want the ordering first.** Option 1 is bounded only when children
arrive grouped; option 2 needs the merge to be monotonic; option 3 does not care
but is the weakest. So the swap recorded below is wrong in one respect: phase 3
is not merely testable-after-5, it is a *prerequisite* for 5 being bounded.

**Recommendation: land 3 and 5 as one unit, implemented as option 1.** The
ordering is imposed and the fold consumes it in the same change, which is also
the only arrangement in which phase 3's control is honest — an ordering nobody
consumes cannot be shown to matter.

Option 2 is the better long-term shape and should be revisited if the parent
side ever stops being small; recorded here rather than lost.

## Phase ordering: 3 and 5 must swap *(superseded by the above)*

*Found 2026-08-03, on starting phase 3.*

Phase 3's stated test is **reassembly of a known fixture** — which requires
`CALL`. Phase 2's guard refuses `CALL` of a nested procedure, so phase 3 is
not testable in the order written. The plan assumed `CALL` stayed available;
the guard, which was the right thing to add, coupled the two phases.

Three ways out, and only one of them is good:

- *Lift the guard for phase 3.* Reinstates the fabricated column. No.
- *Report only the value columns until phase 5.* `CALL` works, segments stream,
  and it is exactly the unfolded view `procgen` wants — but a stock CLI then
  sees a two-column result and no indication the nested table exists. A
  narrower lie than the fabricated value, still a lie.
- **Do phase 5 first, then phase 3.** Phase 5 makes `CALL` correct and the
  guard liftable: three columns, the third readable as JSON, invariant
  satisfied against the unpatched shell.

The swap is not merely tolerable, it is *better*, because of what it does to
phase 3's control. Phase 3's stated way-to-fail is:

> Disable the imposed ordering and the reassembly test must go red with the
> POC 2 signature — far fewer pairs than expected, **without** an error.

Run in this order, that control costs nothing and is not a simulation: the
state *before* phase 3 **is** the disabled-ordering state. So phase 5 writes
the reassembly test pinned to the **wrong** answer, with POC 2's signature
recorded in the expectation, and phase 3 flips it to the right one. The suite
stays green throughout, and the ordering's value is demonstrated by a test that
was observed failing for the real reason rather than by one that has only ever
been green.

The risk this accepts: phase 5 briefly lands a fold that mis-groups children.
It is on a branch, phase 3 immediately follows, and the mis-grouping is pinned
by a test rather than unknown.

## Phase 3 — the lowering — **DONE** (`nested-shapes`)

The engine attaches `ORDER BY <ordinal>` to every nested table's `SELECT`,
derived from the declared `KEY`. An **ordinal**, not a name: the declaration is
the interface, so a body's `SELECT` need not spell its columns the way the
shape does, and `ORDER BY post_id` would depend on a coincidence.

**Author-written `ORDER BY` on a child `SELECT` is refused**, not overridden.
Overriding discards their code silently, which is the failure class this phase
exists to remove. *Merging* — prepending the correlation term so
`ORDER BY created_at` sorts by date within a parent — serves a real need and is
the documented growth path; refuse-to-merge breaks nothing later, merge-to-
refuse would break bodies. `LIMIT` is likewise refused: its meaning per parent
row is undefined, and an imposed sort silently changes which rows it keeps.

**The bug worth recording.** The lowering was first placed inside the
conformance check, which runs only when `!db->init.busy`. But a real
`CREATE PROCEDURE` builds a `Proc`, writes the schema row, and then *re-parses*
it — so the object checked at DDL time is discarded, and the one that answers
`CALL` is built by the reparse with `init.busy` set. The lowering was being
applied only to the copy that gets thrown away. Skipping the pass on that path
was sound while it only *validated*; once it also *rewrites*, it has to run
wherever the body is materialised. Renamed `procCheckAndLower` so the calling
contract is visible at the call site.

**How it was measured, since `CALL` is still refused.** `EXPLAIN QUERY PLAN`
does not descend into a procedure's `SubProgram` — it returns empty for every
`CALL`, so both-empty proves nothing and that instrument was discarded rather
than believed. The `bytecode()` virtual table does see inside, counting
`Sorter%` opcodes:

| | sorter opcodes |
|---|---|
| two shapes, no nesting *(control: what "no sort" looks like)* | 0 |
| two shapes, **hand-written** `ORDER BY 1` *(control: instrument can see)* | 5 |
| one shape with a nested table, **lowered** | 5 |

Before the fix the lowered body read **0** — the same number as the no-sort
control, which is how the bug surfaced. Only the rejections could be pinned
permanently: `bytecode('CALL ...')` must prepare the statement, which the phase
2 guard refuses, so the three rows above become permanent tests when phase 5
lifts it.

### As originally planned

**Deliverable.** The engine adds the `ORDER BY` to the child query itself,
derived from the declared `KEY`. **The author never writes it.**

This is the phase that pays for the whole design: POC 2's silent 98% loss
required the author to write the ordering and get it wrong, and here they do
not write it at all.

**Test, written first.**
- The child query's plan is ordered by the correlation column
  (`EXPLAIN QUERY PLAN`, or `sqlite3WhereIsOrdered()` internally).
- A body whose child SELECT carries a *contradictory* `ORDER BY` is either
  overridden or rejected — decide which, and pin it.
- Reassembly of a known parent/child fixture returns every child under the
  right parent.

**How this test can fail.** Disable the imposed ordering and the reassembly
test must go red with the POC 2 signature — far fewer pairs than expected,
*without* an error.

---

## Status, 2026-08-04

| | |
|---|---|
| 1 grammar and storage | **done** |
| 2 conformance | **done** |
| 3 the lowering | **done** |
| 5a segment reachability | **done** |
| 5b flat-client column | **done** |
| DOCKET 3c projection | **done** |
| **4 per-parent cardinality** | **not started** |
| **6 index advisory (R7)** | **not started** |
| **7 generated reassembler** | **not started** |

Branch `nested-shapes`, unmerged. Merging before 7 would ship a feature whose
typed client does not exist yet, which is the half-present state this plan
exists to avoid — so the merge waits on 7, and 4 and 6 are independent of it.

## Phase 4 — per-parent cardinality

**Deliverable.** Each parent carries the number of children belonging to it.

**Test, written first.** POC 3's matrix, re-run against the real engine rather
than a simulation: children lost, children duplicated, one child misattributed
— all detected and localised to the offending parent. Legitimate variable
cardinality stays quiet.

**How this test can fail.** The control from POC 3: with correct data every
check must be silent. A check that never goes quiet is an alarm.

**Record in the docs, not just the code:** this catches transport integrity,
not query correctness. A logic error inside the body produces consistent counts
and passes.

---

## Phase 5a — segment reachability — **DONE** (`nested-shapes`)

`sqlite3VdbeSetProcShapes` built one descriptor per **declared shape**, so a
shape holding a nested table left its child segment with no metadata and
`next_resultset()` returned `SQLITE_DONE` at the parent's end — the child rows
were *unreachable*, not merely unvisited. It now builds one descriptor per
**segment**: the parent carrying only its value columns, then one per nested
table in declaration order. This is the **unfolded** view, the one `procgen`'s
reassembler consumes; 5b layers the flat client's wide row on top.

That made phase 3 testable end to end, which immediately found phase 3 wrong.

### The phase 3 bug this exposed

The imposed `ORDER BY` was built as a constructed `TK_INTEGER` — the ordinal
form, `ORDER BY 1`. **A constructed integer is not resolved as an ordinal the
way a parsed one is.** It sorted by a *constant*: a no-op that still emits
sorter opcodes. Every static check agreed the body was ordered while rows came
back in scan order.

This is worth remembering as a class. The `bytecode()` instrument was *correct*
— five sorter opcodes really were present — and I read "sorter present" as
"ordering imposed", which is a different claim. The filtered opcode dump that
seemed to confirm the sort key made it worse: I selected eight opcode names,
saw `Column 1 1` reading `post_id` near the sorter, and read a partial listing
as though it were the program. The `Column` I was looking at was the projection,
not the key.

Fixed by sorting on a **copy of the projected expression** rather than an
ordinal, which cannot drift from what the column actually yields. A compound
child `SELECT` is now refused, since that term belongs to the leftmost arm and
is not legal for a compound.

Measured end to end — identical bodies, only the declaration differing:

| | child rows returned |
|---|---|
| two shapes, no nesting *(control)* | `2 1 2 1` — scan order |
| one shape with a nested table | **`1 1 2 2`** — grouped by the key |

**Guard lifted, and every measurement above is now a test.** The guard was
raised because a nested `CALL` fabricated a value from an unwritten register.
That is fixed by the per-segment metadata, so what remained was only the flat
client's missing column — incomplete rather than wrong, a materially weaker
objection than the one it was raised against. `proc6` grew from 38 cases to 46
and now pins the streaming contract, the ordering, and laziness against a
genuinely nested procedure rather than a two-shape stand-in.

**Phase 3's stated way-to-fail was run.** With the `pOrderBy` assignment
disabled, `proc6-8.4` — and 8.4 *alone* out of 46 — returns `2 1 2 1`: wrong
grouping, no error, POC 2's signature. Nothing else in the suite detects the
ordering, so that one case carries it. Observed red, not merely believed capable
of it.

## FIXED — the generated correlation could resolve inward

*Found 2026-08-04 by `test/prochostile.test` case 3.0, seen red, fixed the same
day. The fix is the wrapped parent described at the end of this section.*

Phase 5b generates `<child key expr> = <parent key expr>`, both copies of
projected expressions. **If the child's `FROM` can supply a name matching the
parent expression, the parent side resolves inward**, and the correlation
becomes a tautology: every child row matches every parent, silently, with no
error.

```sql
-- child and parent both read posts; "id = id" is trivially true
SELECT id FROM posts WHERE id = pid;
SELECT id, title FROM posts;          -- nested, KEY(id2 = id)
```
returns *every* post as a sibling of post 1.

**It is not limited to self-reference.** Any child whose `FROM` exposes a column
of the parent key's name collapses the same way — a `comments` table with its
own `id` would do it. The working cases work because the names happen not to
collide, which is not a property anyone chose.

**Why the technique bit here.** "Copy the projected expression" was the right
fix for phase 3's `ORDER BY`, where the copy stays in its own query. Copying an
expression *into a different scope* is a different operation: a duplicated
expression re-resolves, and re-resolution is exactly what must not happen to the
outer half of a correlation.

**Fixed by wrapping the parent** under an alias the inner scope cannot supply:

```sql
SELECT sqlite_proc_parent.id, sqlite_proc_parent.title, <fold subquery>
  FROM ( <the original parent SELECT> ) AS sqlite_proc_parent
```

The parent half of the correlation is then `sqlite_proc_parent.<key>`. No child
`FROM` can legally contain a source of that name, so the reference resolves
outward **by construction** rather than by names happening not to collide. The
inner `SELECT`'s outputs are aliased to the declared value-column names, which
is what makes them referenceable at all — a projected expression need not have
a name — and is legitimate because the declaration is the interface.

**Directions considered and not taken:**
- Put the child in an aliased derived table and qualify the parent side so the
  inner scope cannot provide it. Needs the parent's source name or alias, which
  is not known without resolution.
- Require the parent key to be a simple column reference and reject anything
  else at CREATE — narrower, checkable, and probably right for v1.
- Resolve the parent expression in the parent's context first and duplicate the
  *resolved* `TK_COLUMN`, so it carries a cursor rather than a name. Correct in
  principle; needs the rewrite to move from CREATE to codegen — which is where
  DOCKET 3c is moving it anyway.

**That last point matters for sequencing:** 3c already requires the fold to move
to CALL-compile time. If it moves there, the parent expression can be resolved
before duplication, and this bug may be fixed by the same change rather than
separately.

## Phase 5b — the flat-client column — **DONE** (`nested-shapes`)

Implemented as generated SQL, then made conditional by DOCKET 3c. `fold.test`
holds the invariant against an unmodified client: `column_count` 3, third column
valid JSON, BLOB children base64 via `sqlite_proc_jsonval`, childless parent
`[]`. `proc3c.test` holds the projection, including the restored scan counts.

**Superseded within the phase:** the fold was first applied at CREATE, baked
into the stored body. That is now recorded as a *recipe* and applied at
CALL-compile onto a copy — because a fold baked into the body cannot be
declined, and declining it is the whole of 3c.

**The column is generated as SQL, not assembled in C.** At CALL-compile time the
parent `SELECT` gains a result column at the nested position:

```sql
(SELECT json_group_array(json_object('post_id', post_id, 'cid', cid, 'body', body))
   FROM (<the child SELECT, verbatim>)
  WHERE post_id = <copy of the parent's projected key expression>)
```

The correlation reference is a **copy of the parent's projected key expression**
— the same technique that fixed phase 3's ordering, and for the same reason: it
cannot drift from what the column actually yields.

**Why this rather than the parent-buffer layer.** The buffering design meant new
state on the statement and new behaviour in `sqlite3_step`, `sqlite3_column_*`,
`reset` and `finalize` — a large surface with lifetime bugs that tests pass
over. The rewrite reuses machinery that is already correct and already tested,
and `sqlite3_column_count` becomes 3 *structurally*, because the parent row
genuinely has three registers. That was the strongest argument for the engine
fold all along; it turns out to be available without giving up the segments.

**What it costs, stated plainly: the flat path is EAGER.** The subquery is
evaluated for every parent row whether or not the client reads the column, so
phase 5's laziness test does not hold for flat clients — and laziness was the
argument that chose the boundary fold over the engine fold. Two things make this
acceptable rather than a quiet reversal. Segment-aware clients are unaffected:
the child segment is still lazy, still ordered, still what `procgen` consumes,
and `proc6` section 8 pins it. And the property is recoverable later behind the
same surface — DOCKET 3c's projection lets a client say up front that it does
not want the column, which is *better* than discovering it afterwards.

**Both views coexist.** A flat client sees three columns and stops at segment 1;
a segment-aware client ignores the column and advances. A client doing both
scans the children twice — documented, not prevented.

### Verified by hand before generating any of it (`test/foldshape.test`)

- **POC 1's refusal still holds on this build** — `json_group_array` rejects a
  BLOB (`"JSON cannot hold BLOB values"`). Encoding BLOB children is load-bearing,
  not decorative.
- **A childless parent yields `[]`, not NULL** — falls out of
  `json_group_array`, so a reassembler needs no special case.
- **R6 was wrong about base64.** `ext/misc/basexx.c` is compiled in but exposes
  `sqlite3_basexx_init()` as a *loadable extension*, so a default build has **no
  `base64()` function**. The plan assumed it "already ships" and it does not, in
  the sense that matters.

**Decision that follows, and it wants a name rather than a borrowed one:** the
fold needs an SQL-callable encoder. `hex()` always exists but doubles the payload
where base64 costs 1.33×. Registering `base64()` globally changes the function
namespace for every database in the fork. The narrower move is an *internal*
built-in used only by generated fold SQL — separation, in the sense of keeping
the fold's machinery out of the user's namespace where it could be relied on or
collided with.


**Deliverable.** The third column, lazy: materialised as JSON only when a
client actually reads it. BLOB children base64-encoded (R6); `ext/misc/base64.c`
already ships.

**Test, written first.**
- The stock `sqlite3` CLI sees three columns and valid JSON. **This is the
  invariant test and it uses the unpatched shell.**
- A BLOB child round-trips through base64 unchanged.
- Laziness: a client that never reads the column causes no child scan —
  asserted by counting scanned rows, per POC 2's measured 80% recovery.

**How this test can fail.** Force eager materialisation; the scan-count
assertion must go red.

---

## Phase 6 — the index advisory (R7)

**Deliverable.** When the imposed ordering is not supplied by an index, log
once at prepare in the `SQLITE_WARNING_AUTOINDEX` style, and make the fact
introspectable so `procgen` can report it at build time.

**Test, written first.** Unindexed correlation logs; the same schema with an
index does not; **neither is ever an error**, and the CALL succeeds in both
cases.

**How this test can fail.** Drop the index and the advisory must appear;
create it and the advisory must vanish. Both directions, or it is not a
detector.

---

## Phase 7 — the generated reassembler

**Deliverable.** `procgen` emits the client-side reassembler: typed parent
accessors, a child cursor per nested column, and the cardinality check.

**Test, written first.** Generate, **compile, and call** — as `procgen_test.c`
already does — and compare the reassembled tree against the same data fetched
by hand. That comparison is the positive control; the generated client cannot
pass by agreeing with itself.

**How this test can fail.** Corrupt one child row server-side; the comparison
must go red.

---

## Deliberately out of scope

- **Depth ≥ 2.** Rejected at the grammar in phase 1. Each level would have to
  be ordered by the full *ancestor path*, not by its own parent key, and each
  level would have to expose every ancestor key — which a `replies` table
  holding only `comment_id` does not have. That is a genuine blocker no syntax
  fixes; it needs its own docket entry.
- **Balanced misattribution.** Two equal-sized parents whose children are
  exchanged is invisible to every count-based check at any cost (POC 3).
  Catching it needs a checksum over correlation keys — a different feature.
- **Option 2's surface** (a correlated nested subquery in the body). Attractive,
  and it makes the correlation structural rather than imposed, but SQLite has
  no cell that is a row set: a multi-column value subquery is a parse error
  today. If it is ever wanted, it is a *lowering* onto phase 3, not a
  replacement for it — which is why phase 3 is described as the lowering rather
  than as the body form.
