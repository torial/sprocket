# PLAN-DAEMON — sprocketd: the integrating daemon

*Written 2026-08-14, on Sean's "let's proceed" after the protocol
ruling, with the stated aim: knock out the sprocket work so Graze can
take advantage of it.  Composes the six PROVEN transport phases
(PLAN-TRANSPORT, complete 2026-08-03) plus the queued-write engine mode
(PLAN-QUEUE) into one production process.  Protocol posture: binary
native, ruled 2026-08-14 — see DESIGN-NETWORK.*

## The shape, in one paragraph

`tool/sprocketd.c`: a TCP daemon owning one SQLite database.  Requests
are CALLs in the proven binary framing; responses are the proven frame
stream with the shape-cache handshake.  **Writes route through the
phase-5 group-commit queue on a connection that declares
`queue_writer=ON`; read-only CALLs run on concurrent reader
connections — and the router does not guess which is which: the engine
declares it (`pragma_proc_list.writes`, built for exactly this).**  The
truth surface of DESIGN-NETWORK §1a (queue depth, oldest waiter age,
last commit latency) is served in-protocol.  Everything is
self-testing in the established tool-harness pattern.

## Decisions inside the ruling (Sean may veto)

- **v1 is one database per daemon instance.**  Shard routing (phase 6)
  becomes a config concern when a second file exists; the isolation
  tactic is that dispatch reaches the db through one handle-plus-queue
  pair, so per-shard pairs slot in without callers changing.
- **Protocol versioning from frame one** (the ruling's named cost): the
  connection opens with a HELLO carrying a protocol version and the
  schema cookie; mismatch is a refusal with the reason, never a guess.
  Frames carry a request id even though v1 is one-in-flight per
  connection — pipelining is a client-side upgrade later, not a frame
  change.
- **Routing trusts the declaration, and the declaration is enforced.**
  A proc with `writes=0` runs on a reader connection; if it lies (a
  UDF mutation, a bug), the queued-write mode is the backstop: the
  reader connection is not a declarant, so its write attempt REFUSES
  at the engine gate rather than corrupting the single-writer
  discipline.  The daemon converts that refusal into a routing error
  naming the proc — the lie is caught, attributed, and priced.
- **The stats surface is a reserved CALL name** (`sprocket_stats`),
  answered by the daemon itself in ordinary result-set frames: no new
  frame kinds, and every generated client can already read it.
- **Windows first** (the deployment reality), sockets isolated behind
  the same thin shim proc_server.c proved; the Linux leg follows the
  WSL validation path.

## Phases

*D0-D4 (Windows) EXECUTED 2026-08-14, same day as D0.  Receipts:
`tool/sprocketd.c`, selftest 33 checks 0 failures -- the full nine-point
matrix including the lying-proc backstop with attribution, the 40/40
malformed-request sweep against the live socket, and count-in ==
count-out at shutdown.  Serve mode smoked against a real file.  The
engine was not touched, so the self-test is the campaign gate by the
tool-harness discipline.  CORRECTION to D4 as planned: the Linux leg
requires a socket/thread shim (v1 is Winsock + Win32 threads, per the
"Windows first" decision) and moves to D5's scope rather than being
faked here.  Two selftest catches worth the record: wbBytes is the
LENGTH-PREFIXED writer, and using it to build both the wire name field
(doubled length) and the CALL SQL text (leading NUL -> prepare("")
returns a NULL statement whose binds MISUSE with a clean errmsg) --
the same trap in two costumes, both found by the matrix, neither by
inspection.*

- **D0 — this plan, and the self-test matrix enumerated red** (below).
- **D1 — skeleton**: config (db path, port), listener, HELLO/version
  handshake, read-only CALL dispatch on reader connections.
- **D2 — the write path**: queue integration, `queue_writer` declared
  and checked, writes-flag routing, the lying-proc backstop.
- **D3 — truth surface + lifecycle**: `sprocket_stats`, graceful
  shutdown draining the queue, error frames for every refusal.
- **D4 — gates**: the full self-test matrix green; fork sweep
  unaffected (the daemon is tool-only — the engine does not change in
  this campaign); the matrix run on Linux via the WSL path.
- **D5 — the consumer leg — RESOLVED 2026-08-14, artifact in view.**
  Read `graze/mosaicclient.zbr` and `zebra-sprocket/generated_atlas.zbr`
  before deciding: Graze consumes generated typed clients over
  in-process FFI (`d.query_segments("CALL ...")`, client-side stitch),
  and since Graze IS a server that shape is architecturally right, not
  a stopgap — it keeps it.  The daemon's consumers are out-of-process
  tooling and the future TypeScript story (N-API local / wire browser,
  DOCKET #2's sequencing).  The "visible log channel by default"
  ruling (Sean, same day) landed in the ENGINE (`printf.c`:
  `sqlite3_log` -> stderr when unregistered), so every consumer —
  Graze's DLL, Python's ctypes, sprocketd, any future embedder —
  inherits it with zero client changes; that engine default IS this
  phase's deliverable, placed one layer down from where the plan
  guessed it would live.

## The self-test matrix (D0, written before the daemon exists)

Each line is a check the harness must make, with its failure mode:

1. HELLO round-trip; wrong version refused with reason (control: right
   version accepted).
2. Read-only CALL returns the proven frame stream; shape-cache
   handshake saves the re-describe (assert by byte count, the phase-4
   measurement re-run through the daemon).
3. Write CALL through the queue: durable, serialized, one commit per
   batch window (the phase-5 deterministic gate test, driven over TCP).
4. Concurrent readers during a write storm: reader latency bounded by
   readers-never-block (WAL), asserted by counts not timings.
5. Out-of-band writer against the daemon's database: REFUSED by the
   engine mode while the daemon lives (the proc_queue dogfood, now
   cross-process).
6. A proc that lies about `writes=0`: the engine refuses, the daemon
   reports a routing error naming the proc (control: the same proc
   declared honestly succeeds through the queue).
7. `sprocket_stats` answers with plausible instruments (depth ≥ 0,
   ages sane); after a known write burst the counters moved.
8. Malformed frames: the phase-1 corruption sweep re-run against the
   live socket — large fraction rejected, zero crashes, zero silent
   accepts.
9. Graceful shutdown under load: every accepted request answered or
   cleanly errored; no request silently dropped (count in == count
   out).

## Deliberately NOT in this campaign

Pipelining (frames carry ids; the client upgrade comes later); TLS and
auth (LAN/localhost deployment reality; the HTTP gateway carries any
public story, per the ruling); shard routing (v2 config); the HTTP
gateway itself; changes to the engine.
