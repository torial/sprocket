/*
** proc_wire.c -- a wire codec for CALL-as-request, plus its self-test.
**
** THE IDEA
**
** In a networked SQLite the unit of work should be a procedure call, not a
** statement: procedures carry data-dependent control flow, which is the one
** thing no transport and no pipeline can flatten (see DESIGN-NETWORK.md).
** So the protocol's request is "CALL p(args)" and its response is a sequence
** of declared result sets.
**
** What this fork makes possible that a general SQL protocol cannot:
** a procedure's result shape is fixed at CREATE time and checked against the
** body, so it is a STATIC contract.  A client may therefore cache the shape
** by (procedure, schema-cookie) and the server may then send rows with no
** per-response schema at all.  PostgreSQL must re-describe because an
** arbitrary query's shape is only known after planning; here it is known
** before the call is ever made.  That is the "describe once, then rows
** forever" mode below, and on small result sets it is most of the bytes.
**
** SCOPE
**
** Codec only -- no sockets.  Framing and encoding are where the design
** decisions live; a transport is mechanical and can be bolted on after.
** Everything here is bounds-checked on decode: this code would face the
** network, so a malformed frame must be an error, never a read past the end.
**
** ON THE SELF-TEST
**
** Built in from the first commit, not added after something lied:
**   - positive controls: values that MUST round-trip bit-exactly
**   - liveness: the test prints what it did, and counts must be non-trivial
**   - expected magnitude: asserted, so a silently-empty run cannot pass
**   - the instrument can fail: every byte of a frame is corrupted in turn and
**     the decoder must reject a large fraction and crash on none.  A decoder
**     that accepts everything would pass a round-trip test happily.
**
** Build (from repo root, VS dev prompt):
**   cl /O2 /I. tool\proc_wire.c sqlite3.c /Fe:proc_wire.exe
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sqlite3.h"

/* ---------------------------------------------------------------- frames -- */
#define WF_SHAPE   1   /* describes the current result set              */
#define WF_ROW     2   /* one row of the current result set             */
#define WF_SETEND  3   /* current result set is complete                */
#define WF_DONE    4   /* the whole CALL is complete                    */
#define WF_ERROR   5   /* code + message; terminates the response       */

/* value tags */
#define WV_NULL    0
#define WV_INT     1
#define WV_FLOAT   2
#define WV_TEXT    3
#define WV_BLOB    4

/* ------------------------------------------------------------- write buf -- */
typedef struct WireBuf {
  unsigned char *a;
  int n;
  int nAlloc;
  int oom;
} WireBuf;

static void wbFree(WireBuf *p){ free(p->a); p->a = 0; p->n = p->nAlloc = 0; }

static int wbNeed(WireBuf *p, int n){
  if( p->oom ) return 1;
  if( p->n + n > p->nAlloc ){
    int nNew = (p->nAlloc ? p->nAlloc*2 : 256);
    unsigned char *aNew;
    while( nNew < p->n + n ) nNew *= 2;
    aNew = (unsigned char*)realloc(p->a, nNew);
    if( aNew==0 ){ p->oom = 1; return 1; }
    p->a = aNew;
    p->nAlloc = nNew;
  }
  return 0;
}
static void wbU8(WireBuf *p, unsigned v){
  if( wbNeed(p,1) ) return;
  p->a[p->n++] = (unsigned char)(v & 0xff);
}
static void wbU32(WireBuf *p, unsigned v){
  if( wbNeed(p,4) ) return;
  p->a[p->n++] = (unsigned char)(v>>24);
  p->a[p->n++] = (unsigned char)(v>>16);
  p->a[p->n++] = (unsigned char)(v>>8);
  p->a[p->n++] = (unsigned char)(v);
}
static void wbI64(WireBuf *p, sqlite3_int64 v){
  wbU32(p, (unsigned)((sqlite3_uint64)v >> 32));
  wbU32(p, (unsigned)((sqlite3_uint64)v & 0xffffffffu));
}
static void wbDouble(WireBuf *p, double d){
  sqlite3_uint64 u;
  memcpy(&u, &d, 8);                     /* IEEE-754 bit pattern, big-endian */
  wbI64(p, (sqlite3_int64)u);
}
static void wbBytes(WireBuf *p, const void *z, int n){
  wbU32(p, (unsigned)n);
  if( n>0 ){
    if( wbNeed(p,n) ) return;
    memcpy(p->a + p->n, z, n);
    p->n += n;
  }
}
static void wbStr(WireBuf *p, const char *z){
  wbBytes(p, z ? z : "", z ? (int)strlen(z) : 0);
}

