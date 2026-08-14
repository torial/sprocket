# DESIGN-TEMPORAL — system-versioned tables (DOCKET #6)

*Written 2026-08-15 on Sean's "go as your hankerings lead you."  Three
POCs ran before any question was drafted (tool/temporal_poc1.tcl,
poc2.tcl, poc2b.tcl); their receipts are inlined where they bear.  The
questions await Sean's rulings, DESIGN-IVM style; nothing below is
implemented.*

## What the POCs established before design began

**POC 1 — the hand-rolled pattern has two clock-borne diseases, and
triggers cannot cure either.**  The universal workaround (shadow
history + AFTER triggers stamping wall-clock) was built and probed:

1. *Intra-transaction states become historical "facts."*  Two UPDATEs
   inside one transaction left the intermediate value as a 1ms-wide
   interval — `AS OF` that millisecond answers a state **no committed
   snapshot ever contained** (measured: bal=200 lived at
   23:13:19.827–.828 inside an uncommitted transaction).
2. *The stamp-to-commit window.*  A row stamped `.828` committed at
   `.965`; for 137ms the history asserts a state every live reader
   could refute.  Wall-clock `AS OF` inside such windows answers
   states that were never observable.

Both diseases share one cure: **system time is COMMIT time — one stamp
per transaction, only the transaction's final per-row states enter
history.**  Triggers run mid-transaction and cannot know the commit
moment or survive to it; only the engine stands at the commit point.
This single fact shapes Q1 and Q3.

**POC 2 — the obvious proof instrument is circular.**  Replay-of-the-
history-table vs lookup-of-the-history-table both read the same rows;
a deliberately corrupted interior state passed every check, including
`asof(now)==live` (the corruption sat in an interval later overwritten).
POC 3 of the IVM campaign, reincarnated: a check drawn from the same
source as the data cannot validate it.

**POC 2b — the honest proof, seen red.**  Replay the WORKLOAD, not the
history: per checkpoint k, a fresh database applies statements 1..k and
its live state is diffed against `AS OF k` on the temporal side —
independent by construction.  Equality held at every checkpoint, and
the control (the same interior corruption POC 2 missed) went red.
This is the done-means instrument, proven before the feature exists.

## The questions, with scenarios and my recommendations

### Q1 — What is system time?

The axis `AS OF` resolves against.

- **A. Statement wall-clock** (the universal workaround).  Ruled out
  by POC 1 unless we accept answering states nobody ever saw.
- **B. Commit sequence, primary; wall-clock as a commit-log mapping.**
  Every committing transaction that touched a versioned table gets the
  next monotonic sequence number; a small commit log records
  (seq, wall-clock-at-commit).  `AS OF <seq>` is exact; `AS OF
  <timestamp>` resolves through the log to the last commit at-or-before
  that instant — and therefore only ever answers **states that were
  genuinely current** at some moment.  In a single-writer engine,
  commit order and write-lock order agree, so the sequence is also the
  wal order — the total-order oracle DESIGN-NETWORK already prizes.
- **C. Wall-clock captured at commit.**  Honest, but two commits in
  the same millisecond collide, and clock regressions (NTP, DST
  mishandling) corrupt the axis.  B keeps wall-clock as a *view* of
  the sequence instead of the truth.

**Recommend B.**  Scenario that decides it: a backup restored on a
machine with a skewed clock continues appending history; under C the
axis folds back on itself; under B the sequence marches on and the log
merely records what the wall said, honestly.

### Q2 — Where does history live?

- **A. Shadow history table per versioned table** — a real table
  (the mview precedent: ordinary storage, indexable, .dump-able,
  integrity_check for free), holding the row image plus
  `(seq_from, seq_to)`.  Name in the protected namespace:
  `sqlite_hist_<table>` (the `sqlite_ivm_` precedent).
- **B. In-row intervals in the main table** (one table, WHERE-filtered
  current view).  Every live query pays the filter; indexes bloat with
  dead versions; VACUUM semantics get strange.  SQL Server tried both
  and moved to shadow.
- **C. A single global history table** (entity-attribute-value-ish).
  Type fidelity dies (the IVM POC 1 lesson: BLOBs refuse JSON-shaped
  storage; EAV is worse).

**Recommend A.**  The current table stays exactly a table — reads pay
nothing (the inertness pin) — and history is queryable SQL, not a
format.

