# UNGIT — design considerations for a tool that does not fight its users

*Started 2026-08-06 at Sean's direction ("let's ungit this puppy"), from the
practical retrospective: where would a user of this fork feel joy, and where
would they feel* git*?  This file is the standing checklist for every future
surface.  git is the reference pathology: a tool whose core model is sound and
whose daily experience teaches learned helplessness.*

## Principles already load-bearing here (keep them so)

1. **Refusals name the reason and the fix.**  "nested tables xs and ys
   correlate on different parent columns (a vs b) and cannot share one
   interleaved order" tells the author what to change.  The anti-pattern is
   `fatal: refusing to merge unrelated histories`.
2. **Errors at declaration time, not at use time.**  A nonconforming procedure
   cannot be installed.  git's opposite: accept everything now, let someone
   else discover it later.
3. **Empty is never spelled like missing.**  -1 vs 0 counts, [] vs NULL
   arrays, PATCH_REFUSED vs PATCH_ALREADY_APPLIED.  A reassembler that cannot
   tell an empty set from a missing answer is broken by our definition.
4. **State is introspectable as rows.**  proc_list / proc_info / proc_nested
   / proc_check.  Nobody should parse SQL, read source, or learn plumbing
   incantations to ask what the system believes.
5. **Options compose as words, not flags.**  `WITH COUNTS, INTERLEAVED` —
   refused loudly when a composition is unbuilt, never accepted-and-ignored.
6. **Unknown is a value.**  If the system has not computed an answer yet, it
   says "unknown until X", never a default that reads as an answer
   (see the fifth detector: ¬observed(X) is not observed(¬X)).

## The git-shaped scars, ranked, with their removal paths

| # | scar | why it is git | removal |
|---|---|---|---|
| 1 | Advisories go to `sqlite3_log`, which the stock CLI cannot display | our most protective warnings reach a channel the default tool hides; documentation instead of surface | **PRAGMA proc_check(name)** — advisories as SELECT-able rows (built 2026-08-06) |
| 2 | `@segments` magic string in the zebra seam | a mode selected by incantation inside another language | dies when zebra BUG-271 (unknown methods stamped void) is fixed; becomes a real `query_segments` method |
| 3 | `WITH INTERLEAVED` child payloads are positional (`_pad2` columns, parent names over child values) | fine for generated code, hostile to a human at a console | acceptable while opt-in + procgen-owned; if humans start typing it, revisit (per-segment names need the metadata channel the one-result-set design traded away — a real tension, not an oversight) |
| 4 | Silent performance tiers (body-cache disqualification; fold computed whether or not read) | invisible cost cliffs train helplessness | surface through proc_check as rows ("does not cache because ..."); fold cost already documented at the site and declinable via RETURNING |

## The test for every new surface

Before shipping any feature, ask the git question: **when this goes wrong at
2am for someone who did not build it, does the tool tell them what it knows,
in the place they are already looking, in words that name the fix?**  If the
answer involves "it's in the docs" or "there's a log", the surface is not
done.
