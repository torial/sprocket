/*
** proc_frame.c -- Phase 1 of PLAN-TRANSPORT.md: message framing over a byte
** stream, with no sockets involved.
**
** WHY THIS IS ITS OWN PHASE
**
** A stream reader that only works when each recv() happens to return exactly
** one whole message is the classic transport bug.  It is also invisible in
** casual testing: on loopback, with small payloads, the kernel almost always
** hands you the whole thing at once, so the broken version passes every manual
** trial and then fails in production against a real network that splits at
** MTU boundaries or coalesces two requests into one read.
**
** So the framer is built and proven against EVERY possible chunk boundary
** before a socket is allowed anywhere near it.
**
** WIRE FORM
**
**   [u32 length][payload]
**
** length is capped (FRAME_MAX) so that a corrupted or hostile prefix cannot
** drive a huge allocation, and zero-length messages are rejected outright --
** they carry no information and only exist as a way to desynchronise a parser.
**
** Build (from repo root, VS dev prompt):
**   cl /O2 /I. tool\proc_frame.c sqlite3.c /Fe:proc_frame.exe
*/
#define PROC_WIRE_NO_MAIN 1
#include "proc_wire.c"          /* codec + shared fixture + check()/nFail */

#define FRAME_MAX (64*1024*1024)

/* ---------------------------------------------------------------- framer -- */
typedef struct Framer {
  unsigned char *a;
  int n;
  int nAlloc;
  int bad;                       /* sticky: a protocol violation was seen */
} Framer;

static void frFree(Framer *p){ free(p->a); p->a = 0; p->n = p->nAlloc = 0; }

static int frGrow(Framer *p, int nExtra){
  if( p->n + nExtra > p->nAlloc ){
    int nNew = p->nAlloc ? p->nAlloc*2 : 256;
    unsigned char *aNew;
    while( nNew < p->n + nExtra ) nNew *= 2;
    aNew = (unsigned char*)realloc(p->a, nNew);
    if( aNew==0 ) return 1;
    p->a = aNew;
    p->nAlloc = nNew;
  }
  return 0;
}

/* Prepend a length header to a payload. */
static void frWrap(WireBuf *out, const unsigned char *z, int n){
  wbU32(out, (unsigned)n);
  if( wbNeed(out, n)==0 ){
    memcpy(out->a + out->n, z, n);
    out->n += n;
  }
}

/*
** Feed n bytes of stream.  Every complete message is handed to xMsg.  Returns
** SQLITE_OK if the stream is still well-formed (possibly with a partial
** message buffered), SQLITE_CORRUPT once a violation is seen.
**
** The caller does NOT get to assume anything about how its bytes were split.
*/
static int frFeed(
  Framer *p,
  const unsigned char *z, int n,
  void (*xMsg)(void*, const unsigned char*, int), void *pArg
){
  if( p->bad ) return SQLITE_CORRUPT;
  if( n>0 ){
    if( frGrow(p, n) ) return SQLITE_NOMEM;
    memcpy(p->a + p->n, z, n);
    p->n += n;
  }
  while( p->n >= 4 ){
    unsigned len = ((unsigned)p->a[0]<<24) | ((unsigned)p->a[1]<<16)
                 | ((unsigned)p->a[2]<<8)  | ((unsigned)p->a[3]);
    int total;
    if( len==0 || len > (unsigned)FRAME_MAX ){
      p->bad = 1;                /* implausible length: refuse to guess */
      return SQLITE_CORRUPT;
    }
    total = 4 + (int)len;
    if( p->n < total ) break;    /* incomplete; wait for more bytes */
    xMsg(pArg, p->a + 4, (int)len);
    memmove(p->a, p->a + total, p->n - total);
    p->n -= total;
  }
  return SQLITE_OK;
}

/* Bytes held back because a message is not yet complete. */
static int frPending(Framer *p){ return p->n; }

/* ============================== self-test ================================= */

typedef struct Collect {
  int nMsg;
  int bMismatch;                 /* a delivered message differed from expected */
  int bBadChecksum;              /* a delivered message failed to decode       */
  const unsigned char *aExpect;
  int nExpect;
  sqlite3_int64 chkExpect;
} Collect;

