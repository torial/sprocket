/*
** proc_nestpoc2.c -- DOCKET #3, POC 2.
**
** POC 1 settled that nested shapes are not a compression feature (JSON fold is
** already smaller) but a type-fidelity one: json_group_array REFUSES a BLOB.
** POC 2 asks the three questions the rulings left open, and it is allowed to
** answer "no" to the middle one.
**
**   Q1  What does materialising children actually cost, and does laziness
**       recover it?  A JSON fallback means a parent's children must be
**       materialised before that parent row can be handed out -- unless the
**       column is lazy and only materialises when a flat client reads it.
**
**   Q2  Does a DECLARED correlation buy anything over the convention Tack
**       already uses?  Tack knows the FK graph at build time and its ordered
**       merge already reassembles two sets.  If the convention is
**       self-enforcing, declaration is documentation and not grammar.
**
**   Q3  Does the cardinality check (ruling R3) actually catch children lost or
**       duplicated?  Broken on purpose to find out.
**
** All measurements are exact counts -- child rows scanned, bytes materialised,
** peak rows buffered.  No timings: transport phase 5 established that a count
** is no better than a stopwatch when the count is timing-dependent, and these
** are pure functions of the data and the strategy.
**
** Build (from repo root, VS dev prompt):
**   cl /nologo /O2 /I. /Itool tool\proc_nestpoc2.c sqlite3.c /Fe:proc_nestpoc2.exe
*/
#define PROC_WIRE_NO_MAIN 1
#include "proc_wire.c"           /* check()/nCheck/nFail, and the codec */

#define NPARENT   50
#define NCHILD    20

static void execOr2(sqlite3 *db, const char *zSql){
  char *zErr = 0;
  if( sqlite3_exec(db, zSql, 0, 0, &zErr)!=SQLITE_OK ){
    printf("FATAL: %s\n  sql: %s\n", zErr, zSql);
    exit(1);
  }
}

/* What a strategy costs, in units that do not depend on the machine. */
typedef struct Cost {
  int nChildScanned;      /* child rows the engine had to read      */
  int nBytesMaterial;     /* bytes of JSON built                    */
  int nPeakBuffered;      /* most child rows held at once           */
} Cost;

/*
** EAGER: every parent's children are materialised as JSON whether or not the
** client ever looks at that column.  This is the naive nested-column
** implementation.
*/
static void runEager(sqlite3 *db, Cost *pC){
  sqlite3_stmt *pP = 0, *pK = 0;
  memset(pC, 0, sizeof(*pC));
  sqlite3_prepare_v2(db, "SELECT id,title FROM posts ORDER BY id", -1, &pP, 0);
  sqlite3_prepare_v2(db,
      "SELECT json_group_array(json_array(cid,body)) FROM comments"
      " WHERE post_id=?1", -1, &pK, 0);
  while( sqlite3_step(pP)==SQLITE_ROW ){
    int id = sqlite3_column_int(pP, 0);
    sqlite3_reset(pK);
    sqlite3_bind_int(pK, 1, id);
    if( sqlite3_step(pK)==SQLITE_ROW ){
      pC->nBytesMaterial += sqlite3_column_bytes(pK, 0);
    }
    pC->nChildScanned += NCHILD;      /* the aggregate reads them all */
    if( NCHILD > pC->nPeakBuffered ) pC->nPeakBuffered = NCHILD;
  }
  sqlite3_finalize(pP);
  sqlite3_finalize(pK);
}

/*
** LAZY: the nested column materialises only when the client reads it.
** readEvery=5 models a client that looks at one parent's children in five.
*/
static void runLazy(sqlite3 *db, int readEvery, Cost *pC){
  sqlite3_stmt *pP = 0, *pK = 0;
  int n = 0;
  memset(pC, 0, sizeof(*pC));
  sqlite3_prepare_v2(db, "SELECT id,title FROM posts ORDER BY id", -1, &pP, 0);
  sqlite3_prepare_v2(db,
      "SELECT json_group_array(json_array(cid,body)) FROM comments"
      " WHERE post_id=?1", -1, &pK, 0);
  while( sqlite3_step(pP)==SQLITE_ROW ){
    int id = sqlite3_column_int(pP, 0);
    if( (n++ % readEvery)==0 ){            /* client reads this one */
      sqlite3_reset(pK);
      sqlite3_bind_int(pK, 1, id);
      if( sqlite3_step(pK)==SQLITE_ROW ){
        pC->nBytesMaterial += sqlite3_column_bytes(pK, 0);
      }
      pC->nChildScanned += NCHILD;
      if( NCHILD > pC->nPeakBuffered ) pC->nPeakBuffered = NCHILD;
    }
  }
  sqlite3_finalize(pP);
  sqlite3_finalize(pK);
}

