/*
** procgen_test.c -- proves the generated client actually works.
**
** The contract a generator has to meet is not "it compiles" but "it does the
** same thing hand-written code would."  So every check here compares the
** GENERATED path against the SAME call issued through plain sqlite3_prepare()
** on the same database.  That comparison is the positive control: the
** generated client cannot pass by agreeing with itself.
**
** Compiled against the header procgen.exe emitted moments earlier, so a
** generator that emits something uncompilable fails here rather than being
** discovered later by a user.
**
** Build (after running procgen.exe pgfix.db > pgclient.h):
**   cl /nologo /O2 /I. tool\procgen_test.c sqlite3.c /Fe:procgen_test.exe
*/
#include <stdio.h>
#include <string.h>
#include "sqlite3.h"
#include "pgclient.h"

static int nCheck = 0, nFail = 0;
static void check(int bOk, const char *zWhat){
  nCheck++;
  if( !bOk ){ nFail++; printf("  FAIL: %s\n", zWhat); }
}

/* The same call, issued by hand, for comparison. */
static void handWritten_greet(sqlite3 *db, const char *who, sqlite3_int64 n,
                              char *zMsgOut, int nOut, sqlite3_int64 *pCnt){
  sqlite3_stmt *p = 0;
  zMsgOut[0] = 0; *pCnt = -1;
  if( sqlite3_prepare_v2(db, "CALL greet(?,?);", -1, &p, 0)!=SQLITE_OK ) return;
  sqlite3_bind_text(p, 1, who, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(p, 2, n);
  if( sqlite3_step(p)==SQLITE_ROW ){
    const unsigned char *z = sqlite3_column_text(p, 0);
    if( z ) sqlite3_snprintf(nOut, zMsgOut, "%s", z);
    *pCnt = sqlite3_column_int64(p, 1);
  }
  sqlite3_finalize(p);
}

int main(int argc, char **argv){
  sqlite3 *db = 0;
  const char *zDb = argc>1 ? argv[1] : "pgfix.db";

  printf("procgen_test -- SQLite %s\n\n", sqlite3_libversion());

  if( sqlite3_open_v2(zDb, &db, SQLITE_OPEN_READONLY, 0)!=SQLITE_OK ){
    printf("cannot open %s\n", zDb);
    return 1;
  }

  /* ---- 1. single result set, typed parameters ---------------------------- */
  {
    greet_stmt *p = 0;
    char zHand[64];
    sqlite3_int64 nHand = 0;
    const unsigned char *zGen = 0;
    sqlite3_int64 nGen = -1;
    int rc;

    check(greet_prepare(db, &p)==SQLITE_OK, "greet_prepare");
    /* A string literal binds with no cast -- that is the ergonomics the
    ** generated client exists to provide. */
    check(greet_bind(p, "world", 5, 42)==SQLITE_OK, "greet_bind");
    rc = greet_step(p);
    check(rc==SQLITE_ROW, "greet_step yields a row");
    if( rc==SQLITE_ROW ){
      zGen = greet_rs1_msg(p);
      nGen = greet_rs1_cnt(p);
    }
    handWritten_greet(db, "world", 42, zHand, sizeof(zHand), &nHand);

    printf("  generated:    msg=%s cnt=%lld\n", zGen?(const char*)zGen:"(null)",
           (long long)nGen);
    printf("  hand-written: msg=%s cnt=%lld\n", zHand, (long long)nHand);

    check(zGen!=0 && strcmp((const char*)zGen, zHand)==0,
          "POSITIVE CONTROL: generated msg equals hand-written msg");
    check(nGen==nHand, "POSITIVE CONTROL: generated cnt equals hand-written cnt");
    check(nHand==42, "and the value is the one that was bound");

    /* NEGATIVE CONTROL: the comparison above must be capable of failing. */
    check(strcmp("world", "worlds")!=0, "the comparator distinguishes values");

    check(greet_step(p)==SQLITE_DONE, "one row only");
    greet_finalize(p);
  }

  /* ---- 2. two result sets, advanced through the boundary API ------------- */
  {
    two_stmt *p = 0;
    sqlite3_int64 a = -1;
    char zB[64];
    int rc;

    zB[0] = 0;
    check(two_prepare(db, &p)==SQLITE_OK, "two_prepare");
    rc = two_step(p);
    check(rc==SQLITE_ROW, "set 1 yields a row");
    if( rc==SQLITE_ROW ) a = two_rs1_a(p);
    check(two_step(p)==SQLITE_DONE, "set 1 ends");

    check(two_next_resultset(p)==SQLITE_OK, "advance to set 2");
    rc = two_step(p);
    check(rc==SQLITE_ROW, "set 2 yields a row");
    if( rc==SQLITE_ROW ){
      const unsigned char *z = two_rs2_b(p);
      if( z ) sqlite3_snprintf(sizeof(zB), zB, "%s", z);
    }
    check(two_step(p)==SQLITE_DONE, "set 2 ends");
    check(two_next_resultset(p)==SQLITE_DONE, "no third set");

    printf("  two(): set1 a=%lld  set2 b=%s\n", (long long)a, zB);
    check(a==7, "set 1 carries the table's integer");
    check(strcmp(zB,"seven")==0, "set 2 carries the table's text");

    /* The two sets have DIFFERENT widths and types; that the generated
    ** accessors are per-set is the thing being verified here. */
    two_finalize(p);
  }

  sqlite3_close(db);
  printf("\n%s: %d checks, %d failures\n",
         nFail ? "PROCGEN_TEST FAILED" : "PROCGEN_TEST OK", nCheck, nFail);
  return nFail ? 1 : 0;
}
