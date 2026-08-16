/*
** sprocketd.c -- PLAN-DAEMON: the integrating daemon.
**
** Composes the six proven transport phases (PLAN-TRANSPORT) plus the
** queued-write engine mode (PLAN-QUEUE) into one process: TCP in front,
** one SQLite database behind, requests are CALLs in the ruled binary
** protocol, writes serialize through a group-commit writer thread on a
** connection that has declared PRAGMA queue_writer=ON, and read-only
** CALLs run on a concurrent reader pool.
**
** THE ROUTING RULE, WHICH IS THE POINT
**
** The daemon never guesses whether a CALL writes: the engine declares it
** (pragma_proc_list.writes, checked at CREATE against the body).  A proc
** the table calls read-only runs on a reader connection; if the
** declaration were ever wrong, the queued-write mode is the backstop --
** reader connections are not declarants, so a write from one REFUSES at
** the engine gate instead of corrupting the single-writer discipline,
** and the daemon reports a routing error naming the procedure.
**
** PROTOCOL (v1; version negotiated in HELLO, frames carry a request id
** so pipelining later is a client upgrade, not a frame change)
**
**   HELLO:  [u8 2][u32 protoVer]
**        -> framed [u32 protoVer][u32 schemaCookie]     (refusal: ver 0)
**   CALL:   [u8 1][u32 reqId][bytes name][u32 cookie][u32 nArg]
**           [ (tag)(payload) x nArg ]
**        -> framed [u32 reqId][u32 serverCookie]
**           framed <proc_wire body; WF_SHAPE omitted iff cookie matched>
**
** The reserved name "sprocket_stats" answers from the daemon itself, in
** ordinary result-set frames: (metric TEXT, value INT) rows -- no new
** frame kinds, so every generated client can already read it.
**
** REPLICATION (PLAN-REPL R4; needs -DSQLITE_ENABLE_SESSION and
** -DSQLITE_ENABLE_PREUPDATE_HOOK)
**
**   SUB:    [u8 3][u32 reqId][i64 fromSeq]
**        -> framed [u32 reqId][u32 serverCookie]
**           framed <(metric,value) ack carrying head_seq; or WF_ERROR>
**           then one framed SEGMENT per commit, forever (each frame is
**           the raw self-describing segment bytes -- the same bytes as
**           the archive file on disk).  Catch-up and live-tail are ONE
**           loop: the daemon always serves seq N from the archive,
**           waiting on the cut signal when N does not exist yet.
**
** The archive is a DECLARED source (--archive DIR): nothing is watched
** ambiently, and a daemon started without one refuses SUBSCRIBE naming
** the fix.  At startup the daemon verifies it can serve the whole
** lineage 1..head (refuse-never-skip needs the daemon to KNOW there
** are no holes) and that the archive belongs to this database.
**
** Build (Windows, VS dev prompt, repo root):
**   cl /O2 /I. /Itool -DSQLITE_ENABLE_SESSION -DSQLITE_ENABLE_PREUPDATE_HOOK
**      tool\sprocketd.c sqlite3.c ws2_32.lib /Fe:sprocketd.exe
** Build (Linux/POSIX, via tool/sd_port.h -- the 2026-08-16 shim):
**   gcc -O2 -I. -Itool -DSQLITE_ENABLE_SESSION -DSQLITE_ENABLE_PREUPDATE_HOOK
**      tool/sprocketd.c sqlite3.c -lpthread -ldl -lm -o sprocketd
** Run:      sprocketd DB [port] [--archive DIR]       (default port 7690)
** Selftest: sprocketd --selftest       (the PLAN-DAEMON D0 matrix; the
**           SAME 57 checks are the cross-platform gate)
*/
#include "sd_port.h"           /* sockets/threads/events/files: Win32 and
                               ** POSIX implementations of one vocabulary
                               ** (the Linux shim, 2026-08-16) */

#define PROC_WIRE_NO_MAIN  1
#define PROC_FRAME_NO_MAIN 1
#include "proc_frame.c"        /* framer + codec + check()/nFail */

#define SPROCKET_REPL_LIBRARY 1
#include "sprocket_repl.c"     /* segment writer/applier/encoder (PLAN-REPL):
                               ** one implementation of the contract, two
                               ** homes -- the harness proves it standalone,
                               ** the daemon serves it */

#define REQ_CALL   1
#define REQ_HELLO  2
#define REQ_SUB    3
#define PROTO_VER  1
#define IO_TIMEOUT_MS 5000

/* ------------------------------------------------------------ io helpers --
** Borrowed shapes from proc_server.c (phase 2), which proved them: a
** connection OWNS its framer (a recv can hold one message's tail and the
** next's head), and everything gets a timeout so no failure mode wedges. */
static int sdSendAll(SdSocket s, const unsigned char *z, int n){
  int i = 0;
  while( i < n ){
    int k = send(s, (const char*)z + i, n - i, 0);
    if( k<=0 ) return 1;
    i += k;
  }
  return 0;
}
static void sdTimeouts(SdSocket s){
  sdSockTimeouts(s, IO_TIMEOUT_MS);
}

typedef struct SdMsg { unsigned char *a; int n; } SdMsg;
typedef struct SdMsgQ { SdMsg a[4]; int n; } SdMsgQ;
static void sdOnMsg(void *pArg, const unsigned char *z, int n){
  SdMsgQ *q = (SdMsgQ*)pArg;
  if( q->n < 4 ){
    q->a[q->n].a = (unsigned char*)malloc(n ? n : 1);
    if( q->a[q->n].a ){ memcpy(q->a[q->n].a, z, n); q->a[q->n].n = n; }
    q->n++;
  }
}
typedef struct SdConn {
  SdSocket s;
  Framer fr;
  SdMsgQ q;                    /* messages assembled but not yet consumed */
} SdConn;
static void sdConnInit(SdConn *pc, SdSocket s){ memset(pc,0,sizeof(*pc)); pc->s=s; }
static void sdConnFree(SdConn *pc){
  int i;
  for(i=0;i<pc->q.n;i++) free(pc->q.a[i].a);
  frFree(&pc->fr);
}
/* Return the next whole framed message (caller frees), or nonzero. */
static int sdRecvMsg(SdConn *pc, SdMsg *pOut){
  unsigned char buf[256];
  memset(pOut, 0, sizeof(*pOut));
  for(;;){
    if( pc->q.n>0 ){
      int i;
      *pOut = pc->q.a[0];
      for(i=1;i<pc->q.n;i++) pc->q.a[i-1] = pc->q.a[i];
      pc->q.n--;
      return pOut->a ? 0 : 1;
    }
    {
      int k = recv(pc->s, (char*)buf, sizeof(buf), 0);
      if( k<=0 ) return 1;
      if( frFeed(&pc->fr, buf, k, sdOnMsg, &pc->q) ) return 1;
    }
  }
}

/* ------------------------------------------------------- the writer queue --
** The phase-5 pattern (proc_queue.c) re-shaped for CALLs: items carry a
** decoded request rather than an SQL string, and the worker executes the
** CALL on the ONE declared connection, encoding the response while the
** statement streams.  The gate and the wake-all lesson carry over
** verbatim -- both were paid for once already. */
#define SDQCAP 1024

typedef struct SdItem {
  const unsigned char *aReq;   /* the CALL message, from [u32 nameLen] on  */
  int nReq;
  unsigned cookie;             /* client's claimed shape cookie            */
  WireBuf out;                 /* encoded body (worker fills)              */
  unsigned outCookie;          /* server cookie at execution               */
  int rc;
  char zErr[256];
  SdEvent hDone;
} SdItem;

typedef struct SdQueue {
  sqlite3 *db;                 /* the declared queue_writer connection     */
  SdMutex mx;
  SdCond cvItem, cvSpace;
  SdItem *aItem[SDQCAP];
  int iHead, iTail, nQueued;
  SdCount bStop;
  SdThread hThread;
  SdEvent hGate;               /* manual-reset, signalled; tests close it  */
  /* instruments (deterministic counts + one duration) */
  SdCount nDone;
  SdCount lastBatch;
  SdCount lastCommitMs;
  long long oldestQueuedAt;    /* monotonic ms of current head, 0=empty    */
  /* replication (PLAN-REPL R4): set iff an archive was declared */
  ReplWriter *pRepl;           /* segment writer on THIS connection        */
  const char *zArchive;        /* declared archive directory, or 0         */
  SdCond cvCut;    /* broadcast after each archived segment    */
  sqlite3_int64 iHeadSeq;      /* highest archived seq (under mx)          */
  char zReplErr[300];          /* sticky archive failure; "" = healthy     */
} SdQueue;

