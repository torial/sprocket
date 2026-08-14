# Design note: SQLite as a high-traffic web database, and where procedures fit

*Status: design note. Nothing here is implemented except the benchmark it
cites (`tool/bench_proc_roundtrip.c`). Written 2026-08-01.*

## The thesis

Stored procedures look like ceremony in an embedded database. A `sqlite3_step()`
costs nanoseconds, so bundling five queries into one `CALL` saves nothing worth
the syntax — which is a large part of why SQLite never had them.

**Put a network in front of SQLite and that calculus inverts.** Round trips
become the dominant cost of a request. The feature this fork built for
ergonomics turns out to be a *networking* feature that predates its network.

But state the argument carefully, because the obvious version of it is weak.
"One CALL replaces N round trips" is only half true: **pipelining gets most of
that for free.** A client can send N queries without waiting and read N
responses — 1 RTT, plain TCP, no procedures involved. Redis and HTTP/2 both do
exactly this. Batching alone is therefore not a strong argument for stored
procedures.

The argument that survives is **data-dependent control flow.** Pipelining
requires all N requests to be knowable in advance. The moment request 3 depends
on the *result* of request 2, the client must round trip — unless the logic
runs server-side. That is precisely what `IF`, `WHILE`, `SELECT … INTO`,
`LEAVE`, and `RAISE(ABORT)` in a procedure body are: branches no transport can
flatten.

The `pay_down` procedure in README-PROCS.md is the canonical case — read a
balance, branch on it, abort or update. Over a wire that is at minimum two
round trips *with a race window between them*; as a CALL it is one round trip
inside one statement-journal atom. No transport choice can collapse that, and
no pipeline can either.

So the honest ordering of benefits is:

1. **Data-dependent control flow** — irreducible; only server-side logic wins.
2. **Atomicity across dependent steps** — the race window above disappears.
3. **Round-trip batching** — real, but partly available via pipelining.
4. **Not sending N query texts** — minor.

This note records what we measured, what is actually required to run SQLite
under high traffic, and — importantly — which parts need no engine changes at
all, so nobody goes looking for them in here.

## The reframe worth holding onto

SQLite's single writer is usually described as its scaling limit. It is more
useful to describe it as a **total-order oracle that other databases pay
consensus protocols to obtain**. Postgres and MySQL spend real machinery
establishing a serialization point; SQLite hands you one per file, free.

The engineering task is therefore not *defeat the single writer*. It is
**shrink the domain each writer governs until one writer per domain is
abundant.**

## What we measured

The round-trip saving is arithmetic — you save `N-1` of them, and no benchmark
is needed to know that. The part that is *not* arithmetic, and the only part
an in-process benchmark can settle, is whether the engine charges us for the
privilege: **does a CALL do more server-side work than the N statements it
replaces?**

`tool/bench_proc_roundtrip.c` measures exactly that, with no network involved:

- **Path A** — N already-prepared `SELECT`s, stepped to completion, reset.
- **Path B** — one already-prepared `CALL` of a procedure with N declared
  result sets, advanced with `sqlite3_proc_next_resultset()`.

Both paths are asserted to return identical row counts *and* an identical
checksum of the data before anything is timed; a benchmark of two different
workloads is worthless. Caches are warmed, and each path runs for ≥1s.

In-memory database, 10 rows per result set, warm caches, MSVC /O2:

| N result sets | A (µs/op) | B (µs/op) | delta | break-even RTT |
|---|---|---|---|---|
| 2 | ~3.9 | ~4.1 | ±0.2 (noise) | ~0.2 µs |
| 5 | ~9.4 | ~10.3 | +0.90 | ~0.22 µs |
| 10 | ~22.6 | ~24.3 | +1.73 | ~0.19 µs |

**Findings.**

1. A `CALL` costs roughly **0.2 µs more per additional result set** than the
   loose statements it replaces. The delta is real and grows with N — it is
   not free.
2. The **break-even round-trip time is ~0.2 µs and is essentially flat in N.**
   That is the network latency above which the CALL wins no matter what.
