# DESIGN-REPL — replication: shipping the fold

*Opened 2026-08-15 on Sean's "proceed w/ the items that make the most
sense," replication being the thread every prior campaign quietly
funded: the queue serializes writes, sprocketd owns them, wal2's hook
semantics are documented, temporal built a commit log, and the tiers
essay's closing thesis is that the engine is one fold with several
readouts — so replication should ship the log that already exists
rather than invent one.  Two POCs ran before any question was drafted
(tool/repl_poc1.tcl, repl_poc1b.tcl).  Questions await Sean's rulings;
nothing below is implemented.*

## What the POCs established

**POC 1 — full-copy shipping is correct and unaffordable.**  Copying
db+wal+wal2 to the replica after each commit never diverged across 30
checkpoints including TRUNCATE checkpoints mid-stream (single writer
means inter-commit copies cannot tear, and uncommitted spilled frames
are handled by the wal format's own commit-frame rule).  But it is
O(database) per ship — backup wearing replication's clothes.

**POC 1b — incremental shipping fails silently, and not where
expected.**  Appending only new wal bytes to the replica froze it at
the FIRST ship — before any checkpoint — with no error anywhere:

1. *A replica is not a passive file.*  Merely READING it (open, query,
   close) checkpoints and truncates its wal on last-connection-close.
   The shipper's subsequent appends land after a truncation it never
   saw.
2. *Salts are the wal's generation marker.*  Frames appended across a
   reset carry the old generation's salts, and recovery IGNORES them —
   silently, by design (that is how the wal format rejects stale
   frames).  A shipper that is not salt-aware produces replicas that
   freeze rather than error: the fabricated-silence failure class.
3. The checkpoint probe (disease "a") also fired: the primary's own
   TRUNCATE resets the wal under the shipper's saved offset.
4. wal2 doubles all of this bookkeeping: two files alternating under
   a hook whose argument means "uncheckpointed across both."

Litestream solved these with GENERATIONS (salt-keyed segment lineages)
and by never letting the replica live as an openable database until
restore time.  Any physical design here inherits that shape.

## RULINGS — Sean, 2026-08-15 (partial)

- **Q1: C ruled** — session changesets, commit-sequence order, carried
  by the queue/daemon machinery.
- **Q2: ruled as leaned** — read-replicas + PITR in v1; promotion is a
  v2 campaign.
- **Q3: ruled as leaned** — `PRAGMA replica_status` (last_seq,
  last_utc, lag_source); freshness always carries a receipt.
- **Q4: C ruled** (segments canonical; subscription one delivery
  method) — WITH Sean's UNGIT caveat worked through and three
  commitments made part of the ruling:
    1. segments are self-describing (format version, database
       lineage/genesis identity, seq range, checksum) and live on disk
       where the operator already looks — nothing exists only in
       flight;
    2. APPLY REFUSES on gap, lineage mismatch, or checksum failure —
       by name, with the fix, never skipping (POC 1b's silent-freeze
       class made structurally impossible);
    3. no ambient directory magic: a replica DECLARES its source
       (subscription address or archive path), and the declaration is
       checked when declared (lineage validated at HELLO / at first
       segment).
  Sean accepted the sidecar-shipper unavailability as a tradeoff of
  Q1-C.
- **Q5: BLESSED as a test obligation** ("go forth and multiply"): the
  day a replica exists, temporal2's replay proof runs against the
  REPLICA and passes identically — replica history byte-equal to
  primary history, proven by the same instrument, in the suite
  forever.

## POC 2 (2026-08-16, during R5) — sessions vs the temporal axis

Run before building PITR, because the blessed pin (Q5) depends on
history riding the changesets.  Findings, with a scar:

1. **My first instrument lied, and I briefly believed it.**  The
   changeset-dump helper deduplicated table names wrongly and silently
   stopped iterating; I concluded "sessions skip sqlite_* tables" and
   began designing around a limitation that did not exist.  Arithmetic
   implausibility (83 bytes is too many for one small row) forced a
   hexdump, which showed the capture had been fine all along.  Kethiv:
   sessions skip sqlite_-prefixed tables.  Qere: they never did — the
   dump was broken.  The no-instrument-trusted-un-red rule exists for
   exactly this.
2. **Sessions capture trigger-context writes** (marked indirect),
   including our synthesized capture triggers' writes to
   sqlite_hist_commits — the commit clock ships as VALUES with no
   engine change at all.
3. **What did NOT ship, and why**: sqlite_hist_<name> had no PRIMARY
   KEY (the session module's documented no-PK ignore), and hidden
   columns are excluded from the session's column model — so history
   rows lacked their intervals.  Both were OURS to fix: shadows now
   declare PRIMARY KEY(base-key, seq_from) — their natural version
   key, which also gives AS OF an index — and the fork's session
   includes explicitly-hidden (h==1) columns while still excluding
   generated ones.
4. **sqlite3changeset_apply silently ignores missing tables and
   too-few-column tables** — measured, and exactly the fabricated
   success Q4's refuse-never-skip forbids.  The applier now pre-scans
   the manifest and refuses by name; and apply runs as a MAINTENANCE
   context (DBFLAG_TemporalMaint): replica shadows are writable to the
   stream, and the replica's own capture triggers stand down so the
   primary's history arrives verbatim instead of being re-stamped.

The Q5 pin lives: test/repl2.test runs temporal2's replay proof
against a replica built from per-commit changesets, corruption control
included, wired into the session permutation.

## The questions for rulings

### Q1 — Physical or logical?

- **A. Physical: wal-frame shipping**, salt/generation-aware, wal2's
  two files handled.  Byte-exact replicas: temporal history, commit-
  log UTC values, rowids — identical by construction.  The cost is
  the litestream-class machinery above, doubled for wal2.
- **B. Logical: workload shipping** through the pieces we own — the
  daemon already serializes every write as a CALL stream; ship THAT
  and replay it on the replica.  The replay proof (temporal2's shape)
  is the correctness instrument, and the queue is already the
  serialization point.  **The measured tension: replayed writes
  re-stamp `strftime('now')` — a replica's temporal commit-log UTC
  diverges from the primary's** (the seq axis stays identical; the
  chart diverges).  Any nondeterministic function in a proc body
  diverges likewise.  Logical replication needs either a determinism
  fence (the mview conformance walk knows how) or shipped-value
  substitution.
- **C. Session changesets** (the Q3-B′ layer): logical at the ROW
  level rather than the statement level — deterministic by
  construction (ships values, not expressions), conflict-handling
  built in, but per-row volume and no free total order (the queue
  would supply one).

My lean: **C carried by the queue** — row-level changesets generated
at the primary (deterministic, value-based, immune to the 'now'
divergence), shipped in commit-sequence order through the transport
we already have, applied under the replay-proof instrument.  A is the
fallback if byte-exactness is ever a hard requirement; B is the one I
would decline (the determinism fence would constrain proc authors
retroactively).

### Q2 — v1 scope: what is a replica FOR?

Read-replicas (live, queryable, lagging) vs PITR/backup (restore to a
moment) vs failover (promotion).  My lean: **read-replicas + PITR in
v1** (they share the shipped stream; PITR is the stream plus temporal's
own AS OF), promotion recorded for v2 (it needs fencing decisions that
deserve their own campaign).

### Q3 — The staleness surface (UNGIT)

A replica that cannot say how far behind it is fabricates freshness.
Lean: the replica knows the last applied commit seq and its UTC (the
commit log ships with the stream); `PRAGMA replica_status` reports
(last_seq, last_utc, lag_source).  Nothing answers "fresh" without a
receipt.

### Q4 — Transport integration

The stream rides sprocketd's protocol (a subscription surface beside
CALL) vs a separate shipper process vs file-based segments (litestream
style, object-storage friendly).  Lean: **daemon subscription** for
live replicas, file segments recorded as the PITR/backup variant —
they are the same bytes at different addresses.

### Q5 — Temporal interplay

Under Q1-C the seq axis replicates exactly; the commit-log UTC ships
as VALUES (no re-stamping), so the chart replicates exactly too.
This is the strongest argument for C over B, and it should be pinned
by a test the day one exists: replica temporal2 == primary temporal2.

## Not in scope until ruled

Everything.  This document plus the two POCs is the campaign so far.
