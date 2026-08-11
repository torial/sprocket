# PLAN-PROJECTION — the fold moves to CALL-compile time (`CALL … RETURNING`)

*Drafted 2026-08-11 by Fable, before implementation, as a cold-start brief:
assumes no memory of prior sessions. Design authority is DOCKET.md §3c
("written up 2026-08-04 — now the next thing, not future work"); this file
turns it into an executable campaign. Read HANDOFF-3h.md for build mechanics
and suite baselines. Sibling docket item 3d (`mayAbort` assert) is a
stored-procs bug — do NOT bundle it into this branch's work.*

## Why now — a measured regression against the feature's own client

Phase 5b baked the nested-table fold into the parent SELECT at CREATE
(`procCheckAndLower`), so **every** caller pays for it:

- A client that never reads the fold column still scans the children.
- A segment-aware typed client (the one this feature exists for) reads the
  child segment too — and scans the children **twice**.
- Measured: `proc6-8.5/8.6` show **4 and 8** child scans where laziness gives
  **0 and 4**. Those two numbers are this campaign's acceptance test:
  projection restores 0 and 4.

The Mosaic typed client (`witnesses_full`, depth-2) and the Graze docs-browser
are live consumers of exactly this path.

## The design, already decided (docket §3c — do not re-litigate)

1. **The fold moves from CREATE to CALL-compile.** The stored body stays
   canonical and unfolded; fold columns are generated per request in
   `codeProcBody`, where the requested projection is known. Runtime
   suppression is NOT available — SQLite evaluates every result column — so
   compile-time is the only place this can live.
2. **Surface: `CALL p(args) RETURNING id, title` — zero new keywords.**
   `RETURNING` is an existing token that already means "the columns I want
   back." Omitted clause = fold everything: **the invariant is the default**,
   an unmodified client sees no change.
3. **The proc cache re-keys to (procedure, projection).** Without this, two
   clients with different projections silently share one compiled body — a
   wrong-answer bug of exactly the family this feature keeps producing. This
   is the highest-risk edge of the campaign; it gets its own planted test.
4. **v1 scope, deliberately narrow:** the clause controls *only which
   generated fold columns are produced*. Segments are unaffected — they are
   the body's own SELECTs and always stream. Per-table naming works
   (posts + comments but not posts + authors).
5. **Deliberately NOT v1:** dropping the child *segment* for an unprojected
   table. It makes `nresultsets` projection-dependent — separate item, own
   docket entry when it comes.

## Verify before building (the localisation pass)

The docket names the sites; the fresh session must read them before editing —
names below are from §3c/§5b and must be confirmed against current source:

- `procCheckAndLower` — where the fold is currently baked at CREATE; the
  unfolding change lands here.
- `codeProcBody` — where per-request fold generation lands.
- The proc cache key struct + lookup (grep `PROCCACHE`) — the re-key. Check
  how the cache interacts with schema reload: a stored proc re-lowered on a
  fresh connection must produce the same canonical (unfolded) body, or two
  connections will disagree about what is cached.
- `parse.y` CALL rule — where the optional `RETURNING` idlist attaches.
  Confirm no grammar conflict with `RETURNING`'s DML use (it appears in
  UPDATE/INSERT/DELETE contexts; CALL is a distinct statement head, expect
  clean, but *see* the lemon output say so).

## Phases (each ends runnable, gates green)

- **P0 — Grammar.** `CALL … RETURNING idlist` parses into the Parse struct;
  no codegen change yet; a rejected projection (unknown column) errors at
  prepare with the column named (UNGIT: refusals name the reason).
- **P1 — Canonical body.** CREATE stops baking the fold; the fold descriptors
  are stored alongside. Gate: with NO `RETURNING` anywhere, all suites stay
  green and depth-1/depth-N outputs stay **byte-identical** (the PLAN-DEPTH
  standard) — this phase must be invisible on its own.
- **P2 — Projection codegen.** `codeProcBody` compiles requested folds only.
  Gate: `proc6-8.5/8.6` under full projection still 4 and 8 (default
  unchanged); under `RETURNING` value-columns-only, **0 and 4**; EXPLAIN CALL
  shows no fold subquery when projected out (the plan, not just the count —
  counts alone have a non-coroutine path that lies; see the cb9e13dc probe's
  method).
- **P3 — Cache re-key.** Planted collision test: same proc, two projections,
  interleaved on one connection AND on two connections — each must get its
  own compiled body. Negative control: same projection twice must HIT (prove
  the key isn't over-splitting; a cache that never hits is also broken).
- **P4 — procgen.** Settle the open question BEFORE emitting: accessors per
  projection, not per procedure (`sqlite3_column_count` now varies per
  statement). Zebra emitter first (Mosaic + docs-browser are the consumers);
  regenerated unchanged-schema output stays byte-identical when no projection
  is requested.

## Controls (instrument discipline — state them before running)

- Expected magnitudes: 0/4 projected, 4/8 default. Any other pair is a finding.
- Positive control: a projection that keeps one fold must still produce that
  fold's rows (prove the suppressor can NOT fire).
- Negative control: unknown column in RETURNING must error at prepare —
  seen red once before trusting the green path.
- The cache test must be run in BOTH directions (miss on differing projection,
  hit on matching) — a one-legged version proves nothing.
- Build `DEBUG=3` at least once per phase (3d's standing lesson: release-only
  regimes cannot see an assert, and this codebase is assert-dense).

## Done means

`CALL witnesses_full() RETURNING wid, siglum` from the Mosaic client drops the
double scan (verified by EXPLAIN + counts), an unmodified caller sees
byte-identical behavior, the cache provably separates projections, and DOCKET
§3c's "Open question" paragraph is replaced by a pointer here.