3. For scale: loopback TCP is ~30 µs, same-datacenter ~200–500 µs, cross-region
   10–100 ms. Even loopback is **150×** above break-even; a LAN is ~1,000×;
   a WAN is ~50,000×.

So the thesis holds, and it holds by orders of magnitude rather than by a
margin. But state it precisely: *procedures do not make SQLite faster.* They
make it possible to spend one round trip where a client would otherwise spend
N, and the engine's own overhead for that is negligible against any real link.

**Honesty about the numbers.** Absolute per-op figures move ~1.7× run to run on
a loaded workstation; only the *ratio* is stable. A noisy early run put the
break-even at 1.0 µs instead of 0.2 µs — which changes nothing, since the
conclusion needs only "far below 30 µs". Treat the break-even column as the
result and the µs/op columns as context.

**A hypothesis about the per-set cost, since refuted.** The obvious suspect was
declared-shape application on each advance: `sqlite3VdbeApplyProcSet()` calls
`sqlite3VdbeSetColName()` with `SQLITE_TRANSIENT` for every column name and
decltype, which reads like two copies per column per result set.

That is wrong, and the benchmark now proves it. Timing could not settle it —
the delta is well under a microsecond per set and sits beneath the noise floor
of a loaded workstation even with minimum-of-nine interleaved rounds. So the
harness measures **allocations** instead, which is deterministic and is what
the mechanism actually predicts:

```
   N   allocations/request:      A        B      B-A
   2                          6.00     6.00    +0.00
   5                         15.00    15.00    +0.00
  10                         30.00    30.00    +0.00
```

A CALL performs **exactly the same number of allocations** as the loose
statements it replaces. `SQLITE_TRANSIENT` does not allocate here, because
`sqlite3VdbeMemGrow()` reuses `pMem->zMalloc` when the existing buffer is large
enough — the column-name Mems are reused across advances and the strings are
the same size every time, so the cost is a `strlen` plus a `memcpy` of a
ten-byte string, not a malloc.

Switching those calls to `SQLITE_STATIC` was tried and reverted: it is safe
(the strings are owned by `aProcSet` and outlive the Mems), but it buys only
those memcpys, and changing memory-management semantics in the VDBE for an
effect that cannot be measured is a bad trade.

Whatever remains of the per-set delta is therefore *not* metadata handling.
The next suspect is `OP_Program` frame setup per result set. Unresolved, and
not worth resolving until it matters against something other than a network.

**A note on the instrument.** The allocation counter initially reported the
per-request count *falling* as N rose (30 → 10), which is impossible. Cause:
the default lookaside pool (~40 slots) is exhausted by ten result sets, after
which allocations fall through to the general allocator and stop being counted
as hits. The harness now sizes lookaside far beyond what any path holds live.
A saturating counter that silently under-reports is worse than no counter.

## The three-part spine

For a genuinely high-traffic deployment, in dependency order. **Only the third
involves this fork at all.**

### 1. Group-commit write queue — *application, no engine change*

One writer thread owns each database. Request handlers push work onto a
channel; the writer drains up to K items inside **one** transaction and **one**
`fsync`. Write throughput becomes `fsyncs/sec × batch size` rather than
`fsyncs/sec`.

This is the single largest available win and it is pure application
architecture. Two engine-adjacent settings matter:

- `PRAGMA journal_mode=WAL` with `synchronous=NORMAL` — in WAL mode this
  risks losing at most the last transaction on power loss and **cannot**
  corrupt.
- Every write transaction must be `BEGIN IMMEDIATE`. The classic SQLite
  production outage is a deferred transaction that reads, then tries to
  upgrade to a write lock, and deadlocks against another doing the same;
  `busy_timeout` cannot rescue it because neither side can yield.

A dedicated checkpointer is not optional in classic WAL. Under continuous read
traffic the WAL never resets and grows without bound until latency collapses.
Monitor WAL size as a first-class metric.