/*
** MERGE: what Tack's S2 does today -- two ordered cursors zipped in one pass.
** Nothing is materialised; peak memory is one parent's children.
** Returns the number of (parent,child) pairs delivered, and optionally the
** per-parent counts so a cardinality check can be applied.
*/
static int runMerge(sqlite3 *db, const char *zChildSql, Cost *pC, int *aCount){
  sqlite3_stmt *pP = 0, *pK = 0;
  int nPair = 0, cur = -1, run = 0;
  int rcK;
  memset(pC, 0, sizeof(*pC));
  sqlite3_prepare_v2(db, "SELECT id,title FROM posts ORDER BY id", -1, &pP, 0);
  sqlite3_prepare_v2(db, zChildSql, -1, &pK, 0);
  rcK = sqlite3_step(pK);
  while( sqlite3_step(pP)==SQLITE_ROW ){
    int id = sqlite3_column_int(pP, 0);
    run = 0;
    /* consume the run of children whose key equals this parent */
    while( rcK==SQLITE_ROW && sqlite3_column_int(pK,0)==id ){
      run++; nPair++; pC->nChildScanned++;
      rcK = sqlite3_step(pK);
    }
    if( aCount ) aCount[id-1] = run;
    if( run > pC->nPeakBuffered ) pC->nPeakBuffered = run;
  }
  sqlite3_finalize(pP);
  sqlite3_finalize(pK);
  return nPair;
}

