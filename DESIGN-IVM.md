# DESIGN-IVM — incremental view maintenance: the six decisions, with scenarios

*Written 2026-08-12 (Fable), expanding DOCKET #4's design questions into
pros/cons and worked examples at Sean's request. The running example is
the docket's own motivating pattern (DESIGN-NETWORK's append-only ledger
+ rollups), with Graze (read-heavy docs site), Mosaic (witness/claims),
and bugbook (status counts) as the live consumers.*

## RULINGS — Sean, 2026-08-12 (design is now DECIDED; plan in PLAN-IVM.md)

- **Q1: the per-view CHOICE (scenario C)** — `WITH MAINTENANCE EAGER`
  (the default) `| DEFERRED`. Overrules the eager-only recommendation,
  with the structural constraint intact: DEFERRED means *declared
  staleness plus an explicit refresh verb*, never a read-side fold. The
  two-paths cost shrinks by construction (recorded in the plan): ONE
  capture path — the same triggers — with two application schedules,
  inline vs logged-and-folded, rather than two capture mechanisms.
- **Q1 addendum, index auto-creation:** detection is wanted and cheap
  (the planner already knows — the R7 instrument). Creation stays
  ADVISORY: the advisory names the exact `CREATE INDEX` statement;
  auto-creation, if ever, is an explicit opt-in clause, never ambient.
- **Q2: Tier 1 for v1**, Tier 2 (MIN/MAX with the advisory, inner
  equi-joins) as the anticipated follow-on.
- **Q3: A — generated internal triggers**, with the proc-body-cache
  tradeoff DOCUMENTED PROMINENTLY, so choosing a materialized view is
  done with the cost in hand (a write-procedure touching a maintained
  table compiles per statement; `PRAGMA proc_check` names it).
- **Q4: A — the FTS5-convention shadow table.** "An easy way to leverage
  all of the existing SQLite machinery / quality."
- **Q5: as recommended** — `PRAGMA view_check` ships in the same commit
  as the feature; the acceptance gate is a randomized write storm
  leaving it empty.
- **Q6: follows Q3** — the cache exclusion is accepted and documented.

## The running example

```sql
CREATE TABLE ledger(
  id INTEGER PRIMARY KEY,
  account TEXT NOT NULL,
  amount  INTEGER NOT NULL,
  at      TEXT NOT NULL
);

CREATE MATERIALIZED VIEW balances AS
  SELECT account, sum(amount) AS balance, count(*) AS entries
    FROM ledger
   GROUP BY account;
```

The correctness contract, everywhere below: **at any observation point,
`balances` equals its from-scratch recomputation.** Every design choice is
a choice about *when* observation points occur and *who pays* between them.

---

## Q1. Eager or deferred maintenance?

### Scenario A — eager: maintain inside the writer's transaction

```sql
BEGIN;
INSERT INTO ledger(account, amount, at) VALUES('alice', 50, '...');
-- the engine, same transaction, effectively:
--   UPDATE balances SET balance = balance + 50, entries = entries + 1
--    WHERE account = 'alice';        (upsert when the group is new)
COMMIT;
-- any reader, immediately: SELECT balance FROM balances → exact
```