static void onMsg(void *pArg, const unsigned char *z, int n){
  Collect *p = (Collect*)pArg;
  DecodeStats st;
  p->nMsg++;
  if( n!=p->nExpect || memcmp(z, p->aExpect, n)!=0 ) p->bMismatch = 1;
  if( procWireDecode(z, n, &st)!=SQLITE_OK || st.checksum!=p->chkExpect ){
    p->bBadChecksum = 1;
  }
}

/* deterministic pseudo-random, so a failure is reproducible */
static unsigned rnd(unsigned *pSeed){
  *pSeed = (*pSeed * 1103515245u) + 12345u;
  return (*pSeed >> 16) & 0x7fff;
}

/*
** Drive one framed message through the framer using the given chunk size
** (0 means "pseudo-random splits").  Returns 1 if everything matched.
*/
static int driveChunked(
  const unsigned char *aStream, int nStream,
  Collect *pProto, int chunk, unsigned seed
){
  Framer fr;
  Collect c = *pProto;
  int i = 0, rc = SQLITE_OK;
  memset(&fr, 0, sizeof(fr));
  c.nMsg = 0; c.bMismatch = 0; c.bBadChecksum = 0;
  while( i < nStream && rc==SQLITE_OK ){
    int k = chunk>0 ? chunk : (int)(rnd(&seed) % 7) + 1;
    if( k > nStream - i ) k = nStream - i;
    rc = frFeed(&fr, aStream + i, k, onMsg, &c);
    i += k;
  }
  {
    int ok = (rc==SQLITE_OK) && c.nMsg==1 && !c.bMismatch
          && !c.bBadChecksum && frPending(&fr)==0;
    frFree(&fr);
    return ok;
  }
}

