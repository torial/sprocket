/*
** proc_server.c -- Phase 2 of PLAN-TRANSPORT.md: a real TCP loopback echo of
** one real CALL, end to end.
**
** SHAPE OF THIS TEST
**
** The plan called for two processes coordinated by a shell script.  This runs
** the server on a THREAD in the same binary instead, over genuine TCP on
** 127.0.0.1.  The socket path is real -- connect, send, recv, close -- but the
** test cannot hang waiting on a process that died, cannot leak an orphan
** listener, and needs no orchestration outside the binary.  Everything gets a
** receive timeout, so the failure mode of every negative case below is a
** prompt error rather than a wedged run.
**
** THE POSITIVE CONTROL
**
** The client checks that what came off the socket decodes to the SAME checksum
** that tool/proc_wire.c pins with no transport involved at all.  That is the
** whole point of the arrangement: the network path cannot pass by agreeing
** with itself, only by agreeing with a value proven without a network.
**
** Build (from repo root, VS dev prompt):
**   cl /O2 /I. /Itool tool\proc_server.c sqlite3.c ws2_32.lib /Fe:proc_server.exe
*/
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#define PROC_WIRE_NO_MAIN  1
#define PROC_FRAME_NO_MAIN 1
#include "proc_frame.c"        /* framer + codec + fixture + check()/nFail */

#pragma comment(lib, "ws2_32.lib")

#define IO_TIMEOUT_MS 5000     /* nothing in this test may block longer */

/* What a given server run should do -- the negative cases need a server that
** misbehaves in one specific way. */
#define SRV_NORMAL 0           /* answer the CALL properly                  */
#define SRV_SHORT  1           /* declare N bytes, send N-1 (truncated)     */
#define SRV_ALTERED 2          /* flip one payload byte before framing      */
#define SRV_ALTERARG 3         /* flip one ARGUMENT byte before binding     */

/* Request opcodes.  A request is:
**   [u8 REQ_CALL][u32 nameLen][name][u32 nArg][ (u8 tag)(payload) x nArg ]
** The argument encoding is deliberately the SAME value encoding the response
** rows use, so a round trip can be checked by folding both with one function
** rather than by two hand-written comparisons that could disagree. */
#define REQ_CALL 1

typedef struct ServerCtx {
  SOCKET sListen;
  int port;
  int mode;
  int bAccepted;               /* did a client actually arrive?             */
  int bBadRequest;             /* did we reject a malformed request?        */
  HANDLE hReady;               /* signalled once listening, before accept   */
} ServerCtx;

/* ------------------------------------------------------------ io helpers -- */
static int sendAll(SOCKET s, const unsigned char *z, int n){
  int i = 0;
  while( i < n ){
    int k = send(s, (const char*)z + i, n - i, 0);
    if( k<=0 ) return 1;
    i += k;
  }
  return 0;
}
static void setTimeouts(SOCKET s){
  DWORD ms = IO_TIMEOUT_MS;
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&ms, sizeof(ms));
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&ms, sizeof(ms));
}

/* Collector used by both ends to capture exactly one framed message. */
typedef struct OneMsg {
  unsigned char *a;
  int n;
  int nMsg;
} OneMsg;

static void takeMsg(void *pArg, const unsigned char *z, int n){
  OneMsg *p = (OneMsg*)pArg;
  p->nMsg++;
  if( p->a==0 ){
    p->a = (unsigned char*)malloc(n ? n : 1);
    if( p->a ){ memcpy(p->a, z, n); p->n = n; }
  }
}

/*
** Read from a socket until the framer yields one complete message, the peer
** closes, the framer rejects the stream, or the receive timeout fires.
** Returns SQLITE_OK only when a whole message was assembled.
*/
static int recvOneMessage(SOCKET s, OneMsg *pOut){
  Framer fr;
  unsigned char buf[64];        /* deliberately small: forces multiple reads
                                ** and re-exercises the Phase 1 reassembly on
                                ** boundaries the kernel actually produces */
  int rc = SQLITE_OK;
  memset(&fr, 0, sizeof(fr));
  memset(pOut, 0, sizeof(*pOut));
  while( pOut->nMsg==0 ){
    int k = recv(s, (char*)buf, (int)sizeof(buf), 0);
    if( k<=0 ){ rc = SQLITE_IOERR; break; }        /* closed or timed out */
    rc = frFeed(&fr, buf, k, takeMsg, pOut);
    if( rc!=SQLITE_OK ) break;
  }
  frFree(&fr);
  if( pOut->nMsg==0 && rc==SQLITE_OK ) rc = SQLITE_IOERR;
  return rc;
}