**Resolved 2026-08-02: this fork now carries `wal2`.** Upstream's two-WAL-file
mode bounds WAL growth by construction — writers fill `-wal`, switch to
`-wal2`, and the first file is checkpointed *and overwritten* while nobody is
appending to it. Upstream's own summary: "wal files do not grow indefinitely
even if the checkpointer never has a chance to finish uninterrupted." Enable
with `PRAGMA journal_mode=wal2`.

Two consequences worth carrying forward:

- **A wal2 database cannot be read by stock SQLite** (`SQLITE_NOTADB`). It is
  reversible via `PRAGMA journal_mode=delete`. Verified 2026-08-02 that this
  fork still reads rollback-mode and classic-`wal` databases unchanged —
  correct mode reported, data intact, `integrity_check` ok for all three. Only
  wal2 files are one-way.
- **`sqlite3_wal_hook`'s argument changes meaning.** In wal mode it is the page
  count of the WAL; in wal2 it is the total *uncheckpointed* pages across both
  files, or 0 when the other file is empty or already checkpointed. Anything
  built on the wal-hook — notably the WAL-shipping replication described below —
  must be wal2-aware from the start rather than retrofitted.

wal2 does **not** provide concurrent writers. It bounds space and tail latency,
not throughput. Multiple writers remain `BEGIN CONCURRENT`'s job.

### 1a. The queue, designed — *sketch of 2026-08-13, decisions with reasons*

*From a design conversation with Sean; recorded before any transport code
exists so the build starts from rulings, not re-derivation.  The theme:
v1 is deliberately simple, and every deferred sophistication sits behind
an interface that does not change when it arrives.*

**v1 drains per-item: each queue item runs in its own BEGIN
IMMEDIATE/COMMIT.**  This dissolves batch failure coupling — item 7's
constraint violation rolls back item 7 and reports to waiter 7; no
savepoint choreography, no error-attribution machinery.  The classic
objection (one fsync per item) mostly does not apply at our settings:
in WAL with `synchronous=NORMAL`, COMMIT is a WAL append, not an fsync.
The batched drain (savepoint per item, one commit per batch) is the
recorded v2, adopted only if measured commit overhead demands it — and
it is invisible to callers, because the queue's interface is "submit an
item, await its result" under either strategy.

**Requests are synchronous, one in flight per connection.**  A
connection submits and blocks; it physically cannot enqueue a second.
Consequences, all free: queue depth is bounded by connection count,
each connection's writes apply in order, and backpressure is built in.
Pipelining and async submission are later versions, noted and not
designed here.

**The work unit is closed — a CALL, not a conversation.**  An open
`BEGIN … think … COMMIT` would hold the singleton writer hostage and
never enters the queue.  Procedures-as-API (part 3) is what makes this
constraint nearly free: the fork already has the closed unit.