int main(int argc, char **argv){
  sqlite3 *db;
  sqlite3_stmt *pCall = 0;
  WireBuf payload, stream;
  DecodeStats st;
  Collect proto;
  int nChunkings = 0, nOk = 0;
  int i;

  (void)argc; (void)argv;
  memset(&payload, 0, sizeof(payload));
  memset(&stream, 0, sizeof(stream));

  printf("proc_frame self-test -- SQLite %s\n\n", sqlite3_libversion());

  db = procWireTestDb();
  if( db==0 ){ printf("fixture failed\n"); return 1; }
  if( sqlite3_prepare_v2(db, "CALL fetch_all();", -1, &pCall, 0)!=SQLITE_OK ){
    printf("prepare failed: %s\n", sqlite3_errmsg(db));
    return 1;
  }
  check(procWireEncode(pCall, 1, &payload)==SQLITE_OK, "fixture payload encodes");
  sqlite3_reset(pCall);
  check(procWireDecode(payload.a, payload.n, &st)==SQLITE_OK,
        "fixture payload decodes");
  check(payload.n > 150, "fixture payload is not trivially small");

  /* The positive control: the checksum proven by proc_wire's own self-test,
  ** with no transport in sight.  Every framed delivery must reproduce it. */
  memset(&proto, 0, sizeof(proto));
  proto.aExpect  = payload.a;
  proto.nExpect  = payload.n;
  proto.chkExpect = st.checksum;

  frWrap(&stream, payload.a, payload.n);
  check(stream.n == payload.n + 4, "framed stream is payload + 4 header bytes");

  /* ---- 1. EVERY chunk boundary ---------------------------------------- */
  /* A framer that assumes one message per read passes at chunk == stream.n
  ** and fails at 1.  Both are exercised, along with everything between. */
  for(i=1; i<=stream.n; i++){
    nChunkings++;
    if( driveChunked(stream.a, stream.n, &proto, i, 0) ) nOk++;
  }
  for(i=0; i<64; i++){
    nChunkings++;
    if( driveChunked(stream.a, stream.n, &proto, 0, (unsigned)(i*2654435761u)) ){
      nOk++;
    }
  }
  printf("  chunkings exercised: %d (sizes 1..%d, plus 64 random splits)\n",
         nChunkings, stream.n);
  check(nChunkings >= 300, "expected magnitude: at least 300 chunkings");
  check(nOk == nChunkings, "EVERY chunking reassembles identically");

  /* ---- 2. two messages arriving in one read ---------------------------- */
  {
    WireBuf two;
    Framer fr;
    Collect c = proto;
    memset(&two, 0, sizeof(two));
    memset(&fr, 0, sizeof(fr));
    c.nMsg = 0; c.bMismatch = 0; c.bBadChecksum = 0;
    frWrap(&two, payload.a, payload.n);
    frWrap(&two, payload.a, payload.n);
    check(frFeed(&fr, two.a, two.n, onMsg, &c)==SQLITE_OK,
          "two concatenated messages feed cleanly");
    check(c.nMsg==2, "two messages in one read yield exactly two messages");
    check(!c.bMismatch && !c.bBadChecksum, "both messages are intact");
    check(frPending(&fr)==0, "nothing left buffered");
    frFree(&fr);
    wbFree(&two);
  }

  /* ---- 3. THE INSTRUMENT MUST BE ABLE TO FAIL -------------------------- */
  /* Each of these must be REJECTED.  If any is accepted, the framer is not
  ** validating and this phase is not done. */
  {
    Framer fr;
    Collect c = proto;
    unsigned char hdr[8];
    memset(&fr, 0, sizeof(fr));
    c.nMsg = 0;
    hdr[0]=0x7f; hdr[1]=0xff; hdr[2]=0xff; hdr[3]=0xff;   /* ~2 GiB */
    check(frFeed(&fr, hdr, 4, onMsg, &c)==SQLITE_CORRUPT,
          "a length beyond the cap is rejected");
    check(c.nMsg==0, "and delivers nothing");
    frFree(&fr);

    memset(&fr, 0, sizeof(fr));
    c.nMsg = 0;
    hdr[0]=0; hdr[1]=0; hdr[2]=0; hdr[3]=0;               /* zero length */
    check(frFeed(&fr, hdr, 4, onMsg, &c)==SQLITE_CORRUPT,
          "a zero length is rejected");
    frFree(&fr);

    /* sticky: once bad, always bad */
    memset(&fr, 0, sizeof(fr));
    fr.bad = 1;
    check(frFeed(&fr, stream.a, stream.n, onMsg, &c)==SQLITE_CORRUPT,
          "a framer that has seen a violation stays failed");
    frFree(&fr);
  }

  /* ---- 4. truncation is incomplete, never a short message -------------- */
  {
    int nTrunc = 0, nDelivered = 0;
    for(i=1; i<stream.n; i++){
      Framer fr;
      Collect c = proto;
      memset(&fr, 0, sizeof(fr));
      c.nMsg = 0;
      if( frFeed(&fr, stream.a, i, onMsg, &c)==SQLITE_OK ){
        nTrunc++;
        if( c.nMsg!=0 ) nDelivered++;
        if( frPending(&fr)!=i ) nDelivered++;   /* must hold ALL of it back */
      }
      frFree(&fr);
    }
    check(nTrunc == stream.n-1, "every truncation is accepted as incomplete");
    check(nDelivered == 0,
          "NO truncation ever yields a message or drops buffered bytes");
  }

  /* ---- 5. the comparator itself is not vacuous ------------------------- */
  /* If onMsg's memcmp were broken, tests 1 and 2 would pass no matter what.
  ** Feed a deliberately different payload and require the mismatch to fire. */
  {
    WireBuf bogus;
    Framer fr;
    Collect c = proto;
    memset(&bogus, 0, sizeof(bogus));
    memset(&fr, 0, sizeof(fr));
    c.nMsg = 0; c.bMismatch = 0; c.bBadChecksum = 0;
    frWrap(&bogus, payload.a, payload.n - 1);   /* one byte shorter */
    frFeed(&fr, bogus.a, bogus.n, onMsg, &c);
    check(c.nMsg==1, "the wrong payload is still delivered as one message");
    check(c.bMismatch==1,
          "the comparator DETECTS a wrong payload (it is not vacuous)");
    frFree(&fr);
    wbFree(&bogus);
  }

  printf("  payload %d bytes, framed %d bytes, checksum %lld\n",
         payload.n, stream.n, (long long)st.checksum);

  sqlite3_finalize(pCall);
  sqlite3_close(db);
  wbFree(&payload);
  wbFree(&stream);

  printf("\n%s: %d checks, %d failures\n",
         nFail ? "PROC_FRAME FAILED" : "PROC_FRAME OK", nCheck, nFail);
  return nFail ? 1 : 0;
}
