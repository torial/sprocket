# PLAN: transport for CALL-as-request

*Working plan, written 2026-08-01. Committed deliberately so it survives
context loss — if you are a later session picking this up, this file plus
`DESIGN-NETWORK.md` is the whole briefing.*

## Where things stand

| Piece | State |
|---|---|
| Stored procedures, PSM, declared shapes | done, `veryquick` 0 errors / 392,950 |
| `sqlite3_proc_next_resultset()` boundary API | done; deliberately NOT in the extension table (README-PROCS.md says why) |
| Thesis + measurements | `DESIGN-NETWORK.md` |
| Wire **codec** (framing/encoding, no sockets) | done, `tool/proc_wire.c`, 22 checks |
| Wire **transport** phase 1 (framing) | done, `tool/proc_frame.c`, 18 checks, 369 chunkings |
| Wire **transport** phase 2 (TCP loopback) | done, `tool/proc_server.c`, 15 checks |
| Wire **transport** phase 3 (arguments) | done |
| Wire **transport** phase 4 (shape cache) | done, 34.9% saved on the wire |
| Wire **transport** phase 5 (group commit) | done, `tool/proc_queue.c`, 6 checks |
| Wire **transport** phase 6 (shard routing) | done, `tool/proc_shard.c`, 9 checks |
| **All six phases complete** | 2026-08-03 |

The durable argument is **data-dependent control flow**, not batching
(pipelining supplies batching). Do not let the transport work drift back into
justifying itself on round-trip counts alone.

## Non-negotiable: every phase ships with its test, written first

Each phase below names its test *and* names how that test is made capable of
failing. This is not ceremony — in this codebase, on this machine, four
separate instruments have reported green while measuring nothing. The rule
that has actually caught things is **"break it on purpose and confirm the
check goes red."**

Standing rules for anything built here (see also the global CLAUDE.md entry):

1. State the expected magnitude before reading the result.
2. Include a positive control the instrument must find.
3. Prove liveness before trusting silence.
4. Prefer deterministic counts to timings.
5. Break it on purpose once, and watch it go red.

### Windows build recipe (hard-won; do not rediscover)

```
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
cd /d C:\Projects\sqlite
set PATH=C:\Projects\sqlite;C:\Tcl\bin;%PATH%
cl /nologo /W3 /O2 /I. tool\<file>.c sqlite3.c /Fe:<out>.exe /Fo:obj\
```

- The repo dir **must** be on PATH (`jimsh0.exe`, `testfixture.exe` are invoked
  unqualified and Windows may not search the working directory).
- `C:\Tcl\bin` **must** be on PATH or `testfixture.exe` exits 0 printing
  nothing — a false green.
- Invoking a `.bat` from bash: use a **Windows** path (`cmd //c "C:\...\x.bat"`).
  A `/tmp/x.bat` path makes cmd parse `/t` as a switch.
- Link Winsock with `ws2_32.lib`.

---

## Phase 1 — framing over a byte stream *(no sockets)* — **DONE**

**Deliverable.** `tool/proc_frame.c`: a length-prefixed framer that reassembles
messages from arbitrary chunk boundaries. A stream reader that only works when
each `recv()` happens to return a whole message is the classic transport bug,
and it is invisible on loopback with small payloads.

Wire form: `[u32 length][payload]`, length capped (propose 64 MiB) so a hostile
or corrupted prefix cannot drive an allocation.

**Test, written first.**
- Encode a known response with `proc_wire`, then feed it to the framer in
  *every* chunking: one byte at a time, two at a time, … and several pseudo
  random splits with a fixed seed. Every chunking must yield byte-identical
  reassembly. **Expected magnitude:** ≥ 300 distinct chunkings exercised.
- Positive control: the reassembled payload must decode to the checksum
  `proc_wire`'s self-test already pins.
- Two messages back-to-back in one chunk must yield exactly two messages
  (catches "assume one message per read").

**How this test can fail.** Corrupt the length prefix to exceed the cap and to
under-run; both must be rejected. Truncate the final message; must be reported
as incomplete, never as a short message. If any of these pass, the framer is
not validating and the phase is not done.

---

## Phase 2 — loopback echo of one real CALL — **DONE**

**Deliverable.** `tool/proc_server.c` (single-threaded, blocking, one
connection) and a client in the same binary behind `--client`, so the whole
thing is one file and one build. Server binds `127.0.0.1:0` (ephemeral),
prints the chosen port, accepts, reads one framed request, executes a fixed
`CALL fetch_all()`, writes the framed response, exits.

**Test, written first.**
- End-to-end: client's decoded checksum **equals the constant already pinned**
  in `proc_wire`'s self-test. That constant is the positive control — it ties
  the network path to a value proven without a network.
- Liveness: server prints `LISTENING <port>` before accept; the test fails if
  that line never appears, rather than hanging forever. Hard timeout.
- Negative: a client that sends 64 random bytes must get a clean error and the
  server must still exit 0 rather than crash or hang.

**How this test can fail.** Point the client at a closed port — must fail
loudly and promptly, not hang. Have the server send one byte fewer than the
frame declares — client must report incomplete.

**Built as a server THREAD in one binary** rather than two processes: the
socket path is genuine TCP on 127.0.0.1, but the test cannot hang on a dead
process, cannot leak an orphan listener, and needs no shell orchestration.
Every socket carries a 5s timeout, so each negative case fails promptly
instead of wedging. A hang is worse than a failure -- it reads as progress.

---

## Phase 3 — requests carry arguments — **DONE**

**Deliverable.** Request frame: `[u8 REQ_CALL][str procname][u32 nArg][values…]`
using the existing value tags. Server binds them to the prepared `CALL`.

