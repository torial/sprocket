# PLAN-DEPTH — nested tables past one level

*Drafted 2026-08-06, before implementation, while veryquick gates the
stored-procs merge. Assumes no memory of prior sessions. Read HANDOFF-3h.md
first for build mechanics and suite baselines.*

## Why now, and the driving schema

Sean opened depth ≥ 2 on 2026-08-05. The false blocker (ancestor-path
ordering) is retracted in DOCKET 3g — reassembly is key-based grouping, so a
grandchild ordered by its own key attaches to child rows by exactly the rule
children attach to parents. The population that needs this is larger than
"schemas designed nested": every schema that widens a scalar into a
collection arrives here (the Mosaic case study,
`C:\Projects\mosaic\sprocket\CASE-STUDY-schema-change.md`).

**Driving schema: Mosaic.** `witness → claims → confidences` is a real depth-2
shape currently served by a hand-rolled `json_group_array` that draws its own
advisory (3g). The finish line is that workaround deleted:

```sql
CREATE PROCEDURE witness_claims(wid INTEGER)
RETURNS TABLE(wid INTEGER, siglum TEXT,
              claims TABLE(cid INTEGER, wid INTEGER, subject TEXT,
                           confidences TABLE(cid INTEGER, aspect TEXT,
                                             confidence TEXT)
                             KEY(cid = cid))
                KEY(wid = wid))
BEGIN
  SELECT wid, siglum FROM witness WHERE wid = :wid;
  SELECT cid, wid, subject FROM claim WHERE wid = :wid;
  SELECT k.cid, k.aspect, k.confidence FROM claim_confidence k
    JOIN claim c ON c.cid = k.cid WHERE c.wid = :wid;
END;
```

Segments in PRE-ORDER: parent, child, grandchild — 3 segments. A grandchild's
KEY names a column of the CHILD, never an ancestor beyond it.

## Tests first (all failing today)

1. `procdeep-1.x`: the CREATE above succeeds (today: "nested tables may not
   themselves nest"). Negative controls: KEY naming a grandparent column
   refused with a message that says WHY; depth 3 works or is explicitly
   capped (decide: cap at a constant with a clear error, or no cap — lean
   no-cap, the machinery is recursive either way).
2. `procdeep-2.x`: segment walk yields 3 segments, pre-order, each ordered by
   its own key; `proc_info`/`proc_nested`/`proc_list.nresultsets` agree.
3. `procdeep-3.x`: the FLAT path — parent row's fold column contains
   *recursively nested* JSON (`claims: [...{confidences: [...]}...]`).
4. `procdeep-4.x`: `WITH COUNTS` — child rows carry grandchild counts (the
   wire already supports per-segment hidden columns; measured 08-05).
5. `procdeep-5.x`: `WITH INTERLEAVED` **refused at depth ≥ 2** with a clear
   error — a grandchild orders by its parent's key, not the root's, so one
   merge order cannot serve three levels. The segmented protocol is the
   depth-N protocol; this is why both exist.
6. `procdeep-6.x`: the 3g advisory does NOT fire on the declared form; the
   hand-rolled Mosaic workaround still draws it (both directions).
7. `procfault`: a depth-2 case (the recursive constructors get failure-path
   coverage or their green is unearned).
8. Mosaic itself: `d2work.sql` replaced by the declared form; the case
   study gains its ending.

## The recursion sites (audit before coding; list from reading, may grow)

Engine, `src/proc.c` + friends:
- `sqlite3ProcNestedAppend` — DELETE the depth refusal; validate grandchild
  KEY against the child's columns (already how it works one level down).
- `sqlite3ProcSegmentCount` — count recursively (pre-order).
- `procBuildEmits` — flatten shapes recursively; parent-side key check
  resolves against the IMMEDIATELY containing table's value columns.
- `procApplyFolds` / `procAddFoldColumn` — the flat fold becomes recursive:
  the child's array elements each carry the grandchild's array. Build
  inside-out.
- `procLowerChild` — per-segment, already level-agnostic (each segment
  ordered by its own key); verify, don't assume.
- `sqlite3VdbeSetProcShapes` — descriptor walk recurses; `nHidden` per
  segment already supports counts at any level.
- `PRAGMA proc_nested` — recursive walk, segment numbering matching
  `proc_info`'s (which also recurses).
- `procDetectHandRolled` (3g) — declared depth-2 must stop matching the
  "hand-rolled" pattern... it never did (it matches json_group_array in the
  AUTHOR's SELECT); verify with test 6.
- INTERLEAVED validation — refuse depth ≥ 2 explicitly (test 5).

Tooling:
- `tool/procgen.c` — recursive struct emission and stitch (child structs
  gain `List(Grandchild)` fields; stitch innermost-first). The `@segments`
  seam needs nothing: `_segment` values just go deeper, and the client
  stitches by key at every level.

## Deliberately out of scope

- Interleaved depth-2 (impossible under one merge order; recorded, refused).
- Depth-aware `WITH COUNTS` integrity beyond per-parent (the POC 3 limit is
  unchanged at every level).
- Any Zebra-language change (the seam carries it).

## Order of work

Grammar acceptance + conformance recursion → segment path green (tests 1,2)
→ folds (3) → counts (4) → refusals/advisory (5,6) → procgen (and only then
Mosaic, 8) → fault coverage (7) throughout, not at the end.