/*
** Bind one wire-encoded value to a statement parameter.  Strings and blobs are
** bound SQLITE_TRANSIENT because they point into the request buffer, which is
** freed as soon as binding finishes.
*/
static int bindOneValue(sqlite3_stmt *pStmt, int idx, WireRd *rd){
  unsigned t = rdU8(rd);
  int n = 0;
  const unsigned char *z;
  switch( t ){
    case WV_NULL:  return sqlite3_bind_null(pStmt, idx);
    case WV_INT:   return sqlite3_bind_int64(pStmt, idx, rdI64(rd));
    case WV_FLOAT: return sqlite3_bind_double(pStmt, idx, rdDouble(rd));
    case WV_TEXT:
      z = rdBytes(rd, &n);
      if( rd->bad ) return SQLITE_CORRUPT;
      return sqlite3_bind_text(pStmt, idx, (const char*)z, n, SQLITE_TRANSIENT);
    case WV_BLOB:
      z = rdBytes(rd, &n);
      if( rd->bad ) return SQLITE_CORRUPT;
      return sqlite3_bind_blob(pStmt, idx, z, n, SQLITE_TRANSIENT);
  }
  rd->bad = 1;
  return SQLITE_CORRUPT;
}

/* --------------------------------------------------------------- server -- */
static DWORD WINAPI serverThread(LPVOID pArg){
  ServerCtx *p = (ServerCtx*)pArg;
  SOCKET sConn;
  OneMsg req;
  sqlite3 *db = 0;
  sqlite3_stmt *pCall = 0;
  WireBuf body, out;

  memset(&body, 0, sizeof(body));
  memset(&out, 0, sizeof(out));

  SetEvent(p->hReady);                       /* liveness: we are listening */

  sConn = accept(p->sListen, 0, 0);
  if( sConn==INVALID_SOCKET ) return 0;
  p->bAccepted = 1;
  setTimeouts(sConn);

  /* Parse the request.  Every field is bounds-checked before anything is
  ** prepared or executed: a malformed request must cost us a closed socket,
  ** not a query. */
  {
    WireRd rd;
    const unsigned char *zName;
    char zSql[512];
    char zArgs[160];
    int nName = 0, nArg, i;

    if( recvOneMessage(sConn, &req)!=SQLITE_OK ){
      p->bBadRequest = 1;
      free(req.a);
      closesocket(sConn);
      return 0;
    }
    /* Negative control for Phase 3: corrupt one argument byte on arrival. */
    if( p->mode==SRV_ALTERARG && req.n>0 ) req.a[req.n-1] ^= 0xA5;

    rdInit(&rd, req.a, req.n);
    zName = 0;
    if( rdU8(&rd)==REQ_CALL ) zName = rdBytes(&rd, &nName);
    nArg = rd.bad ? 0 : (int)rdU32(&rd);
    if( rd.bad || zName==0 || nName<=0 || nName>64 || nArg<0 || nArg>32 ){
      p->bBadRequest = 1;
      free(req.a);
      closesocket(sConn);
      return 0;
    }

    { int k = 0;
      for(i=0; i<nArg; i++){
        if( i ) zArgs[k++] = ',';
        zArgs[k++] = '?';
      }
      zArgs[k] = 0;
    }
    sqlite3_snprintf(sizeof(zSql), zSql, "CALL %.*s(%s);",
                     nName, (const char*)zName, zArgs);

    db = procWireTestDb();
    if( db==0 || sqlite3_prepare_v2(db, zSql, -1, &pCall, 0)!=SQLITE_OK ){
      p->bBadRequest = 1;
      free(req.a);
      if( db ) sqlite3_close(db);
      closesocket(sConn);
      return 0;
    }
    for(i=0; i<nArg && !rd.bad; i++){
      if( bindOneValue(pCall, i+1, &rd)!=SQLITE_OK ) rd.bad = 1;
    }
    free(req.a);
    req.a = 0;
    if( rd.bad ){
      p->bBadRequest = 1;
      sqlite3_finalize(pCall);
      sqlite3_close(db);
      closesocket(sConn);
      return 0;
    }
  }

  {
    if( procWireEncode(pCall, 1, &body)==SQLITE_OK ){
      if( p->mode==SRV_ALTERED && body.n>0 ){
        body.a[body.n/2] ^= 0xA5;                    /* corrupt one byte */
      }
      frWrap(&out, body.a, body.n);
      if( p->mode==SRV_SHORT && out.n>1 ) out.n--;   /* one byte short */
      sendAll(sConn, out.a, out.n);
    }
    sqlite3_finalize(pCall);
  }
  if( db ) sqlite3_close(db);
  wbFree(&body);
  wbFree(&out);
  closesocket(sConn);
  return 0;
}

