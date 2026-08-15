/*
** 2026-08-15
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** Fork (PLAN-TEMPORAL, DOCKET #6): system-versioned tables.
**
** A TEMPORAL table is an ordinary table whose writes are captured into
** a shadow history table (sqlite_hist_<name>) under a per-COMMIT
** sequence.  The design record is DESIGN-TEMPORAL.md (eight rulings,
** Sean, 2026-08-15) and the P1 architecture note in PLAN-TEMPORAL.md:
**
**   - db->pendingHistSeq holds the transaction's reserved sequence;
**     it is 0 outside a transaction and is cleared C-side at commit
**     and rollback (sqlite3TemporalTxnEnd) -- no SQL runs at commit.
**   - Capture is synthesized internal triggers (the mview machinery).
**     Each write closes the prior open interval, deletes any same-
**     transaction pending row for the key, and inserts the new open
**     image: last-write-wins is incremental, so intra-transaction
**     intermediate states never survive (POC 1 disease 1).
**   - Sequence order IS commit order: reservation happens under the
**     write lock and the single writer serializes both (disease 2's
**     seq-axis cure).  The commit-log UTC is stamped at the
**     transaction's first temporal write -- the seq axis is the truth,
**     the text axis a chart (DESIGN-TEMPORAL Q1).
**
** The two SQL functions registered here touch ONLY the C-side pending
** value; every read and write of storage lives in the synthesized
** trigger text where it is visible to EXPLAIN and to the tests:
**
**   sqlite_temporal_seq()       -> pending seq, or NULL if unreserved
**   sqlite_temporal_reserve(n)  -> sets pending to n, returns n
**
** v1 scope: temporal tables live in 'main' (PLAN-TEMPORAL records the
** attached-database generalization as future work).
*/
#include "sqliteInt.h"

#ifndef SQLITE_OMIT_TEMPORAL

/*
** Called from sqlite3EndTable when a CREATE TEMPORAL TABLE has just
** written its schema row (codegen path, never during schema load).
** Creates, via nested parse (the sqlite_sequence precedent, which is
** what lets the sqlite_ names through):
**   - the shadow: sqlite_hist_<name>(<cols>, seq_from, seq_to)
**   - the db-wide commit log and watermark meta, on first need
*/
void sqlite3TemporalEndTable(Parse *pParse, Table *pTab){
  sqlite3 *db = pParse->db;
  int iDb = sqlite3SchemaToIndex(db, pTab->pSchema);
  const char *zDb = db->aDb[iDb].zDbSName;
  sqlite3_str *pCols;
  char *zCols;
  int i;

  pCols = sqlite3_str_new(db);
  for(i=0; i<pTab->nCol; i++){
    Column *pCol = &pTab->aCol[i];
    if( i ) sqlite3_str_appendall(pCols, ", ");
    sqlite3_str_appendf(pCols, "\"%w\"", pCol->zCnName);
  }
  zCols = sqlite3_str_finish(pCols);
  if( zCols==0 ){
    sqlite3OomFault(db);
    return;
  }
  sqlite3NestedParse(pParse,
    "CREATE TABLE %Q.sqlite_hist_%s(%s, seq_from INTEGER, seq_to INTEGER)",
    zDb, pTab->zName, zCols);
  sqlite3_free(zCols);
  sqlite3NestedParse(pParse,
    "CREATE TABLE IF NOT EXISTS %Q.sqlite_hist_commits"
    "(seq INTEGER PRIMARY KEY, utc TEXT)", zDb);
  sqlite3NestedParse(pParse,
    "CREATE TABLE IF NOT EXISTS %Q.sqlite_hist_meta"
    "(tbl TEXT PRIMARY KEY, watermark INTEGER) WITHOUT ROWID", zDb);
  /* No eager meta row: a nested INSERT would compile before the nested
  ** CREATE above ever runs (compile-time resolution, runtime creation
  ** -- it failed).  An ABSENT row reads as watermark 0, which is
  ** honest: nothing has been pruned. */
}

/*
** The key predicate for one row of pTab, as SQL text comparing the
** history row's key columns to REF (either "NEW" or "OLD").  Rowid
** tables key on rowid (captured into the shadow's seq-free image via
** the declared INTEGER PRIMARY KEY when one exists); WITHOUT ROWID
** tables key on their primary-key columns.
*/
static void temporalKeyPred(sqlite3_str *p, Table *pTab, const char *zRef){
  if( HasRowid(pTab) && pTab->iPKey>=0 ){
    sqlite3_str_appendf(p, "\"%w\" = %s.\"%w\"",
        pTab->aCol[pTab->iPKey].zCnName, zRef,
        pTab->aCol[pTab->iPKey].zCnName);
  }else if( HasRowid(pTab) ){
    sqlite3_str_appendf(p, "rowid = %s.rowid", zRef);
  }else{
    Index *pPk = sqlite3PrimaryKeyIndex(pTab);
    int i;
    for(i=0; i<pPk->nKeyCol; i++){
      const char *zCol = pTab->aCol[pPk->aiColumn[i]].zCnName;
      if( i ) sqlite3_str_appendall(p, " AND ");
      sqlite3_str_appendf(p, "\"%w\" = %s.\"%w\"", zCol, zRef, zCol);
    }
  }
}