/* -------------------------------------------------------------- read buf -- */
typedef struct WireRd {
  const unsigned char *a;
  int n;
  int i;
  int bad;                                /* sticky: any violation sets it */
} WireRd;

static void rdInit(WireRd *p, const unsigned char *a, int n){
  p->a = a; p->n = n; p->i = 0; p->bad = 0;
}
static int rdHave(WireRd *p, int n){
  if( p->bad ) return 0;
  if( n < 0 || p->i + n > p->n ){ p->bad = 1; return 0; }
  return 1;
}
static unsigned rdU8(WireRd *p){
  if( !rdHave(p,1) ) return 0;
  return p->a[p->i++];
}
static unsigned rdU32(WireRd *p){
  unsigned v;
  if( !rdHave(p,4) ) return 0;
  v  = ((unsigned)p->a[p->i])<<24;
  v |= ((unsigned)p->a[p->i+1])<<16;
  v |= ((unsigned)p->a[p->i+2])<<8;
  v |= ((unsigned)p->a[p->i+3]);
  p->i += 4;
  return v;
}
static sqlite3_int64 rdI64(WireRd *p){
  sqlite3_uint64 hi = rdU32(p);
  sqlite3_uint64 lo = rdU32(p);
  return (sqlite3_int64)((hi<<32) | lo);
}
static double rdDouble(WireRd *p){
  sqlite3_uint64 u = (sqlite3_uint64)rdI64(p);
  double d;
  memcpy(&d, &u, 8);
  return d;
}
/*
** Read a length-prefixed byte run.  Returns a pointer INTO the buffer (no
** copy) and sets *pn.  A length that does not fit is a hard error, which is
** the single most important check in this file: it is the difference between
** rejecting a malformed frame and reading off the end of a network buffer.
*/
static const unsigned char *rdBytes(WireRd *p, int *pn){
  unsigned n = rdU32(p);
  *pn = 0;
  if( p->bad ) return 0;
  if( n > (unsigned)0x7fffffff ){ p->bad = 1; return 0; }
  if( !rdHave(p, (int)n) ) return 0;
  *pn = (int)n;
  p->i += (int)n;
  return p->a + p->i - (int)n;
}