static int sdExecCall(sqlite3 *db, const unsigned char *aReq, int nReq,
                      unsigned clientCookie, WireBuf *pOut,
                      unsigned *pOutCookie, char *zErr, int nErr);

/* ------------------------------------------------------ archive helpers -- */
static void sdSegPath(char *zOut, int n, const char *zDir, sqlite3_int64 seq){
  /* forward slash: both platforms accept it, so archives carry one
  ** spelling everywhere */
  sqlite3_snprintf(n, zOut, "%s/%016llx.sprkseg", zDir,
                   (sqlite3_uint64)seq);
}
/* Write-then-rename so a segment file either exists whole or not at all
** -- a half-written segment would fail its own checksum, but why hand a
** subscriber even that. */
static int sdWriteFileAtomic(const char *zPath, const unsigned char *a, int n){
  char zTmp[SD_PATHMAX+8];
  FILE *f;
  int ok;
  sqlite3_snprintf(sizeof(zTmp), zTmp, "%s.tmp", zPath);
  f = fopen(zTmp, "wb");
  if( f==0 ) return 1;
  ok = (int)fwrite(a, 1, n, f)==n;
  ok = sdFsync(f)==0 && ok;
  fclose(f);
  if( !ok || sdFileRename(zTmp, zPath) ){
    sdFileDelete(zTmp);
    return 1;
  }
  return 0;
}
/* Whole file, sqlite3_malloc'd; 0 on any failure (*pn stays 0). */
static unsigned char *sdSegRead(const char *zPath, int *pn){
  FILE *f;
  long sz;
  unsigned char *a;
  *pn = 0;
  f = fopen(zPath, "rb");
  if( f==0 ) return 0;
  if( fseek(f, 0, SEEK_END) || (sz = ftell(f))<0 || sz>0x7fffffff
   || fseek(f, 0, SEEK_SET) ){
    fclose(f);
    return 0;
  }
  a = (unsigned char*)sqlite3_malloc((int)(sz ? sz : 1));
  if( a==0 ){ fclose(f); return 0; }
  if( (long)fread(a, 1, sz, f)!=sz ){
    fclose(f); sqlite3_free(a); return 0;
  }
  fclose(f);
  *pn = (int)sz;
  return a;
}

static SdThreadRet SD_THREAD_CALL sdWriter(SdThreadArg pArg){
  SdQueue *q = (SdQueue*)pArg;
  SdItem *aBatch[SDQCAP];
  while( 1 ){
    int nBatch = 0, i;
    sdMutexEnter(&q->mx);
    while( q->nQueued==0 && !q->bStop ){
      sdCondWaitMs(&q->cvItem, &q->mx, 100);
    }
    if( q->bStop && q->nQueued==0 ){ sdMutexLeave(&q->mx); break; }
    sdMutexLeave(&q->mx);
    sdEventWait(q->hGate);                     /* after the wait: the gate
                                               ** must see queued items */
    sdMutexEnter(&q->mx);
    while( q->nQueued>0 ){
      aBatch[nBatch++] = q->aItem[q->iHead];
      q->iHead = (q->iHead+1) % SDQCAP;
      q->nQueued--;
    }
    q->oldestQueuedAt = 0;
    sdMutexLeave(&q->mx);
    if( nBatch>0 ){
      long long t0 = sdMonotonicMs();
      sqlite3_exec(q->db, "BEGIN IMMEDIATE;", 0, 0, 0);
      for(i=0;i<nBatch;i++){
        SdItem *it = aBatch[i];
        it->rc = sdExecCall(q->db, it->aReq, it->nReq, it->cookie,
                            &it->out, &it->outCookie,
                            it->zErr, sizeof(it->zErr));
      }
      sqlite3_exec(q->db, "COMMIT;", 0, 0, 0);
      /* R4: one commit -> one segment, archived BEFORE the callers wake,
      ** so an acknowledged write is never invisible to a subscriber.  A
      ** batch that changed nothing (every item refused) cuts nothing --
      ** SQLITE_DONE, not a fabricated empty segment. */
      if( q->pRepl ){
        unsigned char *aSeg = 0;
        int nSeg = 0;
        int rc2 = replWriterCut(q->pRepl, &aSeg, &nSeg);
        if( rc2==SQLITE_OK ){
          char zPath[SD_PATHMAX];
          sdSegPath(zPath, sizeof(zPath), q->zArchive, q->pRepl->iLastCut);
          if( sdWriteFileAtomic(zPath, aSeg, nSeg) ){
            sdMutexEnter(&q->mx);
            sqlite3_snprintf(sizeof(q->zReplErr), q->zReplErr,
              "archive write failed at seq %lld (%s); replication is "
              "STOPPED at the last archived segment",
              q->pRepl->iLastCut, zPath);
            sdMutexLeave(&q->mx);
            sdCondWakeAll(&q->cvCut);
          }else{
            sdMutexEnter(&q->mx);
            q->iHeadSeq = q->pRepl->iLastCut;
            sdMutexLeave(&q->mx);
            sdCondWakeAll(&q->cvCut);
          }
          sqlite3_free(aSeg);
        }else if( rc2!=SQLITE_DONE && q->zReplErr[0]==0 ){
          sdMutexEnter(&q->mx);
          sqlite3_snprintf(sizeof(q->zReplErr), q->zReplErr,
            "segment cut failed after seq %lld (rc=%d); replication is "
            "STOPPED at the last archived segment",
            q->pRepl->iLastCut, rc2);
          sdMutexLeave(&q->mx);
          sdCondWakeAll(&q->cvCut);
        }
      }
      sdAtomicExchange(&q->lastCommitMs, (long)(sdMonotonicMs()-t0));
      sdAtomicExchange(&q->lastBatch, nBatch);
      sdAtomicAdd(&q->nDone, nBatch);
      for(i=0;i<nBatch;i++) sdEventSet(aBatch[i]->hDone);
      sdCondWakeAll(&q->cvSpace);
    }
  }
  return 0;
}

static int sdQueueSubmit(SdQueue *q, SdItem *it){
  it->hDone = sdEventCreate(0, 0);
  if( !SD_EVENT_OK(it->hDone) ) return SQLITE_NOMEM;
  sdMutexEnter(&q->mx);
  while( q->nQueued==SDQCAP ){
    sdCondWaitMs(&q->cvSpace, &q->mx, -1);
  }
  if( q->nQueued==0 ) q->oldestQueuedAt = sdMonotonicMs();
  q->aItem[q->iTail] = it;
  q->iTail = (q->iTail+1) % SDQCAP;
  q->nQueued++;
  sdCondWakeOne(&q->cvItem);
  sdMutexLeave(&q->mx);
  sdEventWait(it->hDone);
  sdEventDestroy(it->hDone);
  return SQLITE_OK;
}

/* ------------------------------------------------------------- the daemon -- */
typedef struct RouteEnt { char *zName; int bWrites; } RouteEnt;

#define SD_NREADER 4

typedef struct Sprocketd {
  const char *zDb;
  sqlite3 *aReader[SD_NREADER];
  SdMutex aReaderMx[SD_NREADER];
  SdCount iReaderRR;
  SdQueue wq;
  RouteEnt *aRoute;
  int nRoute;
  unsigned routeCookie;        /* schema cookie the route table reflects  */
  SdMutex routeMx;
  SdSocket sListen;
  int port;
  SdCount bShutdown;
  SdCount nConn, nReqIn, nReqOut;
  SdEvent hReady;
  int bTestMisroute;           /* selftest hook: route writers to readers */
} Sprocketd;

static unsigned sdSchemaCookie(sqlite3 *db){
  sqlite3_stmt *p = 0;
  unsigned v = 0;
  if( sqlite3_prepare_v2(db, "PRAGMA schema_version;", -1, &p, 0)==SQLITE_OK
   && sqlite3_step(p)==SQLITE_ROW ){
    v = (unsigned)sqlite3_column_int64(p, 0);
  }
  sqlite3_finalize(p);
  return v;
}

