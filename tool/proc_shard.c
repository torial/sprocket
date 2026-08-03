/*
** proc_shard.c -- Phase 6 of PLAN-TRANSPORT.md: route by key to one of N
** database files, one writer queue per shard.
**
** THE ARGUMENT
**
** DESIGN-NETWORK.md's reframe: SQLite's single writer is not a scaling limit
** so much as a total-order oracle other databases pay consensus protocols to
** obtain. The engineering task is therefore not to defeat the single writer
** but to SHRINK THE DOMAIN EACH WRITER GOVERNS until one writer per domain is
** abundant. Sharding is that, made concrete: one file per tenant, one queue
** per file, and writer contention disappears because the domains do not
** overlap.
**
** Everything expensive about a hot tenant then becomes a file operation --
** backup, restore, erase, or migrate to its own machine by moving a file.
**
** WHAT IS AND IS NOT TESTED DETERMINISTICALLY
**
** Routing is a pure function of the key, so it can be asserted exactly:
** stability (a key always lands in the same place), coverage (no shard left
** empty), and isolation (a key's rows exist in its shard and nowhere else).
**
** "Writes to distinct shards proceed concurrently" is deliberately NOT
** asserted by timing. Phase 5 taught the sharper form of the rule the hard
** way: a count is no better than a stopwatch if the count is itself
** timing-dependent. What is asserted instead is the property that makes
** concurrency possible and is exactly checkable -- each shard commits its own
** writes in its own transactions, so no shard's commit count is affected by
** another's traffic.
**
** Build (from repo root, VS dev prompt):
**   cl /nologo /O2 /I. tool\proc_shard.c sqlite3.c /Fe:proc_shard.exe
*/
#define PROC_QUEUE_NO_MAIN 1
#include "proc_queue.c"          /* WriteQueue: one writer + group commit */

#define MAXSHARD 16

/* ---------------------------------------------------------------- routing -- */
/*
** FNV-1a. Chosen because it is deterministic, endian-stable for byte strings,
** and short enough to read -- the routing function is the one piece of a
** sharded system that must never surprise anyone, since it decides where data
** *is*. Changing it is a migration, not a tuning knob.
*/
static unsigned shardHash(const char *zKey){
  unsigned h = 2166136261u;
  while( *zKey ){
    h ^= (unsigned char)*zKey++;
    h *= 16777619u;
  }
  return h;
}

/* A deliberately broken router, used to prove the coverage check can fail. */
static unsigned shardHashConstant(const char *zKey){
  (void)zKey;
  return 0;
}

typedef unsigned (*HashFn)(const char*);

typedef struct ShardSet {
  int nShard;
  HashFn xHash;
  WriteQueue *aQ[MAXSHARD];
  char *azFile[MAXSHARD];
} ShardSet;

static int shardOf(ShardSet *p, const char *zKey){
  return (int)(p->xHash(zKey) % (unsigned)p->nShard);
}

static ShardSet *shardOpen(const char *zPrefix, int nShard, int nBatch,
                           HashFn xHash){
  ShardSet *p;
  int i;
  if( nShard<1 || nShard>MAXSHARD ) return 0;
  p = (ShardSet*)sqlite3_malloc(sizeof(*p));
  if( p==0 ) return 0;
  memset(p, 0, sizeof(*p));
  p->nShard = nShard;
  p->xHash = xHash;
  for(i=0; i<nShard; i++){
    p->azFile[i] = sqlite3_mprintf("%s_%d.db", zPrefix, i);
    DeleteFileA(p->azFile[i]);
    p->aQ[i] = wqOpen(p->azFile[i], nBatch);
    if( p->aQ[i]==0 ) return 0;
    sqlite3_exec(p->aQ[i]->db, "CREATE TABLE kv(k TEXT, v INT);", 0, 0, 0);
    InterlockedExchange(&p->aQ[i]->nCommits, 0);
  }
  return p;
}

static void shardClose(ShardSet *p, int bDelete){
  int i;
  if( p==0 ) return;
  for(i=0; i<p->nShard; i++){
    if( p->aQ[i] ) wqClose(p->aQ[i]);
    if( p->azFile[i] ){
      if( bDelete ) DeleteFileA(p->azFile[i]);
      sqlite3_free(p->azFile[i]);
    }
  }
  sqlite3_free(p);
}

/* Write goes to the shard the key names -- nowhere else. */
static int shardPut(ShardSet *p, const char *zKey, int v){
  int i = shardOf(p, zKey);
  char *z = sqlite3_mprintf("INSERT INTO kv(k,v) VALUES(%Q,%d);", zKey, v);
  int rc;
  if( z==0 ) return SQLITE_NOMEM;
  rc = wqSubmit(p->aQ[i], z);
  sqlite3_free(z);
  return rc;
}

/* Count rows for a key in ONE shard file, opened independently. */
static int shardCountIn(ShardSet *p, int iShard, const char *zKey){
  sqlite3 *db = 0;
  sqlite3_stmt *pSt = 0;
  int n = -1;
  if( sqlite3_open(p->azFile[iShard], &db)!=SQLITE_OK ) return -1;
  if( sqlite3_prepare_v2(db, "SELECT count(*) FROM kv WHERE k=?1", -1, &pSt, 0)
      ==SQLITE_OK ){
    sqlite3_bind_text(pSt, 1, zKey, -1, SQLITE_TRANSIENT);
    if( sqlite3_step(pSt)==SQLITE_ROW ) n = sqlite3_column_int(pSt, 0);
  }
  sqlite3_finalize(pSt);
  sqlite3_close(db);
  return n;
}