/* --------------------------------------------------------------- encoder -- */
/*
** Encode the full response to one CALL.  pCall must be a prepared CALL that
** has not yet been stepped.  If bShapes is false the WF_SHAPE frames are
** omitted entirely -- the "client already knows the contract" mode.
*/
static int procWireEncode(sqlite3_stmt *pCall, int bShapes, WireBuf *out){
  int rc;
  for(;;){
    int nCol = sqlite3_column_count(pCall);
    int i;
    if( bShapes ){
      wbU8(out, WF_SHAPE);
      wbU32(out, (unsigned)nCol);
      for(i=0; i<nCol; i++){
        wbStr(out, sqlite3_column_name(pCall, i));
        wbStr(out, sqlite3_column_decltype(pCall, i));
      }
    }
    while( (rc = sqlite3_step(pCall))==SQLITE_ROW ){
      wbU8(out, WF_ROW);
      for(i=0; i<nCol; i++){
        switch( sqlite3_column_type(pCall, i) ){
          case SQLITE_NULL:
            wbU8(out, WV_NULL);
            break;
          case SQLITE_INTEGER:
            wbU8(out, WV_INT);
            wbI64(out, sqlite3_column_int64(pCall, i));
            break;
          case SQLITE_FLOAT:
            wbU8(out, WV_FLOAT);
            wbDouble(out, sqlite3_column_double(pCall, i));
            break;
          case SQLITE_BLOB: {
            const void *z = sqlite3_column_blob(pCall, i);
            int n = sqlite3_column_bytes(pCall, i);
            wbU8(out, WV_BLOB);
            wbBytes(out, z, n);
            break;
          }
          default: {
            const unsigned char *z = sqlite3_column_text(pCall, i);
            int n = sqlite3_column_bytes(pCall, i);
            wbU8(out, WV_TEXT);
            wbBytes(out, z, n);
            break;
          }
        }
      }
    }
    if( rc!=SQLITE_DONE ){
      wbU8(out, WF_ERROR);
      wbU32(out, (unsigned)rc);
      wbStr(out, sqlite3_errmsg(sqlite3_db_handle(pCall)));
      return rc;
    }
    wbU8(out, WF_SETEND);
    rc = sqlite3_proc_next_resultset(pCall);
    if( rc==SQLITE_DONE ) break;
    if( rc!=SQLITE_OK ){
      wbU8(out, WF_ERROR);
      wbU32(out, (unsigned)rc);
      wbStr(out, "advancing result set");
      return rc;
    }
  }
  wbU8(out, WF_DONE);
  return out->oom ? SQLITE_NOMEM : SQLITE_OK;
}

/* --------------------------------------------------------------- decoder -- */
typedef struct DecodeStats {
  int nSet;          /* result sets seen (SETEND frames)  */
  int nRow;          /* rows across all sets              */
  int nVal;          /* values across all rows            */
  int nShape;        /* shape frames seen                 */
  int bDone;         /* saw WF_DONE                       */
  int errCode;       /* set if WF_ERROR seen              */
  sqlite3_int64 checksum;   /* order-sensitive fold of the ROW DATA only.
                            ** Kept data-only so the shape-full and shape-free
                            ** encodings of the same call compare equal. */
  sqlite3_int64 chkMeta;    /* fold of the SHAPE metadata (names, decltypes).
                            ** Separate, because the mutation probe must see
                            ** metadata corruption that the data checksum
                            ** cannot by construction notice. */
} DecodeStats;

/*
** Decode a whole response.  Returns SQLITE_OK only if the byte stream is
** well-formed AND fully consumed AND terminated by DONE or ERROR.  Anything
** else is SQLITE_CORRUPT: a network-facing decoder does not guess.
*/
static int procWireDecode(const unsigned char *a, int n, DecodeStats *st){
  WireRd rd;
  int nCol = -1;
  memset(st, 0, sizeof(*st));
  rdInit(&rd, a, n);
  while( rd.i < rd.n && !rd.bad ){
    unsigned kind = rdU8(&rd);
    if( kind==WF_SHAPE ){
      unsigned c = rdU32(&rd);
      unsigned k;
      if( rd.bad || c > 32768 ){ rd.bad = 1; break; }
      st->chkMeta = st->chkMeta*31 + c;
      for(k=0; k<c && !rd.bad; k++){
        int ln, j;
        const unsigned char *z;
        z = rdBytes(&rd, &ln);   /* name     */
        st->chkMeta = st->chkMeta*31 + ln;
        for(j=0; j<ln; j++) st->chkMeta = st->chkMeta*31 + z[j];
        z = rdBytes(&rd, &ln);   /* decltype */
        st->chkMeta = st->chkMeta*31 + ln;
        for(j=0; j<ln; j++) st->chkMeta = st->chkMeta*31 + z[j];
      }
      nCol = (int)c;
      st->nShape++;
    }else if( kind==WF_ROW ){
      int i;
      if( nCol < 0 ){ rd.bad = 1; break; }   /* a row before any shape */
      st->nRow++;
      for(i=0; i<nCol && !rd.bad; i++){
        unsigned t = rdU8(&rd);
        st->nVal++;
        switch( t ){
          case WV_NULL:
            st->checksum = st->checksum*31 + 7;
            break;
          case WV_INT:
            st->checksum = st->checksum*31 + rdI64(&rd);
            break;
          case WV_FLOAT: {
            double d = rdDouble(&rd);
            sqlite3_uint64 u;
            memcpy(&u, &d, 8);
            st->checksum = st->checksum*31 + (sqlite3_int64)u;
            break;
          }
          case WV_TEXT:
          case WV_BLOB: {
            int ln = 0;
            const unsigned char *z = rdBytes(&rd, &ln);
            int j;
            st->checksum = st->checksum*31 + ln;
            for(j=0; j<ln; j++) st->checksum = st->checksum*31 + z[j];
            break;
          }
          default:
            rd.bad = 1;
            break;
        }
      }
    }else if( kind==WF_SETEND ){
      st->nSet++;
      nCol = -1;                 /* next set must re-describe (or be known) */
    }else if( kind==WF_DONE ){
      st->bDone = 1;
      break;
    }else if( kind==WF_ERROR ){
      int ln;
      st->errCode = (int)rdU32(&rd);
      rdBytes(&rd, &ln);
      break;
    }else{
      rd.bad = 1;
    }
  }
  if( rd.bad ) return SQLITE_CORRUPT;
  if( !st->bDone && st->errCode==0 ) return SQLITE_CORRUPT;  /* truncated */
  return SQLITE_OK;
}