**Queue discipline is a DATABASE mode, not connection courtesy**
(Sean's requirement, 2026-08-13; **BUILT 2026-08-14** — PLAN-QUEUE.md,
`test/queue1.test` 0/20: `PRAGMA queue_writer`/`queue_mode`, shm slot
12 + hint byte 133 inside `WalCkptInfo.notUsed0`, gate at
OP_Transaction, crash-release inherited from os lock semantics):
while any queue-writer connection has
the database, interactive write transactions on it are refused with the
reason and the fix, and the mode releases when the last queue
connection closes.  Transport-level policy cannot enforce this — an
out-of-band CLI writes straight to the file — so honest enforcement is
engine-level, and the fork already owns the exact choke point: the
runtime write gate at OP_Transaction built for degrade-at-load
(DOCKET #9).  The natural home for the cross-process flag is the
shm/wal-index region, which is visible to every connection of the file
and vanishes with the last one — giving "until the queue connections
close" and crash-recovery semantics by construction rather than by
bookkeeping.  Candidate spelling: a `queue_writer` declaration on the
transport's connections plus the gate refusing other writers while any
declarant lives.  This is the one piece of the queue that is engine
work; it is small, and it belongs to the transport campaign, not to
this sketch.

**Priorities, when they come, reorder only the waiting line.**  A
priority lane protects high-priority p99 under load, but no priority
shortens the item currently executing — head-of-line blocking is a
property of the running item.  So priority ships only alongside a bound
on item duration (chunk large deletes/updates; procs make a
bounded-transaction loop natural) and aging so low priority cannot
starve.  v1 is FIFO.

**Nothing is acknowledged before its durability point.**  The waiter
unblocks when its item's COMMIT returns (v1) or its batch's commit
returns (v2).  A crash loses only work nobody was told succeeded.

**The truth surface ships with v1, not after it**: queue depth (how
many connections are blocked on writes now), oldest waiter age (the
head-of-line detector — alarm on this, not on depth), last commit
latency and checkpoint lag (is it us or the storage), and, once
batching exists, last batch size (is the amortization real).  Served
where the operator already looks — pragma-style through the transport.

**Relation to BEGIN CONCURRENT**: this design is the pre-registered
position of DOCKET 5b.  The queue delivers the batching benefit with
zero engine changes and no conflict semantics; BC only pays when
multiple OS processes need long concurrent write transactions against
one file, and eager IVM's write amplification makes BC's page-level
optimism a conflict magnet on precisely this fork's databases.  If the
queue ships and no such multi-process demand appears, BC is never
taken.

### 2. Shard-per-tenant with key routing — *application, no engine change*

One database file per tenant/customer/room. Writer contention disappears
because domains do not overlap. Backup, restore, delete, and erasure requests
become file operations. A hot tenant migrates to its own machine by moving a
file.

Route by key so a tenant always lands on the machine holding its file
(the Durable Object model): the round trip becomes a function call and the
page cache is always warm. Cross-tenant reporting belongs in a separate rollup
store, not in this path.

### 3. Procedures as the request API — *this fork*

With 1 and 2 in place, the remaining cost is round trips, and that is what
`CALL` addresses. A screen that needs a post, its comments, and the author
becomes one call returning three declared result sets — one round trip, one
parse, one plan, one traversal of the wire.

Declared shapes matter here beyond ergonomics: because the body is checked
against the declaration at `CREATE` time, and `prepare("CALL ...")` reports the
first shape through the ordinary `sqlite3_column_*` API, a network layer can
learn a procedure's full result contract **without executing it**. That is what
lets a server generate a typed client, or validate a response, or pre-size
buffers.

## Transport choice: much less important than it looks

A recurring instinct is to reach for UDP to cut protocol overhead. The
arithmetic says otherwise, and it is worth recording so the question is not
reopened from intuition.

On a **pooled** connection with `TCP_NODELAY` set, a request/response costs
exactly 1 RTT. UDP also costs 1 RTT. TCP adds no extra round trip per request;
its handshake (1 RTT, plus 1 for TLS 1.3 unless resumed) is per *connection*
and amortizes to roughly zero under pooling. UDP saves some kernel work — no
congestion-control state, no reassembly — on the order of **2–10 µs against a
~200 µs LAN round trip.**

Against the measured numbers, on a 200 µs LAN serving a 5-result-set page:

| Approach | Approx. cost |
|---|---|
| 5 separate queries, TCP | ~1000 µs |
| 5 separate queries, UDP | ~980 µs |
| **1 CALL, TCP** | **~200 µs** |
| 1 CALL, UDP | ~195 µs |

Removing round trips saves ~800 µs; swapping transport saves ~5–20 µs. **The
round-trip count dominates transport choice by 40–80×.**

UDP does win in three specific places: cold short-lived clients that cannot
pool (serverless/edge pay the handshake every invocation); head-of-line
blocking, which is TCP's real structural flaw — one lost packet stalls every
multiplexed request behind it, including those whose bytes already arrived;
and per-connection memory at millions-of-clients scale.

It loses badly on the thing a database cares most about. **Retransmission plus
writes equals double execution:** a lost response makes the client retry, and
an `INSERT` runs twice. Correctness then requires request IDs and a server-side
dedup cache with a retention window — a transaction log with extra steps. On
top of that, results over ~1472 bytes need hand-rolled fragmentation (IP
fragmentation is a trap: lose one fragment, lose the datagram, and middleboxes
drop them), there is no congestion control to prevent collapse under retry
storms, and NAT expires idle UDP flows in ~30 s versus TCP's hours.

Rebuilding reliable, ordered, congestion-controlled delivery on UDP is exactly
the project that took QUIC most of a decade. **If the UDP benefits are wanted,
take QUIC** — it is UDP underneath with reliability, congestion control, 0-RTT
resumption, and per-stream multiplexing free of cross-stream head-of-line
blocking. The cost is a large dependency and userspace crypto CPU per packet.

**The real latency lever is not a transport at all.** Same-host Unix domain
socket is ~5–15 µs; in-process is ~0.1 µs. Routing a tenant to the machine
holding its shard — step 2 of the spine — cuts RTT by 20–40×, an order of
magnitude more than any protocol choice. Optimize placement before transport.

## What is missing for the networked case

- **A wire codec now exists** (`tool/proc_wire.c`) — framing and encoding only,
  no sockets, because that is where the design decisions live and a transport
  is mechanical. Request is a CALL; response is a frame stream
  (SHAPE / ROW / SETEND / DONE / ERROR) with bounds-checked decoding, since
  this code would face a network and a malformed frame must be an error rather
  than a read past the end.

  It implements the mode this fork uniquely permits. Because a procedure's
  result shape is fixed at CREATE time and checked against the body, it is a
  **static contract** — so a client can cache it by (procedure, schema cookie)
  and the server can then send rows carrying no schema at all. PostgreSQL must
  re-describe because an arbitrary query's shape is only known after planning;
  here it is known before the call is made. Measured on the self-test's own
  payload: **301 bytes with shapes, 196 without — 34.9% saved**, and the
  shape-free stream decodes to a bit-identical checksum.

- **There is still no transport.** Ascending in ambition:
  an HTTP/JSON front (rqlite's approach); a PostgreSQL-wire shim so existing
  client libraries work unchanged; or a purpose-built binary protocol with
  pipelining (libSQL's Hrana). Whatever the choice, **support pipelining** —
  it is cheap, it is orthogonal to procedures, and per the reframing above the
  two solve different problems.
- **`sqlite3_proc_next_resultset()` is not reachable from loadable
  extensions**, deliberately — see README-PROCS.md for the ABI reasoning. A
  network server is a statically-linked embedder, so this does not affect it.
  It would affect an attempt to build the server *as* an extension, which is
  therefore not a supported shape.
- **Replication is unaddressed here.** The two credible directions, both
  needing no engine change: WAL shipping via `sqlite3_wal_hook` (the Litestream
  model, ~200 lines to prototype, gives continuous backup and PITR — **but see
  the wal2 note above: the hook's argument means something different in wal2
  mode, and there are two files to ship, not one**); and the
  session extension's changesets, which are diffs with built-in conflict
  handlers and are the closest thing SQLite has to a replication primitive.
  Note that once writes funnel through the queue in step 1, **that queue is
  already the replication log** — consensus becomes an interception point
  rather than a rewrite.

## Non-goals, recorded so they are not re-litigated

- **Never run over NFS/SMB.** Network filesystem locking is broken in ways that
  corrupt. Replicate bytes; do not share a mount.
- **Do not use shared-cache mode.** It serializes readers. Give each connection
  a private page cache instead.
- **Do not add slots to `sqlite3_api_routines`** to support any of this. See
  README-PROCS.md.

## Open questions

1. ~~Does the per-result-set delta come from declared-shape application?~~
   **Answered: no.** A CALL makes exactly as many allocations as the statements
   it replaces; see above. The remaining suspect is `OP_Program` frame setup,
   and it is not worth chasing while the effect is invisible against any
   network.
2. Is there value in a fan-out virtual table that presents N shard files as one
   logical table with constraint pushdown? It fits SQLite's philosophy and it
   is the one piece of the sharding story that could reasonably live in the
   engine.
3. `BEGIN CONCURRENT` + WAL2 exist as SQLite branches and are unmerged. They
   allow multiple optimistic writers with page-level conflict detection and
   fix checkpoint starvation. They are the highest-leverage thing that is
   *already written* and not in trunk. Whether this fork should carry them is
   a strategy question, not a technical one.