/* Start a listener on an ephemeral loopback port. Returns 0 on success. */
static int startServer(ServerCtx *p, int mode){
  struct sockaddr_in addr;
  int len = sizeof(addr);
  HANDLE hThread;

  memset(p, 0, sizeof(*p));
  p->mode = mode;
  p->sListen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if( p->sListen==INVALID_SOCKET ) return 1;

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = 0;                          /* ephemeral */
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if( bind(p->sListen, (struct sockaddr*)&addr, sizeof(addr))!=0 ) return 1;
  if( listen(p->sListen, 1)!=0 ) return 1;
  if( getsockname(p->sListen, (struct sockaddr*)&addr, &len)!=0 ) return 1;
  p->port = ntohs(addr.sin_port);

  p->hReady = CreateEvent(0, TRUE, FALSE, 0);
  hThread = CreateThread(0, 0, serverThread, p, 0, 0);
  if( hThread==0 ) return 1;
  CloseHandle(hThread);

  /* Liveness, enforced: if the listener never signals, fail promptly rather
  ** than block on connect() forever. */
  if( WaitForSingleObject(p->hReady, IO_TIMEOUT_MS)!=WAIT_OBJECT_0 ) return 1;
  return 0;
}

static void stopServer(ServerCtx *p){
  if( p->sListen!=INVALID_SOCKET ) closesocket(p->sListen);
  if( p->hReady ) CloseHandle(p->hReady);
}

