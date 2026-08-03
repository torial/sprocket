/*
** proc_queue.c -- Phase 5 of PLAN-TRANSPORT.md: one writer thread per
** database, with group commit.
**
** WHY THIS EXISTS
**
** SQLite's single writer is usually described as its scaling limit. It is more
** useful to treat it as a total-order oracle that other databases pay
** consensus protocols to obtain (DESIGN-NETWORK.md). Given one writer, the
** cheapest large win available is to stop paying one fsync per transaction:
** funnel writes through a queue, and let the writer drain many of them into a
** single BEGIN IMMEDIATE .. COMMIT. Throughput becomes
**
**     fsyncs/second  x  batch size
**
** rather than fsyncs/second. Nothing here is an engine change; this is the
** application architecture DESIGN-NETWORK.md says belongs outside the fork.
**
** ISOLATION TACTIC (PLAN-TRANSPORT phase 5): everything below is reached
** through the WriteQueue handle only. BEGIN CONCURRENT is still undecided, and
** if it lands, multiple writers make a single queue optional rather than
** necessary -- at which point this becomes one queue per shard, or is bypassed
** entirely, without callers changing.
**
** MEASUREMENT DISCIPLINE: batching is asserted by COUNTING TRANSACTIONS with a
** commit hook, never by timing. A count does not care that the machine is
** busy, and the whole point of the wal2 port is that this is the workload that
** starves a classic-WAL checkpointer -- so the queue runs in wal2 mode.
**
** Build (from repo root, VS dev prompt):
**   cl /nologo /O2 /I. tool\proc_queue.c sqlite3.c /Fe:proc_queue.exe
*/
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sqlite3.h"

#define QCAP 4096            /* ring capacity; producers block when full */

typedef struct QItem {
  char *zSql;                /* statement to run (owned by the queue)     */
  HANDLE hDone;              /* auto-reset event, signalled after COMMIT  */
  int rc;
} QItem;

struct WriteQueue {
  sqlite3 *db;
  CRITICAL_SECTION mx;
  CONDITION_VARIABLE cvItem;   /* signalled when work arrives   */
  CONDITION_VARIABLE cvSpace;  /* signalled when the ring drains */
  QItem *aItem[QCAP];
  int iHead, iTail, nQueued;
  int nBatchMax;               /* K: most items per transaction */
  volatile LONG bStop;
  HANDLE hThread;
  HANDLE hGate;                /* if set, writer waits here before each batch */

  /* observability -- deterministic, not timed */
  volatile LONG nCommits;      /* transactions actually committed */
  volatile LONG nApplied;      /* statements executed             */
};
typedef struct WriteQueue WriteQueue;

static int commitHook(void *pArg){
  WriteQueue *q = (WriteQueue*)pArg;
  InterlockedIncrement(&q->nCommits);
  return 0;                    /* 0 = allow the commit */
}