/* (Re)load the routing table from the engine's own declaration. */
static int sdLoadRoutes(Sprocketd *d, sqlite3 *db){
  sqlite3_stmt *p = 0;
  int i;
  sdMutexEnter(&d->routeMx);
  for(i=0;i<d->nRoute;i++) sqlite3_free(d->aRoute[i].zName);
  sqlite3_free(d->aRoute);
  d->aRoute = 0; d->nRoute = 0;
  if( sqlite3_prepare_v2(db,
        "SELECT name, writes FROM pragma_proc_list;", -1, &p, 0)==SQLITE_OK ){
    while( sqlite3_step(p)==SQLITE_ROW ){
      RouteEnt *aNew = (RouteEnt*)sqlite3_realloc(d->aRoute,
                          (d->nRoute+1)*(int)sizeof(RouteEnt));
      if( aNew==0 ) break;
      d->aRoute = aNew;
      d->aRoute[d->nRoute].zName =
          sqlite3_mprintf("%s", sqlite3_column_text(p, 0));
      d->aRoute[d->nRoute].bWrites = sqlite3_column_int(p, 1);
      d->nRoute++;
    }
  }
  sqlite3_finalize(p);
  d->routeCookie = sdSchemaCookie(db);
  sdMutexLeave(&d->routeMx);
  return 0;
}

/* -1 unknown, else the writes flag. */
static int sdRouteWrites(Sprocketd *d, const char *zName, int nName){
  int i, r = -1;
  sdMutexEnter(&d->routeMx);
  for(i=0;i<d->nRoute;i++){
    if( (int)strlen(d->aRoute[i].zName)==nName
     && sqlite3_strnicmp(d->aRoute[i].zName, zName, nName)==0 ){
      r = d->aRoute[i].bWrites;
      break;
    }
  }
  sdMutexLeave(&d->routeMx);
  return r;
}

/*
** Execute one CALL request (from [u32 nameLen] onward) on the given
** connection, encoding the response body.  Returns SQLITE_OK or an
** error code with zErr filled.  Shapes are omitted iff clientCookie
** matches the live schema cookie -- the phase-4 contract.
*/
static int sdExecCall(sqlite3 *db, const unsigned char *aReq, int nReq,
                      unsigned clientCookie, WireBuf *pOut,
                      unsigned *pOutCookie, char *zErr, int nErr){
  WireRd rd;
  sqlite3_stmt *pStmt = 0;
  char *zSql = 0;
  const unsigned char *zName;
  unsigned nName, nArg, i;
  int rc;

  rdInit(&rd, aReq, nReq);
  { int nn = 0;
    zName = rdBytes(&rd, &nn);
    if( zName==0 || nn==0 ) goto malformed;
    nName = (unsigned)nn; }
  clientCookie = rdU32(&rd);          /* re-read here so queue path and
                                      ** reader path decode identically */
  nArg = rdU32(&rd);
  if( rd.bad || nArg>127 ) goto malformed;

  {
    /* Plain text, NOT WireBuf: wbBytes is the LENGTH-PREFIXED writer,
    ** and a length prefix here is a leading NUL -- prepare("") returns
    ** OK with a NULL statement, and the binds then MISUSE.  Found by
    ** the selftest; %w does the identifier quoting correctly. */
    char zName2[513];
    sqlite3_str *pS = sqlite3_str_new(0);
    if( nName>512 || pS==0 ){ sqlite3_free(sqlite3_str_finish(pS)); goto oom; }
    memcpy(zName2, zName, nName);
    zName2[nName] = 0;
    sqlite3_str_appendf(pS, "CALL \"%w\"(", zName2);
    for(i=0;i<nArg;i++) sqlite3_str_appendall(pS, i ? ",?" : "?");
    sqlite3_str_appendall(pS, ");");
    zSql = sqlite3_str_finish(pS);
    if( zSql==0 ) goto oom;
  }
  rc = sqlite3_prepare_v2(db, zSql, -1, &pStmt, 0);
  sqlite3_free(zSql);
  if( rc!=SQLITE_OK ){
    sqlite3_snprintf(nErr, zErr, "%s", sqlite3_errmsg(db));
    return rc;
  }
  if( pStmt==0 ){
    /* prepare("") and prepare of pure whitespace return SQLITE_OK with
    ** a NULL statement -- upstream contract, load-bearing for
    ** multi-statement iteration.  Left unchecked, the binds below
    ** would MISUSE while sqlite3_errmsg still said "not an error"
    ** (the accurate message goes to sqlite3_log, which sdLogTo routes
    ** to stderr).  Name the condition where it is born instead. */
    sqlite3_snprintf(nErr, zErr,
        "generated CALL SQL was empty (daemon builder bug)");
    return SQLITE_INTERNAL;
  }
  for(i=1;i<=nArg;i++){
    unsigned tag = rdU8(&rd);
    switch( tag ){
      case WV_NULL:  sqlite3_bind_null(pStmt, (int)i); break;
      case WV_INT:   sqlite3_bind_int64(pStmt, (int)i, rdI64(&rd)); break;
      case WV_FLOAT: sqlite3_bind_double(pStmt, (int)i, rdDouble(&rd)); break;
      case WV_TEXT: {
        int n; const unsigned char *z = rdBytes(&rd, &n);
        if( z==0 ) goto malformed_stmt;
        sqlite3_bind_text(pStmt, (int)i, (const char*)z, n, SQLITE_TRANSIENT);
        break;
      }
      case WV_BLOB: {
        int n; const unsigned char *z = rdBytes(&rd, &n);
        if( z==0 ) goto malformed_stmt;
        sqlite3_bind_blob(pStmt, (int)i, z, n, SQLITE_TRANSIENT);
        break;
      }
      default: goto malformed_stmt;
    }
    if( rd.bad ) goto malformed_stmt;
  }
  *pOutCookie = sdSchemaCookie(db);
  rc = procWireEncode(pStmt, clientCookie!=*pOutCookie, pOut);
  if( rc!=SQLITE_OK ){
    sqlite3_snprintf(nErr, zErr, "%s", sqlite3_errmsg(db));
  }
  sqlite3_finalize(pStmt);
  return rc;

malformed_stmt:
  sqlite3_finalize(pStmt);
malformed:
  sqlite3_snprintf(nErr, zErr, "malformed request");
  return SQLITE_MISUSE;
oom:
  sqlite3_snprintf(nErr, zErr, "out of memory");
  return SQLITE_NOMEM;
}

/* Encode a daemon-side error as a WF_ERROR body, so refusals ride the
** same frames as everything else. */
static void sdErrorBody(WireBuf *pOut, int code, const char *zMsg){
  wbU8(pOut, WF_ERROR);
  wbU32(pOut, (unsigned)code);
  wbStr(pOut, zMsg);
}

/* The reserved stats CALL: (metric TEXT, value INT) rows. */
static void sdStatsBody(Sprocketd *d, WireBuf *pOut){
  struct { const char *z; sqlite3_int64 v; } a[7];
  int i, n = 0;
  long long oldest;
  sdMutexEnter(&d->wq.mx);
  a[n].z = "queue_depth";      a[n++].v = d->wq.nQueued;
  oldest = d->wq.oldestQueuedAt;
  if( d->wq.zArchive ){
    a[n].z = "repl_head_seq";  a[n++].v = d->wq.iHeadSeq;
  }
  sdMutexLeave(&d->wq.mx);
  a[n].z = "oldest_waiter_ms";
  a[n++].v = oldest ? (sqlite3_int64)(sdMonotonicMs()-oldest) : 0;
  a[n].z = "last_batch";       a[n++].v = d->wq.lastBatch;
  a[n].z = "last_commit_ms";   a[n++].v = d->wq.lastCommitMs;
  a[n].z = "writes_done";      a[n++].v = d->wq.nDone;
  a[n].z = "connections";      a[n++].v = d->nConn;
  /* one SHAPE then rows -- hand-built, tiny, same frame grammar */
  wbU8(pOut, WF_SHAPE);
  wbU32(pOut, 2);
  wbStr(pOut, "metric"); wbStr(pOut, "value");
  for(i=0;i<n;i++){
    wbU8(pOut, WF_ROW);
    wbU8(pOut, WV_TEXT); wbStr(pOut, a[i].z);
    wbU8(pOut, WV_INT);  wbI64(pOut, a[i].v);
  }
  wbU8(pOut, WF_SETEND);
  wbU8(pOut, WF_DONE);
}

/* --------------------------------------------------- per-connection thread -- */
typedef struct SdConnArg { Sprocketd *d; SdSocket s; } SdConnArg;

