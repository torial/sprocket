/*
** bench_proc_roundtrip.c -- what does a CALL cost, server-side, against the
** equivalent loose statements?
**
** The pitch for stored procedures in a NETWORKED SQLite is that one CALL
** returning N declared result sets replaces N client round trips.  That part
** is arithmetic: you save (N-1) round trips.  The part that is NOT arithmetic,
** and is the only thing a benchmark can tell us, is whether the engine charges
** us for the privilege -- i.e. whether a CALL does MORE server-side work than
** the N statements it replaces.
**
** So this measures, in-process, with no network at all:
**
**   Path A: N already-prepared SELECTs, stepped to completion, reset.
**   Path B: one already-prepared CALL of a procedure with N declared result
**           sets, stepped and advanced with sqlite3_proc_next_resultset().
**
** Both paths must consume byte-identical rows; the harness asserts that before
** it times anything.  The output is the signed delta and the break-even
** round-trip time: the network latency above which Path B wins regardless.
**
** If delta is NEGATIVE, the CALL is cheaper server-side as well as cheaper on
** the wire, and the thesis is stronger than "it saves round trips".
**
** Build (from the repo root, in a VS dev prompt):
**   cl /O2 /I. tool\bench_proc_roundtrip.c sqlite3.c /Fe:bench_proc.exe
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sqlite3.h"

#ifdef _WIN32
# include <windows.h>
static double nowSeconds(void){
  LARGE_INTEGER f, t;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&t);
  return (double)t.QuadPart / (double)f.QuadPart;
}
#else
# include <time.h>
static double nowSeconds(void){
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}
#endif

#define ROWS_PER_SET 10          /* rows each result set returns */
#define MIN_SECONDS  1.0         /* run at least this long, per path */

static void fatal(sqlite3 *db, const char *zWhat){
  fprintf(stderr, "FATAL: %s: %s\n", zWhat, db ? sqlite3_errmsg(db) : "(no db)");
  exit(1);
}

static void execOrDie(sqlite3 *db, const char *zSql){
  char *zErr = 0;
  if( sqlite3_exec(db, zSql, 0, 0, &zErr)!=SQLITE_OK ){
    fprintf(stderr, "FATAL exec: %s\n  while running: %s\n", zErr, zSql);
    exit(1);
  }
}

/*
** Consume every row of one result set.  Returns rows seen and folds the
** column values into *pSum so the optimizer cannot discard the work and so
** the two paths can be proven to have read the same data.
*/
static int drainSet(sqlite3_stmt *p, sqlite3_int64 *pSum){
  int n = 0;
  int rc;
  while( (rc = sqlite3_step(p))==SQLITE_ROW ){
    const unsigned char *z = sqlite3_column_text(p, 1);
    *pSum += sqlite3_column_int64(p, 0);
    if( z ) *pSum += (sqlite3_int64)z[0];
    n++;
  }
  if( rc!=SQLITE_DONE ){
    fprintf(stderr, "FATAL: step returned %d\n", rc);
    exit(1);
  }
  return n;
}

/* Path A: N loose statements. */
static int runPathA(sqlite3_stmt **apStmt, int nSet, sqlite3_int64 *pSum){
  int i, rows = 0;
  for(i=0; i<nSet; i++){
    rows += drainSet(apStmt[i], pSum);
    sqlite3_reset(apStmt[i]);
  }
  return rows;
}

/* Path B: one CALL, advanced across its declared result sets. */
static int runPathB(sqlite3_stmt *pCall, sqlite3_int64 *pSum){
  int rows = 0;
  for(;;){
    int rc;
    rows += drainSet(pCall, pSum);
    rc = sqlite3_proc_next_resultset(pCall);
    if( rc==SQLITE_DONE ) break;
    if( rc!=SQLITE_OK ){
      fprintf(stderr, "FATAL: next_resultset returned %d\n", rc);
      exit(1);
    }
  }
  sqlite3_reset(pCall);
  return rows;
}