**Pros.** Reads are always exact and always free — the view is just a
table. The equality contract is at its simplest: *equal at every commit*,
no staleness vocabulary needed. Atomicity is inherited — a rollback undoes
the delta with the write, and the statement-journal semantics the 3d fix
straightened apply to maintenance writes like any writes. Remote
consumers (the transport's CALL-as-request) never see a stale rollup
after a write RPC.

**Cons.** Writers pay for every view whether or not anyone reads it — ten
views over `ledger` is ten maintenance passes per write statement. A
maintenance failure (constraint on the shadow table, OOM mid-delta)
aborts the *user's* write: views become constraints on writes. Write-hot
tables amplify: a bulk import of 100k ledger rows does 100k group updates
unless the delta application is batched per-statement (it must be — see
the mitigation below).

**Mitigation that changes the arithmetic:** apply deltas **per statement,
not per row** — group the statement's changes first
(`SELECT account, sum(amount), count(*) FROM <delta rows> GROUP BY
account`), then apply one upsert per *touched group*. The bulk import
becomes 100k appends + one grouped pass + ~10k group upserts, not 100k
upserts.

### Scenario B — deferred: mark dirty, fold later

```sql
-- 500 writes arrive; balances is untouched, only marked stale.
PRAGMA view_refresh(balances);   -- or: fold at the next write-commit
-- one grouped pass applies all 500 deltas: ~N-touched-groups upserts
```

**Pros.** Writers keep append speed — the whole point of the append-only
ledger. Batching amortizes beautifully (the 500-write example costs one
pass). Refresh can ride an existing cadence (wal2 checkpoint, a cron
CALL).

**Cons — and one of them is structural, not a tuning matter.** In
SQLite's transaction model **a read transaction cannot write**, so
"fold in the pending deltas when someone reads" would require the read
path to take a write lock — upgrade-deadlock territory, `SQLITE_BUSY` on
a `SELECT`, and a read that sometimes stalls for someone else's backlog.
The honest deferred design therefore serves **declared staleness**:
reads see the last refreshed state, and the staleness must be queryable
(a `last_refreshed` / pending-delta count surface), or the view silently
lies. First-reader-pays is not actually available; it degrades to
*refresher*-pays, and someone must be the refresher.

**A correction to the docket note that seeded this question:** it said
the read-heavy consumers argue for deferred. Backwards. **Read-heavy is
exactly where eager shines** — the write surcharge lands on rare events
and every one of the many reads is free and exact. Deferred earns its
keep on *write-hot* tables with tolerant readers (metrics, logs), which
is not what Graze or Mosaic are.

### Scenario C — per-view choice

```sql
CREATE MATERIALIZED VIEW balances ... WITH MAINTENANCE EAGER;
CREATE MATERIALIZED VIEW hourly_stats ... WITH MAINTENANCE DEFERRED;
```

Matches workload per view; costs two engine paths forever — the docket's
own 3-D lesson ("doubles the engine's paths, which is the cost that keeps
compounding") applies verbatim.

**Recommendation (moderate-high confidence):** v1 eager with per-statement
grouped delta application. Add `PRAGMA view_refresh` later as the *only*
deferred spelling — an explicit verb, never an implicit read-side fold —
if a write-hot consumer ever materializes. Never fold on read.

---

## Q2. The maintainable subset — where to draw the line

The theory divides aggregates by whether a delete can be applied without
rereading the base table:

### Tier 1 — self-maintainable, proposed v1

`COUNT(*)`, `COUNT(x)`, `SUM(x)`, `TOTAL(x)`, and `AVG(x)` (stored as
SUM+COUNT, quotient emitted on read); `WHERE` (filters the delta before
application); `HAVING` (visibility over maintained groups); deterministic
expressions in keys and arguments; single base table.

**AMENDMENT 2026-08-12 (P3 implementation; kethiv above, qere here):
HAVING moves to Tier 2.**  This section placed HAVING in Tier 1 as mere
"visibility over maintained groups", and implementation showed that
reading is wrong: a group that fails HAVING must be ABSENT from the
stored table (reads are ordinary table reads) while its aggregates KEEP
ACCUMULATING so it can reappear.  Under the ruled storage model — the
row IS the storage (Q4 + the hidden-columns ruling) — an absent group
has nowhere to accumulate.  v1 therefore refuses HAVING at CREATE, by
name, with the fix (filter at query time over the maintained view).
The Tier-2 path when wanted: on a delta for an absent group, rescan
that one group from the base to rebuild its accumulators — the same
cost class as MIN/MAX-on-delete, served by the same R7 index advisory.

```sql
-- insert (alice, 50):  balance += 50, entries += 1     -- O(1)
-- delete (alice, 50):  balance -= 50, entries -= 1     -- O(1)
-- entries hitting 0 deletes the group row              -- COUNT is the
--                                                         group's liveness
```

The subtlety worth naming: `SUM` alone cannot tell "group sums to zero"
from "group has no rows" — maintaining `COUNT(*)` alongside is mandatory
even when undeclared, which is why AVG is nearly free.

### Tier 2 — maintainable with help

**MIN/MAX.** Inserts are a comparison; *deleting the current extremum*
forces a per-group rescan:

```sql
-- view: SELECT account, max(amount) AS biggest FROM ledger GROUP BY account
-- DELETE the row holding alice's max (90):
--   SELECT max(amount) FROM ledger WHERE account='alice'   -- the rescan
-- with INDEX ON ledger(account, amount): one seek.  Without: full group scan
-- per qualifying delete.
```

The fork already owns the right surface for this: the **R7 index-advisory
channel**. Accept MIN/MAX, and advise (or refuse — decide) when no index
makes the rescan a seek. Advisory keeps "the author may know better"
(ten-row tables); refusal keeps the cost model honest. My lean: accept
with the advisory, same as nested-shape correlations.

**Inner equi-joins.** Delta algebra is textbook —
`Δ(A⋈B) = ΔA⋈B_old ∪ A_new⋈ΔB` — and each delta row needs one probe of
the other side:

```sql
-- view: SELECT p.id, count(c.cid) FROM posts p JOIN comments c ON c.post_id=p.id GROUP BY p.id
-- INSERT a comment: probe posts by id (PK seek), update one group.  O(1)
-- DELETE a post:    probe comments by post_id -- needs the index, advisory again
```

### Tier 3 — expensive machinery, defer

`COUNT(DISTINCT x)` needs a per-group multiset (a hidden
`ivm$balances$distinct(account, x, cnt)` shadow); outer joins flip
null-extended rows on membership changes (a comment's arrival must
*retract* the null-extended post row); both are real designs, neither is
v1.

### Refused by name, possibly forever

Non-deterministic functions anywhere (`random()`, `datetime('now')` —
the view's meaning would drift without any base change), window
functions, recursive CTEs, `LIMIT` in the definition. The refusal is the
UNGIT shape — at CREATE, naming the construct:

```
Error: balances_today cannot be incrementally maintained:
GROUP BY date('now') is non-deterministic; a maintained view's
definition may not depend on when it is evaluated
```

**Recommendation (high confidence on the tiers, moderate on
MIN/MAX-in-v1):** v1 = Tier 1. First follow-on = MIN/MAX with the
advisory, then inner joins. The property-test harness (Q5) is what makes
each tier promotion cheap to trust.

---

## Q3. Delta capture — triggers underneath, or a write-path hook?

### Scenario A — generated internal triggers

`CREATE MATERIALIZED VIEW` compiles three hidden triggers on `ledger`
(INSERT/UPDATE/DELETE) doing the grouped upserts.

**Pros.** The trigger machinery's DML coverage is battle-tested upstream
— upsert, RETURNING, FK cascade actions, conflict clauses all fire
triggers correctly, and the fast paths that would *bypass* row-level
visibility (the xfer optimization for `INSERT INTO x SELECT ...`, the
truncate optimization for un-WHERE'd `DELETE`) **disable themselves when
triggers exist** — correctness arrives by default. Implementation is
mostly codegen sugar over shipped parts. And it is *visible*: EXPLAIN
shows the maintenance program, which is the honest place for the cost to
live.

**Cons.** Interactions: trigger-depth limits, `recursive_triggers`
semantics, ordering against user triggers on the same table. The
hidden triggers must be undroppable-by-accident and excluded from
`.dump` in favor of the view's own DDL. Losing the xfer/truncate fast
paths is the price of correctness (the hook approach could keep them).
**And one interaction that is specifically ours: a procedure body
writing a maintained table now fires a trigger, and bodies that fire
triggers are excluded from the body cache (`PROC_CACHE_SUBPROG`)** —
every write-proc touching a maintained table drops to per-statement
compilation. The cost is at least *visible* (`PRAGMA proc_check` names
it), but it is real, and it couples this choice to Q6.

### Scenario B — a write-path hook (vdbe/btree layer)

**Pros.** Uniform capture below codegen; the fast paths could stay
enabled and stream their deltas; no trigger-limit entanglement; no proc
cache exclusion.

**Cons.** The most invasive option — the hottest core paths, maximal
upstream-merge drift, and it reimplements change description that
triggers get for free.

### Scenario B′ — build on the preupdate hook / session machinery

Upstream already ships change capture: `SQLITE_ENABLE_PREUPDATE_HOOK`
and the session extension, which describe row-level deltas well enough
to build changesets. IVM becomes "apply the changeset delta to the
views."

**Pros.** Upstream maintains the capture layer; the same delta stream is
the natural changefeed for DESIGN-NETWORK's replication ambitions — one
mechanism, two features.

**Cons.** The hook is per-connection and single-tenant (composition with
a user's own preupdate hook needs a mux); it is C-level machinery being
given SQL-level semantics; the session extension is a real dependency.

**Recommendation (moderate confidence):** A for v1 — shipped semantics
and self-disabling fast paths buy correctness-by-default, and the proc
cache interaction is priced and visible. Revisit B′ the day replication
work starts, since it would then pay for itself twice.

---

## Q4. Where does the materialization live?

### Scenario A — a real shadow table

`balances` resolves to a hidden `ivm$balances` with a rootpage — the
FTS5 shadow-table convention, which is the strong precedent.

**Pros.** Crash story is WAL's, checkpointing is free, cold start is
instant, size is accounted like any table — and, quietly the biggest
win: **you can `CREATE INDEX` on a materialized view**, because it *is*
a table.

**Cons.** Schema-table presence (naming convention, `.dump`/restore
discipline: emit the view DDL, decide data-vs-refresh on restore),
and a persisted format is a commitment.

### Scenario B — in-memory, rebuild on open

**Pros.** No format commitment; trivially correct after crash.

**Cons.** The cold-start recompute lands on exactly the wrong consumer:

```
-- Graze docs site, 1M-row ledger, 10k groups:
-- shadow table:      first query after open: one seek
-- rebuild-on-open:   full GROUP BY scan of 1M rows before first render
```

**Recommendation (high confidence):** A, on the FTS5 convention.

---

## Q5. The equality oracle as a first-class surface

`PRAGMA view_check(balances)` — recompute from scratch inside one
snapshot, diff against the materialization, report **rows**, not a
boolean:

```
account | stored_balance | computed_balance
alice   | 140            | 190
-- plus always a summary row: groups compared, rows scanned --
-- so "no differences over 0 rows" can never read as a clean bill
```

**Pros.** The correctness contract becomes a user-facing instrument from
day one — the property harness for development (`after arbitrary write
sequences, view_check is empty` is the whole test), and the 2am tool in
production. Costs a full recompute, which is fine: it is
`integrity_check`-class by declared intent.

**Cons.** Essentially none on *whether* — only on making its cost
legible and keeping the snapshot consistent (run both sides in one
statement). Under any future deferred mode it must refuse to say "clean"
about a stale view without saying "stale" first.

**Recommendation (as close to certain as design gets):** ship it with
v1, and make the v1 acceptance test literally be `view_check` staying
empty under a randomized write storm.

---

## Q6. Interactions with procedures

Three concrete scenarios, one real coupling:

**A proc writes a maintained table (eager + trigger capture):**

```sql
CREATE PROCEDURE post_entry(acct TEXT, amt INTEGER) RETURNS NOTHING
BEGIN
  INSERT INTO ledger(account, amount, at) VALUES(acct, amt, datetime('now'));
END;
CALL post_entry('alice', 50);
-- maintenance trigger fires inside the body's INSERT: balances exact at COMMIT.
-- BUT: the body now embeds a trigger subprogram → PROC_CACHE_SUBPROG →
-- this procedure compiles per statement instead of hitting the body cache.
```

Correctness composes (the 3d opcode-derived `mayAbort` even accounts for
the maintenance writes automatically); the *cost* is the cache
exclusion, visible via `proc_check` but real. This is the strongest
argument anyone will ever make for Q3-B′, or for teaching the body cache
to carry trigger subprograms (the known refcount work its exclusion
comment describes).

**A proc reads a view mid-transaction (eager):** exact, including its own
transaction's writes — a body that inserts then `SELECT balance` sees
the updated number. Under any deferred mode this becomes
staleness-inside-your-own-transaction, which is disqualifying for
deferred-by-default all by itself.

**Views as procedure sources:** a materialized view is a table, so
declared shapes, nested tables correlated on it, `RETURNING *`, and
`procgen` clients all work over it with nothing new — rollups become
typed RPC surfaces for free, which is rather the point of the whole
fork.

---

## The proposed v1 cut, in one paragraph

Eager maintenance with per-statement grouped delta application; Tier-1
aggregates (COUNT/SUM/TOTAL/AVG, WHERE/HAVING, deterministic
expressions, single table) with everything else refused by name at
CREATE; capture via generated internal triggers; materialization as an
FTS5-convention shadow table; `PRAGMA view_check` shipped in the same
commit as the feature, with a randomized-write-storm property test as
the acceptance gate. First follow-ons, in order: MIN/MAX with the R7
advisory, `PRAGMA view_refresh` as an explicit deferred verb, inner
equi-joins. The decisions that most deserve Sean's eyes before code:
Q1's never-fold-on-read stance, Q3's acceptance of the proc-cache
exclusion, and whether MIN/MAX makes v1.