**Test, written first.** Round-trip every value class through *arguments* and
back out as a result: NULL, `-9223372036854775807`, a full-precision double,
empty text, UTF-8, and a blob with an embedded NUL. Identity is the control —
what goes in must come out. **Expected magnitude:** ≥ 6 value classes.

**How this test can fail.** Alter one argument byte server-side before binding;
the identity check must go red.

---

## Phase 4 — the shape-cache handshake *(the fork's differentiator)* — **DONE**

**Deliverable.** Client sends `(procedure, schema cookie)`; if the server agrees
the cookie is current it omits `WF_SHAPE` frames entirely. Because declared
shapes are checked at `CREATE` time, the contract is static — this is the thing
PostgreSQL structurally cannot do.

**Test, written first.**
- Shape-free response is strictly smaller (already 34.9% on the codec's sample
  payload — *note that figure is for short column names and few rows; the
  fraction shrinks as rows grow, and the doc should say so*).
- Data checksum identical between modes.
- **Stale cookie forces re-describe:** run a DDL statement to bump the schema
  cookie, then confirm the server sends shapes again rather than trusting the
  client's cache.

**How this test can fail.** Make the server ignore the cookie mismatch; the
stale-cookie case must then go red. If it does not, the cache is unsafe and
would serve a client wrong column names after a migration.

---

## Phase 5 — one writer thread, group commit — **DONE**

**Deliverable.** Per database: a queue, one writer thread, drain up to K items
into one `BEGIN IMMEDIATE` … `COMMIT`.

**Carry into this phase (added 2026-08-02, after porting wal2):**
- Run the queue's database in `journal_mode=wal2`. Group commit concentrates
  writes, which is precisely the load that starves a classic-WAL checkpointer.
- If replication is wired to `sqlite3_wal_hook` here, it must be wal2-aware:
  the callback argument is total *uncheckpointed* pages across **both** wal
  files (0 when the other is empty/checkpointed), and there are two files to
  ship rather than one.
- `BEGIN CONCURRENT` is still undecided and would change this design (multiple
  writers make a single queue optional rather than necessary). Keep the queue
  behind an interface so it can become one-queue-per-shard or be bypassed
  entirely — that is the isolation tactic for this phase.

**Test as specified, and why it was WRONG.** The plan said: submit 1000 writes
from 8 threads, count transactions with a commit hook, assert the count is far
below 1000. That was built, and it passes — but the number it produces is not
deterministic. It measures how many items happened to arrive while a commit was
in flight, which is *scheduling*. The same workload went from 251 commits to
868 after an unrelated change to how waiters are woken, and "batches grow with
concurrency" held from 8→32 writers and then reversed at 64.

**Sharper rule, learned here: counting rather than timing is not sufficient if
the COUNT is itself timing-dependent.** A commit count under free-running
concurrency is a stopwatch wearing a different hat.

**What is actually deterministic**, and what the test now asserts: *if K items
are queued when the writer takes a batch, exactly one transaction commits them.*
The writer is gated, a known number of items is queued, the gate opens, and the
commit count is asserted **exactly** — 64 items at K=64 is 1 transaction, at
K=16 is 4, at K=1 is 64. Identical across runs.

**How this test can fail.** K=1 must give exactly one transaction per item; if
it does not, the commit hook is not counting transactions and nothing else
holds. That control runs *first*, before the interesting cases.

**Two harness bugs worth remembering.** The gate must be created *before* the
writer thread, and checked *after* the wait for work rather than at the top of
the loop — otherwise the writer is already parked inside the loop and the first
batch escapes the gate. And an orphaned `proc_queue.exe` from a failed run held
the binary so the next `cl` blocked, which presented as a hang in the code; kill
stale processes before diagnosing a build that stopped talking.

---

## Phase 6 — shard routing — **DONE**

**Deliverable.** Route by key to one of N database files, one writer per shard.

**Test, as built.** Routing is a pure function of the key, so it is asserted
exactly: stability (a key always lands in the same place, in range), coverage
(200 keys over 8 shards leaves none empty — measured 26/25/25/26/24/25/25/24),
and isolation (each key's row is in its own shard and in **no** other, checked
by opening every shard file independently).

**How this test can fail — and it is run, not asserted.** `coversAllShards()`
*returns* a verdict rather than asserting one, so the suite runs it against a
deliberately constant router and requires the answer to be "no". Without that,
"no empty shard" might hold because the checker cannot see an empty shard.

**"Writes to distinct shards proceed concurrently" is deliberately NOT timed.**
Phase 5 taught the sharper rule: a count is no better than a stopwatch when the
count is timing-dependent. What is asserted instead is the property that makes
concurrency possible and is exactly checkable — each shard commits its own
writes in its own transactions, so a shard with no traffic must show **zero**
commits. Measured: 50 commits on the addressed shard, 0 on every other.

---

## All six phases complete — 2026-08-03

codec → framing → TCP loopback → arguments → shape cache → group commit →
shard routing. What exists is a working spine, not a product: there is no
authentication, no TLS, no connection pooling, no backpressure policy, and the
server handles one connection at a time. Those are the next honest steps if
this ever becomes something to run.

What the six phases do establish is that the thesis holds end to end — a CALL
is the request, the declared shape is the contract, and a client that knows the
contract at build time never has to be told it.

## Open questions carried forward

1. ~~Is the per-result-set overhead declared-shape application?~~ **No** —
   refuted by allocation counting; see `DESIGN-NETWORK.md`.
2. Should the transport be a Postgres-wire shim instead of a bespoke protocol?
   A shim wins every existing client library for free and loses the shape-cache
   trick, which is the whole differentiator. Currently leaning bespoke, with a
   shim as a later compatibility layer rather than the native protocol.
3. `BEGIN CONCURRENT` + WAL2 are written and unmerged upstream. Carrying them
   is a strategy decision, not a technical one.
