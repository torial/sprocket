# PLAN-TSGEN — the TypeScript emitter over N-API

*Written 2026-08-14, on Sean's ratification ("a nice way to also dogfood
some of the work").  Closes DOCKET #2 to its own done-means, whose
emitter list has named TypeScript since 2026-08-03.  Substrate ruled
2026-08-13: an N-API addon — stable ABI across Node majors, plain C,
statically linked — is the local client, Python-ctypes' equivalent; the
wire client (browser) waits on the HTTP gateway per the protocol ruling.*

## The shape

- **`tool/napi/sprocket_node.c`** — the addon, deliberately minimal:
  `open(dbPath)`, `close(db)`, and ONE work function
  `segments(db, sql, params) -> [[{col: value}]]`, the exact contract
  of the Python runtime's `_segments`.  It statically links the fork's
  amalgamation, so the `.node` file is self-contained — no DLL-path
  ambiguity, and the "server is a statically-linked embedder" doctrine
  extends to clients.  Types map: INTEGER -> bigint-safe number or
  BigInt (decide by measurement: JS numbers lose precision past 2^53;
  the addon returns BigInt for out-of-range, number otherwise — the
  honest spelling, pinned by a test with a 2^60 value), REAL -> number,
  TEXT -> string, BLOB -> Uint8Array, NULL -> null.
- **`procgen --lang ts`** — interfaces per shape (depth-N, the zebra
  recursion as template), typed functions calling `db.segments(...)`,
  the bucket stitch deepest-first (the python emitter's algorithm),
  a TS/JS keyword shield, explicit `connect(dbPath, addonPath)` — the
  runtime takes its dependencies as arguments, never from ambient
  state, same as python's `connect(db_path, dll_path)`.
- **Gate driver `tool/procgen_tstest.py`** — mirrors procgen_pytest.py
  against the SAME fixture (`procgen_pynest_fixture.sql`) and the same
  expectations: flat+param, depth-1 stitch, depth-2 with the empty
  leaf.  Adds: `tsc --noEmit` (the type layer must CHECK, not merely
  strip), the BigInt boundary case, and byte-identical regeneration.
  Node 24 runs .ts natively (type stripping); typescript is fetched
  once into tool/napi for the check step.

## Phases

- **Q0** — this plan; the driver written red (it demands an addon and
  an emitter that do not exist).
- **P1** — the addon builds via node-gyp against the amalgamation;
  a hand-written smoke (open fixture, segments of a CALL, close).
- **P2** — the emitter; driver goes green through tsc and node.
- **P3** — determinism pinned; docs (README-PROCS typed-clients
  section gains ts; DOCKET #2 annotated: done-means met in full).

## Deliberately NOT here

The wire/browser TS client (waits on the HTTP gateway); prebuilt
binaries for platforms this machine cannot build (the matrix is a
distribution task for the 0.9 packaging conversation); publishing to
npm (torial/sprocket distribution is Sean's call).