static SdThreadRet SD_THREAD_CALL sdConnThread(SdThreadArg pArg){
  SdConnArg *ca = (SdConnArg*)pArg;
  Sprocketd *d = ca->d;
  SdConn c;
  int bHello = 0;
  sdConnInit(&c, ca->s);
  free(ca);
  sdTimeouts(c.s);
  sdAtomicInc(&d->nConn);

  while( !d->bShutdown ){
    SdMsg m;
    WireRd rd;
    unsigned op;
    if( sdRecvMsg(&c, &m) ) break;
    sdAtomicInc(&d->nReqIn);
    rdInit(&rd, m.a, m.n);
    op = rdU8(&rd);

    if( op==REQ_HELLO ){
      unsigned ver = rdU32(&rd);
      WireBuf b; memset(&b, 0, sizeof(b));
      if( rd.bad || ver!=PROTO_VER ){
        wbU32(&b, 0);                    /* version 0 = refused */
        wbStr(&b, "protocol version not supported; this daemon speaks 1");
      }else{
        wbU32(&b, PROTO_VER);
        wbU32(&b, sdSchemaCookie(d->wq.db));
        bHello = 1;
      }
      { WireBuf f; memset(&f,0,sizeof(f)); frWrap(&f, b.a, b.n);
        sdSendAll(c.s, f.a, f.n); wbFree(&f); }
      wbFree(&b);
      sdAtomicInc(&d->nReqOut);
      free(m.a);
      if( !bHello ) break;
      continue;
    }

    if( op==REQ_CALL && bHello ){
      unsigned reqId = rdU32(&rd);
      int nName = 0;
      const unsigned char *zNameB = rdBytes(&rd, &nName);
      WireBuf body, hdr;
      unsigned outCookie = 0;
      char zErr[256]; zErr[0] = 0;
      memset(&body, 0, sizeof(body));
      memset(&hdr, 0, sizeof(hdr));

      if( rd.bad || zNameB==0 || nName==0 || nName>512 ){
        sdErrorBody(&body, SQLITE_MISUSE, "malformed request");
        outCookie = 0;
      }else{
        const char *zName = (const char*)zNameB;
        const unsigned char *aCallReq = m.a + 1 + 4;   /* past op+reqId */
        int nCallReq = m.n - 1 - 4;
        if( (int)nName==14 && sqlite3_strnicmp(zName,"sprocket_stats",14)==0 ){
          outCookie = sdSchemaCookie(d->wq.db);
          sdStatsBody(d, &body);
        }else{
          int w = sdRouteWrites(d, zName, (int)nName);
          if( w<0 ){
            /* Unknown name: maybe the schema moved under us.  Reload
            ** the routes ONCE from the truth and retry the lookup --
            ** never guess a route. */
            sdLoadRoutes(d, d->wq.db);
            w = sdRouteWrites(d, zName, (int)nName);
          }
          if( d->bTestMisroute ) w = 0;   /* selftest: force reader path */
          if( w<0 ){
            char zMsg[300];
            sqlite3_snprintf(sizeof(zMsg), zMsg,
               "no such procedure: %.*s", (int)nName, zName);
            sdErrorBody(&body, SQLITE_ERROR, zMsg);
          }else if( w ){
            SdItem it;
            memset(&it, 0, sizeof(it));
            it.aReq = aCallReq; it.nReq = nCallReq;
            sdQueueSubmit(&d->wq, &it);
            if( it.rc!=SQLITE_OK ){
              wbFree(&it.out); memset(&it.out, 0, sizeof(it.out));
              sdErrorBody(&it.out, it.rc, it.zErr);
            }
            body = it.out;
            outCookie = it.outCookie;
          }else{
            int ir = (int)(sdAtomicInc(&d->iReaderRR)
                           % SD_NREADER);
            int rc;
            sdMutexEnter(&d->aReaderMx[ir]);
            rc = sdExecCall(d->aReader[ir], aCallReq, nCallReq, 0,
                            &body, &outCookie, zErr, sizeof(zErr));
            sdMutexLeave(&d->aReaderMx[ir]);
            if( rc==SQLITE_BUSY
             && strstr(zErr, "queued-write mode")!=0 ){
              /* The backstop fired: a proc routed read-only tried to
              ** write.  Attribute it. */
              char zMsg[400];
              wbFree(&body); memset(&body, 0, sizeof(body));
              sqlite3_snprintf(sizeof(zMsg), zMsg,
                 "routing error: %.*s is routed read-only but attempted "
                 "a write (engine refused: %s)", (int)nName, zName, zErr);
              sdErrorBody(&body, rc, zMsg);
            }else if( rc!=SQLITE_OK ){
              wbFree(&body); memset(&body, 0, sizeof(body));
              sdErrorBody(&body, rc, zErr);
            }
          }
        }
      }
      wbU32(&hdr, reqId);
      wbU32(&hdr, outCookie);
      { WireBuf f; memset(&f,0,sizeof(f));
        frWrap(&f, hdr.a, hdr.n);
        frWrap(&f, body.a, body.n);
        sdSendAll(c.s, f.a, f.n);
        wbFree(&f); }
      wbFree(&hdr); wbFree(&body);
      sdAtomicInc(&d->nReqOut);
      free(m.a);
      continue;
    }

    if( op==REQ_SUB && bHello ){
      unsigned reqId = rdU32(&rd);
      sqlite3_int64 from = rdI64(&rd);
      SdQueue *q = &d->wq;
      WireBuf hb, bb;
      sqlite3_int64 head;
      int bRefused = 1;
      char zMsg[360];
      memset(&hb, 0, sizeof(hb));
      memset(&bb, 0, sizeof(bb));

      sdMutexEnter(&q->mx);
      head = q->iHeadSeq;
      sdMutexLeave(&q->mx);

      if( rd.bad ){
        sdErrorBody(&bb, SQLITE_MISUSE, "malformed request");
      }else if( q->zArchive==0 ){
        sdErrorBody(&bb, SQLITE_MISUSE,
          "this daemon serves no replication archive; restart sprocketd "
          "with --archive DIR to declare one");
      }else if( q->zReplErr[0] ){
        sdErrorBody(&bb, SQLITE_IOERR, q->zReplErr);
      }else if( from<1 ){
        sdErrorBody(&bb, SQLITE_MISUSE,
          "segments begin at seq 1; a fresh replica subscribes from 1");
      }else if( from>head+1 ){
        sqlite3_snprintf(sizeof(zMsg), zMsg,
          "archive head is seq %lld; cannot subscribe from %lld (the "
          "future).  A replica ahead of its primary has diverged -- "
          "check which database this replica followed.", head, from);
        sdErrorBody(&bb, SQLITE_MISUSE, zMsg);
      }else{
        /* the ack carries head_seq: the subscriber knows its catch-up
        ** span before the first segment arrives */
        wbU8(&bb, WF_SHAPE);
        wbU32(&bb, 1);
        wbStr(&bb, "head_seq");
        wbU8(&bb, WF_ROW);
        wbU8(&bb, WV_INT);  wbI64(&bb, head);
        wbU8(&bb, WF_SETEND);
        wbU8(&bb, WF_DONE);
        bRefused = 0;
      }
      wbU32(&hb, reqId);
      wbU32(&hb, sdSchemaCookie(d->wq.db));
      { WireBuf f; memset(&f,0,sizeof(f));
        frWrap(&f, hb.a, hb.n);
        frWrap(&f, bb.a, bb.n);
        sdSendAll(c.s, f.a, f.n);
        wbFree(&f); }
      wbFree(&hb); wbFree(&bb);
      sdAtomicInc(&d->nReqOut);
      free(m.a);
      if( bRefused ) continue;      /* a refusal is an answer, not a hangup */

      /* The serve loop: this connection is now a segment stream.  Always
      ** serve seq N from the archive file, waiting on the cut signal when
      ** N does not exist yet -- catch-up and live-tail are ONE loop. */
      {
        sqlite3_int64 seq;
        int bDead = 0;
        for(seq=from; !bDead; seq++){
          unsigned char *aSeg;
          int nSeg = 0;
          char zPath[SD_PATHMAX];
          sdMutexEnter(&q->mx);
          while( q->iHeadSeq<seq && !d->bShutdown && q->zReplErr[0]==0 ){
            sdCondWaitMs(&q->cvCut, &q->mx, 200);
          }
          bDead = (q->zReplErr[0]!=0);
          sdMutexLeave(&q->mx);
          if( d->bShutdown ) break;
          if( bDead ){
            WireBuf eb, f;
            memset(&eb,0,sizeof(eb)); memset(&f,0,sizeof(f));
            sdErrorBody(&eb, SQLITE_IOERR, q->zReplErr);
            frWrap(&f, eb.a, eb.n);
            sdSendAll(c.s, f.a, f.n);
            wbFree(&f); wbFree(&eb);
            break;
          }
          sdSegPath(zPath, sizeof(zPath), q->zArchive, seq);
          aSeg = sdSegRead(zPath, &nSeg);
          if( aSeg==0 ){
            WireBuf eb, f;
            memset(&eb,0,sizeof(eb)); memset(&f,0,sizeof(f));
            sqlite3_snprintf(sizeof(zMsg), zMsg,
              "archive segment %lld unreadable at %s; the stream cannot "
              "continue without a hole", seq, zPath);
            sdErrorBody(&eb, SQLITE_IOERR, zMsg);
            frWrap(&f, eb.a, eb.n);
            sdSendAll(c.s, f.a, f.n);
            wbFree(&f); wbFree(&eb);
            break;
          }
          { WireBuf f; memset(&f,0,sizeof(f));
            frWrap(&f, aSeg, nSeg);
            bDead = sdSendAll(c.s, f.a, f.n);   /* client went away: done */
            wbFree(&f); }
          sqlite3_free(aSeg);
        }
      }
      break;                        /* subscription ends the connection */
    }

    /* Unknown opcode or CALL before HELLO: refuse and close. */
    { WireBuf b, f; memset(&b,0,sizeof(b)); memset(&f,0,sizeof(f));
      sdErrorBody(&b, SQLITE_MISUSE,
                  bHello ? "unknown request opcode" : "HELLO first");
      frWrap(&f, b.a, b.n);
      sdSendAll(c.s, f.a, f.n);
      wbFree(&f); wbFree(&b); }
    sdAtomicInc(&d->nReqOut);
    free(m.a);
    break;
  }

  sdSockClose(c.s);
  sdConnFree(&c);
  sdAtomicDec(&d->nConn);
  return 0;
}