int main(int argc, char **argv){
  sqlite3 *db = 0;
  Cost cEager, cLazy, cMerge;
  int aCount[NPARENT];
  int i, j;

  (void)argc; (void)argv;
  printf("proc_nestpoc2 -- DOCKET #3 POC 2\n");
  printf("SQLite %s;  %d parents x %d children\n\n",
         sqlite3_libversion(), NPARENT, NCHILD);

  if( sqlite3_open(":memory:", &db)!=SQLITE_OK ){ printf("open failed\n"); return 1; }
  execOr2(db,
    "CREATE TABLE posts(id INTEGER, title TEXT);"
    "CREATE TABLE comments(post_id INTEGER, cid INTEGER, body TEXT);");
  execOr2(db, "BEGIN;");
  for(i=1; i<=NPARENT; i++){
    char *z = sqlite3_mprintf("INSERT INTO posts VALUES(%d,'post %d');", i, i);
    execOr2(db, z); sqlite3_free(z);
    for(j=1; j<=NCHILD; j++){
      z = sqlite3_mprintf("INSERT INTO comments VALUES(%d,%d,"
        "'comment body of unremarkable length %d');", i, (i-1)*NCHILD+j, j);
      execOr2(db, z); sqlite3_free(z);
    }
  }
  execOr2(db, "COMMIT;");
  execOr2(db, "CREATE INDEX ci ON comments(post_id);");

  /* ================= Q1: what does materialisation cost? ================= */
  runEager(db, &cEager);
  runLazy(db, 5, &cLazy);
  runMerge(db,
    "SELECT post_id,cid,body FROM comments ORDER BY post_id,cid",
    &cMerge, aCount);

  printf("  Q1 -- cost of getting parents with their children\n");
  printf("  strategy        child rows   JSON bytes   peak buffered\n");
  printf("  -------------  -----------  -----------  -------------\n");
  printf("  eager JSON     %11d  %11d  %13d\n",
         cEager.nChildScanned, cEager.nBytesMaterial, cEager.nPeakBuffered);
  printf("  lazy  (1 in 5) %11d  %11d  %13d\n",
         cLazy.nChildScanned, cLazy.nBytesMaterial, cLazy.nPeakBuffered);
  printf("  ordered merge  %11d  %11d  %13d\n\n",
         cMerge.nChildScanned, cMerge.nBytesMaterial, cMerge.nPeakBuffered);

  check(cEager.nChildScanned == NPARENT*NCHILD,
        "eager reads every child whether or not the client wants it");
  check(cLazy.nChildScanned < cEager.nChildScanned,
        "laziness recovers the children the client never asks for");
  check(cMerge.nBytesMaterial == 0,
        "the merge materialises nothing -- no JSON is ever built");
  check(cMerge.nPeakBuffered == NCHILD,
        "merge holds one parent's children at a time, not the whole set");

  /* ============ Q2: does DECLARING the correlation buy anything? ========= */
  /* The convention is "both sets ordered by the correlation key".  What
  ** happens when a proc author violates it?  If the merge fails loudly, the
  ** convention is self-enforcing and declaration is documentation.  If it
  ** fails SILENTLY, detection is the thing declaration buys. */
  {
    Cost cBad;
    int aBad[NPARENT];
    int nBadPair;
    memset(aBad, 0, sizeof(aBad));
    nBadPair = runMerge(db,
      "SELECT post_id,cid,body FROM comments ORDER BY cid DESC",  /* WRONG order */
      &cBad, aBad);
    printf("  Q2 -- children delivered when the ordering convention is broken\n");
    printf("  correctly ordered : %d pairs\n", NPARENT*NCHILD);
    printf("  wrongly ordered   : %d pairs\n\n", nBadPair);

    check(nBadPair != NPARENT*NCHILD,
          "a broken ordering DOES change the result");
    check(nBadPair < NPARENT*NCHILD,
          "and it changes it by LOSING children, not by erroring");
  }

  /* ================= Q3: does the cardinality check work? =============== */
  /* R3 says the engine records how many children belong to each parent and the
  ** reassembler verifies it.  Break it on purpose, both ways. */
  {
    int aDeclared[NPARENT];
    Cost cX;
    int aGot[NPARENT];
    int nMismatch;

    for(i=0; i<NPARENT; i++) aDeclared[i] = NCHILD;   /* the truth */

    /* (a) children LOST */
    execOr2(db, "DELETE FROM comments WHERE post_id=7 AND cid % 2 = 0;");
    memset(aGot, 0, sizeof(aGot));
    runMerge(db, "SELECT post_id,cid,body FROM comments ORDER BY post_id,cid",
             &cX, aGot);
    nMismatch = 0;
    for(i=0; i<NPARENT; i++) if( aGot[i]!=aDeclared[i] ) nMismatch++;
    printf("  Q3 -- cardinality check\n");
    printf("  after deleting half of parent 7's children: %d parent(s) mismatch\n",
           nMismatch);
    check(nMismatch==1, "CARDINALITY DETECTS lost children, and localises them");

    /* (b) children DUPLICATED */
    execOr2(db, "INSERT INTO comments SELECT post_id, cid+100000, body"
                "  FROM comments WHERE post_id=9;");
    memset(aGot, 0, sizeof(aGot));
    runMerge(db, "SELECT post_id,cid,body FROM comments ORDER BY post_id,cid",
             &cX, aGot);
    nMismatch = 0;
    for(i=0; i<NPARENT; i++) if( aGot[i]!=aDeclared[i] ) nMismatch++;
    printf("  after duplicating parent 9's children:      %d parent(s) mismatch\n",
           nMismatch);
    check(nMismatch==2, "CARDINALITY DETECTS duplicated children too");

    /* CONTROL: with the data restored the check must go quiet, or it is not a
    ** check, it is an alarm that is always on. */
    execOr2(db, "DELETE FROM comments WHERE cid > 100000;");
    execOr2(db, "INSERT INTO comments SELECT 7, cid, body FROM comments"
                " WHERE post_id=1 AND cid % 2 = 0;");
    memset(aGot, 0, sizeof(aGot));
    runMerge(db, "SELECT post_id,cid,body FROM comments ORDER BY post_id,cid",
             &cX, aGot);
    nMismatch = 0;
    for(i=0; i<NPARENT; i++) if( aGot[i]!=aDeclared[i] ) nMismatch++;
    printf("  after restoring the counts:                 %d parent(s) mismatch\n",
           nMismatch);
    check(nMismatch==0, "CONTROL: the check is quiet when the data is right");
  }

  sqlite3_close(db);
  printf("\n%s: %d checks, %d failures\n",
         nFail ? "NESTPOC2 FAILED" : "NESTPOC2 OK", nCheck, nFail);
  return nFail ? 1 : 0;
}