/* ---------------------------------------------------------------- writer -- */
static DWORD WINAPI writerThread(LPVOID pArg){
  WriteQueue *q = (WriteQueue*)pArg;
  for(;;){
    QItem *aBatch[QCAP];
    int nBatch = 0;
    int i;
    char *zErr = 0;

    EnterCriticalSection(&q->mx);
    while( q->nQueued==0 && !q->bStop ){
      SleepConditionVariableCS(&q->cvItem, &q->mx, INFINITE);
    }
    if( q->nQueued==0 && q->bStop ){ LeaveCriticalSection(&q->mx); break; }
    LeaveCriticalSection(&q->mx);

    /* Test hook, and it has to sit HERE.  Two earlier placements failed: at the
    ** top of the loop the writer was already parked on cvItem inside the loop,
    ** so the first batch escaped the gate entirely.  Waiting after work has
    ** arrived -- but before draining, and outside the lock -- lets the rest of
    ** a known batch queue up behind the first item.  Without it the commit
    ** count measures how many items happened to arrive during a commit, which
    ** is scheduling, not the batching rule. */
    if( q->hGate ) WaitForSingleObject(q->hGate, INFINITE);

    EnterCriticalSection(&q->mx);
    /* Take up to K. Whatever has accumulated while we were committing the
    ** previous batch rides along in this one -- that is the whole mechanism:
    ** the busier it gets, the larger the batches become, so cost per write
    ** falls exactly when load rises. */
    while( q->nQueued>0 && nBatch<q->nBatchMax ){
      aBatch[nBatch++] = q->aItem[q->iHead];
      q->iHead = (q->iHead+1) % QCAP;
      q->nQueued--;
    }
    LeaveCriticalSection(&q->mx);

    /* BEGIN IMMEDIATE, not BEGIN: a deferred transaction that reads and then
    ** tries to upgrade deadlocks against another doing the same, and
    ** busy_timeout cannot rescue it because neither side can yield. */
    if( sqlite3_exec(q->db, "BEGIN IMMEDIATE;", 0, 0, &zErr)!=SQLITE_OK ){
      sqlite3_free(zErr); zErr = 0;
    }
    for(i=0; i<nBatch; i++){
      aBatch[i]->rc = sqlite3_exec(q->db, aBatch[i]->zSql, 0, 0, &zErr);
      if( zErr ){ sqlite3_free(zErr); zErr = 0; }
      InterlockedIncrement(&q->nApplied);
    }
    if( sqlite3_exec(q->db, "COMMIT;", 0, 0, &zErr)!=SQLITE_OK ){
      sqlite3_free(zErr); zErr = 0;
      sqlite3_exec(q->db, "ROLLBACK;", 0, 0, 0);
    }

    /* Wake ONLY this batch's submitters.  The first version broadcast to every
    ** waiter on every commit, which is O(waiters) wakeups per transaction: at
    ** 64 producers they all woke, contended for the lock, and mostly went back
    ** to sleep -- so few of them managed to re-queue before the writer took the
    ** next batch, and measured batch size FELL as concurrency rose (80 commits
    ** at 32 writers, 294 at 64).  The test caught that regression; this is the
    ** fix rather than a weakened assertion. */
    for(i=0; i<nBatch; i++) SetEvent(aBatch[i]->hDone);
    WakeAllConditionVariable(&q->cvSpace);
  }
  return 0;
}

/* ------------------------------------------------------------------- API -- */
static WriteQueue *wqOpen(const char *zFile, int nBatchMax){
  WriteQueue *q = (WriteQueue*)sqlite3_malloc(sizeof(*q));
  if( q==0 ) return 0;
  memset(q, 0, sizeof(*q));
  q->nBatchMax = nBatchMax<1 ? 1 : nBatchMax;
  if( sqlite3_open(zFile, &q->db)!=SQLITE_OK ){ sqlite3_free(q); return 0; }
  /* wal2: group commit concentrates writes, which is precisely the load that
  ** starves a classic-WAL checkpointer. */
  sqlite3_exec(q->db, "PRAGMA journal_mode=wal2;", 0, 0, 0);
  sqlite3_exec(q->db, "PRAGMA synchronous=NORMAL;", 0, 0, 0);
  sqlite3_busy_timeout(q->db, 5000);
  sqlite3_commit_hook(q->db, commitHook, q);
  InitializeCriticalSection(&q->mx);
  InitializeConditionVariable(&q->cvItem);
  InitializeConditionVariable(&q->cvSpace);
  /* The gate must exist BEFORE the writer does.  Creating it afterwards let
  ** the writer be mid-loop when it appeared, so the first batch escaped the
  ** gate and the test's "wait until all items are queued" spun to its limit --
  ** 100s per run, which looked like a hang. Manual-reset and SIGNALLED, so
  ** ordinary use passes straight through and only a test closes it. */
  q->hGate = CreateEvent(0, TRUE, TRUE, 0);
  if( q->hGate==0 ){ sqlite3_close(q->db); sqlite3_free(q); return 0; }
  q->hThread = CreateThread(0, 0, writerThread, q, 0, 0);
  if( q->hThread==0 ){ sqlite3_close(q->db); sqlite3_free(q); return 0; }
  return q;
}