/* ------------------------------------------------------------- lifecycle -- */
/* Open the declared archive: verify at startup that the daemon can
** serve the WHOLE lineage 1..head with this database's genesis --
** refuse-never-skip is only honest if holes are found when declared,
** not discovered by a subscriber at 2am. */
static int sdArchiveOpen(SdQueue *q, const char *zDir){
  sqlite3_int64 s, head;
  char zPath[SD_PATHMAX];
  sdDirCreate(zDir);           /* declared explicitly: create if absent */
  if( replWriterOpen(q->db, &q->pRepl)!=SQLITE_OK || q->pRepl==0 ){
    fprintf(stderr, "sprocketd: cannot open the segment writer on %s "
            "(session support missing from this build?)\n", zDir);
    return 1;
  }
  head = q->pRepl->iLastCut;
  for(s=1; s<=head; s++){
    sdSegPath(zPath, sizeof(zPath), zDir, s);
    if( !sdFileExists(zPath) ){
      fprintf(stderr, "sprocketd: this database has cut %lld segments "
              "but the archive is missing seq %lld (%s); refusing to "
              "serve a lineage with a hole.  Point --archive at the "
              "directory holding this lineage.\n", head, s, zPath);
      return 1;
    }
  }
  if( head>0 ){
    int nSeg = 0;
    unsigned char *aSeg;
    sdSegPath(zPath, sizeof(zPath), zDir, 1);
    aSeg = sdSegRead(zPath, &nSeg);
    if( aSeg==0 || nSeg<SEG_HDRSIZE
     || memcmp(aSeg+12, q->pRepl->aGenesis, 16)!=0 ){
      fprintf(stderr, "sprocketd: the archive at %s belongs to a "
              "different database lineage; refusing to mix them.\n", zDir);
      sqlite3_free(aSeg);
      return 1;
    }
    sqlite3_free(aSeg);
  }
  /* files BEYOND the writer's head mean the database went backward
  ** under its archive (restored from a stale copy?) */
  {
    SdDir dd;
    if( sdDirOpen(&dd, zDir)==0 ){
      const char *zName;
      while( (zName = sdDirNext(&dd))!=0 ){
        sqlite3_int64 v = 0;
        const char *z = zName;
        const char *zDot = strchr(zName, '.');
        if( zDot==0 || strcmp(zDot, ".sprkseg")!=0 ) continue;
        while( *z && *z!='.' ){
          int c = *z++;
          v = v*16 + (c<='9' ? c-'0' : (c|0x20)-'a'+10);
        }
        if( v>head ){
          fprintf(stderr, "sprocketd: archive holds seq %lld but this "
                  "database has only cut %lld -- the database is BEHIND "
                  "its own archive (restored from a stale copy?); "
                  "refusing to overwrite the lineage.\n", v, head);
          sdDirClose(&dd);
          return 1;
        }
      }
      sdDirClose(&dd);
    }
  }
  q->iHeadSeq = head;
  q->zArchive = zDir;
  return 0;
}

static int sdOpen(Sprocketd *d, const char *zDb, int port,
                  const char *zArchive){
  int i;
  memset(d, 0, sizeof(*d));
  d->zDb = zDb;
  d->port = port;
  sdMutexInit(&d->routeMx);

  /* The writer connection declares the mode -- checked when declared. */
  if( sqlite3_open(zDb, &d->wq.db)!=SQLITE_OK ) return 1;
  sqlite3_exec(d->wq.db, "PRAGMA journal_mode=wal2;"
                         "PRAGMA synchronous=NORMAL;", 0, 0, 0);
  {
    sqlite3_stmt *p = 0;
    int ok = 0;
    if( sqlite3_prepare_v2(d->wq.db, "PRAGMA queue_writer=ON;", -1, &p, 0)
          ==SQLITE_OK && sqlite3_step(p)==SQLITE_ROW ){
      ok = sqlite3_column_int(p, 0);
    }
    sqlite3_finalize(p);
    if( !ok ){
      fprintf(stderr, "sprocketd: queue_writer did not take on %s "
              "(not WAL, or empty database?); refusing to serve "
              "unprotected\n", zDb);
      return 1;
    }
  }
  sqlite3_busy_timeout(d->wq.db, 5000);
  sdMutexInit(&d->wq.mx);
  sdCondInit(&d->wq.cvItem);
  sdCondInit(&d->wq.cvSpace);
  sdCondInit(&d->wq.cvCut);
  if( zArchive && sdArchiveOpen(&d->wq, zArchive) ) return 1;
  d->wq.hGate = sdEventCreate(1, 1);
  d->wq.hThread = sdThreadCreate(sdWriter, &d->wq);
  if( !SD_EVENT_OK(d->wq.hGate) || !SD_THREAD_OK(d->wq.hThread) ) return 1;

  for(i=0;i<SD_NREADER;i++){
    if( sqlite3_open(zDb, &d->aReader[i])!=SQLITE_OK ) return 1;
    sqlite3_busy_timeout(d->aReader[i], 0);   /* the backstop must answer
                                              ** promptly, not spin */
    sdMutexInit(&d->aReaderMx[i]);
  }
  sdLoadRoutes(d, d->wq.db);

  d->sListen = socket(AF_INET, SOCK_STREAM, 0);
  if( d->sListen==SD_INVALID_SOCKET ) return 1;
  {
    struct sockaddr_in a;
    SdSockLen n = (SdSockLen)sizeof(a);
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((unsigned short)d->port);
    if( bind(d->sListen, (struct sockaddr*)&a, sizeof(a))!=0 ) return 1;
    if( d->port==0 ){
      getsockname(d->sListen, (struct sockaddr*)&a, &n);
      d->port = ntohs(a.sin_port);
    }
  }
  if( listen(d->sListen, 16)!=0 ) return 1;
  d->hReady = sdEventCreate(1, 0);
  return 0;
}

static SdThreadRet SD_THREAD_CALL sdAcceptLoop(SdThreadArg pArg){
  Sprocketd *d = (Sprocketd*)pArg;
  sdEventSet(d->hReady);
  while( !d->bShutdown ){
    SdSocket s = accept(d->sListen, 0, 0);
    if( s==SD_INVALID_SOCKET ) break;
    {
      SdConnArg *ca = (SdConnArg*)malloc(sizeof(*ca));
      if( ca==0 ){ sdSockClose(s); continue; }
      ca->d = d; ca->s = s;
      if( sdThreadCreateDetached(sdConnThread, ca) ){
        sdSockClose(s);
        free(ca);
      }
    }
  }
  return 0;
}

/* Graceful: stop accepting, drain the queue, close everything. */
static void sdClose(Sprocketd *d){
  int i;
  sdAtomicExchange(&d->bShutdown, 1);
  if( d->sListen!=SD_INVALID_SOCKET ){ sdSockClose(d->sListen); }
  sdCondWakeAll(&d->wq.cvCut);   /* subscribers re-check */
  /* connections notice bShutdown at their next message or timeout */
  for(i=0;i<200 && d->nConn>0;i++) sdSleepMs(25);
  sdMutexEnter(&d->wq.mx);
  d->wq.bStop = 1;
  sdCondWakeAll(&d->wq.cvItem);
  sdMutexLeave(&d->wq.mx);
  sdThreadJoin(d->wq.hThread, -1);
  sdEventDestroy(d->wq.hGate);
  sdMutexDestroy(&d->wq.mx);
  if( d->wq.pRepl ) replWriterClose(d->wq.pRepl);
  sqlite3_close(d->wq.db);
  for(i=0;i<SD_NREADER;i++){
    sqlite3_close(d->aReader[i]);
    sdMutexDestroy(&d->aReaderMx[i]);
  }
  for(i=0;i<d->nRoute;i++) sqlite3_free(d->aRoute[i].zName);
  sqlite3_free(d->aRoute);
  sdMutexDestroy(&d->routeMx);
  sdEventDestroy(d->hReady);
}