/*
** Shape-free variant: the client already holds the contract, so the decoder
** is told the column counts up front and the stream carries no WF_SHAPE.
*/
static int procWireDecodeKnown(const unsigned char *a, int n,
                               const int *aNCol, int nSetExpected,
                               DecodeStats *st){
  WireRd rd;
  int iSet = 0;
  int nCol;
  memset(st, 0, sizeof(*st));
  if( nSetExpected<=0 ) return SQLITE_MISUSE;
  nCol = aNCol[0];
  rdInit(&rd, a, n);
  while( rd.i < rd.n && !rd.bad ){
    unsigned kind = rdU8(&rd);
    if( kind==WF_ROW ){
      int i;
      st->nRow++;
      for(i=0; i<nCol && !rd.bad; i++){
        unsigned t = rdU8(&rd);
        st->nVal++;
        switch( t ){
          case WV_NULL:  st->checksum = st->checksum*31 + 7; break;
          case WV_INT:   st->checksum = st->checksum*31 + rdI64(&rd); break;
          case WV_FLOAT: {
            double d = rdDouble(&rd);
            sqlite3_uint64 u;
            memcpy(&u, &d, 8);
            st->checksum = st->checksum*31 + (sqlite3_int64)u;
            break;
          }
          case WV_TEXT:
          case WV_BLOB: {
            int ln = 0, j;
            const unsigned char *z = rdBytes(&rd, &ln);
            st->checksum = st->checksum*31 + ln;
            for(j=0; j<ln; j++) st->checksum = st->checksum*31 + z[j];
            break;
          }
          default: rd.bad = 1; break;
        }
      }
    }else if( kind==WF_SETEND ){
      st->nSet++;
      iSet++;
      if( iSet < nSetExpected ) nCol = aNCol[iSet];
    }else if( kind==WF_DONE ){
      st->bDone = 1;
      break;
    }else if( kind==WF_ERROR ){
      int ln;
      st->errCode = (int)rdU32(&rd);
      rdBytes(&rd, &ln);
      break;
    }else{
      rd.bad = 1;
    }
  }
  if( rd.bad ) return SQLITE_CORRUPT;
  if( !st->bDone && st->errCode==0 ) return SQLITE_CORRUPT;
  return SQLITE_OK;
}

/* ============================== self-test ================================= */

static int nFail = 0;
static int nCheck = 0;

static void check(int bOk, const char *zWhat){
  nCheck++;
  if( !bOk ){
    nFail++;
    printf("  FAIL: %s\n", zWhat);
  }
}