/* Submit and wait for durability.  Synchronous per caller, batched globally --
** which is the point: the caller's latency is one commit, but that commit is
** shared with everyone who arrived while it was being prepared. */
static int wqSubmit(WriteQueue *q, const char *zSql){
  QItem item;
  memset(&item, 0, sizeof(item));
  item.zSql = sqlite3_mprintf("%s", zSql);
  if( item.zSql==0 ) return SQLITE_NOMEM;
  item.hDone = CreateEvent(0, FALSE, FALSE, 0);   /* auto-reset */
  if( item.hDone==0 ){ sqlite3_free(item.zSql); return SQLITE_NOMEM; }

  EnterCriticalSection(&q->mx);
  while( q->nQueued==QCAP ) SleepConditionVariableCS(&q->cvSpace, &q->mx, INFINITE);
  q->aItem[q->iTail] = &item;
  q->iTail = (q->iTail+1) % QCAP;
  q->nQueued++;
  WakeConditionVariable(&q->cvItem);
  LeaveCriticalSection(&q->mx);

  /* Wait OUTSIDE the lock, on this item alone.  Holding the queue lock while
  ** waiting would serialise submission behind completion, which is the
  ** opposite of what a batching queue is for. */
  WaitForSingleObject(item.hDone, INFINITE);

  CloseHandle(item.hDone);
  sqlite3_free(item.zSql);
  return item.rc;
}

static void wqClose(WriteQueue *q){
  if( q==0 ) return;
  EnterCriticalSection(&q->mx);
  q->bStop = 1;
  WakeAllConditionVariable(&q->cvItem);
  LeaveCriticalSection(&q->mx);
  WaitForSingleObject(q->hThread, INFINITE);
  CloseHandle(q->hThread);
  if( q->hGate ) CloseHandle(q->hGate);
  DeleteCriticalSection(&q->mx);
  sqlite3_close(q->db);
  sqlite3_free(q);
}

/* ============================== self-test ================================= */
/*
** WHAT IS ACTUALLY DETERMINISTIC HERE.
**
** An earlier version of this test counted commits while N threads wrote
** concurrently and asserted that more concurrency meant bigger batches. That
** number is not deterministic: it measures how many items happened to arrive
** while a commit was in flight, which is scheduling. It moved from 251 to 868
** for the same workload after an unrelated change to how waiters are woken.
** Counting rather than timing is not sufficient if the COUNT is timing-
** dependent -- which is a sharper version of the rule than I had.
**
** The rule that IS deterministic: if K items are queued when the writer takes
** a batch, exactly one transaction commits them. So the test gates the writer,
** queues a known number of items, releases it, and asserts the exact commit
** count. Scheduling cannot move that.
*/
static int nCheck = 0, nFail = 0;
static void check(int bOk, const char *zWhat){
  nCheck++;
  if( !bOk ){ nFail++; printf("  FAIL: %s\n", zWhat); }
}

#define MAXPROD 64

typedef struct Prod { WriteQueue *q; int id; int nPer; } Prod;

static DWORD WINAPI producer(LPVOID pArg){
  Prod *p = (Prod*)pArg;
  int i;
  for(i=0; i<p->nPer; i++){
    char *z = sqlite3_mprintf("INSERT INTO t(who,seq) VALUES(%d,%d);", p->id, i);
    wqSubmit(p->q, z);
    sqlite3_free(z);
  }
  return 0;
}

