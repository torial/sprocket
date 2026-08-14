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

## Q0 corrections from first contact (2026-08-13, same session)

- **Deaths cascade, and the cascade is honest**: a dead mview's auto
  key index cannot load either (its table never did), so it dead-marks
  too, with the dead-name refusal as its retained reason — the chain
  reads correctly ("ivm$x$key: x is present but unusable…").  The spec
  now pins the cascaded rows rather than pretending three deaths were
  three.
- **The write gate moved from compile time to runtime.**  The first
  placement (sqlite3BeginWriteOperation) misfired on a pure SELECT:
  vtab schema-cookie bookkeeping (sqlite3VtabUsesAllSchemas) books
  write intents without meaning user writes, so reading
  pragma_dead_list through its own vtab form tripped the gate that
  exists to protect the file the pragma describes.  The honest choke
  is OP_Transaction with the write flag: it fires exactly when a
  statement opens a write transaction on the degraded database, never
  on bookkeeping, and grants per-database granularity (a healthy main
  writes fine beside a degraded ATTACH) for free.
- Parser-position pedantry in two pinned messages (the invented syntax
  dies where the grammar actually stops, not where I guessed) and a
  join-fixture arithmetic slip (u lacked the b=2 row), both mine.
- **Degrade belongs to FULL schema load only, never to the incremental
  re-parse of an object this build just wrote.**  Caught by upstream's
  own view-29 (forum daa2c728cc): `CREATE VIEW IF NOT EXISTS IF AS
  SELECT null` parses live but its stored text will not round-trip;
  upstream detects that inside the CREATE via OP_ParseSchema and
  aborts it, persisting nothing.  The first degrade cut swallowed that
  error, PERSISTED the broken view as dead, and locked the file
  read-only — strictly worse than the disease, and a healthy-file
  behavior change (the inertness gate caught it; the charter forbids
  it).  Fix: a new `INITFLAG_FullLoad` bit set only by
  sqlite3InitOne; initCallback degrades only when it is set and no
  ALTER-reparse flag is (ALTER's re-parse also wants its immediate
  error).  At a live CREATE the user is present to receive the error;
  degrade exists for meeting a FOREIGN file, and foreign files only
  arrive via full load.
- **The write gate exempts `writable_schema=ON` — otherwise a file
  whose dead object NO build can parse is frozen forever.**  A
  dangling mview (base vanished under schema surgery) dead-marks in
  every build, old and new alike; with an absolute gate, every build
  refuses writes, so no one can ever delete the corpse.  The fork's
  declared expert exception (mview write protection already honors
  writable_schema) is the escape hatch: under writable_schema=ON the
  gate stands down, schema surgery can remove the dead rows, and the
  next open is healthy.  The spec pins the full repair story.

## Post-campaign corrections (2026-08-14, found by the QUEUE campaign's veryquick)

- **The 41-suite sweep was too narrow a gate for this campaign, and the
  full price arrived a day late.**  veryquick's corrupt2/corruptL/
  corruptM/triggerupfrom pin the OLD whole-file refusal on object-level
  schema corruption — 15 legs — and degrade shipped without running
  them.  All 15 are the ruled posture working as decided ("genuinely
  corrupt files announce themselves through integrity_check and the
  dead_list rather than through a refusal to open"); they are re-pinned
  with fork annotations.  The lesson is recorded here so the next
  loader-touching campaign runs veryquick BEFORE claiming inertness.
- **Empty retained reasons, fixed.**  Upstream signals "the schema row
  and its CREATE statement disagree" with a deliberately EMPTY error
  message (build.c's two `sqlite3ErrorMsg(pParse,"")` sites), leaving
  the words to corruptSchema.  The degrade path retained "" verbatim —
  `unusable in this build ()` — an empty reading as an answer.  The
  loader now spells the condition; corruptM-131/171 pin it.

## Phases

*All four EXECUTED 2026-08-13, one session.  Receipts: degrade1 0/20;
full sweep 41/41 suites clean, DEBUG=3 and release both (the 41st is
degrade1 itself, now in the roster).  The corrections above were paid
for by the gates working as designed: the first sweep caught view-29
(2/123) in both regimes, which forced the FullLoad distinction, which
exposed the frozen-forever trap, which produced the writable_schema
repair story — three findings from one red suite.*

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
