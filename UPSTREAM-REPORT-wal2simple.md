# Draft upstream report: `wal2simple.test` aborts on Windows

*Prepared 2026-08-02. **Not submitted** — Sean's call whether and where to send
it (SQLite forum, https://sqlite.org/forum). Written to be pasteable.*

---

**Subject:** wal2 branch: `test/wal2simple.test` aborts on Windows — `db2`
handle left open across `reset_db`

On the `wal2` branch, `test/wal2simple.test` aborts partway through on Windows.
Because `test/permutations.test` includes the wal2 tests in the `veryquick`
set, this takes the whole `veryquick` run down with it, so the visible symptom
is a suite-wide failure rather than one test file.

**Symptom**

```
wal2simple-7.5.4... Ok
testfixture.exe: error deleting "test.db": permission denied
    while executing
"error $msg "
    (procedure "do_delete_file" line 27)
    ...
    (procedure "reset_db" line 3)
    invoked from within
"reset_db"
    (file "test\wal2simple.test" line 474)
```

**Cause**

Section 7 opens a second connection and does not close it before the section
ends:

- line 439 — `sqlite3 db2 test.db`
- line 474 — `reset_db`   ← deletes `test.db` while `db2` still holds it open
- line 496 — `db2 close`

Unlinking an open file is legal on unix, so the test passes there. Windows
returns `permission denied` and `do_delete_file` raises, aborting the file.

**Suggested fix**

Close the handle before the reset. Section 8 opens its own `db2` immediately
afterwards, so nothing downstream depends on it staying open.

```tcl
#-------------------------------------------------------------------------
catch { db2 close }
reset_db
do_execsql_test 8.0 {
  PRAGMA journal_size_limit = 10000;
```

`catch` keeps it harmless if the handle is already closed. Correct on every
platform; no unix behaviour changes.

**Effect**

`wal2simple.test` goes from aborting at roughly test 200 to completing all
**307** tests with 0 errors. With that one change the full wal2 suite passes on
Windows — 538 tests, 0 errors across `wal2simple`, `wal2big`, `wal2openclose`,
`wal2recover`, `wal2recover2`, `wal2recover3`, `wal2rewrite`, `wal2rollback`,
`wal2savepoint`, `wal2snapshot`, `wal2lock`, `wal2fault` — and `veryquick`
returns 0 errors out of 393,363.

**Environment**

Windows 11, MSVC (`Makefile.msc`), Tcl 8.6. Observed against the `wal2` feature
delta applied onto a `branch-3.53` (3.53.4) base rather than a direct checkout
of the branch, so the line numbers are from `wal2`'s own copy of the file; the
`db2` open/`reset_db`/`db2 close` ordering is what matters and is unchanged.
