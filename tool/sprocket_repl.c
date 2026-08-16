/*
** sprocket_repl.c -- PLAN-REPL R0-R2: the segment contract, the
** writer, and the applier, as a self-testing harness (the transport-
** phase discipline).
**
** THE RULED SHAPE (DESIGN-REPL, all five ruled 2026-08-15)
**
** Replication ships SESSION CHANGESETS in commit-sequence order, cut
** into SELF-DESCRIBING SEGMENTS -- the Q4 ruling's three UNGIT
** commitments are load-bearing here and each is pinned by a check:
**
**   1. Self-description: a segment carries magic, format version, the
**      database's lineage (genesis id), its commit-seq range, payload
**      length, and a CRC.  Everything an operator needs is in the
**      bytes on disk.
**   2. REFUSE, NEVER SKIP: apply refuses a gap (naming expected/got),
**      a foreign lineage (naming both), a bad checksum, and a
**      conflicting replica (divergence) -- and a refusal changes
**      NOTHING (inertness is asserted, and the reason is RETAINED as
**      a receipt).  POC 1b's frozen-silent replica is the negative
**      control this design exists to kill.
**   3. Declared sources: this library takes paths and connections as
**      ARGUMENTS.  Nothing watches a directory.
**
** Lineage and receipts live in an ordinary table (sprocket_repl_meta)
** on each side: the primary's holds (genesis, last_cut_seq); the
** replica's holds (genesis, last_seq, last_utc, last_error) -- the
** backing store for PRAGMA replica_status (R3).
**
** SEGMENT FORMAT v1 (all integers big-endian):
**   offset  size  field
**   0       8     magic "SPRKSEG1"
**   8       4     format version (1)
**   12      16    genesis id
**   28      8     seq_from
**   36      8     seq_to      (v1 cuts one commit per segment: == from)
**   44      8     payload length N
**   52      4     CRC-32 of payload
**   56      N     payload: the changeset bytes
**
** Build (VS dev prompt, repo root):
**   cl /O2 /I. -DSQLITE_ENABLE_SESSION -DSQLITE_ENABLE_PREUPDATE_HOOK
**      tool\sprocket_repl.c sqlite3.c /Fe:sprocket_repl.exe
** Selftest:  sprocket_repl --selftest
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sqlite3.h"

#define SEG_MAGIC   "SPRKSEG1"
#define SEG_VERSION 1
#define SEG_HDRSIZE 56

/* ------------------------------------------------------------- crc32 ---- */
static unsigned int crc32buf(const unsigned char *z, int n){
  static unsigned int aTab[256];
  static int init = 0;
  unsigned int c;
  int i, k;
  if( !init ){
    for(i=0; i<256; i++){
      c = (unsigned int)i;
      for(k=0; k<8; k++) c = (c&1) ? 0xEDB88320u ^ (c>>1) : c>>1;
      aTab[i] = c;
    }
    init = 1;
  }
  c = 0xFFFFFFFFu;
  for(i=0; i<n; i++) c = aTab[(c ^ z[i]) & 0xFF] ^ (c>>8);
  return c ^ 0xFFFFFFFFu;
}

/* -------------------------------------------------------- byte helpers -- */
static void putU32(unsigned char *p, unsigned int v){
  p[0]=(unsigned char)(v>>24); p[1]=(unsigned char)(v>>16);
  p[2]=(unsigned char)(v>>8);  p[3]=(unsigned char)v;
}
static void putU64(unsigned char *p, sqlite3_uint64 v){
  putU32(p, (unsigned int)(v>>32)); putU32(p+4, (unsigned int)v);
}
static unsigned int getU32(const unsigned char *p){
  return ((unsigned int)p[0]<<24)|((unsigned int)p[1]<<16)
       | ((unsigned int)p[2]<<8)|p[3];
}
static sqlite3_uint64 getU64(const unsigned char *p){
  return (((sqlite3_uint64)getU32(p))<<32) | getU32(p+4);
}