/*
** Build the SQL text of capture trigger k (0=INSERT, 1=UPDATE,
** 2=DELETE) for pTab.  The shape shared by all three:
**
**   1. ensure the transaction has a reserved seq: insert the commit-
**      log row (seq = max+1, utc = now) if none reserved, then latch
**      the value into the C side via sqlite_temporal_reserve().
**   2. close the prior open interval for this key (seq_to = pending)
**      -- only intervals from EARLIER transactions (seq_from < pending).
**   3. delete any pending open row for this key (same-txn last-write-
**      wins; for DELETE this erases a row inserted this txn, which is
**      correct: it was never committed-visible).
**   4. INSERT/UPDATE only: insert the new open image.
*/
static char *temporalBuildTriggerSql(
  sqlite3 *db,
  Table *pTab,
  const char *zDb,
  int k                 /* 0=INSERT, 1=UPDATE, 2=DELETE */
){
  static const char *azOp[] = { "INSERT", "UPDATE", "DELETE" };
  const char *zRef = (k==2) ? "OLD" : "NEW";
  sqlite3_str *p = sqlite3_str_new(db);
  int i;

  sqlite3_str_appendf(p,
    "CREATE TRIGGER \"%w\".\"sqlite_hist_%c_%w\" AFTER %s ON \"%w\" BEGIN\n",
    zDb, "iud"[k], pTab->zName, azOp[k], pTab->zName);

  /* 1: reserve on first temporal write of the transaction.  Trigger
  ** bodies may not qualify table names (parser rule); a body resolves
  ** in its trigger's own database, which is where the companions live. */
  sqlite3_str_appendall(p,
    "INSERT INTO sqlite_hist_commits(seq, utc)"
    " SELECT (SELECT coalesce(max(seq),0)+1 FROM sqlite_hist_commits),"
    " strftime('%Y-%m-%d %H:%M:%f','now')"
    " WHERE sqlite_temporal_seq() IS NULL;\n");
  sqlite3_str_appendall(p,
    "SELECT sqlite_temporal_reserve("
    "(SELECT max(seq) FROM sqlite_hist_commits))"
    " WHERE sqlite_temporal_seq() IS NULL;\n");

  /* 2: close the prior open interval */
  sqlite3_str_appendf(p,
    "UPDATE \"sqlite_hist_%w\" SET seq_to = sqlite_temporal_seq()"
    " WHERE ", pTab->zName);
  temporalKeyPred(p, pTab, zRef);
  sqlite3_str_appendall(p,
    " AND seq_to IS NULL AND seq_from < sqlite_temporal_seq();\n");

  /* 3: same-transaction last-write-wins */
  sqlite3_str_appendf(p,
    "DELETE FROM \"sqlite_hist_%w\" WHERE ", pTab->zName);
  temporalKeyPred(p, pTab, zRef);
  sqlite3_str_appendall(p,
    " AND seq_from = sqlite_temporal_seq() AND seq_to IS NULL;\n");

  /* 4: the new open image */
  if( k!=2 ){
    sqlite3_str_appendf(p,
      "INSERT INTO \"sqlite_hist_%w\"(", pTab->zName);
    for(i=0; i<pTab->nCol; i++){
      sqlite3_str_appendf(p, "\"%w\", ", pTab->aCol[i].zCnName);
    }
    sqlite3_str_appendall(p, "seq_from, seq_to) VALUES(");
    for(i=0; i<pTab->nCol; i++){
      sqlite3_str_appendf(p, "NEW.\"%w\", ", pTab->aCol[i].zCnName);
    }
    sqlite3_str_appendall(p, "sqlite_temporal_seq(), NULL);\n");
  }
  sqlite3_str_appendall(p, "END");
  return sqlite3_str_finish(p);
}

/*
** Sub-parse one synthesized trigger.  The clone of mviewParseTrigger
** (mview.c) -- including the zombie-Vdbe finalization that file paid
** to learn -- reusing bMViewTrigSynth so the tokenize.c hand-off guard
** applies unchanged.
*/
static Trigger *temporalParseTrigger(Parse *pOuter, const char *zSql){
  sqlite3 *db = pOuter->db;
  Parse sParse;
  Trigger *pRet;
#ifndef SQLITE_OMIT_AUTHORIZATION
  sqlite3_xauth xAuth = db->xAuth;
  db->xAuth = 0;
#endif
  sqlite3ParseObjectInit(&sParse, db);
  sParse.bMViewTrigSynth = 1;
  sParse.nQueryLoop = 1;
  sqlite3RunParser(&sParse, zSql);
  pRet = sParse.pNewTrigger;
  sParse.pNewTrigger = 0;
  if( sParse.pVdbe ){
    sqlite3VdbeFinalize(sParse.pVdbe);
    sParse.pVdbe = 0;
  }
  if( pRet==0 && pOuter->nErr==0 ){
    sqlite3ErrorMsg(pOuter,
       "internal error: history trigger for a system-versioned table "
       "failed to compile: %s", sParse.zErrMsg ? sParse.zErrMsg : "?");
  }
  sqlite3DbFree(db, sParse.zErrMsg);
  sqlite3ParseObjectReset(&sParse);
#ifndef SQLITE_OMIT_AUTHORIZATION
  db->xAuth = xAuth;
#endif
  return pRet;
}

