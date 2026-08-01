# Design note: multi-result-set delivery for CALL

**Status:** IMPLEMENTED 2026-07-31 along the lines recommended below
(option A). What shipped, and how it differs from this note, is recorded
in §7 at the end.
**Context:** `SPROCKET_RETURNS_TABLE_SPEC.md` §5.3 asks for
`sqlite3_proc_next_resultset()` and notes: *"sprocket already streams
multiple sets through one CALL somehow — locate that mechanism and
formalize the boundary rather than inventing a parallel path."*
This note is the result of locating it.

---

## 1. What the mechanism actually is today

There isn't one. Multiple sets are **concatenated**, not delimited:

- Each row-returning `SELECT` in a body compiles with `SRT_Output` inside
  the procedure's `SubProgram`; rows leave the sub-frame through ordinary
  `OP_ResultRow` and reach the caller through ordinary `sqlite3_step()`.
- The caller's statement has exactly one column count (`Vdbe.nResColumn`)
  and one `aColName` array, so `proc.c` **requires every row-returning
  SELECT in a body to have the same width** and reports
  *"all row-producing SELECT statements in procedure X must have the same
  number of result columns"* otherwise.
- Consequence (found while implementing the conformance checker): a
  two-shape declaration of differing widths passed `CREATE` and then died
  at `CALL`. The checker now rejects it early, and that restriction is
  the thing a boundary API exists to lift.

## 2. The decisive precedent: nResColumn is already variable

`Vdbe` separates two quantities (`vdbeInt.h`):

| Field | Meaning |
|---|---|
| `nResAlloc` | column slots **allocated** in `aColName[]` |
| `nResColumn` | columns the statement is **currently reporting** |

`sqlite3VdbeSetNumCols()` sets both, but `sqlite3_stmt_explain()` then
freely reassigns `nResColumn` alone — 12, 8 or 4 columns for the *same
prepared statement* depending on EXPLAIN mode — and `sqlite3_reset()`
restores `nResColumn = nResAlloc` (`vdbeapi.c`). `sqlite3_column_count()`
simply returns `nResColumn`.

**A statement whose reported column count changes during its life is
therefore already an idiom of this engine, not a violation of it.** That
is the strongest argument for doing boundaries in-statement rather than
inventing a parallel delivery path.

## 3. Options considered

### A. In-statement boundaries (recommended)

An explicit boundary opcode; the statement pauses between sets and swaps
its column metadata on request.

- **Prepare time:** allocate `aColName` for `max(width(S₁..Sₙ))` so
  `nResAlloc` never has to grow mid-flight; load S₁'s names/types; set
  `nResColumn = |S₁|`. Copy the shape descriptors into **statement-owned**
  memory (never point into the schema-owned `Proc` — README-PROCS
  invariant 6).
- **Codegen:** after each streaming SELECT of a declared procedure, emit
  `OP_ProcSetEnd` with **P1 = index of the set just completed**.
- **Runtime:** `OP_ProcSetEnd` records the resume point, sets a
  "paused at boundary" flag, and returns `SQLITE_DONE` *without* halting
  the statement. Further `sqlite3_step()` calls return `SQLITE_DONE`
  idempotently — sets never bleed into one another (the loud in-order
  consumption the spec asks for).
- **`sqlite3_proc_next_resultset(stmt)`:** if paused at a boundary, load
  shape k+1 into `aColName`, set `nResColumn = |S₍ₖ₊₁₎|`, clear the flag,
  resume; return `SQLITE_OK`. If the body has finished, `SQLITE_DONE`.
- **`sqlite3_reset()`:** must restore S₁'s metadata (the same place
  `nResColumn = nResAlloc` happens today) and, if the client abandoned
  mid-sequence, unwind exactly as an abandoned CALL does now.
- `OP_ResultRow`'s `assert(p->nResColumn==pOp->p2)` keeps holding,
  because the boundary sets `nResColumn` before the next set's rows.

**The key property:** the column count only ever changes *after the
client explicitly calls the new API*. A caller that never calls it sees
exactly today's contract — set 1's metadata, set 1's rows. **No existing
client can observe the change.** The invariant is not broken; it is
opt-in.

Costs: a new opcode, a pause/resume state on `Vdbe`, per-set descriptors,
and careful interaction with `reset`/error paths. No materialization, no
memory growth, streaming preserved.

### B. Materialize each set into an ephemeral table

Body writes set k into an ephemeral b-tree; delivery switches readers.

- Pros: sets are trivially independent (any widths); random access and
  re-reading become possible; a natural stepping stone to *table-valued
  procedures* (`SELECT ... FROM p(...)`).
- Cons: loses streaming, costs memory/spill for large sets, changes when
  side effects happen relative to row delivery, and is a parallel
  delivery path — exactly what the spec warned against.