/* ------------------------------------------------------------ meta ------ */
/* Ordinary-named receipts table, both sides.  role: 'primary'|'replica'. */
static int replMetaEnsure(sqlite3 *db){
  return sqlite3_exec(db,
    "CREATE TABLE IF NOT EXISTS sprocket_repl_meta("
    " role TEXT PRIMARY KEY, genesis BLOB, last_seq INTEGER,"
    " last_utc TEXT, last_error TEXT)", 0, 0, 0);
}
static int replMetaGetBlob(sqlite3 *db, const char *zRole,
                           unsigned char *aOut16, sqlite3_int64 *pSeq){
  sqlite3_stmt *p = 0;
  int rc, bFound = 0;
  rc = sqlite3_prepare_v2(db,
      "SELECT genesis, coalesce(last_seq,0) FROM sprocket_repl_meta"
      " WHERE role=?1", -1, &p, 0);
  if( rc!=SQLITE_OK ) return -1;
  sqlite3_bind_text(p, 1, zRole, -1, SQLITE_STATIC);
  if( sqlite3_step(p)==SQLITE_ROW ){
    const void *z = sqlite3_column_blob(p, 0);
    if( z && sqlite3_column_bytes(p, 0)==16 ){
      memcpy(aOut16, z, 16);
      bFound = 1;
    }
    if( pSeq ) *pSeq = sqlite3_column_int64(p, 1);
  }
  sqlite3_finalize(p);
  return bFound ? 0 : -1;
}

/* The receipts table is bookkeeping, not payload: the writer's own
** meta updates would otherwise ride the NEXT segment as noise (the
** first selftest run caught exactly that -- the in-code comment that
** claimed the updates fell outside the window was wrong). */
static int replTableFilter(void *pCtx, const char *zTab){
  (void)pCtx;
  return sqlite3_stricmp(zTab, "sprocket_repl_meta")!=0;
}

/* ---------------------------------------------------------- encoder ---- */
/* Encode a segment as a PURE function of (genesis, seq range, payload):
** no clock, no counter, no hidden state.  Determinism is PLAN-REPL's
** archive-comparability pin -- the same changeset must yield the same
** bytes across time and machines, or archives cannot be compared. */
static unsigned char *replSegmentBuild(const unsigned char *aGen,
    sqlite3_int64 iFrom, sqlite3_int64 iTo,
    const unsigned char *pCh, int nCh){
  unsigned char *aSeg = (unsigned char*)sqlite3_malloc(SEG_HDRSIZE + nCh);
  if( aSeg==0 ) return 0;
  memcpy(aSeg, SEG_MAGIC, 8);
  putU32(aSeg+8, SEG_VERSION);
  memcpy(aSeg+12, aGen, 16);
  putU64(aSeg+28, (sqlite3_uint64)iFrom);
  putU64(aSeg+36, (sqlite3_uint64)iTo);
  putU64(aSeg+44, (sqlite3_uint64)nCh);
  putU32(aSeg+52, crc32buf(pCh, nCh));
  memcpy(aSeg+SEG_HDRSIZE, pCh, nCh);
  return aSeg;
}

/* ------------------------------------------------------------ writer ---- */
typedef struct ReplWriter {
  sqlite3 *db;
  sqlite3_session *pSession;
  unsigned char aGenesis[16];
  sqlite3_int64 iLastCut;
} ReplWriter;

/*
** Open a writer on the given connection: ensure meta, mint or load the
** genesis, attach a session to every table in 'main'.
*/
static int replWriterOpen(sqlite3 *db, ReplWriter **ppW){
  ReplWriter *pW;
  int rc;
  *ppW = 0;
  pW = (ReplWriter*)sqlite3_malloc(sizeof(*pW));
  if( pW==0 ) return SQLITE_NOMEM;
  memset(pW, 0, sizeof(*pW));
  pW->db = db;
  rc = replMetaEnsure(db);
  if( rc!=SQLITE_OK ){ sqlite3_free(pW); return rc; }
  if( replMetaGetBlob(db, "primary", pW->aGenesis, &pW->iLastCut)!=0 ){
    sqlite3_stmt *p = 0;
    sqlite3_randomness(16, pW->aGenesis);
    rc = sqlite3_prepare_v2(db,
        "INSERT INTO sprocket_repl_meta(role, genesis, last_seq)"
        " VALUES('primary', ?1, 0)", -1, &p, 0);
    if( rc==SQLITE_OK ){
      sqlite3_bind_blob(p, 1, pW->aGenesis, 16, SQLITE_TRANSIENT);
      sqlite3_step(p);
    }
    sqlite3_finalize(p);
    pW->iLastCut = 0;
  }
  rc = sqlite3session_create(db, "main", &pW->pSession);
  if( rc==SQLITE_OK ){
    rc = sqlite3session_attach(pW->pSession, 0);   /* all tables */
    sqlite3session_table_filter(pW->pSession, replTableFilter, 0);
  }
  if( rc!=SQLITE_OK ){
    if( pW->pSession ) sqlite3session_delete(pW->pSession);
    sqlite3_free(pW);
    return rc;
  }
  *ppW = pW;
  return SQLITE_OK;
}