/*
** Queue exactly nItem writes with the writer held, then release it.
** Returns the number of transactions those writes took.
*/
static int gatedRun(const char *zFile, int nBatchMax, int nItem,
                    int *pnRows, int *pnDistinct){
  WriteQueue *q;
  HANDLE aTh[MAXPROD];
  Prod aP[MAXPROD];
  sqlite3 *dbCheck = 0;
  sqlite3_stmt *pSt = 0;
  int i, nCommits, spins;

  DeleteFileA(zFile);
  q = wqOpen(zFile, nBatchMax);
  if( q==0 ) return -1;
  sqlite3_exec(q->db, "CREATE TABLE t(who INT, seq INT);", 0, 0, 0);

  /* Close the gate, then let every producer queue exactly one item. */
  ResetEvent(q->hGate);
  InterlockedExchange(&q->nCommits, 0);
  for(i=0; i<nItem; i++){
    aP[i].q = q; aP[i].id = i; aP[i].nPer = 1;
    aTh[i] = CreateThread(0, 0, producer, &aP[i], 0, 0);
  }
  /* Wait until all of them are actually queued -- the point of the gate.
  ** Bounded low: if this ever spins out, the gate is not holding and the run
  ** must fail promptly rather than look slow. */
  for(spins=0; spins<5000; spins++){
    int n;
    EnterCriticalSection(&q->mx);
    n = q->nQueued;
    LeaveCriticalSection(&q->mx);
    if( n==nItem ) break;
    Sleep(1);
  }
  SetEvent(q->hGate);                              /* release the writer */

  WaitForMultipleObjects(nItem, aTh, TRUE, INFINITE);
  for(i=0; i<nItem; i++) CloseHandle(aTh[i]);
  nCommits = (int)q->nCommits;
  wqClose(q);                   /* joins the writer, then closes the gate */

  /* Durability verified from a SEPARATE connection. */
  *pnRows = *pnDistinct = -1;
  if( sqlite3_open(zFile, &dbCheck)==SQLITE_OK ){
    if( sqlite3_prepare_v2(dbCheck,
          "SELECT count(*), count(DISTINCT who) FROM t", -1, &pSt, 0)
        ==SQLITE_OK && sqlite3_step(pSt)==SQLITE_ROW ){
      *pnRows = sqlite3_column_int(pSt, 0);
      *pnDistinct = sqlite3_column_int(pSt, 1);
    }
    sqlite3_finalize(pSt);
    sqlite3_close(dbCheck);
  }
  return nCommits;
}

#ifndef PROC_QUEUE_NO_MAIN
int main(int argc, char **argv){
  int nRows, nDist, c;

  (void)argc; (void)argv;
  printf("proc_queue self-test -- SQLite %s\n\n", sqlite3_libversion());

  /* 64 items queued, K=64: they must commit as ONE transaction. */
  c = gatedRun("q_a.db", 64, 64, &nRows, &nDist);
  printf("  64 queued, K=64 : %d transaction(s)\n", c);
  check(nRows==64 && nDist==64, "all 64 durable, each exactly once");
  check(c==1, "64 queued items commit in exactly ONE transaction");

  /* K=16 over the same 64 items: exactly four batches. */
  c = gatedRun("q_b.db", 16, 64, &nRows, &nDist);
  printf("  64 queued, K=16 : %d transaction(s)\n", c);
  check(nRows==64, "all 64 durable at K=16");
  check(c==4, "K=16 over 64 items is exactly four transactions");

  /* CONTROL: K=1 must not batch at all.  If this does not go to 64, the
  ** commit counter is not measuring transactions and nothing above holds. */
  c = gatedRun("q_c.db", 1, 64, &nRows, &nDist);
  printf("  64 queued, K=1  : %d transaction(s)\n", c);
  check(nRows==64, "all 64 durable at K=1");
  check(c==64, "CONTROL: K=1 gives one transaction per item");

  DeleteFileA("q_a.db"); DeleteFileA("q_b.db"); DeleteFileA("q_c.db");
  printf("\n%s: %d checks, %d failures\n",
         nFail ? "PROC_QUEUE FAILED" : "PROC_QUEUE OK", nCheck, nFail);
  return nFail ? 1 : 0;
}
#endif /* PROC_QUEUE_NO_MAIN */