### Q3 — What writes history?

- **A. Synthesized triggers** (the IVM machinery).  Ruled out by POC 1
  — mid-transaction stamping is the disease, not an implementation
  detail of it.
- **B. Engine-native capture at commit**: the engine buffers each
  versioned table's per-row FINAL images during the transaction
  (last-write-wins in the buffer; the machinery is preupdate-shaped,
  which DESIGN-IVM Q3-B′ already noted becomes self-funding the day
  replication starts) and applies them to the shadow table with the
  new commit seq, inside the same commit — atomic with the data,
  rolled back with the data.
- **C. WAL-frame mining after commit.**  Deferred, complex, and the
  history lags the data it describes.

**Recommend B.**  The Q3-B′ note was written for exactly this moment.

### Q4 — The grammar

- **DDL:** `CREATE TEMPORAL TABLE t(...) WITH SYSTEM VERSIONING` —
  the spelling degrade1.test has been *planting as the future* since
  2026-08-13; making the fixture's fiction real is exactly the lineage
  story #9 built.  Keywords non-reserved (the fork's standing rule).
- **Query:** SQL:2011 table-suffix form,
  `SELECT ... FROM t FOR SYSTEM_TIME AS OF <expr>`, where expr yields
  a seq (integer) or timestamp (text) per Q1-B.  Per-table-reference,
  so a join may mix moments deliberately.
- `DROP TEMPORAL TABLE` drops history with the table (one object to
  the user); a bare `DROP TABLE` on a versioned table refuses with the
  reason and the fix — history is data someone chose to keep, and
  dropping it silently is the opposite of this fork.

**Recommend as stated**; the open sub-choice is whether
`FOR SYSTEM_TIME BETWEEN x AND y` (range form) lands in v1 or waits.
I lean waits — AS OF is the load-bearing 90%.

### Q5 — Schema evolution over history

The thorniest.  History rows have the shape their era had.

- **v1 recommendation: `ADD COLUMN` allowed** (absent-in-history reads
  NULL — honestly: the column did not exist, and NULL is SQL's absent);
  **every other shape change refuses** on a versioned table, naming
  the reason and the fix (the DROP/RENAME refusal idiom from mviews).
  Recorded for v2: era-stamped schema in the history table, which is
  a real design, not a patch.

### Q6 — Retention

- `PRAGMA history_prune(table, <upto-seq-or-time>)` — explicit, never
  automatic (the view_refresh precedent: the cost has an owner).
  Pruning writes a WATERMARK into the history table's metadata, and
  **`AS OF` earlier than the watermark REFUSES** — "history pruned
  before seq N; the answer would be fabricated" — rather than
  answering from partial history as every hand-rolled version does.
  Nothing fabricated; the UNGIT clause that matters most here.

### Q7 — Interactions, declared up front

- **Version skew:** temporal tables inherit degrade-at-load (#9) by
  construction — an older fork build opens the file, dead-marks the
  temporal table, reads everything else.  This was the ruling's
  stated purpose; it now gets its first real tenant.
- **Materialized views over versioned tables:** v1 refuses by name
  (the maintenance capture and the history capture are separate
  machineries; composing them — time-travel mviews — is a real
  future feature, not a v1 accident).
- **Procs:** `FOR SYSTEM_TIME` inside bodies works for free (it is
  query surface); the conformance walk needs no change.
- **Queue mode / daemon:** orthogonal; history capture rides the same
  commit the queue already owns.
- **Replication:** the commit log of Q1-B is a replication-shaped
  artifact; noted, not built.

### Q8 — The proof (done-means)

POC 2b becomes the permanent spec instrument: a workload with
checkpoints; per checkpoint, from-scratch replay diffed against
`AS OF`; the corruption control stays in the suite so the instrument
is never trusted un-red.  Plus POC 1's two diseases pinned as tests
that the ENGINE version cannot exhibit: intra-transaction intermediate
states must be absent from history, and no `AS OF` answer may name a
state no committed snapshot contained.

## Sequencing sketch (after rulings)

Q0 red spec → P1 commit-seq + commit log → P2 capture buffer + shadow
apply → P3 DDL + degrade-fixture graduation → P4 AS OF query rewrite →
P5 prune + watermark → P6 proof harness + gates (sweeps, veryquick both
regimes — the loader lesson) → docs.