/*
** Cut everything the session has captured since the last cut into ONE
** segment (the caller invokes this after a commit -- v1's live
** cadence).  Returns SQLITE_OK and *pzSeg/*pnSeg (sqlite3_malloc'd),
** or SQLITE_DONE when the session captured nothing.
*/
static int replWriterCut(ReplWriter *pW, unsigned char **paSeg, int *pnSeg){
  int nCh = 0;
  void *pCh = 0;
  unsigned char *aSeg;
  sqlite3_int64 iSeq;
  int rc;

  *paSeg = 0; *pnSeg = 0;
  rc = sqlite3session_changeset(pW->pSession, &nCh, &pCh);
  if( rc!=SQLITE_OK ) return rc;
  if( nCh==0 ){
    sqlite3_free(pCh);
    return SQLITE_DONE;
  }
  iSeq = pW->iLastCut + 1;
  aSeg = replSegmentBuild(pW->aGenesis, iSeq, iSeq,
                          (const unsigned char*)pCh, nCh);
  sqlite3_free(pCh);
  if( aSeg==0 ) return SQLITE_NOMEM;

  /* restart the session for the next window: delete + recreate */
  sqlite3session_delete(pW->pSession);
  pW->pSession = 0;
  rc = sqlite3session_create(pW->db, "main", &pW->pSession);
  if( rc==SQLITE_OK ){
    rc = sqlite3session_attach(pW->pSession, 0);
    sqlite3session_table_filter(pW->pSession, replTableFilter, 0);
  }
  if( rc!=SQLITE_OK ){ sqlite3_free(aSeg); return rc; }

  /* the cut receipt (filtered out of the capture window) */
  {
    char *zSql = sqlite3_mprintf(
      "UPDATE sprocket_repl_meta SET last_seq=%lld,"
      " last_utc=strftime('%%Y-%%m-%%d %%H:%%M:%%f','now')"
      " WHERE role='primary'",
      iSeq);
    sqlite3_exec(pW->db, zSql, 0, 0, 0);
    sqlite3_free(zSql);
  }
  pW->iLastCut = iSeq;
  *paSeg = aSeg;
  *pnSeg = SEG_HDRSIZE + nCh;
  return SQLITE_OK;
}

static void replWriterClose(ReplWriter *pW){
  if( pW==0 ) return;
  if( pW->pSession ) sqlite3session_delete(pW->pSession);
  sqlite3_free(pW);
}

/* ----------------------------------------------------------- applier ---- */
/* Conflict = divergence = refuse; the handler aborts the whole apply. */
static int replConflict(void *pCtx, int eConflict, sqlite3_changeset_iter *p){
  (void)pCtx; (void)eConflict; (void)p;
  return SQLITE_CHANGESET_ABORT;
}

/* Record the refusal receipt (never silent) and return its code. */
static int replRefuse(sqlite3 *db, const char *zErr, char **pzErr){
  char *zSql = sqlite3_mprintf(
    "UPDATE sprocket_repl_meta SET last_error=%Q WHERE role='replica'",
    zErr);
  if( zSql ){ sqlite3_exec(db, zSql, 0, 0, 0); sqlite3_free(zSql); }
  if( pzErr ) *pzErr = sqlite3_mprintf("%s", zErr);
  return SQLITE_CONSTRAINT;
}