/* ================================ selftest =============================== */
#ifndef SPROCKETD_NO_MAIN

/* -- tiny client ---------------------------------------------------------- */
static SdSocket stConnect(int port){
  SdSocket s = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = htons((u_short)port);
  if( connect(s, (struct sockaddr*)&a, sizeof(a))!=0 ){
    sdSockClose(s); return SD_INVALID_SOCKET;
  }
  sdTimeouts(s);
  return s;
}
static int stHello(SdConn *pc){
  WireBuf b, f; SdMsg m; WireRd rd; unsigned ver = 0;
  memset(&b,0,sizeof(b)); memset(&f,0,sizeof(f));
  wbU8(&b, REQ_HELLO); wbU32(&b, PROTO_VER);
  frWrap(&f, b.a, b.n);
  if( sdSendAll(pc->s, f.a, f.n) ){ wbFree(&f); wbFree(&b); return 0; }
  wbFree(&f); wbFree(&b);
  if( sdRecvMsg(pc, &m) ) return 0;
  rdInit(&rd, m.a, m.n);
  ver = rdU32(&rd);
  free(m.a);
  return ver==PROTO_VER;
}
/* Send a CALL; returns the two response messages (hdr, body). */
static int stCall(SdConn *pc, unsigned reqId, const char *zName,
                  unsigned cookie, int nArg, sqlite3_int64 *aInt,
                  SdMsg *pHdr, SdMsg *pBody){
  WireBuf b, f; int i;
  memset(&b,0,sizeof(b)); memset(&f,0,sizeof(f));
  wbU8(&b, REQ_CALL); wbU32(&b, reqId);
  wbBytes(&b, zName, (int)strlen(zName));   /* length-prefixed by wbBytes */
  wbU32(&b, cookie);
  wbU32(&b, (unsigned)nArg);
  for(i=0;i<nArg;i++){ wbU8(&b, WV_INT); wbI64(&b, aInt[i]); }
  frWrap(&f, b.a, b.n);
  if( sdSendAll(pc->s, f.a, f.n) ){ wbFree(&f); wbFree(&b); return 1; }
  wbFree(&f); wbFree(&b);
  if( sdRecvMsg(pc, pHdr) ) return 1;
  if( sdRecvMsg(pc, pBody) ){ free(pHdr->a); return 1; }
  return 0;
}
static int memmem_like(const unsigned char *z, int n, const char *zN);

/* Send SUBSCRIBE; returns the ack pair (hdr, body).  Segment frames
** after the ack are read by the caller with sdRecvMsg directly. */
static int stSub(SdConn *pc, unsigned reqId, sqlite3_int64 from,
                 SdMsg *pHdr, SdMsg *pBody){
  WireBuf b, f;
  memset(&b,0,sizeof(b)); memset(&f,0,sizeof(f));
  wbU8(&b, REQ_SUB); wbU32(&b, reqId); wbI64(&b, from);
  frWrap(&f, b.a, b.n);
  if( sdSendAll(pc->s, f.a, f.n) ){ wbFree(&f); wbFree(&b); return 1; }
  wbFree(&f); wbFree(&b);
  if( sdRecvMsg(pc, pHdr) ) return 1;
  if( sdRecvMsg(pc, pBody) ){ free(pHdr->a); return 1; }
  return 0;
}

/* First WV_INT found in the body's rows, or -1. */
static sqlite3_int64 stFirstInt(SdMsg *pBody){
  WireRd rd;
  rdInit(&rd, pBody->a, pBody->n);
  while( rdHave(&rd, 1) ){
    unsigned fr = rdU8(&rd);
    if( fr==WF_SHAPE ){
      unsigned nc = rdU32(&rd), i;
      for(i=0;i<nc && !rd.bad;i++){ int n; rdBytes(&rd,&n); }
    }else if( fr==WF_ROW ){
      unsigned tag = rdU8(&rd);
      if( tag==WV_INT ) return rdI64(&rd);
      if( tag==WV_TEXT || tag==WV_BLOB ){ int n; rdBytes(&rd,&n); }
      else if( tag==WV_FLOAT ) rdDouble(&rd);
      /* skip rest of row: this helper only needs the first value of
      ** single-column results and (metric,value) pairs -- for the
      ** latter the caller matches metric text itself */
    }else if( fr==WF_ERROR ){
      return -1;
    }else if( fr==WF_DONE ){
      break;
    }
    if( rd.bad ) break;
  }
  return -1;
}
static int stBodyIsError(SdMsg *pBody, const char *zNeedle){
  WireRd rd;
  unsigned fr;
  rdInit(&rd, pBody->a, pBody->n);
  fr = rdU8(&rd);
  if( fr!=WF_ERROR ) return 0;
  rdU32(&rd);
  { int n; const unsigned char *z = rdBytes(&rd, &n);
    if( z==0 ) return 0;
    if( zNeedle && !memmem_like(z, n, zNeedle) ) return 0; }
  return 1;
}
/* strstr over unterminated bytes */
static int memmem_like(const unsigned char *z, int n, const char *zN){
  int nn = (int)strlen(zN), i;
  for(i=0; i+nn<=n; i++) if( memcmp(z+i, zN, nn)==0 ) return 1;
  return 0;
}

static void stFixture(const char *zDb){
  sqlite3 *db = 0;
  sdFileDelete(zDb);
  sqlite3_open(zDb, &db);
  sqlite3_exec(db,
    "PRAGMA journal_mode=wal2;"
    "CREATE TABLE t(a INTEGER, b TEXT);"
    "INSERT INTO t VALUES(1,'one'),(2,'two'),(3,'three');"
    "CREATE PROCEDURE readn(lim INTEGER)"
    "  RETURNS TABLE(a INTEGER, b TEXT)"
    "BEGIN SELECT a, b FROM t WHERE a <= lim ORDER BY a; END;"
    "CREATE PROCEDURE addrow(v INTEGER)"
    "  RETURNS TABLE(n INTEGER)"
    "BEGIN INSERT INTO t(a,b) VALUES(v, 'w'); "
    "SELECT count(*) AS n FROM t; END;",
    0, 0, 0);
  sqlite3_close(db);
}

/* The replication fixture is its OWN database: the session module
** silently skips tables without a declared PRIMARY KEY (the capture
** would be EMPTY -- the fabricated-silence class again), so the
** replicated table declares one, and the main fixture's PK-less table
** stays as a naturally-occurring control that never reaches a session. */
static void stFixtureRepl(const char *zDb){
  sqlite3 *db = 0;
  sdFileDelete(zDb);
  sqlite3_open(zDb, &db);
  sqlite3_exec(db,
    "PRAGMA journal_mode=wal2;"
    "CREATE TABLE t(a INTEGER PRIMARY KEY, b TEXT);"
    "CREATE PROCEDURE addrow(v INTEGER)"
    "  RETURNS TABLE(n INTEGER)"
    "BEGIN INSERT INTO t(a,b) VALUES(v, 'w'); "
    "SELECT count(*) AS n FROM t; END;",
    0, 0, 0);
  sqlite3_close(db);
}
static void stRmArchive(const char *zDir){
  SdDir dd;
  char zP[SD_PATHMAX];
  if( sdDirOpen(&dd, zDir)==0 ){
    const char *zName;
    while( (zName = sdDirNext(&dd))!=0 ){
      sqlite3_snprintf(sizeof(zP), zP, "%s/%s", zDir, zName);
      sdFileDelete(zP);
    }
    sdDirClose(&dd);
  }
  sdDirRemove(zDir);
}

/* Route the engine's error log to stderr.  sqlite3_log carries the
** reports that have no connection to land on -- "API called with NULL
** prepared statement" and its kin -- and an unregistered log callback
** silently discards them.  The "not an error" misdirection during this
** daemon's own debugging was exactly one of these: the accurate
** message existed and evaporated. */
static void sdLogTo(void *pArg, int iErrCode, const char *zMsg){
  (void)pArg;
  fprintf(stderr, "sprocketd engine log (%d): %s\n", iErrCode, zMsg);
}

