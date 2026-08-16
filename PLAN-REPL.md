# PLAN-REPL — replication: segments of the fold

*Written 2026-08-15, all five DESIGN-REPL questions ruled (Sean).  The
record is `git log`.  Spec-first: each phase ships with its instrument,
and the campaign's crown is the blessed pin — temporal2's replay proof
running against a REPLICA.*

## The ruled shape

Session changesets (Q1-C), cut into **self-describing segments**
(format version, lineage/genesis identity, commit-seq range, checksum),
in commit-sequence order supplied by the single writer.  Segments are
the canonical substance (Q4-C): the daemon serves them to live
subscribers, the same bytes archive for backup, and PITR is "apply
through seq/UTC."  Read-replicas + PITR in v1; promotion is v2 (Q2).
`PRAGMA replica_status` gives (last_seq, last_utc, lag_source) —
freshness always carries a receipt (Q3).  The three UNGIT commitments
of the Q4 ruling are load-bearing: self-description, refuse-never-skip,
declared sources.

## Component placement (decided here, Sean may veto)

The segment WRITER and APPLIER are a small C library
(`tool/sprocket_repl.c`, the transport-harness discipline) over the
session extension, used by BOTH sprocketd (live) and in-process
embedders like Graze (archive-only, no daemon needed).  Engine changes
in v1 are limited to `PRAGMA replica_status` — the fork's surface
stays lean while the format proves itself; promotion of the library
into the engine is a recorded v2 question.

## Phases

- **R0 — the segment contract, pinned red.**  A self-test harness
  (`sprocket_repl --selftest`) whose expectations are written before
  the code: header fields exact; checksum verified; gap refusal by
  name; lineage refusal by name; a segment is byte-identical when
  re-cut from the same changeset (determinism).
- **R1 — the writer**: session-based capture on the write connection,
  segment cut per commit (live path) and batched (archive path),
  genesis identity minted at first cut.
- **R2 — the applier**: ordered apply with the three refusals;
  `replica_status` file-side state.
- **R3 — the pragma**: `PRAGMA replica_status` reading the applier's
  receipts.
- **R4 — daemon subscription**: sprocketd serves segments over the
  ruled protocol (a subscribe request beside CALL); catch-up = archive
  fetch then resume, one code path.  *Design note (2026-08-16): the
  archive IS the one code path.  The writer cuts a segment per commit
  into a declared archive directory (one file per seq, self-describing
  bytes); the subscriber loop always serves seq N from the archive
  file, waiting on the cut event when N does not exist yet.  Catch-up
  and live-tail are the same loop by construction, and the daemon
  serves the same bytes the operator sees on disk.*
- **R5 — PITR**: apply-through-seq/UTC, composed with temporal's own
  AS OF where both exist.
- **R6 — the blessed pin + gates**: temporal2 run against a replica,
  byte-equal history; sweeps + veryquick both regimes + Linux (the
  standing lesson).
- **R7 — docs**: README-REPL, DOCKET entry, DESIGN-NETWORK cross-refs.

## Controls

- POC 1b's frozen-silent replica is the negative control: every R2
  refusal must be SEEN RED in the harness before it is trusted.
- The determinism pin (same changeset → byte-identical segment) is
  what keeps archives comparable across time and machines.
- Nondeterministic proc bodies are NOT fenced (Q1-C ships values);
  the design records that statement-replay was declined precisely to
  avoid that fence.

## Deliberately NOT in v1

Promotion/failover; multi-primary anything; segment encryption
(transport's TLS story per the protocol ruling); engine-resident
writer; compression (recorded — segments are compressible files, the
operator may).