static void execOrDie(sqlite3 *db, const char *zSql){
  char *zErr = 0;
  if( sqlite3_exec(db, zSql, 0, 0, &zErr)!=SQLITE_OK ){
    printf("FATAL exec: %s\n  sql: %s\n", zErr, zSql);
    exit(1);
  }
}

int main(int argc, char **argv){
  sqlite3 *db = 0;
  sqlite3_stmt *pCall = 0;
  WireBuf full, lean;
  DecodeStats st, st2;
  int rc, i;
  int aNCol[2];

  (void)argc; (void)argv;
  memset(&full, 0, sizeof(full));
  memset(&lean, 0, sizeof(lean));

  printf("proc_wire self-test -- SQLite %s\n\n", sqlite3_libversion());

  if( sqlite3_open(":memory:", &db)!=SQLITE_OK ){ printf("open failed\n"); return 1; }

  /* Positive controls: every value class the codec claims to carry, including
  ** the ones that break naive encoders -- NULL, a negative 64-bit integer, a
  ** float needing full precision, empty text, UTF-8, and a blob with an
  ** embedded NUL (which a strlen-based encoder silently truncates). */
  execOrDie(db,
    "CREATE TABLE vals(k INTEGER, i INTEGER, f REAL, t TEXT, b BLOB);");
  execOrDie(db,
    "INSERT INTO vals VALUES"
    " (1, -9223372036854775807, 3.141592653589793, 'hello', x'00ff00'),"
    " (2, 0, -0.5, '', x''),"
    " (3, NULL, NULL, NULL, NULL),"
    " (4, 42, 1e300, 'na" "\xc3\xafve caf" "\xc3\xa9', x'deadbeef');");
  execOrDie(db, "CREATE TABLE notes(id INTEGER, body TEXT);");
  execOrDie(db, "INSERT INTO notes VALUES(1,'first'),(2,'second');");

  execOrDie(db,
    "CREATE PROCEDURE fetch_all()\n"
    "  RETURNS TABLE(k INTEGER, i INTEGER, f REAL, t TEXT, b BLOB)\n"
    "  RETURNS TABLE(id INTEGER, body TEXT)\n"
    "BEGIN\n"
    "  SELECT k, i, f, t, b FROM vals ORDER BY k;\n"
    "  SELECT id, body FROM notes ORDER BY id;\n"
    "END;");

  if( sqlite3_prepare_v2(db, "CALL fetch_all();", -1, &pCall, 0)!=SQLITE_OK ){
    printf("prepare failed: %s\n", sqlite3_errmsg(db));
    return 1;
  }

  /* ---- 1. round trip, schema included --------------------------------- */
  rc = procWireEncode(pCall, 1, &full);
  check(rc==SQLITE_OK, "encode (with shapes) returns OK");
  sqlite3_reset(pCall);

  rc = procWireDecode(full.a, full.n, &st);
  check(rc==SQLITE_OK,  "decode (with shapes) returns OK");
  check(st.bDone,       "stream terminated by DONE");
  check(st.nSet==2,     "two result sets");
  check(st.nShape==2,   "two shape frames");
  check(st.nRow==6,     "six rows total (4 + 2)");
  check(st.nVal==4*5+2*2, "twenty-four values total");
  check(st.errCode==0,  "no error frame");

  /* ---- 2. expected magnitude ------------------------------------------ */
  /* A silently-empty encode must not be able to pass.  The exact size is not
  ** the contract; the order of magnitude is. */
  check(full.n > 150,  "encoded stream is not trivially small");
  check(full.n < 4096, "encoded stream is not absurdly large");

  /* ---- 3. determinism -------------------------------------------------- */
  {
    WireBuf again;
    memset(&again, 0, sizeof(again));
    rc = procWireEncode(pCall, 1, &again);
    sqlite3_reset(pCall);
    check(rc==SQLITE_OK, "second encode returns OK");
    check(again.n==full.n && memcmp(again.a, full.a, full.n)==0,
          "encoding is byte-for-byte deterministic");
    wbFree(&again);
  }

  /* ---- 4. shape-free mode carries the same data, in fewer bytes -------- */
  rc = procWireEncode(pCall, 0, &lean);
  check(rc==SQLITE_OK, "encode (shape-free) returns OK");
  sqlite3_reset(pCall);
  aNCol[0] = 5;
  aNCol[1] = 2;
  rc = procWireDecodeKnown(lean.a, lean.n, aNCol, 2, &st2);
  check(rc==SQLITE_OK, "decode (shape-free) returns OK");
  check(st2.nRow==st.nRow, "shape-free carries the same row count");
  check(st2.nVal==st.nVal, "shape-free carries the same value count");
  check(st2.checksum==st.checksum,
        "shape-free carries BIT-IDENTICAL data (checksums match)");
  check(lean.n < full.n, "shape-free stream is smaller");

  /* ---- 5. THE INSTRUMENT MUST BE ABLE TO FAIL -------------------------- */
  /* A decoder that accepts anything passes every test above.  Corrupt each
  ** byte in turn and require that a large fraction are REJECTED and that the
  ** decoder never runs off the end.  Without this the suite is decorative. */
  {
    unsigned char *aCopy = (unsigned char*)malloc(full.n);
    int nDetected = 0, nTried = 0, nVisible = 0, nSilent = 0;
    memcpy(aCopy, full.a, full.n);
    for(i=0; i<full.n; i++){
      DecodeStats junk;
      unsigned char save = aCopy[i];
      aCopy[i] = (unsigned char)(save ^ 0xA5);   /* flip several bits */
      nTried++;
      if( procWireDecode(aCopy, full.n, &junk)!=SQLITE_OK ){
        nDetected++;                     /* structurally rejected */
      }else if( junk.checksum != st.checksum
             || junk.chkMeta != st.chkMeta
             || junk.nRow != st.nRow
             || junk.nVal != st.nVal
             || junk.nSet != st.nSet ){
        nVisible++;                      /* decoded, but demonstrably changed */
      }else{
        nSilent++;                       /* decoded, and INDISTINGUISHABLE */
      }
      aCopy[i] = save;
    }
    free(aCopy);
    printf("  mutation probe: %d probed -> %d rejected, %d visible in the "
           "data, %d silent\n", nTried, nDetected, nVisible, nSilent);
    check(nTried==full.n, "every byte position was probed");
    check(nDetected > 0, "decoder is capable of rejecting anything at all");
    /* The real invariant, and the one worth having: no single-byte corruption
    ** may pass through BOTH the structural checks and the data unnoticed.
    ** A codec that loses a byte silently is the wire equivalent of an
    ** instrument that reports success while measuring nothing. */
    check(nSilent==0,
          "NO corruption is silent: every one is rejected or changes the data");
  }

  /* ---- 6. truncation is always an error -------------------------------- */
  {
    int nBad = 0;
    for(i=1; i<full.n; i++){
      DecodeStats junk;
      if( procWireDecode(full.a, i, &junk)!=SQLITE_OK ) nBad++;
    }
    check(nBad==full.n-1, "EVERY truncation of the stream is rejected");
  }

  /* ---- 7. liveness ------------------------------------------------------ */
  printf("\n  with shapes: %d bytes   shape-free: %d bytes   saving: %.1f%%\n",
         full.n, lean.n, 100.0*(full.n-lean.n)/(double)full.n);
  printf("  sets=%d rows=%d values=%d checksum=%lld\n",
         st.nSet, st.nRow, st.nVal, (long long)st.checksum);

  sqlite3_finalize(pCall);
  sqlite3_close(db);
  wbFree(&full);
  wbFree(&lean);

  printf("\n%s: %d checks, %d failures\n",
         nFail ? "PROC_WIRE FAILED" : "PROC_WIRE OK", nCheck, nFail);
  return nFail ? 1 : 0;
}
