# PLAN-DEGRADE — degrade-at-load: the fork's lineage insurance

*Written 2026-08-13, on Sean's Posture-2 ruling (DOCKET #9).  The record
is `git log`, not this file's claims.*

## The shape being built, in five sentences

When schema load meets an object it cannot parse or validate — a newer
fork's syntax, a definition whose base vanished under writable_schema,
any CREATE the walk refuses — the loader DEAD-MARKS the object instead
of failing the whole file: name, type, and the refusal reason are kept,
everything else loads, and the file OPENS.  Touching a dead object by
name refuses with the retained reason and the fix ("upgrade or DROP");
everything else works.  A file containing any dead object is READ-ONLY
in this build (v1): an older binary cannot know which live tables feed
a dead view, so writes are refused with that exact sentence — the
recorded v2, if a degraded writer is ever really needed, is a
written-while-degraded marker the newer binary uses to recompute on
re-entry.  `PRAGMA dead_list` is the surface: (name, type, reason),
empty on healthy files, and the whole-file refusal that motivated this
campaign becomes one queryable row.  This ships NOW, before the next
feature, because the loader only protects binaries that carry it —
every fork build from this commit forward survives meeting its
successors' files.

## Decisions inside the ruling (2026-08-13; Sean may veto)

- **Everything that fails object-init degrades** — future syntax,
  failed conformance walks, dangling re-derivations.  No heuristic
  guessing "corruption vs future": the reason string tells the user
  which it was, and genuinely corrupt files still announce themselves
  through integrity_check and the dead_list rather than through a
  refusal to open.  (The old behavior survives for the schema table
  itself and rootpage-level damage: structural corruption is not an
  "object".)
- **v1 degraded = read-only file**, named default: bulletproof against
  the stale-derived-state re-entry hazard, and the walked demand for
  older binaries is reads.  v2 (marker protocol) recorded, not built.
- **Dead objects cannot be dropped by the degraded binary** (v1):
  "upgrade to modify" is the fix; an escape hatch that destroys a
  future object from an older binary is a footgun, not a kindness.
- The degrade path must be INERT on healthy files: zero behavior
  change, zero measurable load cost.

## Phases

- **Q0 — the spec, red.**  `test/degrade1.test`: future-syntax fixtures
  planted via writable_schema (an invented mview clause, an invented
  CREATE type, a mview whose base was renamed underneath it); the file
  OPENS; unrelated reads work; dead names refuse with the reason;
  writes refuse file-wide with the reason; dead_list rows exact;
  healthy-file inertness pinned (dead_list empty, writes fine); reopen
  keeps the degraded state consistent; a CURRENT-tier object (join
  view) loads fine beside a dead future one.
- **P1 — the loader.**  The init callback's error path dead-marks
  instead of corruptSchema for object-level failures; dead registry
  per schema (name, type, reason), lifecycle with the schema.
- **P2 — the fences.**  Name resolution refuses dead names with the
  reason; the file-wide write gate when any dead object exists;
  `PRAGMA dead_list`.
- **P3 — gates + docs.**  All existing specs untouched (ivm1/2/3,
  proc family — degraded-load must not change ONE healthy-file
  behavior); sweep both regimes; README-IVM sharp edge updated to
  name the new behavior; DOCKET #9 annotated.

## Controls

- The inertness pin comes FIRST: healthy files through the new loader,
  byte-identical behavior (the suites are that pin, all 40 of them).
- Every dead-object refusal names the object, the retained reason, and
  the fix — pinned messages.
- The degrade path is seen red before trusted: the spec's fixtures are
  hand-planted future syntax, and the OLD behavior (whole-file
  refusal) is what the spec's first run must show.

## Deliberately NOT in this campaign

The v2 degraded-write marker protocol; dead-object dropping; any
change to stock behavior (ruled out with posture 3); back-porting the
loader to already-shipped binaries (impossible by definition — which
is why this ships today).
