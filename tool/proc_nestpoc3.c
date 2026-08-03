/*
** proc_nestpoc3.c -- DOCKET #3, POC 3: how should cardinality be carried?
**
** POC 2 established that the ordering convention fails SILENTLY -- a wrong
** ORDER BY returns 2% of the rows and reports success -- so what the engine
** buys by declaring cardinality is detection.  The open question it left is
** how that cardinality should travel:
**
**   (a) TOTAL      one integer for the whole response
**   (b) PER-PARENT one integer per parent row
**   (c) CONSTANT   declared once in the shape, no runtime cost
**
** The temptation is to pick by cost.  The right way to pick is by WHAT EACH
** ONE CAN SEE, so this builds a detection matrix: four ways the child stream
** can go wrong, three checks, and which combinations notice.
**
** The failure that decides it is MISATTRIBUTION -- children delivered under
** the wrong parent, with the total still perfectly correct.  A total-only
** check cannot see that by construction; the question is whether it happens
** for realistic reasons.  (It does: an off-by-one in a correlation key, a
** join against the wrong column, a shifted foreign key after a migration.)
**
** Costs are measured through the real codec, exactly, no timings.
**
** Build (from repo root, VS dev prompt):
**   cl /nologo /O2 /I. /Itool tool\proc_nestpoc3.c sqlite3.c /Fe:proc_nestpoc3.exe
*/
#define PROC_WIRE_NO_MAIN 1
#include "proc_wire.c"

#define NPARENT  50
#define NCHILD   20
#define NTOTAL   (NPARENT*NCHILD)

static void ex3(sqlite3 *db, const char *zSql){
  char *zErr = 0;
  if( sqlite3_exec(db, zSql, 0, 0, &zErr)!=SQLITE_OK ){
    printf("FATAL: %s\n  sql: %s\n", zErr, zSql);
    exit(1);
  }
}

/*
** Reassemble by ordered merge and report the per-parent counts actually
** delivered.  This is the reassembler under test; the checks below are applied
** to what it produces.
*/
static void reassemble(sqlite3 *db, const char *zChildSql, int *aGot, int *pnTotal){
  sqlite3_stmt *pP = 0, *pK = 0;
  int rcK, total = 0;
  memset(aGot, 0, NPARENT*sizeof(int));
  sqlite3_prepare_v2(db, "SELECT id FROM posts ORDER BY id", -1, &pP, 0);
  sqlite3_prepare_v2(db, zChildSql, -1, &pK, 0);
  rcK = sqlite3_step(pK);
  while( sqlite3_step(pP)==SQLITE_ROW ){
    int id = sqlite3_column_int(pP, 0);
    int run = 0;
    while( rcK==SQLITE_ROW && sqlite3_column_int(pK,0)==id ){
      run++; total++;
      rcK = sqlite3_step(pK);
    }
    aGot[id-1] = run;
  }
  sqlite3_finalize(pP);
  sqlite3_finalize(pK);
  *pnTotal = total;
}

/* (a) TOTAL: does the number of child rows match what was declared? */
static int checkTotal(int nGot, int nDeclared){ return nGot==nDeclared; }

/* (b) PER-PARENT: does every parent's count match? */
static int checkPerParent(const int *aGot, const int *aDeclared){
  int i;
  for(i=0; i<NPARENT; i++) if( aGot[i]!=aDeclared[i] ) return 0;
  return 1;
}

/* (c) CONSTANT: a single declared cardinality for every parent. */
static int checkConstant(const int *aGot, int nEach){
  int i;
  for(i=0; i<NPARENT; i++) if( aGot[i]!=nEach ) return 0;
  return 1;
}

/* Bytes to carry N integers through the real codec. */
static int costOfIntegers(int n){
  WireBuf b; int i, sz;
  memset(&b, 0, sizeof(b));
  for(i=0; i<n; i++){ wbU8(&b, WV_INT); wbI64(&b, 20); }
  sz = b.n; wbFree(&b);
  return sz;
}

