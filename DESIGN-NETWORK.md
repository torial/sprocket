# Design note: SQLite as a high-traffic web database, and where procedures fit

*Status: design note. Nothing here is implemented except the benchmark it
cites (`tool/bench_proc_roundtrip.c`). Written 2026-08-01.*

## The thesis

Stored procedures look like ceremony in an embedded database. A `sqlite3_step()`
costs nanoseconds, so bundling five queries into one `CALL` saves nothing worth
the syntax — which is a large part of why SQLite never had them.

**Put a network in front of SQLite and that calculus inverts.** Round trips
become the dominant cost of a request, and a `CALL` that returns several
declared result sets collapses N round trips into one. The feature this fork
built for ergonomics turns out to be a *networking* feature that predates its
network.

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

**An optimization target this exposed.** The per-set cost is plausibly the
declared-shape application on each advance (`sqlite3VdbeSetColName` and
friends re-doing column metadata per result set). If someone wants the delta
near zero, that is where to look first — not in the SubProgram machinery.

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

A dedicated checkpointer is not optional. Under continuous read traffic the
WAL never resets and grows without bound until latency collapses. Monitor WAL
size as a first-class metric.

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

## What is missing for the networked case

- **There is no wire protocol.** This is the real gap. Ascending in ambition:
  an HTTP/JSON front (rqlite's approach); a PostgreSQL-wire shim so existing
  client libraries work unchanged; or a purpose-built binary protocol with
  pipelining (libSQL's Hrana).
- **`sqlite3_proc_next_resultset()` is not reachable from loadable
  extensions**, deliberately — see README-PROCS.md for the ABI reasoning. A
  network server is a statically-linked embedder, so this does not affect it.
  It would affect an attempt to build the server *as* an extension, which is
  therefore not a supported shape.
- **Replication is unaddressed here.** The two credible directions, both
  needing no engine change: WAL shipping via `sqlite3_wal_hook` (the Litestream
  model, ~200 lines to prototype, gives continuous backup and PITR); and the
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

1. Does the per-result-set delta come from declared-shape application, as
   guessed above? A profile would settle it, and if so the fix is cheap.
2. Is there value in a fan-out virtual table that presents N shard files as one
   logical table with constraint pushdown? It fits SQLite's philosophy and it
   is the one piece of the sharding story that could reasonably live in the
   engine.
3. `BEGIN CONCURRENT` + WAL2 exist as SQLite branches and are unmerged. They
   allow multiple optimistic writers with page-level conflict detection and
   fix checkpoint starvation. They are the highest-leverage thing that is
   *already written* and not in trunk. Whether this fork should carry them is
   a strategy question, not a technical one.
