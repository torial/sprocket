# HANDOFF — interleaved segments (DOCKET 3h)

*Written 2026-08-06 assuming the reader has no memory of the sessions that
produced it. Branch `nested-shapes`. Read this before `PLAN-NESTED.md`, which is
long and mostly historical.*

## NEXT ACTION

**Build the merge driver.** Compile a shape's parent and child body statements
with `SRT_Coroutine` destinations and drive them from a key merge, then flip
`WITH INTERLEAVED` from refusal to behavior. Everything it depends on is landed,
measured and green — this is the only remaining piece.

Entry points: `procBuildEmits()` and `procLowerChild()` in `src/proc.c`. The
model to copy is `multiSelectOrderBy()` in `src/select.c` (~line 3580), which
already merges two coroutines by comparing sort keys.

## The build incantation — get this right first

`nmake` is **not** on the bash PATH, and the naive fix fails silently in a way
that wastes a cycle:

```
cmd /c 'set PATH=C:\Projects\sqlite;%PATH% && "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 && cd /d C:\Projects\sqlite && nmake /f Makefile.msc testfixture.exe'
```

Three traps, all hit on 2026-08-06:

1. **`jimsh0.exe` lives in the repo root and the makefile invokes it bare**, so
   the repo root must be on `PATH`.
2. **`set PATH=...%PATH%` must come BEFORE vcvars.** `cmd` expands `%PATH%` when
   it parses the whole line, so putting it after vcvars silently discards every
   compiler path vcvars just added, and you get "nmake is not recognized"
   *after* a vcvars run that appeared to succeed.
3. **Gate on `nmake` exit 0 and delete the binary first.** A prior docket item
   exists because a harness gated on file existence and a stale binary satisfied
   it.

**Then prove the binary contains your change before trusting any test.** A
failure against a stale binary and a failure against broken code look identical:

```
echo 'puts [info commands sqlite3_proc_current_segment]' > /c/tmp/live.tcl
./testfixture.exe /c/tmp/live.tcl
```

## Baseline suite counts — a silent drop is the failure mode

State the expected number, then compare. `testfixture` prints a memory summary
even when it reports nothing, so **a clean tail is not a pass.** Always read the
`N errors out of M tests` line, and check M.

| suite | tests | | suite | tests |
|---|---|---|---|---|
| `procinter` | 17 | | `proc6adv` | 12 |
| `proc1` | 33 | | `proc3c` | 13 |
| `proc4` | 47 | | `proc4c` | 13 |
| `proc5` | 24 | | `hiddencol` | 12 |
| `proc6` | 52 | | `procadv` | 8 |
| `psm1` | 35 | | `foldshape` | 7 |
| `prochostile` | 13 | | `fold` | 6 |
| `proclk` | 9 | | `b64` | 6 |
| `procfault` | 1064 | | `procnull` | 6 |
| `procmosaic` | 6 | | | |

## Design — settled, do not relitigate

Decided across 2026-08-05/06 with reasons in `DOCKET.md` §3h.

- **Opt-in per CALL.** `WITH INTERLEAVED`. Non-negotiable: today a client that
  never calls `sqlite3_proc_next_resultset()` sees segment 0 and stops, so
  unaware clients get a well-formed single result set. Interleaving by default
  would break that silently.
- **Discriminator is `sqlite3_proc_current_segment()`** (landed), not a column
  and never inferred from `column_count()` — two segments can be equally wide,
  and reading a column a segment lacks yields NULL, not an error.
- **`WITH COUNTS` needs no change.** The count is a correlated scalar subquery
  on the parent's own wrapped SELECT (`procAddCountColumn`), independent of the
  child stream — proven by `proc4c`, which reads counts while stepping segment 0
  only. Under interleaving it arrives *before* the children, so a client can
  pre-size and then verify at the group boundary.
- **`next_resultset()` → `SQLITE_MISUSE`** in interleaved mode. `SQLITE_DONE`
  would be indistinguishable from the normal "no further set".
- **Each level must be ordered by its parent key.** The ancestor-path constraint
  retracted for the *shipped* protocol genuinely returns in a *merging* one. Do
  not inherit that retraction here.

## Already measured — don't re-derive

- Coroutines survive the proc SubProgram frame (`EXPLAIN CALL` shows
  `InitCoroutine` inside `Program`).
- **Two** concurrent coroutines merge correctly inside a proc: a compound
  `UNION ALL ... ORDER BY` in a body yields `1,2,3,4,5,6` from `1,3,5` and
  `2,4,6` — a real merge, not concatenation.
- Repeated `ApplyProcSet` holds across narrowing *and* widening (4→2→3), with
  byte-identical metadata on a second walk. Per-*row* is still untested; it
  differs only in frequency and the call does not allocate.
- Out-of-range column reads yield empty, **not** the wider segment's stale
  register value.

## Open

- Should `WITH COUNTS, COUNTS` be an error? It parses and ORs the same bit.
  Harmless, untested, undecided.
- Per-row `ApplyProcSet` — argued, not measured.
- `sqlite3_column_name()` pointers go stale per row under interleaving
  (`SQLITE_TRANSIENT` copies that `ApplyProcSet` overwrites). Generated clients
  must read names per row or cache per segment index.

## The habit that found every real bug here

Ask of a clean result: **"what would make this print zero, other than there
being zero?"** On 2026-08-06 alone: a `grep -c` inflated by the probe's own
banner; a memory-bound claim built on a misread of `procAddCountColumn`; and a
use-after-free in a test that returned **4** — the previous segment's width —
rather than crashing. Every one produced a plausible number, never an error.