### C. One prepared statement per set

Impossible without re-executing the body (side effects) or buffering it
first, which collapses into B.

### D. Coroutine / eponymous virtual table

The idiomatic route for making a procedure usable as a **row source**
(`FROM p(...)`) rather than a statement. Complementary to A, not a
substitute: it answers a different question (composition into queries)
than the boundary API (sequential multi-set consumption).

## 4. Which choices open the most future doors

| Future feature | A helps | B helps | Notes |
|---|---|---|---|
| Differing set widths (the immediate unlock) | ✔ | ✔ | |
| Streamed-set **composition** across CALLs (spec §8 deferral, checker rule R4) | ✔ | partly | A callee's `OP_ProcSetEnd` can propagate out of the sub-frame if the opcode carries the set index — the boundary becomes a *signal*, not an inference from statement order |
| Conditional / computed set sequences | ✔ | ✔ | Only if boundaries are explicit opcodes; an implicit "between statements" rule cannot express them |
| Table-valued procedures (`FROM p(...)`) | — | ✔ | Wants D more than either |
| Cursors, re-reading, random access | — | ✔ | |
| Very large result sets | ✔ | ✘ | B buffers |

Three design choices inside option A do most of the door-opening work:

1. **Explicit opcode carrying the set index** rather than inferring
   boundaries from statement order. Composition and conditional sets
   become expressible without redesign; diagnostics get better for free.
2. **Statement-owned per-set descriptors** copied at prepare. Required
   for correctness today (schema-owned `Proc` objects must not carry
   statement state), and the hook a future "shape computed at runtime"
   case would use.
3. **Opt-in semantics.** Because the metadata only changes when the
   client asks, the feature can ship without an API-compatibility
   argument, and old tooling keeps working against declared procedures.

## 5. What the unlock costs on the checker side

The equal-width restriction lives in exactly one loop in
`procCheckConformance()` (`src/proc.c`) — deleting that loop is the whole
relaxation, because per-shape arity is already tracked and checked
against each set individually. That isolation was deliberate.

## 6. Open questions for whoever implements this

- Error semantics mid-sequence: today a failure anywhere in a CALL
  unwinds the whole thing; boundaries should not weaken that.
- Does `sqlite3_stmt_status`/scanstatus need per-set attribution?
- Should `PRAGMA proc_info` grow a "current set" notion, or stay purely
  static? (Recommend: stay static.)
- Naming: `sqlite3_proc_next_resultset` follows the spec; consider
  whether the fork wants a `sqlite3_` prefix that upstream might one day
  claim.

---

## 7. What was actually built (2026-07-31)

Option A, essentially as described.

- `OP_ProcSetEnd P1` is emitted after each row-returning SELECT of a
  declared procedure (P1 = index of the set just finished). It saves the
  program counter and pauses **exactly** the way `OP_ResultRow` does —
  internally it returns `SQLITE_ROW` — but delivers no row and sets a
  `Vdbe.bAtSetEnd` flag.
- `sqlite3Step()` maps that internal pause to `SQLITE_DONE` for the
  client, and `sqlite3_step()` keeps returning `SQLITE_DONE` while the
  flag is set, so sets can never run together silently.
- `sqlite3_proc_next_resultset(stmt)` swaps in the next set's metadata and
  clears the flag: `SQLITE_OK` when another set is current, `SQLITE_DONE`
  when the sequence is exhausted, `SQLITE_MISUSE` on a statement with no
  declared shapes.
- Per-set descriptors (`VdbeProcSet`) are copied into statement-owned
  memory at prepare time; `aColName` is sized for the widest set, so
  advancing never reallocates and `nResColumn` alone varies — the same
  move `sqlite3_stmt_explain()` already makes.
- `sqlite3VdbeReset()` restores set 1's metadata for a statement
  abandoned part-way through a sequence.
- The equal-width precondition is gone: the one loop in
  `procCheckConformance()` that enforced it was deleted, as this note
  predicted, and declared sets may now differ in arity.

**Behavior change, deliberate and confined to declared procedures:** a
`CALL` of a procedure with more than one `RETURNS TABLE` used to deliver
every set's rows concatenated under set 1's column names. It now stops
at each boundary. Undeclared procedures are untouched, and a declared
procedure with a single shape behaves as before.

**One bug this work uncovered** (in the previously committed
`RETURNS NOTHING` path): `sqlite3VdbeSetNumCols(v, 0)` asked the
allocator for zero bytes, and `dbMallocRawFinish()` reports a NULL
return as OOM. It passed unnoticed because lookaside absorbs a 0-byte
request; the full regression suite caught it in configurations where
lookaside is unavailable. `sqlite3VdbeSetNumCols()` now handles a
zero-column statement without allocating.