/*
** Apply one segment to the replica connection.  Refuses -- never
** skips -- on: bad header, version, foreign lineage, gap, checksum,
** or divergence (conflict).  On success updates the receipts.
** *pzErr (sqlite3_malloc'd) carries the refusal text.
*/
static int replApply(sqlite3 *db, const unsigned char *aSeg, int nSeg,
                     char **pzErr){
  unsigned char aGen[16], aMyGen[16];
  sqlite3_int64 iFrom, iTo, iLast = 0;
  sqlite3_uint64 nPay;
  int rc;
  char zMsg[200];

  if( pzErr ) *pzErr = 0;
  rc = replMetaEnsure(db);
  if( rc!=SQLITE_OK ) return rc;

  if( nSeg<SEG_HDRSIZE || memcmp(aSeg, SEG_MAGIC, 8)!=0 ){
    return replRefuse(db, "not a sprocket segment (bad magic)", pzErr);
  }
  if( getU32(aSeg+8)!=SEG_VERSION ){
    sqlite3_snprintf(sizeof(zMsg), zMsg,
      "segment format %u; this build speaks %d", getU32(aSeg+8),
      SEG_VERSION);
    return replRefuse(db, zMsg, pzErr);
  }
  memcpy(aGen, aSeg+12, 16);
  iFrom = (sqlite3_int64)getU64(aSeg+28);
  iTo   = (sqlite3_int64)getU64(aSeg+36);
  nPay  = getU64(aSeg+44);
  if( (sqlite3_uint64)(nSeg-SEG_HDRSIZE)!=nPay ){
    return replRefuse(db, "segment truncated (payload length mismatch)",
                      pzErr);
  }
  if( crc32buf(aSeg+SEG_HDRSIZE, (int)nPay)!=getU32(aSeg+52) ){
    return replRefuse(db, "segment checksum mismatch (corrupt in "
                      "transit or at rest)", pzErr);
  }

  if( replMetaGetBlob(db, "replica", aMyGen, &iLast)!=0 ){
    /* first contact: adopt this lineage at its FIRST segment only */
    if( iFrom!=1 ){
      sqlite3_snprintf(sizeof(zMsg), zMsg,
        "replica has no lineage and this segment starts at seq %lld; "
        "seed from the archive's beginning (seq 1)", iFrom);
      return replRefuse(db, zMsg, pzErr);
    }
    {
      sqlite3_stmt *p = 0;
      rc = sqlite3_prepare_v2(db,
        "INSERT INTO sprocket_repl_meta(role, genesis, last_seq)"
        " VALUES('replica', ?1, 0)", -1, &p, 0);
      if( rc==SQLITE_OK ){
        sqlite3_bind_blob(p, 1, aGen, 16, SQLITE_TRANSIENT);
        sqlite3_step(p);
      }
      sqlite3_finalize(p);
      memcpy(aMyGen, aGen, 16);
      iLast = 0;
    }
  }
  if( memcmp(aGen, aMyGen, 16)!=0 ){
    return replRefuse(db, "segment is from a different database "
      "lineage; this replica follows another primary", pzErr);
  }
  if( iFrom != iLast+1 ){
    sqlite3_snprintf(sizeof(zMsg), zMsg,
      "gap: expected seq %lld, got %lld; fetch the missing segments",
      iLast+1, iFrom);
    return replRefuse(db, zMsg, pzErr);
  }

  rc = sqlite3changeset_apply(db, (int)nPay,
          (void*)(aSeg+SEG_HDRSIZE), 0, replConflict, 0);
  if( rc!=SQLITE_OK ){
    sqlite3_snprintf(sizeof(zMsg), zMsg,
      "segment %lld conflicts: the replica has diverged from the "
      "lineage (local writes?)", iFrom);
    return replRefuse(db, zMsg, pzErr);
  }
  {
    char *zSql = sqlite3_mprintf(
      "UPDATE sprocket_repl_meta SET last_seq=%lld,"
      " last_utc=strftime('%%Y-%%m-%%d %%H:%%M:%%f','now'),"
      " last_error=NULL WHERE role='replica'", iTo);
    sqlite3_exec(db, zSql, 0, 0, 0);
    sqlite3_free(zSql);
  }
  return SQLITE_OK;
}

/* ============================== selftest ================================ */
/* Everything above is the LIBRARY: sprocketd #includes this file with
** SPROCKET_REPL_LIBRARY defined to reuse writer/applier/encoder (one
** implementation of the segment contract, two homes).  The selftest
** below exists only in the standalone build. */
#ifndef SPROCKET_REPL_LIBRARY
static int nCheck = 0, nFail = 0;
static void check(int bOk, const char *zWhat){
  nCheck++;
  if( !bOk ){ nFail++; printf("  FAIL: %s\n", zWhat); }
}

