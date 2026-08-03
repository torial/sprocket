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

**Meanwhile:** `nresultsets` keeps meaning *declared shapes*. Do not read it as
a segment count.

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

## Phase 3 — the lowering (body form 1½)

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

## Phase 5 — the flat-client column

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