static int shardRowsIn(ShardSet *p, int iShard){
  sqlite3 *db = 0;
  sqlite3_stmt *pSt = 0;
  int n = -1;
  if( sqlite3_open(p->azFile[iShard], &db)!=SQLITE_OK ) return -1;
  if( sqlite3_prepare_v2(db, "SELECT count(*) FROM kv", -1, &pSt, 0)==SQLITE_OK
      && sqlite3_step(pSt)==SQLITE_ROW ){
    n = sqlite3_column_int(pSt, 0);
  }
  sqlite3_finalize(pSt);
  sqlite3_close(db);
  return n;
}

/*
** Coverage: does every shard receive at least one of these keys?
** Returned rather than asserted, so the test can run it against a deliberately
** broken router and require the answer to be "no".
*/
static int coversAllShards(ShardSet *p, int nKey){
  int aSeen[MAXSHARD];
  int i, nEmpty = 0;
  memset(aSeen, 0, sizeof(aSeen));
  for(i=0; i<nKey; i++){
    char *z = sqlite3_mprintf("tenant-%d", i);
    aSeen[shardOf(p, z)]++;
    sqlite3_free(z);
  }
  for(i=0; i<p->nShard; i++) if( aSeen[i]==0 ) nEmpty++;
  return nEmpty==0;
}

/* ============================== self-test ================================= */
#define NSHARD 8
#define NKEY   200

int main(int argc, char **argv){
  ShardSet *p;
  int i, k;

  (void)argc; (void)argv;
  printf("proc_shard self-test -- SQLite %s\n\n", sqlite3_libversion());

  /* ---- 1. routing is a pure, stable function of the key ----------------- */
  p = shardOpen("sh", NSHARD, 32, shardHash);
  check(p!=0, "shard set opened");
  if( p==0 ) return 1;

  {
    int bStable = 1;
    for(i=0; i<NKEY; i++){
      char *z = sqlite3_mprintf("tenant-%d", i);
      int a = shardOf(p, z), b = shardOf(p, z);
      if( a!=b || a<0 || a>=NSHARD ) bStable = 0;
      sqlite3_free(z);
    }
    check(bStable, "a key always routes to the same shard, in range");
  }

  /* ---- 2. coverage, with the can-fail control alongside ----------------- */
  check(coversAllShards(p, NKEY), "every shard receives keys (no empty shard)");
  {
    /* THE CONTROL: the same check, against a router that collapses everything
    ** onto shard 0, must report failure.  Without this, "no empty shard" might
    ** be true because the checker cannot detect an empty shard. */
    ShardSet bad;
    memset(&bad, 0, sizeof(bad));
    bad.nShard = NSHARD;
    bad.xHash = shardHashConstant;
    check(!coversAllShards(&bad, NKEY),
          "CONTROL: a constant router is DETECTED as leaving shards empty");
  }

  /* ---- 3. isolation: a key's rows live in its shard and nowhere else ----- */
  for(i=0; i<NKEY; i++){
    char *z = sqlite3_mprintf("tenant-%d", i);
    shardPut(p, z, i);
    sqlite3_free(z);
  }
  {
    int bIsolated = 1, nTotal = 0;
    for(i=0; i<NKEY; i++){
      char *z = sqlite3_mprintf("tenant-%d", i);
      int home = shardOf(p, z);
      for(k=0; k<NSHARD; k++){
        int n = shardCountIn(p, k, z);
        if( k==home ){ if( n!=1 ) bIsolated = 0; }
        else          { if( n!=0 ) bIsolated = 0; }
      }
      sqlite3_free(z);
    }
    for(k=0; k<NSHARD; k++) nTotal += shardRowsIn(p, k);
    check(bIsolated, "each key's row is in its own shard and in NO other");
    check(nTotal==NKEY, "every write is durable, counted across all shards");
    printf("  %d keys over %d shards; rows per shard:", NKEY, NSHARD);
    for(k=0; k<NSHARD; k++) printf(" %d", shardRowsIn(p, k));
    printf("\n");
  }

  /* ---- 4. shards commit independently ----------------------------------- */
  /* Not a timing claim.  Each shard has its own writer and its own
  ** transactions, so a shard that received no traffic must show no commits --
  ** which is what makes cross-shard concurrency possible in the first place. */
  {
    ShardSet *q = shardOpen("iso", 4, 32, shardHash);
    const char *zHot = "only-this-one";
    int home, nOther = 0;
    check(q!=0, "second shard set opened");
    if( q ){
      home = shardOf(q, zHot);
      for(i=0; i<50; i++) shardPut(q, zHot, i);
      for(k=0; k<4; k++) if( k!=home ) nOther += (int)q->aQ[k]->nCommits;
      printf("  traffic to one key: shard %d committed %d, others %d\n",
             home, (int)q->aQ[home]->nCommits, nOther);
      check(q->aQ[home]->nCommits > 0, "the addressed shard committed");
      check(nOther==0, "shards with no traffic commit nothing -- fully independent");
      shardClose(q, 1);
    }
  }

  shardClose(p, 1);
  printf("\n%s: %d checks, %d failures\n",
         nFail ? "PROC_SHARD FAILED" : "PROC_SHARD OK", nCheck, nFail);
  return nFail ? 1 : 0;
}