int main(int argc, char **argv){
  sqlite3 *db = 0;
  int aDeclared[NPARENT], aGot[NPARENT];
  int i, j, nTotal;
  int nCaughtTotal = 0, nCaughtParent = 0, nCaughtConst = 0, nBroken = 0;

  (void)argc; (void)argv;
  printf("proc_nestpoc3 -- DOCKET #3 POC 3: how to carry cardinality\n");
  printf("SQLite %s;  %d parents x %d children\n\n",
         sqlite3_libversion(), NPARENT, NCHILD);

  if( sqlite3_open(":memory:", &db)!=SQLITE_OK ){ printf("open failed\n"); return 1; }
  ex3(db, "CREATE TABLE posts(id INTEGER, title TEXT);"
          "CREATE TABLE comments(post_id INTEGER, cid INTEGER, body TEXT);");
  ex3(db, "BEGIN;");
  for(i=1; i<=NPARENT; i++){
    char *z = sqlite3_mprintf("INSERT INTO posts VALUES(%d,'post %d');", i, i);
    ex3(db, z); sqlite3_free(z);
    for(j=1; j<=NCHILD; j++){
      z = sqlite3_mprintf("INSERT INTO comments VALUES(%d,%d,'body %d');",
                          i, (i-1)*NCHILD+j, j);
      ex3(db, z); sqlite3_free(z);
    }
  }
  ex3(db, "COMMIT;");
  ex3(db, "CREATE INDEX ci ON comments(post_id);");
  for(i=0; i<NPARENT; i++) aDeclared[i] = NCHILD;

  printf("  failure mode              total  per-parent  constant\n");
  printf("  ------------------------  -----  ----------  --------\n");

  /* --- 0. CONTROL: nothing wrong.  Every check must stay QUIET. --------- */
  reassemble(db, "SELECT post_id,cid,body FROM comments ORDER BY post_id,cid",
             aGot, &nTotal);
  {
    int a = checkTotal(nTotal, NTOTAL);
    int b = checkPerParent(aGot, aDeclared);
    int c = checkConstant(aGot, NCHILD);
    printf("  (control: correct data)   %5s  %10s  %8s\n",
           a?"quiet":"ALARM", b?"quiet":"ALARM", c?"quiet":"ALARM");
    check(a && b && c, "CONTROL: all three checks are quiet on correct data");
  }

  /* --- 1. ordering broken (POC 2's silent 98% loss) --------------------- */
  reassemble(db, "SELECT post_id,cid,body FROM comments ORDER BY cid DESC",
             aGot, &nTotal);
  {
    int a = !checkTotal(nTotal, NTOTAL);
    int b = !checkPerParent(aGot, aDeclared);
    int c = !checkConstant(aGot, NCHILD);
    printf("  ordering broken           %5s  %10s  %8s\n",
           a?"CAUGHT":"missed", b?"CAUGHT":"missed", c?"CAUGHT":"missed");
    nBroken++; nCaughtTotal+=a; nCaughtParent+=b; nCaughtConst+=c;
  }

  /* --- 2. children lost -------------------------------------------------- */
  ex3(db, "CREATE TEMP TABLE bak AS SELECT * FROM comments;");
  ex3(db, "DELETE FROM comments WHERE post_id=7 AND cid % 2 = 0;");
  reassemble(db, "SELECT post_id,cid,body FROM comments ORDER BY post_id,cid",
             aGot, &nTotal);
  {
    int a = !checkTotal(nTotal, NTOTAL);
    int b = !checkPerParent(aGot, aDeclared);
    int c = !checkConstant(aGot, NCHILD);
    printf("  children lost             %5s  %10s  %8s\n",
           a?"CAUGHT":"missed", b?"CAUGHT":"missed", c?"CAUGHT":"missed");
    nBroken++; nCaughtTotal+=a; nCaughtParent+=b; nCaughtConst+=c;
  }
  ex3(db, "DELETE FROM comments; INSERT INTO comments SELECT * FROM bak;");

  /* --- 3. children duplicated ------------------------------------------- */
  ex3(db, "INSERT INTO comments SELECT post_id,cid+100000,body FROM comments"
          " WHERE post_id=9;");
  reassemble(db, "SELECT post_id,cid,body FROM comments ORDER BY post_id,cid",
             aGot, &nTotal);
  {
    int a = !checkTotal(nTotal, NTOTAL);
    int b = !checkPerParent(aGot, aDeclared);
    int c = !checkConstant(aGot, NCHILD);
    printf("  children duplicated       %5s  %10s  %8s\n",
           a?"CAUGHT":"missed", b?"CAUGHT":"missed", c?"CAUGHT":"missed");
    nBroken++; nCaughtTotal+=a; nCaughtParent+=b; nCaughtConst+=c;
  }
  ex3(db, "DELETE FROM comments; INSERT INTO comments SELECT * FROM bak;");

  /* --- 4a. MISATTRIBUTION, unbalanced: one child moved to the next parent */
  /* Total unchanged, but parent 3 now has 19 and parent 4 has 21.  This is the
  ** realistic shape of an off-by-one in a correlation key, and it is the case
  ** that separates per-parent counts from a total. */
  reassemble(db,
    "SELECT CASE WHEN cid=41 THEN 4 ELSE post_id END, cid, body"
    "  FROM comments ORDER BY 1, cid", aGot, &nTotal);
  {
    int a = !checkTotal(nTotal, NTOTAL);
    int b = !checkPerParent(aGot, aDeclared);
    int c = !checkConstant(aGot, NCHILD);
    printf("  misattributed (1 child)   %5s  %10s  %8s\n",
           a?"CAUGHT":"missed", b?"CAUGHT":"missed", c?"CAUGHT":"missed");
    nBroken++; nCaughtTotal+=a; nCaughtParent+=b; nCaughtConst+=c;
    check(!a, "moving one child keeps the TOTAL correct");
    check(b,  "but per-parent counts SEE it -- this is what they buy");
  }

  /* --- 4b. MISATTRIBUTION, balanced: two equal-sized parents swapped ----- */
  /* Every count is still exactly right at every granularity; only the CONTENT
  ** moved.  No count-based check can see this, at any cost, because counting
  ** is the wrong instrument for it.  Recorded so the limit is explicit rather
  ** than discovered later. */
  reassemble(db,
    "SELECT CASE WHEN post_id=3 THEN 4 WHEN post_id=4 THEN 3 ELSE post_id END,"
    "       cid, body FROM comments"
    " ORDER BY 1, cid", aGot, &nTotal);
  {
    int a = !checkTotal(nTotal, NTOTAL);
    int b = !checkPerParent(aGot, aDeclared);
    int c = !checkConstant(aGot, NCHILD);
    printf("  misattributed (swap 3/4)  %5s  %10s  %8s\n",
           a?"CAUGHT":"missed", b?"CAUGHT":"missed", c?"CAUGHT":"missed");
    nBroken++; nCaughtTotal+=a; nCaughtParent+=b; nCaughtConst+=c;
    check(!a && !b && !c,
          "a balanced swap is INVISIBLE to every count-based check");
  }

  /* --- 5. variable cardinality: the case CONSTANT cannot express --------- */
  ex3(db, "DELETE FROM comments WHERE post_id=11 AND cid % 4 != 0;");
  ex3(db, "UPDATE posts SET title=title;");
  {
    int aVar[NPARENT];
    int a, b, c;
    for(i=0; i<NPARENT; i++) aVar[i] = NCHILD;
    aVar[10] = 5;                       /* parent 11 legitimately has 5 */
    reassemble(db, "SELECT post_id,cid,body FROM comments ORDER BY post_id,cid",
               aGot, &nTotal);
    a = checkTotal(nTotal, NTOTAL-15);  /* declared total adjusted */
    b = checkPerParent(aGot, aVar);
    c = checkConstant(aGot, NCHILD);
    printf("  (legit variable counts)   %5s  %10s  %8s\n",
           a?"quiet":"ALARM", b?"quiet":"ALARM", c?"quiet":"ALARM");
    check(a && b, "total and per-parent accept legitimate variable cardinality");
    check(!c, "a CONSTANT declaration false-alarms on real data");
  }
  ex3(db, "DELETE FROM comments; INSERT INTO comments SELECT * FROM bak;");

  printf("\n  caught, of %d broken cases:  total %d,  per-parent %d,  constant %d\n",
         nBroken, nCaughtTotal, nCaughtParent, nCaughtConst);

  /* --- cost, through the real codec -------------------------------------- */
  printf("\n  cost to carry the counts:\n");
  printf("    total only    : %4d bytes (1 integer per response)\n",
         costOfIntegers(1));
  printf("    per-parent    : %4d bytes (%d integers)\n",
         costOfIntegers(NPARENT), NPARENT);
  printf("    constant      : %4d bytes (declared in the shape)\n", 0);

  check(nCaughtParent > nCaughtTotal,
        "per-parent catches strictly more than total-only");
  check(nCaughtTotal >= 3, "total-only is still worth having");

  sqlite3_close(db);
  printf("\n%s: %d checks, %d failures\n",
         nFail ? "NESTPOC3 FAILED" : "NESTPOC3 OK", nCheck, nFail);
  return nFail ? 1 : 0;
}