/* --------------------------------------------------------------- client -- */
static SOCKET dialLoopback(int port){
  struct sockaddr_in addr;
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if( s==INVALID_SOCKET ) return INVALID_SOCKET;
  setTimeouts(s);
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((unsigned short)port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if( connect(s, (struct sockaddr*)&addr, sizeof(addr))!=0 ){
    closesocket(s);
    return INVALID_SOCKET;
  }
  return s;
}

/*
** Send a request, read a framed reply, decode it.  Returns SQLITE_OK only on
** a complete, well-formed response.
*/
static int callOverSocket(
  int port,
  const char *zProc,
  const WireBuf *pArgs,          /* concatenated encoded values, may be NULL */
  int nArg,
  DecodeStats *pSt
){
  SOCKET s = dialLoopback(port);
  WireBuf body, req;
  OneMsg rsp;
  int rc;
  if( s==INVALID_SOCKET ) return SQLITE_IOERR;
  memset(&body, 0, sizeof(body));
  memset(&req, 0, sizeof(req));
  wbU8(&body, REQ_CALL);
  wbStr(&body, zProc);
  wbU32(&body, (unsigned)nArg);
  if( pArgs && pArgs->n>0 && wbNeed(&body, pArgs->n)==0 ){
    memcpy(body.a + body.n, pArgs->a, pArgs->n);
    body.n += pArgs->n;
  }
  frWrap(&req, body.a, body.n);
  wbFree(&body);
  if( sendAll(s, req.a, req.n) ){ wbFree(&req); closesocket(s); return SQLITE_IOERR; }
  wbFree(&req);
  rc = recvOneMessage(s, &rsp);
  if( rc==SQLITE_OK ) rc = procWireDecode(rsp.a, rsp.n, pSt);
  free(rsp.a);
  closesocket(s);
  return rc;
}

/* ============================== self-test ================================= */
int main(int argc, char **argv){
  WSADATA wsa;
  sqlite3 *db;
  sqlite3_stmt *pCall = 0;
  WireBuf ref;
  DecodeStats stRef, st;
  ServerCtx srv;

  (void)argc; (void)argv;
  memset(&ref, 0, sizeof(ref));

  printf("proc_server self-test -- SQLite %s\n\n", sqlite3_libversion());

  if( WSAStartup(MAKEWORD(2,2), &wsa)!=0 ){ printf("WSAStartup failed\n"); return 1; }

  /* The control value, computed with no socket in sight. */
  db = procWireTestDb();
  if( db==0 ){ printf("fixture failed\n"); return 1; }
  if( sqlite3_prepare_v2(db, "CALL fetch_all();", -1, &pCall, 0)!=SQLITE_OK ){
    printf("prepare failed\n"); return 1;
  }
  check(procWireEncode(pCall, 1, &ref)==SQLITE_OK, "reference payload encodes");
  check(procWireDecode(ref.a, ref.n, &stRef)==SQLITE_OK, "reference decodes");
  sqlite3_finalize(pCall);
  sqlite3_close(db);

  /* ---- 1. the happy path, over a real socket --------------------------- */
  if( startServer(&srv, SRV_NORMAL)==0 ){
    check(1, "server bound an ephemeral loopback port and signalled ready");
    printf("  listening on 127.0.0.1:%d\n", srv.port);
    check(callOverSocket(srv.port, "fetch_all", 0, 0, &st)==SQLITE_OK,
          "CALL over TCP returns a well-formed response");
    check(srv.bAccepted, "server accepted a connection (liveness)");
    check(st.nSet==2 && st.nRow==6 && st.nVal==24,
          "response carries the expected shape (2 sets, 6 rows, 24 values)");
    check(st.checksum==stRef.checksum,
          "POSITIVE CONTROL: bytes off the wire match the no-network checksum");
    stopServer(&srv);
  }else{
    check(0, "server failed to start");
  }

  /* ---- 2. can-fail: connecting to a closed port ------------------------ */
  /* Must fail promptly, not hang.  A test that hangs here is worse than one
  ** that fails, because a hang reads as "still working". */
  {
    ServerCtx tmp;
    int deadPort;
    DWORD t0, elapsed;
    if( startServer(&tmp, SRV_NORMAL)==0 ){
      deadPort = tmp.port;
      stopServer(&tmp);                     /* close it before dialling */
      t0 = GetTickCount();
      check(callOverSocket(deadPort, "fetch_all", 0, 0, &st)!=SQLITE_OK,
            "a closed port is an error, not a response");
      elapsed = GetTickCount() - t0;
      check(elapsed < IO_TIMEOUT_MS,
            "and it fails promptly rather than hanging");
    }else{
      check(0, "could not allocate a port to close");
    }
  }

  /* ---- 3. can-fail: a truncated response is never a short message ------- */
  if( startServer(&srv, SRV_SHORT)==0 ){
    check(callOverSocket(srv.port, "fetch_all", 0, 0, &st)!=SQLITE_OK,
          "a response one byte short is reported incomplete, not accepted");
    stopServer(&srv);
  }else{
    check(0, "server failed to start (short mode)");
  }

  /* ---- 4. garbage in: rejected without executing anything --------------- */
  if( startServer(&srv, SRV_NORMAL)==0 ){
    SOCKET s = dialLoopback(srv.port);
    unsigned char junk[64];
    int i;
    unsigned seed = 12345;
    for(i=0; i<(int)sizeof(junk); i++) junk[i] = (unsigned char)rnd(&seed);
    junk[0] = 0x40;                          /* ~1 GiB length: over the cap */
    check(s!=INVALID_SOCKET, "connected in order to send garbage");
    if( s!=INVALID_SOCKET ){
      OneMsg rsp;
      sendAll(s, junk, (int)sizeof(junk));
      check(recvOneMessage(s, &rsp)!=SQLITE_OK,
            "garbage yields no response, and no crash");
      free(rsp.a);
      closesocket(s);
    }
    Sleep(50);
    check(srv.bBadRequest, "server explicitly flagged the request as bad");
    stopServer(&srv);
  }else{
    check(0, "server failed to start (garbage mode)");
  }

  /* ---- 5. NEGATIVE CONTROL: the wire path can tell a changed payload ---- */
  /* Every check above compares against stRef.checksum.  If that comparison
  ** could not fail, all of them are decorative.  So run a server that flips
  ** exactly one byte of the encoded body and require the client to notice --
  ** either by refusing to decode it, or by arriving at a different checksum.
  ** It must never decode cleanly AND match. */
  check(stRef.checksum != 0, "the control checksum is not a trivial zero");
  if( startServer(&srv, SRV_ALTERED)==0 ){
    int rcAlt = callOverSocket(srv.port, "fetch_all", 0, 0, &st);
    check(rcAlt!=SQLITE_OK || st.checksum!=stRef.checksum,
          "a single altered byte NEVER decodes to the control checksum");
    printf("  altered-payload probe: %s\n",
           rcAlt!=SQLITE_OK ? "rejected by the decoder"
                            : "decoded, but checksum differs");
    stopServer(&srv);
  }else{
    check(0, "server failed to start (altered mode)");
  }

  /* ---- 6. PHASE 3: arguments survive the wire ------------------------- */
  /* The control here is IDENTITY, which needs no fixed expected value: send
  ** every value class that breaks naive codecs into echo6(), and require what
  ** comes back to fold to the same checksum as what went out.  Both sides are
  ** folded by the SAME function, so the two cannot disagree about the rules
  ** while agreeing about the data. */
  {
    WireBuf args, expect;
    DecodeStats stExp;
    static const unsigned char aBlob[3] = { 0x00, 0xff, 0x00 };
    int i;

    memset(&args, 0, sizeof(args));
    memset(&expect, 0, sizeof(expect));

    wbU8(&args, WV_NULL);                                        /* 1 NULL  */
    wbU8(&args, WV_INT);   wbI64(&args, -9223372036854775807LL);  /* 2 int   */
    wbU8(&args, WV_FLOAT); wbDouble(&args, 3.141592653589793);    /* 3 real  */
    wbU8(&args, WV_TEXT);  wbBytes(&args, "", 0);                 /* 4 empty */
    wbU8(&args, WV_TEXT);
    wbBytes(&args, "na\xc3\xafve caf\xc3\xa9", 12);               /* 5 utf-8 */
    wbU8(&args, WV_BLOB);  wbBytes(&args, aBlob, 3);              /* 6 NUL   */

    /* the same six values, shaped as a one-row response, folded by the same
    ** decoder the client will use on the real answer */
    wbU8(&expect, WF_SHAPE);
    wbU32(&expect, 6);
    for(i=0; i<6; i++){ wbStr(&expect, "c"); wbStr(&expect, ""); }
    wbU8(&expect, WF_ROW);
    if( wbNeed(&expect, args.n)==0 ){
      memcpy(expect.a + expect.n, args.a, args.n);
      expect.n += args.n;
    }
    wbU8(&expect, WF_SETEND);
    wbU8(&expect, WF_DONE);
    check(procWireDecode(expect.a, expect.n, &stExp)==SQLITE_OK,
          "the expected-argument row decodes");
    check(stExp.nVal==6, "expected magnitude: six argument values");

    if( startServer(&srv, SRV_NORMAL)==0 ){
      check(callOverSocket(srv.port, "echo6", &args, 6, &st)==SQLITE_OK,
            "CALL echo6(6 args) over TCP returns a well-formed response");
      check(st.nRow==1 && st.nVal==6, "one row of six values comes back");
      check(st.checksum==stExp.checksum,
            "IDENTITY: every argument survives the wire bit-exactly");
      stopServer(&srv);
    }else{
      check(0, "server failed to start (echo)");
    }

    /* can-fail: corrupt one argument byte server-side before binding */
    if( startServer(&srv, SRV_ALTERARG)==0 ){
      int rcArg = callOverSocket(srv.port, "echo6", &args, 6, &st);
      check(rcArg!=SQLITE_OK || st.checksum!=stExp.checksum,
            "a single altered ARGUMENT byte never echoes back as identity");
      printf("  altered-argument probe: %s\n",
             rcArg!=SQLITE_OK ? "request rejected" : "echoed, checksum differs");
      stopServer(&srv);
    }else{
      check(0, "server failed to start (altered-arg)");
    }

    wbFree(&args);
    wbFree(&expect);
  }

  wbFree(&ref);
  WSACleanup();

  printf("\n%s: %d checks, %d failures\n",
         nFail ? "PROC_SERVER FAILED" : "PROC_SERVER OK", nCheck, nFail);
  return nFail ? 1 : 0;
}