/*
** Lazily build and link pTab's three capture triggers.  Called from
** the same sqlite3TriggerList sites that synthesize mview maintenance
** (by then the schema is loaded).  Idempotent: the built set is
** registered in Schema.histHash keyed by table name, with the same
** lifecycle as mviewHash (callback.c).
*/
void sqlite3TemporalSynthTriggers(Parse *pParse, Table *pTab){
  sqlite3 *db = pParse->db;
  Trigger **apTrig;
  int iDb, k;
  if( (pTab->tabFlags & TF_Temporal)==0 ) return;
  if( db->init.busy || pParse->bMViewTrigSynth ) return;
  if( pParse->nErr ) return;
  if( db->mDbFlags & DBFLAG_Vacuum ) return;
  if( sqlite3HashFind(&pTab->pSchema->histHash, pTab->zName) ) return;
  iDb = sqlite3SchemaToIndex(db, pTab->pSchema);
  if( NEVER(iDb<0) ) return;

  apTrig = (Trigger**)sqlite3MallocZero(3*sizeof(Trigger*));
  if( apTrig==0 ){
    sqlite3OomFault(db);
    return;
  }
  for(k=0; k<3; k++){
    char *zSql = temporalBuildTriggerSql(db, pTab,
                                         db->aDb[iDb].zDbSName, k);
    if( zSql==0 ){
      sqlite3OomFault(db);
      sqlite3_free(apTrig);
      return;
    }
    apTrig[k] = temporalParseTrigger(pParse, zSql);
    sqlite3_free(zSql);
    if( apTrig[k]==0 ){
      sqlite3_free(apTrig);
      return;
    }
    apTrig[k]->bMViewMaint = 1;   /* internal: exempt from user-trigger
                                  ** bookkeeping, same as mview's */
  }
  for(k=0; k<3; k++){
    apTrig[k]->pNext = pTab->pTrigger;
    pTab->pTrigger = apTrig[k];
  }
  sqlite3HashInsert(&pTab->pSchema->histHash, pTab->zName, apTrig);
}

/*
** Free one histHash entry: three parsed triggers and their array.
** Called from callback.c's schema teardown beside the mview registry.
*/
void sqlite3TemporalTrigFree(sqlite3 *db, void *pEntry){
  Trigger **apTrig = (Trigger**)pEntry;
  int k;
  for(k=0; k<3; k++){
    if( apTrig[k] ) sqlite3DeleteTrigger(db, apTrig[k]);
  }
  sqlite3_free(apTrig);
}

/* ------------------------------------------------------ SQL functions -- */
static void temporalSeqFunc(
  sqlite3_context *ctx, int argc, sqlite3_value **argv
){
  sqlite3 *db = sqlite3_context_db_handle(ctx);
  UNUSED_PARAMETER2(argc, argv);
  if( db->pendingHistSeq>0 ){
    sqlite3_result_int64(ctx, db->pendingHistSeq);
  }
  /* else: NULL -- "unreserved" gets its own spelling */
}

static void temporalReserveFunc(
  sqlite3_context *ctx, int argc, sqlite3_value **argv
){
  sqlite3 *db = sqlite3_context_db_handle(ctx);
  UNUSED_PARAMETER(argc);
  if( sqlite3_value_type(argv[0])==SQLITE_INTEGER ){
    db->pendingHistSeq = sqlite3_value_int64(argv[0]);
    sqlite3_result_int64(ctx, db->pendingHistSeq);
  }
}

void sqlite3TemporalFunctions(sqlite3 *db){
  sqlite3_create_function(db, "sqlite_temporal_seq", 0,
      SQLITE_UTF8|SQLITE_DIRECTONLY, 0, temporalSeqFunc, 0, 0);
  sqlite3_create_function(db, "sqlite_temporal_reserve", 1,
      SQLITE_UTF8|SQLITE_DIRECTONLY, 0, temporalReserveFunc, 0, 0);
}

/*
** The transaction is over -- commit or rollback, either way the
** reservation is dead.  Called from the same places that reset the
** deferred-constraint counters.
*/
void sqlite3TemporalTxnEnd(sqlite3 *db){
  db->pendingHistSeq = 0;
}

#endif /* !defined(SQLITE_OMIT_TEMPORAL) */
