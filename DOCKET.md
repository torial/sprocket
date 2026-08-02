# Docket — where this fork could go

*Candidate directions, ranked, with the reason each earns its place and what
"done" would mean. Written 2026-08-01. Committed so it survives context loss.*

Active work is `PLAN-TRANSPORT.md`. This file is what comes after, or instead.

**Companion roadmap.** Zebra's ORM ("Tack") is designed on top of this fork —
`ZEBRA_ORM_ARCHITECTURE.md` in the language project. Its §14 works each item
below from the ORM's side, so the two roadmaps are planned together. Three
couplings worth knowing before picking anything up here:

- **Authorization (#1) gates the ORM too.** Tack makes procedures a
  network-facing application surface, so the proc boundary becomes its security
  boundary. It is currently unguarded on both sides.
- **Typed client generation (#2) is the same mechanism as Tack's build-time
  pre-pass**, pointed at different languages. `PRAGMA proc_info` is the shared
  interface; do not build the introspection twice.
- **The shape-cache handshake has a stronger form for a generated client.**
  Tack knows every result shape at *build* time, so it never needs a describe
  at all — and a cookie mismatch means the binary was compiled against a
  different schema than the server runs, which is a deployment error to fail on
  rather than a cache miss to recover from.

---

## 1. Authorization for procedures — *we created this hole*

`README-PROCS.md` records it as a known limitation: there are no authorizer
action codes for `CALL`, and body statements authorize under the procedure's
context the way trigger steps do. That is a defensible footnote for an embedded
library.

It stops being a footnote the moment procedures become the **network-facing
API**, which is exactly what `PLAN-TRANSPORT.md` proposes. Then the procedure
boundary *is* the security boundary, and right now it is unguarded.

**Done means:** an `SQLITE_CALL` authorizer action; a decision about whether a
procedure runs with definer's or invoker's rights (SQL standard has both, and
the answer determines everything else); body statements authorizable
independently of the call; tests proving a denied CALL cannot be laundered
through a nested CALL.

**Priority: do this before anything listens on a socket.**

## 2. Typed client generation — *cheapest thing with the largest multiplier*

Declared shapes are static, validated at `CREATE` time, and already
introspectable via `PRAGMA proc_info`. So the full request/response contract of
a database is machine-readable **without executing anything**. Emit client stubs
— C, TypeScript, Python, and Zebra — straight from the schema.

This is what turns "SQLite has stored procedures" into "SQLite has a typed RPC
layer," and it compounds with every phase of the transport plan.

**Done means:** `sqlite3 db.sqlite ".procgen ts"` emits typed callables; a
round-trip test that generates, compiles, and calls; regeneration is
deterministic (byte-identical for an unchanged schema).

**Effort: small. Differentiation: high.**

## 3. Incremental view maintenance — *the big one, and the one I most want*

Materialized views that update as writes land rather than being recomputed.

This is the missing structural piece of the architecture in `DESIGN-NETWORK.md`.
The recommendation there is append-only ledger plus periodic rollups — which
only works if rollups maintain themselves. Today that is hand-written triggers,
every time, bespoke per schema, and wrong in the same ways each time.

It is also a genuinely hard and beautiful problem: propagating deltas through
joins and aggregates, deciding what is maintainable at all (aggregates with
inverses like SUM/COUNT are easy; MAX under deletion is not), and choosing
eager versus deferred maintenance.

**Done means:** `CREATE MATERIALIZED VIEW … WITH INCREMENTAL MAINTENANCE`; a
documented, enforced subset of maintainable expressions with a clear error
outside it; property tests asserting the view equals its from-scratch
recomputation after arbitrary write sequences — that equality is the whole
correctness contract and is beautifully testable.

**Effort: large. Value: this is the one that would make the fork worth using
for reasons unrelated to procedures.**

## 4. `wal2` and `BEGIN CONCURRENT` — *two decisions, not one*

**Corrected 2026-08-01 after actually looking.** This entry previously treated
them as a single choice. They are not, and the difference is large.

Both live in the **official** SQLite repository (`github.com/sqlite/sqlite`
mirrors Hipp's Fossil at sqlite.org/src), among ~1,718 branches. Checked
currency:

| Branch | Last merged with trunk |
|---|---|
| `wal2` | **2026-07-13** — current |
| `begin-concurrent` | **2026-07-13** — current |
| `begin-concurrent-wal2` *(combined)* | **2019-01-11** |
| `begin-concurrent-pnu-wal2` | 2023-02-02 |
| `begin-concurrent-report-wal2` | 2021-11-17 |

For reference this fork branched from trunk **2026-07-04** at 3.53.3, so each
feature branch is nine days *ahead* of our fork point on trunk. There are
periodic version pins (`wal2-3.51`, `begin-concurrent-3.51`) but none at 3.53.

**The finding: the SQLite team keeps each feature current and has not kept the
combination current since 2019.** Taking both is therefore not "carry two
upstream branches" — it is doing integration work upstream stopped doing seven
years ago. Do not plan on the pair.

### 4a. `wal2` — take this one first

Fixes checkpoint starvation: under continuous read traffic the WAL never
resets and grows without bound until latency collapses. `DESIGN-NETWORK.md`
already calls that "your production incident" — it is a problem this fork would
actually hit, whereas multi-writer is one it would only hit at scale we have
not reached.

Merge shape is favourable: `wal2` lives almost entirely in `wal.c` and
`pager.c`, **neither of which this fork touches**. Our 19 modified `src/` files
are mostly additive (`proc.c` is new) plus small hooks. Genuine overlap risk is
confined to `vdbe.c` / `vdbeaux.c` / `sqliteInt.h`, where we added opcodes and
struct fields.

### 4b. `BEGIN CONCURRENT` — decide at transport Phase 5

Multiple optimistic writers with page-level conflict detection. Also current
and mergeable, but it changes what group commit should look like, so it belongs
to the Phase 5 design conversation rather than to this one.

## 5. System-versioned temporal tables — *SQLite has no story here at all*

SQL:2011 `AS OF` / `FOR SYSTEM_TIME`. Every row carries a validity interval;
updates close the old version and open a new one; queries can ask what the
table looked like at a past instant.

SQLite has nothing, and the workaround (shadow history tables plus triggers) is
universal, hand-rolled, and subtly wrong in most implementations. Pairs
naturally with the append-only ledger pattern and with #3.

**Done means:** `AS OF` query support, automatic versioning on write, a
retention/pruning policy, and a proof that a point-in-time query equals a
from-scratch replay to that point.

## 6. Fan-out shard virtual table — *the one sharding piece that belongs in the engine*

A vtab presenting N shard files as one logical table with constraint pushdown,
so cross-shard reporting queries do not have to be written by hand. Everything
else about sharding is application architecture and explicitly does not belong
in this fork (`DESIGN-NETWORK.md`); this is the exception.

## 7. `OUT` / `INOUT` parameters — *listed in README-PROCS as planned*

Result sets cover most cases, which is why this was deferred. Worth revisiting
only if the typed-client work (#2) makes the absence awkward at the boundary.

---

## Ordering I would actually recommend

**Authorization → typed clients → incremental views**, with the transport plan
running alongside. Authorization because we opened the hole and it gates
anything network-facing; typed clients because they are cheap and multiply the
value of everything else; incremental views because that is the one that
changes what the fork *is*.

`BEGIN CONCURRENT` needs a decision before transport Phase 5 regardless of
where it sits in this list.
