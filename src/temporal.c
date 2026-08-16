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
  sqlite3_str_appendall(pCols, ", seq_from INTEGER, seq_to INTEGER");
  /* The shadow's natural key: (base key, seq_from) -- one row per
  ** version of each base row.  Declared as a PRIMARY KEY because the
  ** session module ignores tables without one (PLAN-REPL: history must
  ** be REPLICABLE), and because AS OF resolution seeks by exactly this
  ** key.  Every temporal table HAS a key to mirror: the PK-less case
  ** is refused at CREATE (see sqlite3EndTable). */
  if( HasRowid(pTab) && pTab->iPKey>=0 ){
    sqlite3_str_appendf(pCols, ", PRIMARY KEY(\"%w\", seq_from)",
        pTab->aCol[pTab->iPKey].zCnName);
  }else{
    /* Declared PK, rowid or not -- CREATE refused the PK-less case */
    Index *pPk = sqlite3PrimaryKeyIndex(pTab);
    if( ALWAYS(pPk) ){
      sqlite3_str_appendall(pCols, ", PRIMARY KEY(");
      for(i=0; i<pPk->nKeyCol; i++){
        sqlite3_str_appendf(pCols, "\"%w\", ",
            pTab->aCol[pPk->aiColumn[i]].zCnName);
      }
      sqlite3_str_appendall(pCols, "seq_from)");
    }
  }
  zCols = sqlite3_str_finish(pCols);
  if( zCols==0 ){
    sqlite3OomFault(db);
    return;
  }
  sqlite3NestedParse(pParse,
    "CREATE TABLE %Q.sqlite_hist_%s(%s)",
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
  }else{
    /* A declared PRIMARY KEY -- WITHOUT ROWID, or a rowid table with a
    ** non-INTEGER PK.  (A rowid table keyed on nothing but rowid is
    ** REFUSED at CREATE: the shadow's rowids drift from the base's
    ** after the first UPDATE, and history would close the wrong
    ** versions.  Measured before it was refused.) */
    Index *pPk = sqlite3PrimaryKeyIndex(pTab);
    int i;
    assert( pPk!=0 );
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

static void temporalAsofFunc(sqlite3_context*, int, sqlite3_value**);
static void temporalPruneFunc(sqlite3_context*, int, sqlite3_value**);

void sqlite3TemporalFunctions(sqlite3 *db){
  sqlite3_create_function(db, "sqlite_temporal_seq", 0,
      SQLITE_UTF8|SQLITE_DIRECTONLY, 0, temporalSeqFunc, 0, 0);
  sqlite3_create_function(db, "sqlite_temporal_reserve", 1,
      SQLITE_UTF8|SQLITE_DIRECTONLY, 0, temporalReserveFunc, 0, 0);
  /* The AS OF resolver is NOT DIRECTONLY: a user view legitimately
  ** contains FOR SYSTEM_TIME, and the function is read-only with its
  ** own watermark integrity. */
  sqlite3_create_function(db, "sqlite_temporal_asof", 2,
      SQLITE_UTF8, 0, temporalAsofFunc, 0, 0);
  /* Prune WRITES, so it is DIRECTONLY -- never reachable from views,
  ** triggers, or tainted schema. */
  sqlite3_create_function(db, "sqlite_history_prune", 2,
      SQLITE_UTF8|SQLITE_DIRECTONLY, 0, temporalPruneFunc, 0, 0);
}

/*
** The transaction is over -- commit or rollback, either way the
** reservation is dead.  Called from the same places that reset the
** deferred-constraint counters.
*/
void sqlite3TemporalTxnEnd(sqlite3 *db){
  db->pendingHistSeq = 0;
}

/* ------------------------------- P4: FOR SYSTEM_TIME AS OF ------------- */

/* One-value integer query with one bound argument (text or value).
** The nested prepare/step on the same connection between the outer
** statement's steps is legal; the resolver runs once per query because
** the rewrite wraps it in an uncorrelated scalar subquery. */
static sqlite3_int64 temporalQueryI64(
  sqlite3 *db, const char *zSql, const char *zText, sqlite3_value *pVal
){
  sqlite3_stmt *pStmt = 0;
  sqlite3_int64 v = 0;
  if( sqlite3_prepare_v2(db, zSql, -1, &pStmt, 0)==SQLITE_OK && pStmt ){
    if( zText ) sqlite3_bind_text(pStmt, 1, zText, -1, SQLITE_TRANSIENT);
    if( pVal ) sqlite3_bind_value(pStmt, 1, pVal);
    if( sqlite3_step(pStmt)==SQLITE_ROW ){
      v = sqlite3_column_int64(pStmt, 0);
    }
  }
  sqlite3_finalize(pStmt);
  return v;
}

/*
** sqlite_temporal_asof(V, TBL): resolve V -- an integer commit seq, or
** a UTC text timestamp resolved through the commit log to the last
** commit at-or-before that instant -- and enforce TBL's prune
** watermark: an answer from pruned history would be fabricated, so it
** REFUSES (DESIGN-TEMPORAL Q6).
*/
static void temporalAsofFunc(
  sqlite3_context *ctx, int argc, sqlite3_value **argv
){
  sqlite3 *db = sqlite3_context_db_handle(ctx);
  const char *zTbl = (const char*)sqlite3_value_text(argv[1]);
  sqlite3_int64 s, wm;
  int vt = sqlite3_value_type(argv[0]);
  UNUSED_PARAMETER(argc);
  if( vt==SQLITE_INTEGER ){
    s = sqlite3_value_int64(argv[0]);
  }else if( vt==SQLITE_TEXT ){
    s = temporalQueryI64(db,
        "SELECT coalesce(max(seq),0) FROM main.sqlite_hist_commits"
        " WHERE utc <= ?1", 0, argv[0]);
  }else{
    sqlite3_result_error(ctx, "FOR SYSTEM_TIME AS OF requires an integer "
        "sequence or a UTC text timestamp", -1);
    return;
  }
  wm = temporalQueryI64(db,
      "SELECT coalesce(max(watermark),0) FROM main.sqlite_hist_meta"
      " WHERE tbl = ?1", zTbl, 0);
  if( s < wm ){
    char *zMsg = sqlite3_mprintf(
        "history of %s is pruned before seq %lld; the answer would be "
        "fabricated", zTbl ? zTbl : "?", wm);
    if( zMsg ){
      sqlite3_result_error(ctx, zMsg, -1);
      sqlite3_free(zMsg);
    }else{
      sqlite3_result_error_nomem(ctx);
    }
    return;
  }
  sqlite3_result_int64(ctx, s);
}

/*
** sqlite_history_prune(TBL, UPTO): raise TBL's watermark to UPTO and
** delete the closed intervals wholly before it.  Open intervals (the
** present) always survive.  Returns the number of history rows
** removed.  Explicit and owned by its caller (DESIGN-TEMPORAL Q6);
** AS OF earlier than the watermark refuses in the resolver.
*/
static void temporalPruneFunc(
  sqlite3_context *ctx, int argc, sqlite3_value **argv
){
  sqlite3 *db = sqlite3_context_db_handle(ctx);
  const char *zTbl = (const char*)sqlite3_value_text(argv[0]);
  sqlite3_int64 upto = sqlite3_value_int64(argv[1]);
  Table *pTab;
  char *zSql;
  int rc, nPruned = 0;
  UNUSED_PARAMETER(argc);
  pTab = zTbl ? sqlite3FindTable(db, zTbl, "main") : 0;
  if( pTab==0 || (pTab->tabFlags & TF_Temporal)==0 ){
    sqlite3_result_error(ctx,
        "history_prune: not a system-versioned table", -1);
    return;
  }
  db->mDbFlags |= DBFLAG_TemporalMaint;
  zSql = sqlite3_mprintf(
      "INSERT INTO main.sqlite_hist_meta(tbl, watermark) VALUES(%Q, %lld)"
      " ON CONFLICT(tbl) DO UPDATE SET watermark=max(watermark, %lld);"
      "DELETE FROM main.\"sqlite_hist_%w\""
      " WHERE seq_to IS NOT NULL AND seq_to <= %lld;",
      zTbl, upto, upto, zTbl, upto);
  if( zSql==0 ){
    db->mDbFlags &= ~DBFLAG_TemporalMaint;
    sqlite3_result_error_nomem(ctx);
    return;
  }
  rc = sqlite3_exec(db, zSql, 0, 0, 0);
  nPruned = sqlite3_changes(db);
  sqlite3_free(zSql);
  db->mDbFlags &= ~DBFLAG_TemporalMaint;
  if( rc!=SQLITE_OK ){
    sqlite3_result_error(ctx, sqlite3_errmsg(db), -1);
    return;
  }
  sqlite3_result_int(ctx, nPruned);
}

/* alias.col reference */
static Expr *temporalDotRef(Parse *pParse, const char *zAlias,
                            const char *zCol){
  sqlite3 *db = pParse->db;
  return sqlite3PExpr(pParse, TK_DOT,
      sqlite3Expr(db, TK_ID, zAlias),
      sqlite3Expr(db, TK_ID, zCol));
}

/*
** Consume pItem->pAsOf: swap the item onto the shadow history table
** (aliased to the user's spelling, interval columns hidden) and AND
** the interval-containment predicate into the SELECT's WHERE.  The
** resolver rides an uncorrelated scalar subquery so watermark checking
** and text resolution run once per query, not once per row.
*/
void sqlite3TemporalAsOfRewrite(Parse *pParse, Select *pSel, SrcItem *pItem){
  sqlite3 *db = pParse->db;
  Table *pTab = pItem->pSTab;
  Table *pHist;
  char *zHist;
  Expr *pAsOf = pItem->pAsOf;
  Expr *pFn, *pSub, *pLe, *pNull, *pGt, *pOr, *pCond;
  ExprList *pArgs;
  Select *pScalar;
  Token tkFn;
  int iDb;

  pItem->pAsOf = 0;                     /* consumed on every path */
  if( (pTab->tabFlags & TF_Temporal)==0 ){
    sqlite3ErrorMsg(pParse,
       "%s is not system-versioned; FOR SYSTEM_TIME does not apply",
       pTab->zName);
    sqlite3ExprDelete(db, pAsOf);
    return;
  }
  iDb = sqlite3SchemaToIndex(db, pTab->pSchema);
  zHist = sqlite3MPrintf(db, "sqlite_hist_%s", pTab->zName);
  pHist = zHist ? sqlite3FindTable(db, zHist, db->aDb[iDb].zDbSName) : 0;
  sqlite3DbFree(db, zHist);
  if( pHist==0 ){
    sqlite3ErrorMsg(pParse, "history of %s is missing", pTab->zName);
    sqlite3ExprDelete(db, pAsOf);
    return;
  }
  if( pItem->zAlias==0 ){
    pItem->zAlias = sqlite3DbStrDup(db, pTab->zName);
  }
  pTab->nTabRef--;
  pItem->pSTab = pHist;
  pHist->nTabRef++;

  /* (SELECT sqlite_temporal_asof(<expr>, '<table>')) */
  pArgs = sqlite3ExprListAppend(pParse, 0, pAsOf);
  pArgs = sqlite3ExprListAppend(pParse, pArgs,
      sqlite3Expr(db, TK_STRING, pTab->zName));
  tkFn.z = "sqlite_temporal_asof";
  tkFn.n = 20;
  pFn = sqlite3ExprFunction(pParse, pArgs, &tkFn, 0);
  pScalar = sqlite3SelectNew(pParse,
      sqlite3ExprListAppend(pParse, 0, pFn), 0,0,0,0,0, 0, 0);
  pSub = sqlite3PExpr(pParse, TK_SELECT, 0, 0);
  if( pSub && pScalar ){
    pSub->x.pSelect = pScalar;
    ExprSetProperty(pSub, EP_xIsSelect|EP_Subquery);
  }else{
    sqlite3SelectDelete(db, pScalar);
  }

  /* alias.seq_from <= S AND (alias.seq_to IS NULL OR alias.seq_to > S) */
  pLe = sqlite3PExpr(pParse, TK_LE,
      temporalDotRef(pParse, pItem->zAlias, "seq_from"), pSub);
  pNull = sqlite3PExpr(pParse, TK_ISNULL,
      temporalDotRef(pParse, pItem->zAlias, "seq_to"), 0);
  pGt = sqlite3PExpr(pParse, TK_GT,
      temporalDotRef(pParse, pItem->zAlias, "seq_to"),
      sqlite3ExprDup(db, pSub, 0));
  pOr = sqlite3PExpr(pParse, TK_OR, pNull, pGt);
  pCond = sqlite3PExpr(pParse, TK_AND, pLe, pOr);
  pSel->pWhere = sqlite3ExprAnd(pParse, pSel->pWhere, pCond);
}

#endif /* !defined(SQLITE_OMIT_TEMPORAL) */