int main(int argc, char **argv){
  sqlite3_config(SQLITE_CONFIG_LOG, sdLogTo, 0);
  sdSockInit();

  if( argc>=2 && strcmp(argv[1], "--selftest")==0 ){
    Sprocketd d;
    SdThread hAcc;
    SdConn c;
    SdMsg hdr, body;
    const char *zDb = "sprocketd_test.db";

    printf("sprocketd selftest -- SQLite %s\n\n", sqlite3_libversion());
    stFixture(zDb);
    check( sdOpen(&d, zDb, 0, 0)==0, "daemon opens (declares queue_writer)" );
    hAcc = sdThreadCreate(sdAcceptLoop, &d);
    sdEventWaitMs(d.hReady, 5000);

    /* 1 -- HELLO */
    sdConnInit(&c, stConnect(d.port));
    check( c.s!=SD_INVALID_SOCKET, "client connects" );
    check( stHello(&c), "HELLO round-trips at version 1" );
    {
      SdConn c2;
      WireBuf b, f; SdMsg m; WireRd rd;
      sdConnInit(&c2, stConnect(d.port));
      memset(&b,0,sizeof(b)); memset(&f,0,sizeof(f));
      wbU8(&b, REQ_HELLO); wbU32(&b, 99);
      frWrap(&f, b.a, b.n);
      sdSendAll(c2.s, f.a, f.n);
      wbFree(&f); wbFree(&b);
      check( sdRecvMsg(&c2, &m)==0, "wrong-version HELLO gets an answer" );
      rdInit(&rd, m.a, m.n);
      check( rdU32(&rd)==0, "CONTROL: version 99 is refused (ver=0)" );
      free(m.a);
      sdSockClose(c2.s); sdConnFree(&c2);
    }

    /* 2 -- read CALL + shape cache */
    {
      sqlite3_int64 lim = 2;
      unsigned cookie;
      int nFirst, nSecond;
      WireRd rd;
      check( stCall(&c, 7, "readn", 0, 1, &lim, &hdr, &body)==0,
             "read CALL answers" );
      rdInit(&rd, hdr.a, hdr.n);
      check( rdU32(&rd)==7, "response echoes the request id" );
      cookie = rdU32(&rd);
      check( cookie!=0, "server cookie travels in the header" );
      nFirst = body.n;
      free(hdr.a); free(body.a);
      check( stCall(&c, 8, "readn", cookie, 1, &lim, &hdr, &body)==0,
             "cached-cookie CALL answers" );
      nSecond = body.n;
      check( nSecond < nFirst,
             "matching cookie omits shapes (phase-4 contract over the "
             "daemon)" );
      free(hdr.a); free(body.a);
    }

    /* 3+4 -- writes are durable and serialized; readers live meanwhile */
    {
      sqlite3_int64 v;
      int i;
      for(i=0;i<8;i++){
        v = 100+i;
        check( stCall(&c, 20+(unsigned)i, "addrow", 0, 1, &v, &hdr, &body)
                 ==0, "write CALL answers" );
        free(hdr.a); free(body.a);
      }
      v = 999;
      check( stCall(&c, 30, "readn", 0, 1, &v, &hdr, &body)==0,
             "read after writes answers" );
      free(hdr.a); free(body.a);
      /* durability from an out-of-band reader */
      {
        sqlite3 *dbc = 0; sqlite3_stmt *p = 0; int n = -1;
        sqlite3_open(zDb, &dbc);
        if( sqlite3_prepare_v2(dbc, "SELECT count(*) FROM t", -1, &p, 0)
              ==SQLITE_OK && sqlite3_step(p)==SQLITE_ROW ){
          n = sqlite3_column_int(p, 0);
        }
        sqlite3_finalize(p); sqlite3_close(dbc);
        check( n==11, "8 queued writes all durable (3+8 rows)" );
      }
    }

    /* 5 -- out-of-band writer refused while the daemon lives */
    {
      sqlite3 *dbp = 0; int rc;
      sqlite3_open(zDb, &dbp);
      sqlite3_busy_timeout(dbp, 0);
      rc = sqlite3_exec(dbp, "INSERT INTO t VALUES(0,'barge');", 0, 0, 0);
      check( rc==SQLITE_BUSY, "out-of-band writer refused by the engine" );
      check( strstr(sqlite3_errmsg(dbp), "queued-write mode")!=0,
             "the refusal names the mode" );
      sqlite3_close(dbp);
    }

    /* 6 -- the lying-proc backstop, via the test hook */
    {
      sqlite3_int64 v = 500;
      d.bTestMisroute = 1;
      check( stCall(&c, 40, "addrow", 0, 1, &v, &hdr, &body)==0,
             "misrouted write CALL answers" );
      check( stBodyIsError(&body, "routing error"),
             "misrouted write becomes an attributed routing error" );
      free(hdr.a); free(body.a);
      d.bTestMisroute = 0;
      check( stCall(&c, 41, "addrow", 0, 1, &v, &hdr, &body)==0,
             "CONTROL: same proc honestly routed answers" );
      check( !stBodyIsError(&body, 0), "CONTROL: and it is not an error" );
      free(hdr.a); free(body.a);
    }

    /* 7 -- stats */
    {
      check( stCall(&c, 50, "sprocket_stats", 0, 0, 0, &hdr, &body)==0,
             "sprocket_stats answers" );
      check( !stBodyIsError(&body, 0), "stats body is a result set" );
      /* writes_done moved: it is >= the 9 writes above */
      { WireRd rd; int seen = 0;
        rdInit(&rd, body.a, body.n);
        while( rdHave(&rd, 1) && !rd.bad ){
          unsigned fr = rdU8(&rd);
          if( fr==WF_SHAPE ){ unsigned nc=rdU32(&rd),i;
            for(i=0;i<nc;i++){ int n; rdBytes(&rd,&n);} }
          else if( fr==WF_ROW ){
            int n; const unsigned char *z;
            rdU8(&rd); z = rdBytes(&rd, &n);
            rdU8(&rd);
            { sqlite3_int64 v = rdI64(&rd);
              if( z && memmem_like(z, n, "writes_done") && v>=9 ) seen = 1; }
          }
          else break;
        }
        check( seen, "stats: writes_done reflects the burst" );
      }
      free(hdr.a); free(body.a);
    }

    /* 8 -- malformed request bytes: reject, never crash */
    {
      int i, nRejected = 0, nProbe = 0;
      for(i=0;i<40;i++){
        SdConn cm;
        WireBuf b, f; SdMsg m;
        sdConnInit(&cm, stConnect(d.port));
        if( cm.s==SD_INVALID_SOCKET ) continue;
        if( !stHello(&cm) ){ sdSockClose(cm.s); sdConnFree(&cm); continue; }
        memset(&b,0,sizeof(b)); memset(&f,0,sizeof(f));
        wbU8(&b, REQ_CALL); wbU32(&b, 1);
        wbU32(&b, 0xFFFFFFF0u);            /* absurd name length */
        wbU32(&b, (unsigned)i*7919);       /* junk */
        frWrap(&f, b.a, b.n);
        sdSendAll(cm.s, f.a, f.n);
        wbFree(&f); wbFree(&b);
        nProbe++;
        if( sdRecvMsg(&cm, &m)==0 ){
          /* header or error -- accept either, but the FOLLOWING body,
          ** if any, must be an error */
          free(m.a);
          if( sdRecvMsg(&cm, &m)==0 ){
            SdMsg bm = m;
            if( stBodyIsError(&bm, "malformed") ) nRejected++;
            free(m.a);
          }else{
            nRejected++;                   /* refused by close: fine */
          }
        }else{
          nRejected++;
        }
        sdSockClose(cm.s); sdConnFree(&cm);
      }
      printf("  malformed sweep: %d/%d rejected\n", nRejected, nProbe);
      check( nProbe>=30, "malformed sweep actually probed" );
      check( nRejected==nProbe, "every malformed request rejected, "
             "no silent accepts" );
      /* the daemon survived: a live request still works */
      { sqlite3_int64 lim = 1;
        check( stCall(&c, 60, "readn", 0, 1, &lim, &hdr, &body)==0,
               "daemon alive after the corruption sweep" );
        free(hdr.a); free(body.a); }
    }

    /* 8b -- R4: SUBSCRIBE without a declared archive: refused, the
    ** refusal names the fix, and the connection still serves CALLs
    ** (a refusal is an answer, not a hangup). */
    {
      SdMsg sh, sb;
      check( stSub(&c, 70, 1, &sh, &sb)==0,
             "SUBSCRIBE answered on an archiveless daemon" );
      check( stBodyIsError(&sb, "--archive"),
             "refusal names the fix (restart with --archive DIR)" );
      free(sh.a); free(sb.a);
      { sqlite3_int64 lim = 1;
        check( stCall(&c, 71, "readn", 0, 1, &lim, &hdr, &body)==0,
               "the connection still serves CALLs after the refusal" );
        free(hdr.a); free(body.a); }
    }

    /* 9 -- graceful shutdown: everything answered */
    {
      long in, out;
      sdSockClose(c.s); sdConnFree(&c);
      sdSleepMs(100);
      in = d.nReqIn; out = d.nReqOut;
      check( in==out, "count in == count out at shutdown "
             "(no request silently dropped)" );
    }

    sdClose(&d);
    sdThreadJoin(hAcc, 5000);
    sdFileDelete(zDb);

    /* ============ 10 -- R4: the archive daemon ============ */
    {
      Sprocketd d2;
      SdThread hAcc2;
      SdConn c4, cs;
      SdMsg h2, b2;
      const char *zDb2 = "sprocketd_repl.db";
      const char *zArc = "sprocketd_test_archive";

      stFixtureRepl(zDb2);
      stRmArchive(zArc);
      check( sdOpen(&d2, zDb2, 0, zArc)==0,
             "daemon opens with a DECLARED archive" );
      hAcc2 = sdThreadCreate(sdAcceptLoop, &d2);
      sdEventWaitMs(d2.hReady, 5000);

      sdConnInit(&c4, stConnect(d2.port));
      check( c4.s!=SD_INVALID_SOCKET && stHello(&c4),
             "client connects to the archive daemon" );
      { sqlite3_int64 v = 700;
        check( stCall(&c4, 80, "addrow", 0, 1, &v, &h2, &b2)==0,
               "write 1 answers" );
        free(h2.a); free(b2.a);
        v = 701;
        check( stCall(&c4, 81, "addrow", 0, 1, &v, &h2, &b2)==0,
               "write 2 answers" );
        free(h2.a); free(b2.a); }
      { char zP[SD_PATHMAX];
        sdSegPath(zP, sizeof(zP), zArc, 1);
        check( sdFileExists(zP),
               "segment 1 is ON DISK where the operator looks" );
        sdSegPath(zP, sizeof(zP), zArc, 2);
        check( sdFileExists(zP),
               "segment 2 beside it" ); }

      /* 10b -- subscribe from 1: catch-up, then LIVE, one loop */
      sdConnInit(&cs, stConnect(d2.port));
      check( cs.s!=SD_INVALID_SOCKET && stHello(&cs), "subscriber connects" );
      {
        SdMsg sh, sb, m1, m2, m3;
        check( stSub(&cs, 90, 1, &sh, &sb)==0, "SUBSCRIBE from 1 acks" );
        check( !stBodyIsError(&sb, 0), "the ack is not an error" );
        check( stFirstInt(&sb)==2,
               "ack carries head_seq 2 (the catch-up span is announced)" );
        free(sh.a); free(sb.a);
        check( sdRecvMsg(&cs, &m1)==0 && m1.n>SEG_HDRSIZE
            && memcmp(m1.a, SEG_MAGIC, 8)==0
            && getU64(m1.a+28)==1,
               "segment 1 arrives on the wire, self-describing" );
        check( sdRecvMsg(&cs, &m2)==0 && memcmp(m2.a, SEG_MAGIC, 8)==0
            && getU64(m2.a+28)==2, "segment 2 follows in order" );
        /* Q4-C's central claim: the same bytes at different addresses */
        { char zP[SD_PATHMAX]; int nf = 0; unsigned char *af;
          sdSegPath(zP, sizeof(zP), zArc, 1);
          af = sdSegRead(zP, &nf);
          check( af && nf==m1.n && memcmp(af, m1.a, nf)==0,
                 "wire segment 1 is BYTE-EQUAL to the archive file" );
          sqlite3_free(af); }
        /* live tail: a write on the CALL connection arrives on the open
        ** subscription without re-subscribing */
        { sqlite3_int64 v = 702;
          check( stCall(&c4, 82, "addrow", 0, 1, &v, &h2, &b2)==0,
                 "live write answers" );
          free(h2.a); free(b2.a); }
        check( sdRecvMsg(&cs, &m3)==0 && memcmp(m3.a, SEG_MAGIC, 8)==0
            && getU64(m3.a+28)==3,
               "segment 3 arrives LIVE on the open subscription" );
        /* end to end: a replica built from the wire matches the primary */
        {
          sqlite3 *dbrep = 0;
          sqlite3_stmt *p = 0;
          char *zE = 0;
          int nRow = -1, sum = 0, rcA;
          sqlite3_open(":memory:", &dbrep);
          sqlite3_exec(dbrep,
            "CREATE TABLE t(a INTEGER PRIMARY KEY, b TEXT)", 0, 0, 0);
          rcA = replApply(dbrep, m1.a, m1.n, &zE);
          if( rcA==SQLITE_OK ) rcA = replApply(dbrep, m2.a, m2.n, &zE);
          if( rcA==SQLITE_OK ) rcA = replApply(dbrep, m3.a, m3.n, &zE);
          check( rcA==SQLITE_OK, "all three wire segments apply cleanly" );
          sqlite3_free(zE);
          if( sqlite3_prepare_v2(dbrep,
                "SELECT count(*), sum(a) FROM t", -1, &p, 0)==SQLITE_OK
           && sqlite3_step(p)==SQLITE_ROW ){
            nRow = sqlite3_column_int(p, 0);
            sum = sqlite3_column_int(p, 1);
          }
          sqlite3_finalize(p);
          sqlite3_close(dbrep);
          check( nRow==3 && sum==700+701+702,
                 "the wire-built replica equals the primary" );
        }
        free(m1.a); free(m2.a); free(m3.a);
      }

      /* 10c -- a subscription from the future is refused naming head */
      {
        SdConn cf; SdMsg fh, fb;
        sdConnInit(&cf, stConnect(d2.port));
        check( cf.s!=SD_INVALID_SOCKET && stHello(&cf),
               "future-subscriber connects" );
        check( stSub(&cf, 91, 99, &fh, &fb)==0, "future SUBSCRIBE answered" );
        check( stBodyIsError(&fb, "head is seq 3"),
               "refused naming the archive head (a diverged replica is "
               "told, not fed)" );
        free(fh.a); free(fb.a);
        sdSockClose(cf.s); sdConnFree(&cf);
      }

      sdSockClose(cs.s); sdConnFree(&cs);
      sdSockClose(c4.s); sdConnFree(&c4);
      sdClose(&d2);
      sdThreadJoin(hAcc2, 5000);

      /* 10d -- CONTROL: a gapped archive is refused at STARTUP, not
      ** discovered by a subscriber later */
      {
        Sprocketd d3;
        char zP[SD_PATHMAX];
        sdSegPath(zP, sizeof(zP), zArc, 2);
        sdFileDelete(zP);
        check( sdOpen(&d3, zDb2, 0, zArc)!=0,
               "SEEN RED: archive with a hole refused at startup" );
        if( d3.wq.pRepl ) replWriterClose(d3.wq.pRepl);
        sqlite3_close(d3.wq.db);   /* the failed open's connection */
      }

      sdFileDelete(zDb2);
      stRmArchive(zArc);
    }
    printf("\n%s: %d checks, %d failures\n",
           nFail ? "SPROCKETD FAILED" : "SPROCKETD OK", nCheck, nFail);
    sdSockShutdown();
    return nFail ? 1 : 0;
  }

  /* ---- serve mode ---- */
  if( argc<2 ){
    fprintf(stderr,
      "usage: sprocketd DB [port] [--archive DIR]   (or --selftest)\n");
    return 2;
  }
  {
    Sprocketd d;
    int port = 7690;
    const char *zArchive = 0;
    int i;
    for(i=2; i<argc; i++){
      if( strcmp(argv[i], "--archive")==0 && i+1<argc ){
        zArchive = argv[++i];
      }else{
        port = atoi(argv[i]);
      }
    }
    if( sdOpen(&d, argv[1], port, zArchive) ){
      fprintf(stderr, "sprocketd: failed to open %s on port %d\n",
              argv[1], port);
      return 1;
    }
    printf("sprocketd: serving %s on 127.0.0.1:%d (%d procs routed, "
           "%d readers + 1 queued writer)\n",
           argv[1], d.port, d.nRoute, SD_NREADER);
    fflush(stdout);
    sdAcceptLoop(&d);
    sdClose(&d);
  }
  sdSockShutdown();
  return 0;
}
#endif /* SPROCKETD_NO_MAIN */