int main(int argc, char **argv){
  sqlite3 *dbP = 0, *dbR = 0, *dbX = 0;
  ReplWriter *pW = 0, *pWX = 0;
  unsigned char *aSeg1=0, *aSeg2=0, *aSeg3=0, *aSegX=0;
  int n1=0, n2=0, n3=0, nX=0;
  char *zErr = 0;
  int rc;

  if( argc<2 || strcmp(argv[1], "--selftest")!=0 ){
    fprintf(stderr, "usage: sprocket_repl --selftest\n");
    return 2;
  }
  printf("sprocket_repl selftest -- SQLite %s\n\n", sqlite3_libversion());

  sqlite3_open(":memory:", &dbP);
  sqlite3_open(":memory:", &dbR);
  sqlite3_exec(dbP, "CREATE TABLE t(a INTEGER PRIMARY KEY, b TEXT)",0,0,0);
  sqlite3_exec(dbR, "CREATE TABLE t(a INTEGER PRIMARY KEY, b TEXT)",0,0,0);

  /* 1 -- writer opens, mints a genesis, self-describes */
  rc = replWriterOpen(dbP, &pW);
  check( rc==SQLITE_OK, "writer opens and mints a genesis" );
  sqlite3_exec(dbP, "INSERT INTO t VALUES(1,'one')", 0, 0, 0);
  rc = replWriterCut(pW, &aSeg1, &n1);
  check( rc==SQLITE_OK && n1>SEG_HDRSIZE, "first cut yields a segment" );
  check( memcmp(aSeg1, SEG_MAGIC, 8)==0, "self-description: magic" );
  check( getU32(aSeg1+8)==SEG_VERSION, "self-description: version" );
  check( getU64(aSeg1+28)==1 && getU64(aSeg1+36)==1,
         "self-description: seq range [1,1]" );
  check( getU64(aSeg1+44)==(sqlite3_uint64)(n1-SEG_HDRSIZE),
         "self-description: payload length" );

  /* 2 -- empty window cuts nothing (no fabricated segments) */
  { unsigned char *aE=0; int nE=0;
    rc = replWriterCut(pW, &aE, &nE);
    check( rc==SQLITE_DONE && aE==0, "empty window cuts NOTHING" ); }

  /* 3 -- round trip: apply to replica, states equal */
  rc = replApply(dbR, aSeg1, n1, &zErr);
  check( rc==SQLITE_OK, "segment 1 applies" );
  sqlite3_free(zErr); zErr = 0;
  {
    sqlite3_stmt *p = 0; int n = -1;
    sqlite3_prepare_v2(dbR, "SELECT count(*) FROM t", -1, &p, 0);
    if( sqlite3_step(p)==SQLITE_ROW ) n = sqlite3_column_int(p, 0);
    sqlite3_finalize(p);
    check( n==1, "replica state equals primary after apply" );
  }

  /* 4 -- gap refusal, by name, inert */
  sqlite3_exec(dbP, "INSERT INTO t VALUES(2,'two')", 0, 0, 0);
  replWriterCut(pW, &aSeg2, &n2);
  sqlite3_exec(dbP, "INSERT INTO t VALUES(3,'three')", 0, 0, 0);
  replWriterCut(pW, &aSeg3, &n3);
  rc = replApply(dbR, aSeg3, n3, &zErr);   /* skips seg 2 */
  check( rc!=SQLITE_OK, "gap segment refused" );
  check( zErr && strstr(zErr, "expected seq 2, got 3")!=0,
         "gap refusal names expected/got" );
  sqlite3_free(zErr); zErr = 0;
  {
    sqlite3_stmt *p = 0; int n = -1;
    sqlite3_prepare_v2(dbR, "SELECT count(*) FROM t", -1, &p, 0);
    if( sqlite3_step(p)==SQLITE_ROW ) n = sqlite3_column_int(p, 0);
    sqlite3_finalize(p);
    check( n==1, "refusal is INERT: replica unchanged" );
  }

  /* 5 -- the receipts (backing for PRAGMA replica_status) */
  {
    sqlite3_stmt *p = 0; const char *zLE = 0; sqlite3_int64 seq = -1;
    sqlite3_prepare_v2(dbR,
      "SELECT last_seq, last_error FROM sprocket_repl_meta"
      " WHERE role='replica'", -1, &p, 0);
    if( sqlite3_step(p)==SQLITE_ROW ){
      seq = sqlite3_column_int64(p, 0);
      zLE = (const char*)sqlite3_column_text(p, 1);
    }
    check( seq==1, "receipt: last_seq" );
    check( zLE && strstr(zLE, "gap")!=0,
           "receipt: the refusal reason is RETAINED (no silent freeze)" );
    sqlite3_finalize(p);
  }

  /* 6 -- in-order apply proceeds; receipts clear */
  check( replApply(dbR, aSeg2, n2, 0)==SQLITE_OK, "segment 2 applies" );
  check( replApply(dbR, aSeg3, n3, 0)==SQLITE_OK, "segment 3 applies" );
  {
    sqlite3_stmt *p = 0; int n = -1; const unsigned char *zLE = (const unsigned char*)"x";
    sqlite3_prepare_v2(dbR,
      "SELECT (SELECT count(*) FROM t),"
      " (SELECT last_error FROM sprocket_repl_meta WHERE role='replica')",
      -1, &p, 0);
    if( sqlite3_step(p)==SQLITE_ROW ){
      n = sqlite3_column_int(p, 0);
      zLE = sqlite3_column_text(p, 1);
    }
    check( n==3, "replica caught up" );
    check( zLE==0, "receipt: last_error cleared on success" );
    sqlite3_finalize(p);
  }

  /* 7 -- checksum refusal */
  aSeg2[SEG_HDRSIZE] ^= 0x40;
  rc = replApply(dbR, aSeg2, n2, &zErr);
  check( rc!=SQLITE_OK && zErr && strstr(zErr, "checksum")!=0,
         "flipped byte refused by checksum" );
  sqlite3_free(zErr); zErr = 0;
  aSeg2[SEG_HDRSIZE] ^= 0x40;

  /* 8 -- foreign lineage refusal */
  sqlite3_open(":memory:", &dbX);
  sqlite3_exec(dbX, "CREATE TABLE t(a INTEGER PRIMARY KEY, b TEXT)",0,0,0);
  replWriterOpen(dbX, &pWX);
  sqlite3_exec(dbX, "INSERT INTO t VALUES(9,'alien')", 0, 0, 0);
  replWriterCut(pWX, &aSegX, &nX);
  rc = replApply(dbR, aSegX, nX, &zErr);
  check( rc!=SQLITE_OK && zErr && strstr(zErr, "lineage")!=0,
         "foreign-lineage segment refused by name" );
  sqlite3_free(zErr); zErr = 0;

  /* 9 -- divergence refusal: local write on the replica, then apply */
  sqlite3_exec(dbP, "UPDATE t SET b='ONE' WHERE a=1", 0, 0, 0);
  { unsigned char *aSeg4=0; int n4=0;
    replWriterCut(pW, &aSeg4, &n4);
    sqlite3_exec(dbR, "UPDATE t SET b='local-drift' WHERE a=1", 0, 0, 0);
    rc = replApply(dbR, aSeg4, n4, &zErr);
    check( rc!=SQLITE_OK && zErr && strstr(zErr, "diverged")!=0,
           "diverged replica refused (never silently overwritten)" );
    sqlite3_free(zErr); zErr = 0;
    sqlite3_free(aSeg4); }

  /* 10 -- mid-lineage seeding refused (first contact must start at 1) */
  { sqlite3 *dbN = 0;
    sqlite3_open(":memory:", &dbN);
    sqlite3_exec(dbN, "CREATE TABLE t(a INTEGER PRIMARY KEY, b TEXT)",0,0,0);
    rc = replApply(dbN, aSeg3, n3, &zErr);
    check( rc!=SQLITE_OK && zErr && strstr(zErr, "seed")!=0,
           "fresh replica refuses a mid-lineage segment, names the fix" );
    sqlite3_free(zErr); zErr = 0;
    sqlite3_close(dbN); }

  /* 11 -- determinism: the segment is a pure function of its own
  ** described fields.  Re-encode segment 1 from its parsed header and
  ** payload: byte-identical to the original (so the header withholds
  ** nothing -- self-description is COMPLETE), and a second encoding
  ** equals the first (no hidden clock or counter). */
  {
    sqlite3_int64 iF = (sqlite3_int64)getU64(aSeg1+28);
    sqlite3_int64 iT = (sqlite3_int64)getU64(aSeg1+36);
    unsigned char *aRe1 = replSegmentBuild(aSeg1+12, iF, iT,
                              aSeg1+SEG_HDRSIZE, n1-SEG_HDRSIZE);
    unsigned char *aRe2 = replSegmentBuild(aSeg1+12, iF, iT,
                              aSeg1+SEG_HDRSIZE, n1-SEG_HDRSIZE);
    check( aRe1 && memcmp(aRe1, aSeg1, n1)==0,
           "determinism: re-encoding from parsed fields is byte-identical" );
    check( aRe1 && aRe2 && memcmp(aRe1, aRe2, n1)==0,
           "determinism: two encodings of the same changeset agree" );
    sqlite3_free(aRe1); sqlite3_free(aRe2);
  }

  /* 12 -- R3: PRAGMA replica_status, the truth surface over the
  ** receipts.  Freshness never arrives without its basis (lag_source);
  ** a refusal is VISIBLE at the surface, not only in a table the
  ** operator would have to know about; and a database with no
  ** replication state says so in words -- an empty result would be
  ** indistinguishable from an unrecognized pragma. */
  {
    sqlite3_stmt *p = 0;
    rc = sqlite3_prepare_v2(dbR, "PRAGMA replica_status", -1, &p, 0);
    check( rc==SQLITE_OK && sqlite3_step(p)==SQLITE_ROW,
           "replica_status answers on the replica" );
    check( p && sqlite3_column_count(p)==5
        && strcmp(sqlite3_column_name(p,3), "lag_source")==0,
           "replica_status: five columns, basis column named" );
    {
      const char *zRole = (const char*)sqlite3_column_text(p, 0);
      const char *zSrc  = (const char*)sqlite3_column_text(p, 3);
      const char *zLE   = (const char*)sqlite3_column_text(p, 4);
      check( zRole && strcmp(zRole, "replica")==0
          && sqlite3_column_int64(p, 1)==3
          && sqlite3_column_type(p, 2)==SQLITE_TEXT
          && zSrc && strcmp(zSrc, "apply-clock")==0,
             "replica row: seq 3, a UTC, and its basis" );
      check( zLE && strstr(zLE, "diverged")!=0,
             "the divergence refusal is VISIBLE at the pragma surface" );
    }
    sqlite3_finalize(p); p = 0;

    rc = sqlite3_prepare_v2(dbP, "PRAGMA replica_status", -1, &p, 0);
    check( rc==SQLITE_OK && sqlite3_step(p)==SQLITE_ROW, "answers on the primary" );
    {
      const char *zRole = (const char*)sqlite3_column_text(p, 0);
      const char *zSrc  = (const char*)sqlite3_column_text(p, 3);
      check( zRole && strcmp(zRole, "primary")==0
          && sqlite3_column_int64(p, 1)==4
          && sqlite3_column_type(p, 2)==SQLITE_TEXT
          && zSrc && strcmp(zSrc, "cut-clock")==0
          && sqlite3_column_type(p, 4)==SQLITE_NULL,
             "primary row: seq 4, cut-clock basis, no error" );
    }
    sqlite3_finalize(p); p = 0;

    { sqlite3 *dbF = 0;
      sqlite3_open(":memory:", &dbF);
      rc = sqlite3_prepare_v2(dbF, "PRAGMA replica_status", -1, &p, 0);
      check( rc==SQLITE_OK && sqlite3_step(p)==SQLITE_ROW, "answers on a fresh db" );
      {
        const char *zRole = (const char*)sqlite3_column_text(p, 0);
        const char *zSrc  = (const char*)sqlite3_column_text(p, 3);
        check( zRole && strcmp(zRole, "none")==0
            && zSrc && strcmp(zSrc, "no-replication-state")==0,
               "no replication state is SAID, not silence" );
      }
      sqlite3_finalize(p);
      sqlite3_close(dbF); }
  }

  sqlite3_free(aSeg1); sqlite3_free(aSeg2); sqlite3_free(aSeg3);
  sqlite3_free(aSegX);
  replWriterClose(pW); replWriterClose(pWX);
  sqlite3_close(dbP); sqlite3_close(dbR); sqlite3_close(dbX);

  printf("\n%s: %d checks, %d failures\n",
         nFail ? "SPROCKET_REPL FAILED" : "SPROCKET_REPL OK",
         nCheck, nFail);
  return nFail ? 1 : 0;
}
#endif /* SPROCKET_REPL_LIBRARY */