static void benchOne(sqlite3 *db, int nSet){
  char zBuf[8192];
  char zProc[8192];
  sqlite3_stmt **apStmt;
  sqlite3_stmt *pCall = 0;
  int i, n, rowsA, rowsB;
  sqlite3_int64 sumA = 0, sumB = 0;
  double t0, tA, tB;
  long long iters;
  double perA, perB, delta, breakeven;

  /* --- build the procedure: N declared result sets, one SELECT each ------- */
  sqlite3_snprintf(sizeof(zProc), zProc, "CREATE PROCEDURE bench%d()", nSet);
  for(i=1; i<=nSet; i++){
    sqlite3_snprintf(sizeof(zBuf), zBuf,
        "%s\n  RETURNS TABLE(id INTEGER, payload TEXT)", zProc);
    memcpy(zProc, zBuf, strlen(zBuf)+1);
  }
  sqlite3_snprintf(sizeof(zBuf), zBuf, "%s\nBEGIN\n", zProc);
  memcpy(zProc, zBuf, strlen(zBuf)+1);
  for(i=1; i<=nSet; i++){
    sqlite3_snprintf(sizeof(zBuf), zBuf,
        "%s  SELECT id, payload FROM items WHERE grp = %d;\n", zProc, i);
    memcpy(zProc, zBuf, strlen(zBuf)+1);
  }
  sqlite3_snprintf(sizeof(zBuf), zBuf, "%sEND;", zProc);
  memcpy(zProc, zBuf, strlen(zBuf)+1);
  execOrDie(db, zProc);

  /* --- prepare both paths ------------------------------------------------- */
  apStmt = (sqlite3_stmt**)malloc(sizeof(sqlite3_stmt*) * nSet);
  if( apStmt==0 ){ fprintf(stderr,"OOM\n"); exit(1); }
  for(i=1; i<=nSet; i++){
    sqlite3_snprintf(sizeof(zBuf), zBuf,
        "SELECT id, payload FROM items WHERE grp = %d;", i);
    if( sqlite3_prepare_v2(db, zBuf, -1, &apStmt[i-1], 0)!=SQLITE_OK ){
      fatal(db, "prepare path A");
    }
  }
  sqlite3_snprintf(sizeof(zBuf), zBuf, "CALL bench%d();", nSet);
  if( sqlite3_prepare_v2(db, zBuf, -1, &pCall, 0)!=SQLITE_OK ){
    fatal(db, "prepare path B");
  }

  /* --- warm up, and PROVE the two paths read the same data ---------------- */
  rowsA = runPathA(apStmt, nSet, &sumA);
  rowsB = runPathB(pCall, &sumB);
  if( rowsA!=rowsB || sumA!=sumB ){
    fprintf(stderr,
        "FATAL: paths disagree -- A: %d rows sum=%lld   B: %d rows sum=%lld\n"
        "  (a benchmark of two different workloads is worthless)\n",
        rowsA, (long long)sumA, rowsB, (long long)sumB);
    exit(1);
  }
  if( rowsA != nSet*ROWS_PER_SET ){
    fprintf(stderr, "FATAL: expected %d rows, both paths returned %d\n",
            nSet*ROWS_PER_SET, rowsA);
    exit(1);
  }
  /* a few more warm passes so the compiled-body cache and page cache are hot */
  for(n=0; n<50; n++){
    runPathA(apStmt, nSet, &sumA);
    runPathB(pCall, &sumB);
  }

  /* --- calibrate iteration count so each path runs >= MIN_SECONDS --------- */
  iters = 200;
  for(;;){
    t0 = nowSeconds();
    for(n=0; n<iters; n++) runPathB(pCall, &sumB);
    tB = nowSeconds() - t0;
    if( tB >= MIN_SECONDS*0.5 ) break;
    iters *= 4;
    if( iters > 100000000LL ) break;
  }
  if( tB>0 ) iters = (long long)(iters * (MIN_SECONDS/tB)) + 1;

  /* --- measure ------------------------------------------------------------ */
  t0 = nowSeconds();
  for(n=0; n<iters; n++) runPathA(apStmt, nSet, &sumA);
  tA = nowSeconds() - t0;

  t0 = nowSeconds();
  for(n=0; n<iters; n++) runPathB(pCall, &sumB);
  tB = nowSeconds() - t0;

  perA = tA / (double)iters * 1e6;    /* microseconds per request */
  perB = tB / (double)iters * 1e6;
  delta = perB - perA;                /* >0 means CALL costs more */
  breakeven = (nSet>1) ? (delta / (nSet-1)) : 0.0;

  printf("  %2d  %8lld  %9.2f  %9.2f  %+9.3f  ",
         nSet, iters, perA, perB, delta);
  if( nSet<=1 ){
    printf("     n/a\n");
  }else if( delta <= 0 ){
    printf("  always\n");
  }else{
    printf("%8.3f\n", breakeven);
  }

  for(i=0; i<nSet; i++) sqlite3_finalize(apStmt[i]);
  free(apStmt);
  sqlite3_finalize(pCall);
}

int main(int argc, char **argv){
  sqlite3 *db = 0;
  char zBuf[512];
  int aSets[] = { 2, 5, 10 };
  int i, k;

  (void)argc; (void)argv;

  if( sqlite3_open(":memory:", &db)!=SQLITE_OK ) fatal(db, "open");

  execOrDie(db, "PRAGMA journal_mode=WAL;");
  execOrDie(db, "CREATE TABLE items(grp INTEGER, id INTEGER, payload TEXT);");
  execOrDie(db, "BEGIN;");
  for(k=1; k<=10; k++){
    for(i=1; i<=ROWS_PER_SET; i++){
      sqlite3_snprintf(sizeof(zBuf), zBuf,
          "INSERT INTO items VALUES(%d,%d,'payload-row-%d-of-group-%d');",
          k, i, i, k);
      execOrDie(db, zBuf);
    }
  }
  execOrDie(db, "COMMIT;");
  execOrDie(db, "CREATE INDEX items_grp ON items(grp);");
  execOrDie(db, "ANALYZE;");

  printf("bench_proc_roundtrip -- SQLite %s\n", sqlite3_libversion());
  printf("in-memory db, %d rows per result set, warm caches\n\n", ROWS_PER_SET);
  printf("  N = declared result sets in one CALL (== round trips saved + 1)\n");
  printf("  A = N loose prepared SELECTs    B = one CALL\n");
  printf("  break-even RTT = network latency above which B wins anyway\n\n");
  printf("   N     iters     A us/op    B us/op      delta   break-even us\n");
  printf("  ---  --------  ---------  ---------  ---------  -------------\n");

  for(i=0; i<(int)(sizeof(aSets)/sizeof(aSets[0])); i++){
    benchOne(db, aSets[i]);
  }

  printf("\n  delta > 0: the CALL costs more server-side; it still wins on any\n");
  printf("             network whose RTT exceeds the break-even figure.\n");
  printf("  delta < 0: the CALL is cheaper server-side AND on the wire.\n");

  sqlite3_close(db);
  return 0;
}
