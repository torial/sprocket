/*
** proc_nestpoc.c -- DOCKET #3, POC 1: is nesting worth adding at all?
**
** WHY THIS COMES BEFORE SYNTAX
**
** DOCKET #3 records four candidate designs for nested result shapes and Sean's
** steer to prototype rather than argue.  The first question is not which
** syntax but whether the feature earns its place: SQL already has three ways
** to return a parent with its children, and Tack's stitch layer implements all
** three.  If one of them is fine on real shapes, nesting is ceremony.
**
** So this measures the three strategies that exist TODAY, with no engine change
** at all, on the same data:
**
**   S1  join-flatten   one statement; parent columns REPEAT per child row
**   S3  JSON fold      one statement, one row per parent; children as JSON text
**   S4  multi-set      one CALL, two declared result sets; no repetition,
**                      but the client must correlate them
**
** METRICS ARE DETERMINISTIC, NOT TIMED.  Phase 5 of the transport plan taught
** the sharper form of that rule the hard way -- a count is no better than a
** stopwatch if the count is timing-dependent.  What is counted here is bytes
** on the wire (through the real codec) and values transferred, both of which
** are exact functions of the data and the strategy.
**
** Build (from repo root, VS dev prompt):
**   cl /nologo /O2 /I. /Itool tool\proc_nestpoc.c sqlite3.c /Fe:proc_nestpoc.exe
*/
#define PROC_WIRE_NO_MAIN 1
#include "proc_wire.c"           /* the wire codec: real byte counts */

/* nCheck / nFail / check() come from proc_wire.c */
#define chk check

#define NPARENT   10
#define NCHILD    20

static void execOr(sqlite3 *db, const char *zSql){
  char *zErr = 0;
  if( sqlite3_exec(db, zSql, 0, 0, &zErr)!=SQLITE_OK ){
    printf("FATAL: %s\n  sql: %s\n", zErr, zSql);
    exit(1);
  }
}

/* Encode whatever a prepared statement produces, and report the wire size. */
static int wireSizeOf(sqlite3_stmt *p, int bShapes, int *pnValues){
  WireBuf b;
  int nVal = 0, n;
  int nCol = sqlite3_column_count(p);
  memset(&b, 0, sizeof(b));
  if( bShapes ){
    int i;
    wbU8(&b, WF_SHAPE);
    wbU32(&b, (unsigned)nCol);
    for(i=0; i<nCol; i++){
      wbStr(&b, sqlite3_column_name(p,i));
      wbStr(&b, sqlite3_column_decltype(p,i));
    }
  }
  while( sqlite3_step(p)==SQLITE_ROW ){
    int i;
    wbU8(&b, WF_ROW);
    for(i=0; i<nCol; i++){
      const unsigned char *z;
      switch( sqlite3_column_type(p,i) ){
        case SQLITE_INTEGER: wbU8(&b, WV_INT);
                             wbI64(&b, sqlite3_column_int64(p,i)); break;
        case SQLITE_FLOAT:   wbU8(&b, WV_FLOAT);
                             wbDouble(&b, sqlite3_column_double(p,i)); break;
        case SQLITE_NULL:    wbU8(&b, WV_NULL); break;
        default:
          z = sqlite3_column_text(p,i);
          wbU8(&b, WV_TEXT);
          wbBytes(&b, z, sqlite3_column_bytes(p,i));
          break;
      }
      nVal++;
    }
  }
  wbU8(&b, WF_SETEND);
  n = b.n;
  wbFree(&b);
  if( pnValues ) *pnValues += nVal;
  return n;
}

int main(int argc, char **argv){
  sqlite3 *db = 0;
  sqlite3_stmt *p1 = 0, *p2 = 0;
  int nS1, nS3, nS4a, nS4b, nS4;
  int vS1 = 0, vS3 = 0, vS4 = 0;
  int i, j;

  (void)argc; (void)argv;
  printf("proc_nestpoc -- DOCKET #3 POC 1: does nesting earn its place?\n");
  printf("SQLite %s;  %d parents x %d children\n\n",
         sqlite3_libversion(), NPARENT, NCHILD);

  if( sqlite3_open(":memory:", &db)!=SQLITE_OK ){ printf("open failed\n"); return 1; }
  execOr(db,
    "CREATE TABLE posts(id INTEGER, title TEXT, author TEXT, created TEXT);"
    "CREATE TABLE comments(cid INTEGER, post_id INTEGER, body TEXT);");
  execOr(db, "BEGIN;");
  for(i=1; i<=NPARENT; i++){
    char *z = sqlite3_mprintf(
      "INSERT INTO posts VALUES(%d,'A reasonably typical post title %d',"
      "'author-%d','2026-08-03T12:00:00Z');", i, i, i);
    execOr(db, z); sqlite3_free(z);
    for(j=1; j<=NCHILD; j++){
      z = sqlite3_mprintf("INSERT INTO comments VALUES(%d,%d,"
        "'A comment body of unremarkable length, number %d.');",
        (i-1)*NCHILD+j, i, j);
      execOr(db, z); sqlite3_free(z);
    }
  }
  execOr(db, "COMMIT;");
  execOr(db, "CREATE INDEX ci ON comments(post_id);");

  /* ---- S1: join-flatten -- parent columns repeat on every child row ------ */
  sqlite3_prepare_v2(db,
    "SELECT p.id,p.title,p.author,p.created,c.cid,c.body "
    "  FROM posts p JOIN comments c ON c.post_id=p.id ORDER BY p.id,c.cid",
    -1, &p1, 0);
  nS1 = wireSizeOf(p1, 1, &vS1);
  sqlite3_finalize(p1); p1 = 0;

  /* ---- S3: JSON fold -- one row per parent, children as JSON text -------- */
  sqlite3_prepare_v2(db,
    "SELECT p.id,p.title,p.author,p.created,"
    "  (SELECT json_group_array(json_array(c.cid,c.body))"
    "     FROM comments c WHERE c.post_id=p.id) "
    "  FROM posts p ORDER BY p.id",
    -1, &p1, 0);
  nS3 = wireSizeOf(p1, 1, &vS3);
  sqlite3_finalize(p1); p1 = 0;

  /* ---- S4: two result sets -- what the fork can already do --------------- */
  sqlite3_prepare_v2(db,
    "SELECT id,title,author,created FROM posts ORDER BY id", -1, &p1, 0);
  sqlite3_prepare_v2(db,
    "SELECT post_id,cid,body FROM comments ORDER BY post_id,cid", -1, &p2, 0);
  nS4a = wireSizeOf(p1, 1, &vS4);
  nS4b = wireSizeOf(p2, 1, &vS4);
  nS4 = nS4a + nS4b;
  sqlite3_finalize(p1); sqlite3_finalize(p2);

  printf("  strategy                       bytes    values   notes\n");
  printf("  ---------------------------  --------  --------  ------------------\n");
  printf("  S1 join-flatten              %8d  %8d  parent cols repeat\n", nS1, vS1);
  printf("  S3 JSON fold                 %8d  %8d  children as text\n",  nS3, vS3);
  printf("  S4 two result sets           %8d  %8d  needs correlation\n", nS4, vS4);
  printf("\n");
  printf("  S4 vs S1: %.1f%% fewer bytes,  %.1f%% fewer values\n",
         100.0*(nS1-nS4)/(double)nS1, 100.0*(vS1-vS4)/(double)vS1);
  printf("  S4 vs S3: %.1f%% fewer bytes\n", 100.0*(nS3-nS4)/(double)nS3);

  /* ---- what the numbers have to show for the feature to be worth it ----- */
  chk(nS1>0 && nS3>0 && nS4>0, "all three strategies produced a stream");

  /* The whole argument for a nested/multi-set shape is that S1 repeats the
  ** parent on every child row.  If that repetition is not visible here, the
  ** shape chosen for the test is not one the feature would help. */
  chk(vS1 == NPARENT*NCHILD*6,
      "S1 transfers parent columns once PER CHILD (the redundancy)");
  chk(vS4 == NPARENT*4 + NPARENT*NCHILD*3,
      "S4 transfers each parent exactly once");
  chk(nS4 < nS1, "S4 is smaller than join-flatten on this shape");

  /* CONTROL, and the honest half: with ONE child per parent there is nothing
  ** to deduplicate, so S4 must NOT win.  A feature that looks good on every
  ** input is usually being measured wrong. */
  {
    int nFlat1 = 0, nSet1 = 0, vTmp = 0;
    execOr(db, "DELETE FROM comments WHERE cid % 20 != 1;");
    sqlite3_prepare_v2(db,
      "SELECT p.id,p.title,p.author,p.created,c.cid,c.body "
      "  FROM posts p JOIN comments c ON c.post_id=p.id ORDER BY p.id,c.cid",
      -1, &p1, 0);
    nFlat1 = wireSizeOf(p1, 1, &vTmp);
    sqlite3_finalize(p1); p1 = 0;
    sqlite3_prepare_v2(db,
      "SELECT id,title,author,created FROM posts ORDER BY id", -1, &p1, 0);
    sqlite3_prepare_v2(db,
      "SELECT post_id,cid,body FROM comments ORDER BY post_id,cid", -1, &p2, 0);
    nSet1 = wireSizeOf(p1, 1, &vTmp) + wireSizeOf(p2, 1, &vTmp);
    sqlite3_finalize(p1); sqlite3_finalize(p2);
    printf("\n  CONTROL, 1 child per parent:  S1 %d bytes,  S4 %d bytes\n",
           nFlat1, nSet1);
    chk(nSet1 >= nFlat1,
        "CONTROL: with one child per parent, splitting does NOT win");
  }

  /* ---- the finding that actually decides this -------------------------- */
  /* S3 is the SMALLEST above, which kills "fewer bytes" as the argument for
  ** nesting.  So what does a structured child set buy that JSON does not?
  ** Type fidelity.  JSON has no binary type: a BLOB child column cannot
  ** survive json_group_array() without an encoding the client must undo. */
  {
    sqlite3_stmt *pj = 0, *ps = 0;
    int tJson = -1, tSet = -1;
    execOr(db, "CREATE TABLE att(post_id INTEGER, blob_col BLOB);");
    execOr(db, "INSERT INTO att VALUES(1, x'00ff00fe');");

    if( sqlite3_prepare_v2(db,
          "SELECT json_group_array(blob_col) FROM att WHERE post_id=1",
          -1, &pj, 0)==SQLITE_OK && sqlite3_step(pj)==SQLITE_ROW ){
      tJson = sqlite3_column_type(pj, 0);
    }
    sqlite3_finalize(pj);

    if( sqlite3_prepare_v2(db, "SELECT blob_col FROM att WHERE post_id=1",
          -1, &ps, 0)==SQLITE_OK && sqlite3_step(ps)==SQLITE_ROW ){
      tSet = sqlite3_column_type(ps, 0);
    }
    sqlite3_finalize(ps);

    printf("\n  BLOB child column:  via JSON fold -> type %d,"
           "  via result set -> type %d\n", tJson, tSet);
    printf("  (SQLITE_BLOB=%d, SQLITE_TEXT=%d)\n", SQLITE_BLOB, SQLITE_TEXT);
    check(tSet==SQLITE_BLOB, "a result set preserves BLOB as BLOB");
    check(tJson!=SQLITE_BLOB,
          "JSON fold does NOT preserve BLOB -- the client must decode it");
  }

  sqlite3_close(db);
  printf("\n%s: %d checks, %d failures\n",
         nFail ? "NESTPOC FAILED" : "NESTPOC OK", nCheck, nFail);
  return nFail ? 1 : 0;
}
