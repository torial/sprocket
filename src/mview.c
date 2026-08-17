/*
** 2026-08-12
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains the implementation of materialized views (fork
** feature; design record in DESIGN-IVM.md, campaign plan in PLAN-IVM.md).
**
** A materialized view is a REAL TABLE stored under the view's own name:
** its sqlite_schema row has type='mview', a rootpage, and sql holding the
** full CREATE MATERIALIZED VIEW statement.  Because it is a real table,
** ordinary reads, ordinary indexes, WAL crash recovery and size accounting
** all apply to it with no new machinery.  What distinguishes it:
**
**   - The definition SELECT is restricted to the Tier-1 incrementally
**     maintainable subset (COUNT/SUM/TOTAL/AVG, WHERE/HAVING, GROUP BY of
**     deterministic expressions, a single ordinary base table).  Anything
**     outside the subset is refused at CREATE, by name, with the reason.
**     The restriction exists so that later phases can maintain the table
**     incrementally; it is enforced from day one so that no definition
**     can be created that maintenance could not honor.
**
**   - User writes to the view are refused (see sqlite3IsReadOnly());
**     only the engine writes it.  Population happens at CREATE by running
**     the definition SELECT into the table -- the same coroutine codegen
**     that CREATE TABLE ... AS SELECT uses.
**
**   - Unlike CREATE TABLE AS (which synthesizes a plain column-list DDL
**     and forgets its SELECT), the stored DDL keeps the definition.  At
**     every schema load the column list is RE-DERIVED from the SELECT, so
**     the definition must keep resolving.  That is why ALTER or DROP on a
**     base table with a dependent materialized view is refused: a stored
**     definition that stopped resolving would fail the next schema load
**     of the whole database file.  The Schema.mviewHash registry (an
**     MViewInfo per view, keyed by view name) is what makes that refusal
**     cheap; it is rebuilt on every schema load and cleared with the
**     schema.  (Lifecycle mirrors Schema.procHash; see callback.c.)
**
** ISOLATION NOTE (why this is a separate file): everything mview-specific
** lives here; the hooks in upstream files (build.c, delete.c, callback.c,
** parse.y, shell.c.in) are one-to-few-line steers guarded by IsMView() or
** pParse->bMViewCreate, so upstream merges touch this feature minimally.
*/
#include "sqliteInt.h"

#ifndef SQLITE_OMIT_VIEW

/*
** Compose the delta-log table name for a DEFERRED materialized view.
** Caller frees with sqlite3DbFree.  The sqlite_ prefix keeps users from
** CREATE/DROP collisions the same way sqlite_sequence does.
*/
static char *mviewLogName(sqlite3 *db, const char *zView){
  return sqlite3MPrintf(db, "sqlite_ivm_%s_log", zView);
}

/*
** Grammar helper for the optional WITH MAINTENANCE clause.  The words
** MAINTENANCE, EAGER and DEFERRED are checked identifiers, not keywords.
** Returns one of the MVIEW_MAINT_* constants; on an unrecognized word the
** error names what was seen and what is accepted.
*/
int sqlite3MViewMaintOption(Parse *pParse, Token *pWord, Token *pValue){
  if( pWord->n!=11 || sqlite3_strnicmp(pWord->z, "maintenance", 11)!=0 ){
    sqlite3ErrorMsg(pParse,
       "unknown materialized view option: %T (only WITH MAINTENANCE "
       "EAGER or WITH MAINTENANCE DEFERRED is accepted)", pWord);
    return MVIEW_MAINT_UNSPEC;
  }
  if( pValue->n==5 && sqlite3_strnicmp(pValue->z, "eager", 5)==0 ){
    return MVIEW_MAINT_EAGER;
  }
  if( pValue->n==8 && sqlite3_strnicmp(pValue->z, "deferred", 8)==0 ){
    return MVIEW_MAINT_DEFERRED;
  }
  sqlite3ErrorMsg(pParse,
     "unknown MAINTENANCE mode: %T (accepted: EAGER, DEFERRED)", pValue);
  return MVIEW_MAINT_UNSPEC;
}

/*
** Render a function-call expression compactly for an error message:
** "date('now')", "random()", "strftime('%s',...)".  Literal arguments are
** shown; anything else renders as "..." -- the renderer never guesses at
** text it does not have.
*/
static void mviewRenderCall(StrAccum *pStr, const Expr *pExpr){
  int i;
  const ExprList *pArgs;
  assert( pExpr->op==TK_FUNCTION || pExpr->op==TK_AGG_FUNCTION );
  assert( !ExprHasProperty(pExpr, EP_IntValue) );
  sqlite3_str_appendf(pStr, "%s(", pExpr->u.zToken);
  pArgs = ExprUseXList(pExpr) ? pExpr->x.pList : 0;
  for(i=0; pArgs && i<pArgs->nExpr; i++){
    const Expr *pA = pArgs->a[i].pExpr;
    if( i>0 ) sqlite3_str_append(pStr, ",", 1);
    switch( pA->op ){
      case TK_STRING:
        assert( !ExprHasProperty(pA, EP_IntValue) );
        sqlite3_str_appendf(pStr, "'%s'", pA->u.zToken);
        break;
      case TK_INTEGER:
        if( ExprHasProperty(pA, EP_IntValue) ){
          sqlite3_str_appendf(pStr, "%d", pA->u.iValue);
        }else{
          sqlite3_str_appendf(pStr, "%s", pA->u.zToken);
        }
        break;
      case TK_FLOAT:
        assert( !ExprHasProperty(pA, EP_IntValue) );
        sqlite3_str_appendf(pStr, "%s", pA->u.zToken);
        break;
      case TK_NULL:
        sqlite3_str_append(pStr, "NULL", 4);
        break;
      default:
        sqlite3_str_append(pStr, "...", 3);
        break;
    }
  }
  sqlite3_str_append(pStr, ")", 1);
}

/*
** Append the hidden bookkeeping columns to a materialized view's Table:
** always "ivm$count" (the COUNT(*) group-liveness counter), plus per
** SUM column i an "ivm$n$i" (count of non-null arguments, which is what
** lets SUM return to NULL when they are all gone), plus per AVG column
** j an "ivm$s$j" and "ivm$n$j" (the visible average is s/n).  The
** columns carry COLFLAG_HIDDEN: star expansion, NATURAL/USING joins and
** table_info skip them; explicit reads and table_xinfo see them, which
** is the honest arrangement for engine bookkeeping.
**
** Runs at CREATE and at every schema load (both re-derive columns), so
** the stored rows always match the derived shape.
*/
static int mviewAppendHiddenCols(
  Parse *pParse,
  Table *p,                /* The view's table; nCol==nVis on entry */
  int nVis,                /* Number of visible result columns */
  const u8 *aColKind       /* Conformance-walk kinds, nVis entries */
){
  sqlite3 *db = pParse->db;
  int nHidden = 1;         /* ivm$count */
  int i, j;
  Column *aNew;
  for(i=0; i<nVis; i++){
    if( aColKind[i]==MVIEW_COL_SUM ) nHidden += 1;
    if( aColKind[i]==MVIEW_COL_AVG ) nHidden += 2;
  }
  aNew = sqlite3DbRealloc(db, p->aCol, (nVis+nHidden)*sizeof(Column));
  if( aNew==0 ) return SQLITE_NOMEM;
  p->aCol = aNew;
  memset(&p->aCol[nVis], 0, nHidden*sizeof(Column));
  j = nVis;
  for(i=0; i<=nVis; i++){
    char *zName = 0;
    char cAff = 0;
    if( i==nVis ){
      /* Emitted last so the loop shape stays simple; position does not
      ** matter as long as CREATE and reload agree, and they run the
      ** same code.  Placed after the per-column hiddens. */
      zName = sqlite3MPrintf(db, "ivm$count");
      cAff = SQLITE_AFF_INTEGER;
      /* fall through to the append below */
    }else if( aColKind[i]==MVIEW_COL_SUM ){
      zName = sqlite3MPrintf(db, "ivm$n$%d", i);
      cAff = SQLITE_AFF_INTEGER;
    }else if( aColKind[i]==MVIEW_COL_AVG ){
      zName = sqlite3MPrintf(db, "ivm$s$%d", i);
      if( zName==0 ) return SQLITE_NOMEM;
      p->aCol[j].zCnName = zName;
      p->aCol[j].hName = sqlite3StrIHash(zName);
      p->aCol[j].affinity = SQLITE_AFF_NUMERIC;
      p->aCol[j].colFlags = COLFLAG_HIDDEN;
      p->aCol[j].szEst = 1;
      j++;
      zName = sqlite3MPrintf(db, "ivm$n$%d", i);
      cAff = SQLITE_AFF_INTEGER;
    }else{
      continue;
    }
    if( zName==0 ) return SQLITE_NOMEM;
    p->aCol[j].zCnName = zName;
    p->aCol[j].hName = sqlite3StrIHash(zName);
    p->aCol[j].affinity = cAff;
    p->aCol[j].colFlags = COLFLAG_HIDDEN;
    p->aCol[j].szEst = 1;
    j++;
  }
  assert( j==nVis+nHidden );
  p->nCol = p->nNVCol = (i16)(nVis+nHidden);
  p->tabFlags |= TF_HasHidden;
  return SQLITE_OK;
}

/*
** Append to pSel's result list the aggregate expressions that compute
** the hidden bookkeeping columns, IN THE SAME ORDER as
** mviewAppendHiddenCols lays them out: per SUM column count(arg), per
** AVG column sum(arg) then count(arg), then count(*) last.  pSel must
** be an UNRESOLVED tree (a dup taken before sqlite3ResultSetOfSelect):
** appending to a resolved tree would leave the new aggregates
** unresolved forever, because a prepared Select is never prepped twice.
*/
static void mviewAugmentSelect(
  Parse *pParse,
  Select *pSel,            /* Pre-resolution dup of the definition */
  int nVis,
  const u8 *aColKind
){
  static const Token tkCount = { "count", 5 };
  static const Token tkSum   = { "sum", 3 };
  sqlite3 *db = pParse->db;
  int i;
  for(i=0; i<nVis; i++){
    Expr *pArg, *pAgg;
    ExprList *pArgs;
    if( aColKind[i]!=MVIEW_COL_SUM && aColKind[i]!=MVIEW_COL_AVG ){
      continue;
    }
    assert( pSel->pEList->a[i].pExpr->op==TK_FUNCTION );
    assert( ExprUseXList(pSel->pEList->a[i].pExpr) );
    if( aColKind[i]==MVIEW_COL_AVG ){
      pArg = sqlite3ExprDup(db,
                pSel->pEList->a[i].pExpr->x.pList->a[0].pExpr, 0);
      pArgs = sqlite3ExprListAppend(pParse, 0, pArg);
      pAgg = sqlite3ExprFunction(pParse, pArgs, &tkSum, 0);
      pSel->pEList = sqlite3ExprListAppend(pParse, pSel->pEList, pAgg);
    }
    pArg = sqlite3ExprDup(db,
              pSel->pEList->a[i].pExpr->x.pList->a[0].pExpr, 0);
    pArgs = sqlite3ExprListAppend(pParse, 0, pArg);
    pAgg = sqlite3ExprFunction(pParse, pArgs, &tkCount, 0);
    pSel->pEList = sqlite3ExprListAppend(pParse, pSel->pEList, pAgg);
  }
  pSel->pEList = sqlite3ExprListAppend(pParse, pSel->pEList,
                    sqlite3ExprFunction(pParse, 0, &tkCount, 0));
}

/*
** Context threaded through the conformance walk below.
*/
typedef struct MViewWalk MViewWalk;
struct MViewWalk {
  Parse *pParse;         /* Error reporting */
  const char *zName;     /* View name, for the message prefix */
  int rc;                /* SQLITE_OK or SQLITE_ERROR */
};

/*
** Issue the standard-form refusal:
**   <view> cannot be incrementally maintained: <reason>
** All conformance-walk refusals speak this shape so a reader always
** learns which view, what construct, and (in the reason) the fix.
*/
static void mviewRefuse(MViewWalk *p, const char *zFmt, ...){
  va_list ap;
  char *zReason;
  if( p->rc!=SQLITE_OK ) return;      /* first refusal wins */
  va_start(ap, zFmt);
  zReason = sqlite3VMPrintf(p->pParse->db, zFmt, ap);
  va_end(ap);
  sqlite3ErrorMsg(p->pParse, "%s cannot be incrementally maintained: %z",
                  p->zName, zReason);
  p->rc = SQLITE_ERROR;
}

/*
** Refuse a function call as non-deterministic, rendering the call text.
*/
static void mviewRefuseNonDet(MViewWalk *p, const Expr *pExpr){
  char zBuf[200];
  StrAccum str;
  sqlite3StrAccumInit(&str, 0, zBuf, sizeof(zBuf), 0);
  mviewRenderCall(&str, pExpr);
  sqlite3StrAccumFinish(&str);
  mviewRefuse(p, "%s is non-deterministic; a maintained view's definition "
                 "may not depend on when it is evaluated", zBuf);
}

/*
** sqlite3WalkExpr() callbacks enforcing determinism.  Every function
** encountered must be marked SQLITE_FUNC_CONSTANT.  Date/time functions
** (SQLITE_FUNC_SLOCHNG) are allowed only over literal arguments none of
** which invokes the current moment or the local timezone -- date('now')
** is time-dependent, date('2024-01-01') is not, and date(x) over a column
** could resolve 'now' at evaluation time, so only literals pass.
** Subqueries are refused outright: they reference rows the maintenance
** delta cannot see.
*/
static int mviewDetExprCb(Walker *pWalker, Expr *pExpr){
  MViewWalk *p = (MViewWalk*)pWalker->u.pMViewWalk;
  if( p->rc!=SQLITE_OK ) return WRC_Abort;
  if( pExpr->op==TK_FUNCTION ){
    FuncDef *pDef;
    int nArg = (ExprUseXList(pExpr) && pExpr->x.pList)
                  ? pExpr->x.pList->nExpr : 0;
    assert( !ExprHasProperty(pExpr, EP_IntValue) );
    pDef = sqlite3FindFunction(p->pParse->db, pExpr->u.zToken, nArg,
                               ENC(p->pParse->db), 0);
    if( pDef==0 ){
      /* Resolution already succeeded, so this is unreachable in practice;
      ** refuse rather than assume if it ever is reached. */
      mviewRefuse(p, "function %s() could not be analyzed",
                  pExpr->u.zToken);
      return WRC_Abort;
    }
    /* Date/time functions carry SQLITE_FUNC_SLOCHNG -- and the pure-date
    ** family carries SQLITE_FUNC_CONSTANT as well, because upstream
    ** defers 'now'-detection to runtime (OP_PureFunc).  A stored view
    ** definition has no runtime guard, so the arguments are analyzed
    ** here regardless of the CONSTANT bit. */
    if( (pDef->funcFlags & (SQLITE_FUNC_CONSTANT|SQLITE_FUNC_SLOCHNG))!=
           SQLITE_FUNC_CONSTANT ){
      if( (pDef->funcFlags & SQLITE_FUNC_SLOCHNG)!=0 ){
        int i;
        if( nArg==0 ){
          /* date() with no arguments means "now" */
          mviewRefuseNonDet(p, pExpr);
          return WRC_Abort;
        }
        for(i=0; i<nArg; i++){
          const Expr *pA = pExpr->x.pList->a[i].pExpr;
          if( pA->op==TK_STRING ){
            const char *z;
            assert( !ExprHasProperty(pA, EP_IntValue) );
            z = pA->u.zToken;
            if( sqlite3_stricmp(z, "now")==0 ){
              mviewRefuseNonDet(p, pExpr);
              return WRC_Abort;
            }
            if( sqlite3_stricmp(z, "localtime")==0
             || sqlite3_stricmp(z, "utc")==0
            ){
              char zBuf[200];
              StrAccum str;
              sqlite3StrAccumInit(&str, 0, zBuf, sizeof(zBuf), 0);
              mviewRenderCall(&str, pExpr);
              sqlite3StrAccumFinish(&str);
              mviewRefuse(p, "%s depends on the local timezone setting; "
                  "a maintained view's definition may not depend on the "
                  "environment it is evaluated in", zBuf);
              return WRC_Abort;
            }
          }else if( pA->op!=TK_INTEGER && pA->op!=TK_FLOAT
                 && pA->op!=TK_NULL ){
            char zBuf[200];
            StrAccum str;
            sqlite3StrAccumInit(&str, 0, zBuf, sizeof(zBuf), 0);
            mviewRenderCall(&str, pExpr);
            sqlite3StrAccumFinish(&str);
            mviewRefuse(p, "%s applies a date/time function to a "
                "non-literal argument, which could name 'now' at "
                "evaluation time; use literal arguments only", zBuf);
            return WRC_Abort;
          }
        }
      }else{
        mviewRefuseNonDet(p, pExpr);
        return WRC_Abort;
      }
    }
  }
  return WRC_Continue;
}
static int mviewDetSelectCb(Walker *pWalker, Select *pSelect){
  MViewWalk *p = (MViewWalk*)pWalker->u.pMViewWalk;
  UNUSED_PARAMETER(pSelect);
  mviewRefuse(p, "subqueries are not yet supported");
  return WRC_Abort;
}

/*
** Walk one expression tree, refusing non-determinism and subqueries.
** Returns p->rc.
*/
static int mviewCheckDeterministic(MViewWalk *p, Expr *pExpr){
  Walker w;
  if( pExpr==0 || p->rc!=SQLITE_OK ) return p->rc;
  memset(&w, 0, sizeof(w));
  w.xExprCallback = mviewDetExprCb;
  w.xSelectCallback = mviewDetSelectCb;
  w.u.pMViewWalk = p;
  sqlite3WalkExpr(&w, pExpr);
  return p->rc;
}

/*
** Classify pExpr, known to be TK_AGG_FUNCTION, as one of the
** maintainable aggregates and return its MVIEW_COL_* kind.  On anything
** outside the subset, refuse with the named reason and return 0 with
** p->rc set.  For MIN/MAX (Tier 2), the argument must be a plain base
** column: that makes the aggregate's comparison collation the COLUMN's,
** knowable here and captured (with the base column's name) for the
** generated maintenance SQL and the index advisory.
*/
static int mviewAggColKind(
  MViewWalk *p,
  Expr *pExpr,
  const char **pzColl,      /* OUT (MIN/MAX): argument's collation name */
  const char **pzBaseCol    /* OUT (MIN/MAX): argument's base column name */
){
  const char *zFunc;
  int nArg;
  int kind;
  assert( pExpr->op==TK_AGG_FUNCTION );
  assert( !ExprHasProperty(pExpr, EP_IntValue) );
  zFunc = pExpr->u.zToken;
  nArg = (ExprUseXList(pExpr) && pExpr->x.pList)
            ? pExpr->x.pList->nExpr : 0;
  *pzColl = 0;
  *pzBaseCol = 0;
  if( ExprHasProperty(pExpr, EP_Distinct) ){
    mviewRefuse(p, "%s(DISTINCT) requires tracking a per-group multiset "
                   "(Tier 3, not yet supported)", zFunc);
    return 0;
  }
  if( ExprHasProperty(pExpr, EP_WinFunc) ){
    mviewRefuse(p, "window functions are not yet supported");
    return 0;
  }
  if( sqlite3_stricmp(zFunc,"max")==0 || sqlite3_stricmp(zFunc,"min")==0 ){
    Expr *pArg;
    Column *pCol;
    if( nArg!=1
     || (pArg = pExpr->x.pList->a[0].pExpr)==0
     || pArg->op!=TK_COLUMN
     || pArg->y.pTab==0
     || pArg->iColumn<0
    ){
      mviewRefuse(p, "%s() over an expression is not yet supported "
                     "(Tier 2 accepts plain column arguments only)", zFunc);
      return 0;
    }
    pCol = &pArg->y.pTab->aCol[pArg->iColumn];
    *pzColl = sqlite3ColumnColl(pCol);
    if( *pzColl==0 ) *pzColl = "BINARY";
    *pzBaseCol = pCol->zCnName;
    return sqlite3_stricmp(zFunc,"min")==0 ? MVIEW_COL_MIN : MVIEW_COL_MAX;
  }
  if( sqlite3_stricmp(zFunc,"count")==0 ){
    kind = nArg==0 ? MVIEW_COL_COUNT0 : MVIEW_COL_COUNT;
  }else if( sqlite3_stricmp(zFunc,"sum")==0 ){
    kind = MVIEW_COL_SUM;
  }else if( sqlite3_stricmp(zFunc,"total")==0 ){
    kind = MVIEW_COL_TOTAL;
  }else if( sqlite3_stricmp(zFunc,"avg")==0 ){
    kind = MVIEW_COL_AVG;
  }else{
    mviewRefuse(p, "%s() is not incrementally maintainable "
                   "(supported: count, sum, total, avg, min, max)", zFunc);
    return 0;
  }
  /* Arguments must be deterministic (COUNT(*) has none) */
  if( nArg>0 ){
    int i;
    for(i=0; i<nArg; i++){
      if( mviewCheckDeterministic(p, pExpr->x.pList->a[i].pExpr) ) return 0;
    }
  }
  return kind;
}

/*
** Collect the join-probe columns for the index advisory: for every
** top-level equality conjunct in the WHERE (inner-join ON terms live
** there after prepare) whose two sides are plain columns of DIFFERENT
** tables, record each side as (base ordinal, column name).  A rowid /
** INTEGER PRIMARY KEY side is skipped -- the btree key never needs an
** index.  On overflow the whole capture is dropped: the advisory says
** nothing rather than something incomplete.
*/
static void mviewCollectProbes(
  Select *pSelect,
  u8 *aProbeBase,
  const char **azProbeCol,
  int *pnProbe,
  int mxProbe
){
  Expr *apStack[32];
  int nStack = 0;
  int n = 0;
  SrcList *pSrc = pSelect->pSrc;

  *pnProbe = 0;
  if( pSrc->nSrc<2 || pSelect->pWhere==0 ) return;
  apStack[nStack++] = pSelect->pWhere;
  while( nStack>0 ){
    Expr *pE = apStack[--nStack];
    if( pE==0 ) continue;
    if( pE->op==TK_AND ){
      if( nStack+2>(int)ArraySize(apStack) ) return;   /* too deep: drop */
      apStack[nStack++] = pE->pLeft;
      apStack[nStack++] = pE->pRight;
      continue;
    }
    if( pE->op==TK_EQ
     && pE->pLeft && pE->pLeft->op==TK_COLUMN && pE->pLeft->y.pTab
     && pE->pRight && pE->pRight->op==TK_COLUMN && pE->pRight->y.pTab
     && pE->pLeft->iTable!=pE->pRight->iTable
    ){
      Expr *apSide[2];
      int s;
      apSide[0] = pE->pLeft;
      apSide[1] = pE->pRight;
      for(s=0; s<2; s++){
        int k, dup = 0;
        const char *zCol;
        if( apSide[s]->iColumn<0 ) continue;         /* rowid: served */
        zCol = apSide[s]->y.pTab->aCol[apSide[s]->iColumn].zCnName;
        for(k=0; k<pSrc->nSrc; k++){
          if( pSrc->a[k].iCursor==apSide[s]->iTable ) break;
        }
        if( k>=pSrc->nSrc ) continue;
        {
          int m;
          for(m=0; m<n; m++){
            if( aProbeBase[m]==(u8)k
             && sqlite3StrICmp(azProbeCol[m], zCol)==0 ) dup = 1;
          }
        }
        if( dup ) continue;
        if( n>=mxProbe ){ *pnProbe = 0; return; }    /* overflow: drop */
        aProbeBase[n] = (u8)k;
        azProbeCol[n] = zCol;
        n++;
      }
    }
  }
  *pnProbe = n;
}

/*
** The Tier-1 conformance walk.  pSelect has already been through
** sqlite3ResultSetOfSelect() (names resolved, aggregates marked).
** On any violation an error naming the construct and the fix is left in
** pParse and non-zero is returned.
*/
static int mviewCheckSelect(
  Parse *pParse,
  const char *zName,       /* View name for messages */
  Select *pSelect,         /* The prepared definition */
  u8 *aColKind,            /* OUT: one MVIEW_COL_* per result column */
  const char **azColl,     /* OUT: MIN/MAX arg collation names */
  const char **azBaseCol   /* OUT: base column names (keys, MIN/MAX args) */
){
  MViewWalk sWalk;
  int i;

  sWalk.pParse = pParse;
  sWalk.zName = zName;
  sWalk.rc = SQLITE_OK;

  /* Statement shape */
  if( pSelect->pPrior ){
    mviewRefuse(&sWalk, "compound SELECTs (UNION, INTERSECT, EXCEPT) "
                        "are not yet supported");
    return sWalk.rc;
  }
  if( pSelect->pLimit ){
    mviewRefuse(&sWalk, "LIMIT depends on evaluation order and is not "
                        "supported; filter with WHERE instead");
    return sWalk.rc;
  }
  if( pSelect->pOrderBy ){
    mviewRefuse(&sWalk, "ORDER BY has no meaning for stored rows; "
                        "order at query time instead");
    return sWalk.rc;
  }
  if( (pSelect->selFlags & SF_Distinct)!=0 ){
    mviewRefuse(&sWalk, "SELECT DISTINCT requires tracking duplicate "
                        "counts (Tier 3, not yet supported)");
    return sWalk.rc;
  }
  if( pSelect->pWin ){
    mviewRefuse(&sWalk, "window functions are not yet supported");
    return sWalk.rc;
  }

  /* Every FROM item must be an ordinary base table, appearing once,
  ** inner-joined.  The per-base trigger construction shadows ONE table
  ** with the one-row delta and probes the others live, which is
  ** correct for any deterministic inner condition -- an inner join
  ** distributes over the union of one side's rows.  A table appearing
  ** twice breaks that (the shadow would replace both occurrences), and
  ** outer joins flip null-extended rows on membership changes. */
  assert( pSelect->pSrc!=0 && pSelect->pSrc->nSrc>=1 );
  {
    int k, j;
    for(k=0; k<pSelect->pSrc->nSrc; k++){
      SrcItem *pItem = &pSelect->pSrc->a[k];
      Table *pB;
      if( pItem->fg.isSubquery || pItem->pSTab==0 ){
        mviewRefuse(&sWalk, "subqueries in FROM are not yet supported");
        return sWalk.rc;
      }
      pB = pItem->pSTab;
      if( IsView(pB) ){
        mviewRefuse(&sWalk, "the base must be an ordinary table "
                            "(%s is a view)", pB->zName);
        return sWalk.rc;
      }
      if( IsVirtual(pB) ){
        mviewRefuse(&sWalk, "the base must be an ordinary table "
                            "(%s is a virtual table)", pB->zName);
        return sWalk.rc;
      }
      if( IsMView(pB) ){
        mviewRefuse(&sWalk, "the base must be an ordinary table (%s is "
                            "itself a materialized view; cascading "
                            "maintenance is not yet supported)",
                            pB->zName);
        return sWalk.rc;
      }
      /* A system-versioned base is ACCEPTED (DESIGN-IVM-TEMPORAL Q1,
      ** ruled 2026-08-16).  The former refusal here was precautionary;
      ** POC-T1's storm measured the two synthesized trigger families
      ** coexisting cleanly with deterministic write order (base ->
      ** commit log -> history -> mview).  ivmt1 section 1 pins the
      ** composition, REPLACE displacement included. */
      if( (pItem->fg.jointype & JT_OUTER)!=0 ){
        mviewRefuse(&sWalk, "outer joins are not yet supported (a "
            "departing row can flip null-extended rows on the other "
            "side)");
        return sWalk.rc;
      }
      for(j=0; j<k; j++){
        if( pSelect->pSrc->a[j].pSTab==pB ){
          mviewRefuse(&sWalk, "%s appears more than once (self-joins "
                              "are not yet maintainable)", pB->zName);
          return sWalk.rc;
        }
      }
    }
  }

  /* Result columns: each is either a Tier-1 aggregate call or an
  ** expression covered by GROUP BY.  (Bare columns outside GROUP BY are
  ** refused: which row the engine picks for them is unspecified, and a
  ** maintained view may not depend on it.) */
  assert( pSelect->pEList!=0 );
  {
  int bJoin = pSelect->pSrc->nSrc>1;
  int bHasCount0 = 0;
  for(i=0; i<pSelect->pEList->nExpr; i++){
    Expr *pE = pSelect->pEList->a[i].pExpr;
    if( pE->op==TK_AGG_FUNCTION ){
      int kind = mviewAggColKind(&sWalk, pE, &azColl[i], &azBaseCol[i]);
      if( sWalk.rc ) return sWalk.rc;
      if( bJoin ){
        /* A join view carries no hidden bookkeeping (one base row is
        ** many join rows, so the one-row derivations do not exist):
        ** what cannot be derived must be declared or refused. */
        if( kind==MVIEW_COL_SUM ){
          mviewRefuse(&sWalk, "sum() is not yet maintainable in a join "
              "view (its NULL-restoring count cannot be derived across "
              "a join); use total()");
          return sWalk.rc;
        }
        if( kind==MVIEW_COL_AVG ){
          mviewRefuse(&sWalk, "avg() is not yet maintainable in a join "
              "view (its sum and count cannot be derived across a "
              "join); declare total() and count(*) and divide at query "
              "time");
          return sWalk.rc;
        }
        if( kind==MVIEW_COL_COUNT0 ) bHasCount0 = 1;
      }
      aColKind[i] = (u8)kind;
      continue;
    }
    aColKind[i] = MVIEW_COL_KEY;
    /* A key that is a plain base column gets its name captured: the
    ** index advisory is written from these.  Expression keys leave the
    ** slot empty, which the advisory reads as "no index can serve". */
    if( pE->op==TK_COLUMN && pE->y.pTab!=0 && pE->iColumn>=0 ){
      azBaseCol[i] = pE->y.pTab->aCol[pE->iColumn].zCnName;
    }
    /* Aggregates nested inside larger expressions are a Tier-2+ shape:
    ** the raw aggregate would need hidden storage to re-derive the
    ** expression on maintenance.  EP_Agg is set by name resolution on
    ** any expression containing an aggregate. */
    if( ExprHasProperty(pE, EP_Agg) ){
      mviewRefuse(&sWalk, "result column %d computes over an aggregate; "
          "select the bare aggregate and compute at query time", i+1);
      return sWalk.rc;
    }
    if( mviewCheckDeterministic(&sWalk, pE) ) return sWalk.rc;
    /* sqlite3ExprIsConstantOrGroupBy() dereferences its GROUP BY list;
    ** with no GROUP BY the column must stand on constants alone. */
    if( pSelect->pGroupBy
          ? !sqlite3ExprIsConstantOrGroupBy(pParse, pE, pSelect->pGroupBy)
          : !sqlite3ExprIsConstant(pParse, pE) ){
      const char *zCol = pSelect->pEList->a[i].zEName;
      mviewRefuse(&sWalk, "result column %d (%s) is neither an aggregate "
          "nor a GROUP BY expression; a maintained view may not depend on "
          "which row the engine happens to pick",
          i+1, zCol ? zCol : "?");
      return sWalk.rc;
    }
  }
  if( bJoin && !bHasCount0 ){
    mviewRefuse(&sWalk, "a join view must declare count(*) (the "
        "group's liveness cannot be derived across a join)");
    return sWalk.rc;
  }
  }

  /* WHERE filters the delta; must be deterministic, no aggregates by
  ** construction.  GROUP BY keys likewise. */
  if( mviewCheckDeterministic(&sWalk, pSelect->pWhere) ) return sWalk.rc;
  if( pSelect->pGroupBy ){
    for(i=0; i<pSelect->pGroupBy->nExpr; i++){
      if( mviewCheckDeterministic(&sWalk, pSelect->pGroupBy->a[i].pExpr) ){
        return sWalk.rc;
      }
    }
  }

  /* HAVING is Tier 2 (DESIGN-IVM amendment 2026-08-12): a group that
  ** fails HAVING must be absent from the stored table yet keep
  ** accumulating so it can reappear, and with row-as-storage an absent
  ** group has nowhere to accumulate. */
  if( pSelect->pHaving ){
    mviewRefuse(&sWalk, "HAVING requires storage for the groups it "
        "hides (Tier 2, not yet supported); filter at query time over "
        "the maintained view instead");
    return sWalk.rc;
  }

  return sWalk.rc;
}

/*
** Generate code that runs the definition SELECT and inserts every result
** row into the new table whose root page is in register
** pParse->u1.cr.regRoot.  This is the CREATE TABLE ... AS SELECT
** population codegen from sqlite3EndTable(), minus the column derivation
** (already done by the caller).
*/
static void mviewCodePopulate(Parse *pParse, Table *p, Select *pSelect){
  Vdbe *v = sqlite3GetVdbe(pParse);
  SelectDest dest;    /* Where the SELECT should store results */
  int regYield;       /* Register holding co-routine entry-point */
  int addrTop;        /* Top of the co-routine */
  int regRec;         /* A record to be inserted into the new table */
  int regRowid;       /* Rowid of the next row to insert */
  int addrInsLoop;    /* Top of the loop for inserting rows */
  int iDb;            /* Database holding the new table */
  int iCsr;           /* Write cursor on the new table */

  if( NEVER(v==0) ) return;
  iDb = sqlite3SchemaToIndex(pParse->db, p->pSchema);
  iCsr = pParse->nTab++;
  regYield = ++pParse->nMem;
  regRec = ++pParse->nMem;
  regRowid = ++pParse->nMem;
  sqlite3MayAbort(pParse);
  assert( pParse->isCreate );
  sqlite3VdbeAddOp3(v, OP_OpenWrite, iCsr, pParse->u1.cr.regRoot, iDb);
  sqlite3VdbeChangeP5(v, OPFLAG_P2ISREG);
  addrTop = sqlite3VdbeCurrentAddr(v) + 1;
  sqlite3VdbeAddOp3(v, OP_InitCoroutine, regYield, 0, addrTop);
  if( pParse->nErr ) return;
  sqlite3SelectDestInit(&dest, SRT_Coroutine, regYield);
  sqlite3Select(pParse, pSelect, &dest);
  if( pParse->nErr ) return;
  sqlite3VdbeEndCoroutine(v, regYield);
  sqlite3VdbeJumpHere(v, addrTop - 1);
  addrInsLoop = sqlite3VdbeAddOp1(v, OP_Yield, dest.iSDParm);
  VdbeCoverage(v);
  sqlite3VdbeAddOp3(v, OP_MakeRecord, dest.iSdst, dest.nSdst, regRec);
  sqlite3TableAffinity(v, p, 0);
  sqlite3VdbeAddOp2(v, OP_NewRowid, iCsr, regRowid);
  sqlite3VdbeAddOp3(v, OP_Insert, iCsr, regRec, regRowid);
  sqlite3VdbeGoto(v, addrInsLoop);
  sqlite3VdbeJumpHere(v, addrInsLoop);
  sqlite3VdbeAddOp1(v, OP_Close, iCsr);
}

/*
** Register a materialized view in its schema's mviewHash.  Called on the
** db->init.busy path only, which runs both at schema load and (via the
** OP_ParseSchema step of CREATE) immediately after a live CREATE commits
** its schema row -- one write path, two occasions.
*/
static void mviewRegister(
  Parse *pParse,
  Table *p,                /* The view's table object */
  SrcList *pSrc,           /* The resolved FROM: every item an ordinary
                           ** base table (each gets TF_MViewBase) */
  int bDeferred,
  int nCol,                /* Number of VISIBLE result columns */
  const u8 *aColKind,      /* The conformance walk's column kinds */
  const char **azColl,     /* MIN/MAX arg collations (entries may be 0) */
  const char **azBaseCol,  /* Base column names (entries may be 0) */
  const u8 *aProbeBase,    /* Join-probe capture: base ordinal ... */
  const char **azProbeCol, /* ... and column name, nProbe entries */
  int nProbe,
  const char *zSelDef,     /* Definition SELECT text */
  int nSelDef              /* Length of zSelDef */
){
  MViewInfo *pInfo, *pOld;
  sqlite3 *db = pParse->db;
  sqlite3_int64 nName = sqlite3Strlen30(p->zName)+1;
  sqlite3_int64 nStr, nAlloc;
  int nBase = pSrc->nSrc;
  char *zOut;
  int i;
  assert( db->init.busy );
  assert( sqlite3SchemaMutexHeld(db, 0, p->pSchema) );
  /* One allocation: struct, then every pointer array (aligned, so they
  ** come first -- Trigger* and char* share alignment), then the string
  ** pool and the byte arrays.  The hash keys off pInfo->zName rather
  ** than the Table's own name so the entry never points into memory
  ** the table teardown frees. */
  nStr = nName + nSelDef + 1;
  for(i=0; i<nBase; i++){
    nStr += sqlite3Strlen30(pSrc->a[i].pSTab->zName) + 1;
  }
  for(i=0; i<nCol; i++){
    if( azColl[i] ) nStr += sqlite3Strlen30(azColl[i]) + 1;
    if( azBaseCol[i] ) nStr += sqlite3Strlen30(azBaseCol[i]) + 1;
  }
  for(i=0; i<nProbe; i++){
    nStr += sqlite3Strlen30(azProbeCol[i]) + 1;
  }
  nAlloc = sizeof(MViewInfo)
         + (nBase + 2*nCol + nProbe + 3*nBase)*sizeof(char*)
         + nStr + nCol + nProbe;
  pInfo = sqlite3MallocZero(nAlloc);
  if( pInfo==0 ){
    sqlite3OomFault(db);
    return;
  }
  pInfo->azBase = (char**)&pInfo[1];
  pInfo->azColl = pInfo->azBase + nBase;
  pInfo->azBaseCol = pInfo->azColl + nCol;
  pInfo->azProbeCol = pInfo->azBaseCol + nCol;
  pInfo->apTrig = (Trigger**)(pInfo->azProbeCol + nProbe);
  zOut = (char*)(pInfo->apTrig + 3*nBase);
  pInfo->zName = zOut;
  memcpy(zOut, p->zName, nName);      zOut += nName;
  for(i=0; i<nBase; i++){
    const char *zB = pSrc->a[i].pSTab->zName;
    int n = sqlite3Strlen30(zB) + 1;
    pInfo->azBase[i] = zOut;
    memcpy(zOut, zB, n);
    zOut += n;
  }
  pInfo->zSelDef = zOut;
  memcpy(zOut, zSelDef, nSelDef);
  zOut[nSelDef] = 0;                  zOut += nSelDef + 1;
  for(i=0; i<nCol; i++){
    if( azColl[i] ){
      int n = sqlite3Strlen30(azColl[i]) + 1;
      pInfo->azColl[i] = zOut;
      memcpy(zOut, azColl[i], n);
      zOut += n;
    }
    if( azBaseCol[i] ){
      int n = sqlite3Strlen30(azBaseCol[i]) + 1;
      pInfo->azBaseCol[i] = zOut;
      memcpy(zOut, azBaseCol[i], n);
      zOut += n;
    }
  }
  for(i=0; i<nProbe; i++){
    int n = sqlite3Strlen30(azProbeCol[i]) + 1;
    pInfo->azProbeCol[i] = zOut;
    memcpy(zOut, azProbeCol[i], n);
    zOut += n;
  }
  pInfo->aColKind = (u8*)zOut;
  memcpy(pInfo->aColKind, aColKind, nCol);
  zOut += nCol;
  pInfo->aProbeBase = (u8*)zOut;
  if( nProbe>0 ) memcpy(pInfo->aProbeBase, aProbeBase, nProbe);
  pInfo->nCol = nCol;
  pInfo->nBase = nBase;
  pInfo->nProbe = nProbe;
  pInfo->bDeferred = (u8)(bDeferred!=0);
  /* Every base table now has a dependent: TriggerList consults the
  ** registry for them (eager maintenance), and ALTER/DROP refuse. */
  for(i=0; i<nBase; i++){
    pSrc->a[i].pSTab->tabFlags |= TF_MViewBase;
  }
  pOld = sqlite3HashInsert(&p->pSchema->mviewHash, pInfo->zName, pInfo);
  if( pOld ){
    if( pOld==pInfo ){          /* OOM inside HashInsert */
      sqlite3OomFault(db);
      sqlite3_free(pInfo);
    }else{
      /* A stale entry under the same name: replace it. */
      sqlite3_free(pOld);
    }
  }
}

/*
** Free one MViewInfo, including its synthesized maintenance triggers.
** The triggers were built by a CREATE-statement sub-parse, which
** disables lookaside (the createkw action), so freeing them through
** any sqlite3* -- including callback.c's zeroed stand-in -- is sound.
*/
static void mviewInfoFree(sqlite3 *db, MViewInfo *pInfo){
  int i;
  if( pInfo==0 ) return;
  for(i=0; i<3*pInfo->nBase; i++){
    sqlite3DeleteTrigger(db, pInfo->apTrig[i]);
  }
  sqlite3_free(pInfo);
}

/*
** Free every MViewInfo in an mviewHash and reset the hash.  Called from
** the schema-clearing paths in callback.c.
*/
void sqlite3MViewHashClear(sqlite3 *db, Hash *pHash){
  HashElem *pElem;
  for(pElem=sqliteHashFirst(pHash); pElem; pElem=sqliteHashNext(pElem)){
    mviewInfoFree(db, (MViewInfo*)sqliteHashData(pElem));
  }
  sqlite3HashClear(pHash);
}

/*
** Remove one view's registry entry, if present.  Called when the table
** object is unlinked (see sqlite3UnlinkAndDeleteTable in build.c), which
** covers every drop path.  The synthesized triggers are unlinked from
** the base table's trigger list before they are freed: the schema reset
** that follows a DROP re-derives everything, but the base's list must
** never hold a freed pointer even briefly.
*/
void sqlite3UnlinkAndDeleteMView(sqlite3 *db, int iDb, const char *zName){
  Hash *pHash;
  MViewInfo *pInfo;
  int k;
  assert( sqlite3SchemaMutexHeld(db, iDb, 0) );
  pHash = &db->aDb[iDb].pSchema->mviewHash;
  pInfo = sqlite3HashInsert(pHash, zName, 0);
  if( pInfo ){
    for(k=0; k<pInfo->nBase; k++){
      Table *pBase;
      if( pInfo->apTrig[3*k]==0 ) continue;   /* this set never built */
      pBase = sqlite3FindTable(db, pInfo->azBase[k],
                               db->aDb[iDb].zDbSName);
      if( pBase ){
        Trigger **pp = &pBase->pTrigger;
        while( *pp ){
          int i, isMine = 0;
          for(i=0; i<3; i++){
            if( *pp==pInfo->apTrig[3*k+i] ) isMine = 1;
          }
          if( isMine ){
            *pp = (*pp)->pNext;
          }else{
            pp = &(*pp)->pNext;
          }
        }
      }
    }
  }
  mviewInfoFree(db, pInfo);
}

/*
** If any materialized view in database iDb is defined over base table
** zBase, return the name of one such view.  Return 0 if none.  Used to
** refuse DROP/ALTER of a base table that a stored definition depends on
** -- a definition that stopped resolving would fail the next schema load.
*/
const char *sqlite3MViewFindDependent(
  sqlite3 *db,
  int iDb,
  const char *zBase
){
  Hash *pHash;
  HashElem *pElem;
  assert( sqlite3SchemaMutexHeld(db, iDb, 0) );
  pHash = &db->aDb[iDb].pSchema->mviewHash;
  for(pElem=sqliteHashFirst(pHash); pElem; pElem=sqliteHashNext(pElem)){
    MViewInfo *pInfo = (MViewInfo*)sqliteHashData(pElem);
    int k;
    for(k=0; k<pInfo->nBase; k++){
      if( sqlite3_stricmp(pInfo->azBase[k], zBase)==0 ){
        return pInfo->zName;
      }
    }
  }
  return 0;
}

/*
** The parser calls this at the end of a CREATE MATERIALIZED VIEW
** statement.  pMat is the identifier that must spell MATERIALIZED --
** it costs no keyword; anything else here is a syntax error that names
** the accepted word.
*/
void sqlite3CreateMView(
  Parse *pParse,     /* The parsing context */
  Token *pBegin,     /* The CREATE token */
  Token *pMat,       /* The identifier that must be "MATERIALIZED" */
  Token *pName1,     /* First part of the view name */
  Token *pName2,     /* Second part of the view name */
  int maintMode,     /* MVIEW_MAINT_UNSPEC, _EAGER or _DEFERRED */
  Token *pAs,        /* The AS keyword: the definition text follows it */
  Select *pSelect,   /* The definition */
  int isTemp,        /* TRUE if TEMP appeared */
  int noErr          /* IF NOT EXISTS */
){
  Table *p;
  int n;
  const char *z;
  Token sEnd;
  DbFixer sFix;
  Token *pName = 0;
  int iDb;
  sqlite3 *db = pParse->db;
  Table *pSelTab;    /* Result-set shape of the definition */
  Select *pSelDup = 0; /* Pre-resolution copy, for population */
  u8 *aColKind = 0;  /* Conformance walk's per-column kinds */
  const char **azColl = 0;    /* MIN/MAX arg collations, walk-captured */
  const char **azBaseCol = 0; /* Base column names, walk-captured */
  u8 aProbeBase[32];          /* Join-probe capture: base ordinals... */
  const char *azProbeCol[32]; /* ...and column names */
  int nProbe = 0;
  int nVis = 0;      /* Number of visible result columns */

  if( pMat->n!=12 || sqlite3_strnicmp(pMat->z, "materialized", 12)!=0 ){
    sqlite3ErrorMsg(pParse, "near \"%T\": syntax error "
        "(the only word accepted between CREATE and VIEW here is "
        "MATERIALIZED)", pMat);
    goto create_mview_fail;
  }
  if( pParse->nVar>0 ){
    sqlite3ErrorMsg(pParse,
        "parameters are not allowed in materialized views");
    goto create_mview_fail;
  }
  if( isTemp || db->init.iDb==1 ){
    sqlite3ErrorMsg(pParse, "TEMP materialized views are not supported; "
        "create the view in a persistent schema");
    goto create_mview_fail;
  }

  /* A real table is being created: isView=0 so a root page is allocated.
  ** bMViewCreate steers the object-name check's type to 'mview' so the
  ** stored row's type column matches at schema load. */
  pParse->bMViewCreate = 1;
  sqlite3StartTable(pParse, pName1, pName2, 0, 0, 0, noErr);
  pParse->bMViewCreate = 0;
  p = pParse->pNewTable;
  if( p==0 || pParse->nErr ) goto create_mview_fail;
  p->tabFlags |= TF_MView;
  if( maintMode==MVIEW_MAINT_DEFERRED ) p->tabFlags |= TF_MViewDeferred;

  sqlite3TwoPartName(pParse, pName1, pName2, &pName);
  iDb = sqlite3SchemaToIndex(db, p->pSchema);
  assert( iDb>=0 && iDb<db->nDb );
  if( iDb==1 ){
    sqlite3ErrorMsg(pParse, "TEMP materialized views are not supported; "
        "create the view in a persistent schema");
    goto create_mview_fail;
  }
  sqlite3FixInit(&sFix, pParse, iDb, "materialized view", pName);
  if( sqlite3FixSelect(&sFix, pSelect) ) goto create_mview_fail;

  /* SELECT * expands to whatever columns the base table has TODAY; the
  ** stored definition would silently change shape when the base table
  ** does, and the stored rows would no longer match the re-derived
  ** columns at the next schema load.  Refused before expansion. */
  {
    int i;
    for(i=0; i<pSelect->pEList->nExpr; i++){
      const Expr *pE = pSelect->pEList->a[i].pExpr;
      if( pE->op==TK_ASTERISK
       || (pE->op==TK_DOT && pE->pRight && pE->pRight->op==TK_ASTERISK)
      ){
        sqlite3ErrorMsg(pParse, "%s cannot be incrementally maintained: "
            "SELECT * would change shape when the base table does; "
            "name the columns explicitly", p->zName);
        goto create_mview_fail;
      }
    }
  }
  if( pSelect->pWith ){
    sqlite3ErrorMsg(pParse, "%s cannot be incrementally maintained: "
        "common table expressions are not yet supported", p->zName);
    goto create_mview_fail;
  }

  /* A pre-resolution copy for population: the appended bookkeeping
  ** aggregates must go through name resolution WITH the rest of the
  ** tree, and a prepared Select is never prepped twice. */
  if( !db->init.busy ){
    pSelDup = sqlite3SelectDup(db, pSelect, 0);
    if( pSelDup==0 ) goto create_mview_fail;
  }

  /* Derive the column list from the definition.  This runs at CREATE and
  ** again at every schema load (the stored DDL keeps the SELECT), which
  ** is why the base table must outlive the view. */
  pSelTab = sqlite3ResultSetOfSelect(pParse, pSelect, SQLITE_AFF_BLOB);
  if( pSelTab==0 ) goto create_mview_fail;
  assert( p->aCol==0 );
  p->nCol = p->nNVCol = pSelTab->nCol;
  p->aCol = pSelTab->aCol;
  pSelTab->nCol = 0;
  pSelTab->aCol = 0;
  sqlite3DeleteTable(db, pSelTab);
  nVis = p->nCol;

  /* The ivm$ column namespace belongs to the bookkeeping */
  {
    int i;
    for(i=0; i<nVis; i++){
      if( sqlite3_strnicmp(p->aCol[i].zCnName, "ivm$", 4)==0 ){
        sqlite3ErrorMsg(pParse, "%s cannot be incrementally maintained: "
            "column names beginning with ivm$ are reserved for "
            "maintenance bookkeeping (rename %s)",
            p->zName, p->aCol[i].zCnName);
        goto create_mview_fail;
      }
    }
  }

  /* Tier-1 conformance: everything outside the maintainable subset is
  ** refused by name, with the fix, before anything is written. */
  aColKind = sqlite3DbMallocRawNN(db, nVis);
  azColl = (const char**)sqlite3DbMallocZero(db, nVis*sizeof(char*));
  azBaseCol = (const char**)sqlite3DbMallocZero(db, nVis*sizeof(char*));
  if( aColKind==0 || azColl==0 || azBaseCol==0 ) goto create_mview_fail;
  if( mviewCheckSelect(pParse, p->zName, pSelect, aColKind,
                       azColl, azBaseCol) ){
    goto create_mview_fail;
  }
  /* Join-probe capture for the advisory: each base's plain columns in
  ** top-level equality conjuncts against another table.  Overflowing
  ** the fixed capture drops the whole set: no advisory rather than a
  ** wrong one. */
  mviewCollectProbes(pSelect, aProbeBase, azProbeCol, &nProbe,
                     (int)ArraySize(aProbeBase));

  /* The hidden bookkeeping columns, at CREATE and at every load.
  ** Join views carry none: their liveness is the count(*) the walk
  ** required, and their SUM/AVG (the other hidden consumers) are
  ** refused. */
  if( pSelect->pSrc->nSrc==1
   && mviewAppendHiddenCols(pParse, p, nVis, aColKind) ){
    goto create_mview_fail;
  }

  /* Population: run the augmented definition into the new root page. */
  if( !db->init.busy ){
    if( IN_SPECIAL_PARSE ){
      pParse->rc = SQLITE_ERROR;
      pParse->nErr++;
      goto create_mview_fail;
    }
    if( pSelect->pSrc->nSrc==1 ){
      mviewAugmentSelect(pParse, pSelDup, nVis, aColKind);
      if( pParse->nErr || db->mallocFailed ) goto create_mview_fail;
    }
    mviewCodePopulate(pParse, p, pSelDup);
    if( pParse->nErr ) goto create_mview_fail;
  }

  /* Locate the end of the statement so sqlite3EndTable() stores the
  ** complete original DDL (the same technique as CREATE VIEW). */
  sEnd = pParse->sLastToken;
  assert( sEnd.z[0]!=0 || sEnd.n==0 );
  if( sEnd.z[0]!=';' ){
    sEnd.z += sEnd.n;
  }
  sEnd.n = 0;
  n = (int)(sEnd.z - pBegin->z);
  assert( n>0 );
  z = pBegin->z;
  while( sqlite3Isspace(z[n-1]) ){ n--; }
  sEnd.z = &z[n-1];
  sEnd.n = 1;

  /* sqlite3EndTable() writes the schema row (type='mview' -- see the
  ** IsMView() steer there) or, at schema load, links the table into the
  ** schema.  pSelect is NOT passed: population and column derivation
  ** were done above, and EndTable's init path treats a select as
  ** schema corruption. */
  sqlite3EndTable(pParse, 0, &sEnd, 0, 0);
  if( pParse->nErr ) goto create_mview_fail;

  if( !db->init.busy ){
    /* The key index, emitted AFTER EndTable so it executes after the
    ** OP_ParseSchema reload: at that point the view is a real,
    ** populated table and an ordinary CREATE UNIQUE INDEX both builds
    ** and fills it.  Key-less views (whole-table aggregates) hold
    ** exactly one row forever and need no index. */
    int i, nKey = 0;
    for(i=0; i<nVis; i++) if( aColKind[i]==MVIEW_COL_KEY ) nKey++;
    if( nKey>0 ){
      Vdbe *v = sqlite3GetVdbe(pParse);
      sqlite3_str *pStr = sqlite3_str_new(db);
      char *zSql;
      sqlite3_str_appendf(pStr,
         "CREATE UNIQUE INDEX \"%w\".\"ivm$%w$key\" ON \"%w\"(",
         db->aDb[iDb].zDbSName, p->zName, p->zName);
      {
        int nOut = 0;
        for(i=0; i<nVis; i++){
          if( aColKind[i]!=MVIEW_COL_KEY ) continue;
          sqlite3_str_appendf(pStr, "%s\"%w\"", nOut++ ? "," : "",
                              p->aCol[i].zCnName);
        }
      }
      sqlite3_str_append(pStr, ")", 1);
      zSql = sqlite3_str_finish(pStr);
      if( zSql==0 || v==0 ){
        sqlite3_free(zSql);
        sqlite3OomFault(db);
        goto create_mview_fail;
      }
      sqlite3VdbeAddOp4(v, OP_SqlExec, 0x0001, 0, 0, zSql, P4_DYNAMIC);
    }

    /* A DEFERRED view captures into a delta log: the view's columns
    ** plus the weight.  Created here as part of the CREATE statement
    ** (the nested parse is what may use the sqlite_ namespace); its
    ** schema row follows the view's, so reload order is guaranteed. */
    if( p->tabFlags & TF_MViewDeferred ){
      sqlite3_str *pStr = sqlite3_str_new(db);
      char *zSql;
      sqlite3_str_appendf(pStr, "CREATE TABLE \"%w\".\"sqlite_ivm_%w_log\"(",
                          db->aDb[iDb].zDbSName, p->zName);
      for(i=0; i<p->nCol; i++){
        sqlite3_str_appendf(pStr, "%s\"%w\"", i ? "," : "",
                            p->aCol[i].zCnName);
        /* Extremum columns carry the captured collation so the fold's
        ** min/max over logged values compares as the aggregate does */
        if( i<nVis && (aColKind[i]==MVIEW_COL_MIN
                    || aColKind[i]==MVIEW_COL_MAX) ){
          sqlite3_str_appendf(pStr, " COLLATE %s", azColl[i]);
        }
      }
      sqlite3_str_appendall(pStr, ",\"ivm$w\" INTEGER)");
      zSql = sqlite3_str_finish(pStr);
      if( zSql==0 ){
        sqlite3OomFault(db);
        goto create_mview_fail;
      }
      sqlite3NestedParse(pParse, "%s", zSql);
      sqlite3_free(zSql);
      if( pParse->nErr ) goto create_mview_fail;
    }
  }

  /* Register in the in-memory mview registry.  Only the init path runs
  ** this: a live CREATE re-parses its own row via OP_ParseSchema, so the
  ** registry has exactly one write path. */
  if( db->init.busy ){
    /* The definition SELECT's text: everything after the AS keyword,
    ** through the trimmed end of the statement computed above. */
    const char *zSelDef = pAs->z + pAs->n;
    int nSelDef = (int)((z + n) - zSelDef);
    while( nSelDef>0 && sqlite3Isspace(zSelDef[0]) ){
      zSelDef++;
      nSelDef--;
    }
    /* EndTable consumed pNewTable into the schema; p remains valid. */
    mviewRegister(pParse, p, pSelect->pSrc,
                  (p->tabFlags & TF_MViewDeferred)!=0,
                  nVis, aColKind, azColl, azBaseCol,
                  aProbeBase, azProbeCol, nProbe, zSelDef, nSelDef);
  }

create_mview_fail:
  sqlite3DbFree(db, aColKind);
  sqlite3DbFree(db, azColl);
  sqlite3DbFree(db, azBaseCol);
  sqlite3SelectDelete(db, pSelDup);
  sqlite3SelectDelete(db, pSelect);
}

/*
** DROP MATERIALIZED VIEW.  pMat must spell MATERIALIZED (checked
** identifier, same as CREATE).  Plain DROP TABLE and DROP VIEW refuse
** materialized views by name (see sqlite3DropTable); this is their
** third sibling and refuses their objects in return.
*/
void sqlite3DropMView(Parse *pParse, Token *pMat, SrcList *pName, int noErr){
  Table *pTab;
  Vdbe *v;
  sqlite3 *db = pParse->db;
  int iDb;

  if( pMat->n!=12 || sqlite3_strnicmp(pMat->z, "materialized", 12)!=0 ){
    sqlite3ErrorMsg(pParse, "near \"%T\": syntax error "
        "(the only word accepted between DROP and VIEW here is "
        "MATERIALIZED)", pMat);
    goto exit_drop_mview;
  }
  if( db->mallocFailed ) goto exit_drop_mview;
  assert( pParse->nErr==0 );
  assert( pName->nSrc==1 );
  if( sqlite3ReadSchema(pParse) ) goto exit_drop_mview;
  if( noErr ) db->suppressErr++;
  pTab = sqlite3LocateTableItem(pParse, 0, &pName->a[0]);
  if( noErr ) db->suppressErr--;
  if( pTab==0 ){
    if( noErr ){
      sqlite3CodeVerifyNamedSchema(pParse, pName->a[0].u4.zDatabase);
      sqlite3ForceNotReadOnly(pParse);
    }
    goto exit_drop_mview;
  }
  iDb = sqlite3SchemaToIndex(db, pTab->pSchema);
  assert( iDb>=0 && iDb<db->nDb );
  if( !IsMView(pTab) ){
    if( IsView(pTab) ){
      sqlite3ErrorMsg(pParse, "use DROP VIEW to delete view %s",
                      pTab->zName);
    }else{
      sqlite3ErrorMsg(pParse, "use DROP TABLE to delete table %s",
                      pTab->zName);
    }
    goto exit_drop_mview;
  }
#ifndef SQLITE_OMIT_AUTHORIZATION
  {
    const char *zTab = SCHEMA_TABLE(iDb);
    const char *zDb = db->aDb[iDb].zDbSName;
    if( sqlite3AuthCheck(pParse, SQLITE_DELETE, zTab, 0, zDb) ){
      goto exit_drop_mview;
    }
    if( sqlite3AuthCheck(pParse, SQLITE_DROP_VIEW, pTab->zName, 0, zDb) ){
      goto exit_drop_mview;
    }
    if( sqlite3AuthCheck(pParse, SQLITE_DELETE, pTab->zName, 0, zDb) ){
      goto exit_drop_mview;
    }
  }
#endif

  /* Generate code to remove the view and its indexes from the schema
  ** table and destroy their root pages.  isView=0: there is a real
  ** btree to destroy.  The in-memory registry entry is removed by
  ** sqlite3UnlinkAndDeleteTable() when OP_DropTable executes. */
  v = sqlite3GetVdbe(pParse);
  if( v ){
    sqlite3BeginWriteOperation(pParse, 1, iDb);
    sqlite3ClearStatTables(pParse, iDb, "tbl", pTab->zName);
    sqlite3CodeDropTable(pParse, pTab, iDb, 0);
    /* A deferred view's delta log leaves with it */
    if( pTab->tabFlags & TF_MViewDeferred ){
      char *zLog = mviewLogName(db, pTab->zName);
      Table *pLog = zLog ? sqlite3FindTable(db, zLog,
                              db->aDb[iDb].zDbSName) : 0;
      if( pLog ){
        sqlite3ClearStatTables(pParse, iDb, "tbl", pLog->zName);
        sqlite3CodeDropTable(pParse, pLog, iDb, 0);
      }
      sqlite3DbFree(db, zLog);
    }
  }

exit_drop_mview:
  sqlite3SrcListDelete(db, pName);
}

/*
** ---------------------------------------------------------------------
** Eager maintenance: the synthesized triggers (P3).
**
** Three AFTER triggers per eager view, built as SQL TEXT and compiled
** by a private sub-parse, never persisted: no schema row, no trigHash
** entry, not droppable, rebuilt from the stored definition at every
** schema load (lazily, on the first statement that asks the base table
** for its triggers).  The construction leans on one identity: for a
** ONE-ROW delta the definition's own outputs are the bookkeeping
** deltas (the avg of one row is the value; its non-null count is the
** value IS NOT NULL; its group count is 1), so the stored definition
** text is used UNCHANGED -- a CTE named after the base table shadows
** it with a single NEW/OLD row.
**
** Application per delta row: UPDATE-combine (FROM the delta, keys
** matched with IS -- never ON CONFLICT, because UNIQUE treats NULL
** keys as distinct while GROUP BY treats them as one group), then for
** inserts an INSERT-if-absent, and for deletes a keyed removal of
** groups whose ivm$count reached zero.  Key-less views hold exactly
** one row from population and never insert or die.
*/

/*
** The group-liveness column: single-table views carry the hidden
** ivm$count; join views carry no hidden columns, and their liveness is
** the count(*) the conformance walk REQUIRES them to declare.
*/
static const char *mviewLiveName(
  const MViewInfo *pInfo,
  const Table *pView
){
  int i;
  if( pInfo->nBase==1 ) return "ivm$count";
  for(i=0; i<pInfo->nCol; i++){
    if( pInfo->aColKind[i]==MVIEW_COL_COUNT0 ){
      return pView->aCol[i].zCnName;
    }
  }
  assert( !"join view without count(*) got past the walk" );
  return "ivm$count";
}

/*
** Append the DELTA subquery for one affected row:
**   (WITH "base"(cols) AS (VALUES(ROW.cols))
**    SELECT d.*, <bookkeeping>, 1 AS "ivm$count" FROM (
**    <definition text>
**    ) AS d)
** zRow is "NEW" or "OLD".  Newlines around the definition text keep a
** trailing -- comment in the stored text from swallowing the wrapper.
*/
static void mviewAppendDelta(
  sqlite3_str *pStr,
  const MViewInfo *pInfo,
  const Table *pView,
  const Table *pBase,
  const char *zRow
){
  int i;
  sqlite3_str_appendf(pStr, "(WITH \"%w\"(", pBase->zName);
  for(i=0; i<pBase->nCol; i++){
    sqlite3_str_appendf(pStr, "%s\"%w\"", i ? "," : "",
                        pBase->aCol[i].zCnName);
  }
  sqlite3_str_appendall(pStr, ") AS (VALUES(");
  for(i=0; i<pBase->nCol; i++){
    sqlite3_str_appendf(pStr, "%s%s.\"%w\"", i ? "," : "", zRow,
                        pBase->aCol[i].zCnName);
  }
  sqlite3_str_appendall(pStr, ")) SELECT d.*");
  if( pInfo->nBase==1 ){
    /* The ONE-ROW identities: valid only when the delta is a single
    ** base row.  A join view's outputs are already its group deltas
    ** (the shadowed definition aggregates the joined rows), and it
    ** has no hidden columns to feed. */
    for(i=0; i<pInfo->nCol; i++){
      const char *zC = pView->aCol[i].zCnName;
      if( pInfo->aColKind[i]==MVIEW_COL_SUM ){
        sqlite3_str_appendf(pStr,
           ", (d.\"%w\" IS NOT NULL) AS \"ivm$n$%d\"", zC, i);
      }else if( pInfo->aColKind[i]==MVIEW_COL_AVG ){
        sqlite3_str_appendf(pStr,
           ", d.\"%w\" AS \"ivm$s$%d\""
           ", (d.\"%w\" IS NOT NULL) AS \"ivm$n$%d\"", zC, i, zC, i);
      }
    }
    sqlite3_str_appendall(pStr, ", 1 AS \"ivm$count\"");
  }
  sqlite3_str_appendf(pStr,
     " FROM (\n%s\n) AS d)", pInfo->zSelDef);
}

/*
** Append the one-group rescan for column iCol: the definition re-run,
** filtered to the row's own group -- WHERE-pushdown turns the filter
** into a base seek when the advised index exists.  Newlines around the
** definition text keep a trailing -- comment from swallowing the
** wrapper.  Key-less views have exactly one group; the filter is 1.
*/
static void mviewAppendRescan(
  sqlite3_str *pStr,
  const MViewInfo *pInfo,
  const Table *pView,
  int iCol
){
  int i, n = 0;
  sqlite3_str_appendf(pStr, "(SELECT r.\"%w\" FROM (\n%s\n) AS r WHERE ",
                      pView->aCol[iCol].zCnName, pInfo->zSelDef);
  for(i=0; i<pInfo->nCol; i++){
    if( pInfo->aColKind[i]!=MVIEW_COL_KEY ) continue;
    sqlite3_str_appendf(pStr, "%sr.\"%w\" IS \"%w\".\"%w\"",
       n++ ? " AND " : "", pView->aCol[i].zCnName,
       pView->zName, pView->aCol[i].zCnName);
  }
  if( n==0 ) sqlite3_str_append(pStr, "1", 1);
  sqlite3_str_append(pStr, ")", 1);
}

/*
** Append the SET list combining the view's current row with delta row
** d, adding (cSign='+') or subtracting (cSign='-').  Every view-side
** reference is qualified: d carries the same column names.
**
** MIN/MAX columns (Tier 2): the add side is a comparison carrying the
** captured collation explicitly; the subtract side is the lazy rescan
** -- evaluated ONLY when the departing value IS (binary) the stored
** extremum, which is correct under any collation: in a consistent
** state every group value >= the stored extremum under the aggregate's
** collation, so a binary-distinct departure leaves the row that
** produced the stored value in place.  With bFold (the deferred fold)
** extremum columns are SKIPPED here entirely; view_refresh repairs
** them per touched group afterwards.
*/
static void mviewAppendCombine(
  sqlite3_str *pStr,
  const MViewInfo *pInfo,
  const Table *pView,
  char cSign,
  int bFold
){
  const char *zV = pView->zName;
  int i, n = 0;
  for(i=0; i<pInfo->nCol; i++){
    const char *zC = pView->aCol[i].zCnName;
    switch( pInfo->aColKind[i] ){
      case MVIEW_COL_KEY:
        break;
      case MVIEW_COL_MIN:
      case MVIEW_COL_MAX:
        if( bFold ) break;
        if( cSign=='+' ){
          sqlite3_str_appendf(pStr,
             "%s\"%w\" = CASE WHEN d.\"%w\" IS NULL THEN \"%w\".\"%w\""
             " WHEN \"%w\".\"%w\" IS NULL THEN d.\"%w\""
             " WHEN d.\"%w\" %c \"%w\".\"%w\" COLLATE %s THEN d.\"%w\""
             " ELSE \"%w\".\"%w\" END",
             n++ ? ", " : "", zC, zC, zV, zC,
             zV, zC, zC,
             zC, pInfo->aColKind[i]==MVIEW_COL_MIN ? '<' : '>',
             zV, zC, pInfo->azColl[i], zC,
             zV, zC);
        }else{
          sqlite3_str_appendf(pStr,
             "%s\"%w\" = CASE WHEN d.\"%w\" IS NOT NULL"
             " AND d.\"%w\" IS \"%w\".\"%w\" THEN ",
             n++ ? ", " : "", zC, zC, zC, zV, zC);
          mviewAppendRescan(pStr, pInfo, pView, i);
          sqlite3_str_appendf(pStr, " ELSE \"%w\".\"%w\" END", zV, zC);
        }
        break;
      case MVIEW_COL_COUNT0:
      case MVIEW_COL_COUNT:
      case MVIEW_COL_TOTAL:
        sqlite3_str_appendf(pStr,
           "%s\"%w\" = \"%w\".\"%w\" %c d.\"%w\"",
           n++ ? ", " : "", zC, zV, zC, cSign, zC);
        break;
      case MVIEW_COL_SUM:
        sqlite3_str_appendf(pStr,
           "%s\"%w\" = CASE WHEN \"%w\".\"ivm$n$%d\" %c d.\"ivm$n$%d\" = 0"
           " THEN NULL ELSE coalesce(\"%w\".\"%w\",0) %c"
           " coalesce(d.\"%w\",0) END"
           ", \"ivm$n$%d\" = \"%w\".\"ivm$n$%d\" %c d.\"ivm$n$%d\"",
           n++ ? ", " : "", zC, zV, i, cSign, i,
           zV, zC, cSign, zC,
           i, zV, i, cSign, i);
        break;
      case MVIEW_COL_AVG:
        sqlite3_str_appendf(pStr,
           "%s\"%w\" = CASE WHEN \"%w\".\"ivm$n$%d\" %c d.\"ivm$n$%d\" = 0"
           " THEN NULL ELSE"
           " (coalesce(\"%w\".\"ivm$s$%d\",0) %c coalesce(d.\"ivm$s$%d\",0))"
           " / CAST(\"%w\".\"ivm$n$%d\" %c d.\"ivm$n$%d\" AS REAL) END"
           ", \"ivm$s$%d\" = coalesce(\"%w\".\"ivm$s$%d\",0) %c"
           " coalesce(d.\"ivm$s$%d\",0)"
           ", \"ivm$n$%d\" = \"%w\".\"ivm$n$%d\" %c d.\"ivm$n$%d\"",
           n++ ? ", " : "", zC, zV, i, cSign, i,
           zV, i, cSign, i,
           zV, i, cSign, i,
           i, zV, i, cSign, i,
           i, zV, i, cSign, i);
        break;
    }
  }
  if( pInfo->nBase==1 ){
    sqlite3_str_appendf(pStr,
       "%s\"ivm$count\" = \"%w\".\"ivm$count\" %c d.\"ivm$count\"",
       n ? ", " : "", zV, cSign);
  }
}

/*
** Append the key-match predicate "view"."k" IS <alias>."k" AND ... --
** or the constant 1 for a key-less view.
*/
static void mviewAppendKeyMatch(
  sqlite3_str *pStr,
  const MViewInfo *pInfo,
  const Table *pView,
  const char *zAlias
){
  int i, n = 0;
  for(i=0; i<pInfo->nCol; i++){
    if( pInfo->aColKind[i]!=MVIEW_COL_KEY ) continue;
    sqlite3_str_appendf(pStr, "%s\"%w\".\"%w\" IS %s.\"%w\"",
       n++ ? " AND " : "", pView->zName, pView->aCol[i].zCnName,
       zAlias, pView->aCol[i].zCnName);
  }
  if( n==0 ) sqlite3_str_append(pStr, "1", 1);
}

/*
** Append the two (or one) maintenance steps for one side of a delta:
** bAdd!=0 -> combine-add then INSERT-if-absent (zRow=="NEW");
** bAdd==0 -> combine-subtract then keyed group-death (zRow=="OLD").
*/
static void mviewAppendSteps(
  sqlite3_str *pStr,
  const MViewInfo *pInfo,
  const Table *pView,
  const Table *pBase,
  int bAdd,
  int nKey
){
  const char *zRow = bAdd ? "NEW" : "OLD";
  int i;

  if( pInfo->bDeferred ){
    /* Deferred: the same delta, APPENDED to the log with its weight
    ** instead of applied.  PRAGMA view_refresh folds the log later. */
    sqlite3_str_appendf(pStr, "INSERT INTO \"sqlite_ivm_%w_log\"(",
                        pView->zName);
    for(i=0; i<pView->nCol; i++){
      sqlite3_str_appendf(pStr, "%s\"%w\"", i ? "," : "",
                          pView->aCol[i].zCnName);
    }
    sqlite3_str_appendf(pStr, ",\"ivm$w\") SELECT dd.*, %d FROM ",
                        bAdd ? 1 : -1);
    mviewAppendDelta(pStr, pInfo, pView, pBase, zRow);
    sqlite3_str_appendall(pStr, " AS dd; ");
    return;
  }

  sqlite3_str_appendf(pStr, "UPDATE \"%w\" SET ", pView->zName);
  mviewAppendCombine(pStr, pInfo, pView, bAdd ? '+' : '-', 0);
  sqlite3_str_appendall(pStr, " FROM ");
  mviewAppendDelta(pStr, pInfo, pView, pBase, zRow);
  sqlite3_str_appendall(pStr, " AS d WHERE ");
  mviewAppendKeyMatch(pStr, pInfo, pView, "d");
  sqlite3_str_appendall(pStr, "; ");

  if( bAdd ){
    if( nKey>0 ){
      sqlite3_str_appendf(pStr, "INSERT INTO \"%w\"(", pView->zName);
      for(i=0; i<pView->nCol; i++){
        sqlite3_str_appendf(pStr, "%s\"%w\"", i ? "," : "",
                            pView->aCol[i].zCnName);
      }
      sqlite3_str_appendall(pStr, ") SELECT * FROM ");
      mviewAppendDelta(pStr, pInfo, pView, pBase, zRow);
      sqlite3_str_appendall(pStr,
         " AS dd WHERE NOT EXISTS(SELECT 1 FROM ");
      sqlite3_str_appendf(pStr, "\"%w\" WHERE ", pView->zName);
      mviewAppendKeyMatch(pStr, pInfo, pView, "dd");
      sqlite3_str_appendall(pStr, "); ");
    }
    /* key-less: the single group row has existed since population and
    ** the UPDATE above always finds it */
  }else{
    if( nKey>0 ){
      sqlite3_str_appendf(pStr,
         "DELETE FROM \"%w\" WHERE \"%w\".\"%w\"<=0",
         pView->zName, pView->zName, mviewLiveName(pInfo, pView));
      for(i=0; i<pInfo->nCol; i++){
        if( pInfo->aColKind[i]!=MVIEW_COL_KEY ) continue;
        sqlite3_str_appendf(pStr, " AND \"%w\".\"%w\" IS (SELECT \"%w\" FROM ",
           pView->zName, pView->aCol[i].zCnName, pView->aCol[i].zCnName);
        mviewAppendDelta(pStr, pInfo, pView, pBase, zRow);
        sqlite3_str_appendall(pStr, " AS dx)");
      }
      sqlite3_str_appendall(pStr, "; ");
    }
    /* key-less: the single group row never dies (GROUP BY-less
    ** aggregates yield one row even over an empty table) */
  }
}

/*
** Build the CREATE TRIGGER text for one maintenance trigger.
** op: 0=INSERT, 1=DELETE, 2=UPDATE.  Caller frees.
*/
static char *mviewBuildTriggerSql(
  sqlite3 *db,
  const MViewInfo *pInfo,
  const Table *pView,
  const Table *pBase,
  const char *zDbName,
  int iBase,                /* pBase's ordinal: unique trigger names */
  int op
){
  static const char *azOp[] = { "INSERT", "DELETE", "UPDATE" };
  static const char *azSuf[] = { "ins", "del", "upd" };
  sqlite3_str *pStr = sqlite3_str_new(db);
  int i, nKey = 0;
  for(i=0; i<pInfo->nCol; i++){
    if( pInfo->aColKind[i]==MVIEW_COL_KEY ) nKey++;
  }
  sqlite3_str_appendf(pStr,
     "CREATE TRIGGER \"%w\".\"ivm$%w$%d$%s\" AFTER %s ON \"%w\" BEGIN ",
     zDbName, pView->zName, iBase, azSuf[op], azOp[op], pBase->zName);
  if( op!=0 ){  /* DELETE and UPDATE subtract the OLD row */
    mviewAppendSteps(pStr, pInfo, pView, pBase, 0, nKey);
  }
  if( op!=1 ){  /* INSERT and UPDATE add the NEW row */
    mviewAppendSteps(pStr, pInfo, pView, pBase, 1, nKey);
  }
  sqlite3_str_appendall(pStr, "END");
  return sqlite3_str_finish(pStr);
}

/*
** Compile one synthesized CREATE TRIGGER through a private sub-parse.
** pParse->bMViewTrigSynth makes sqlite3FinishTrigger() hand the built
** object back instead of persisting it (see trigger.c).  The
** authorizer is suspended: these are engine internals, like OP_SqlExec
** with the 0x0001 flag.  Returns the Trigger or 0 with an error left
** in pOuter.
*/
static Trigger *mviewParseTrigger(Parse *pOuter, const char *zSql){
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
    /* Statement-completion codegen allocates a Vdbe even for a parse
    ** that emits nothing.  Left unfinalized it would pin the whole
    ** connection open as a zombie at close. */
    sqlite3VdbeFinalize(sParse.pVdbe);
    sParse.pVdbe = 0;
  }
  if( pRet==0 && pOuter->nErr==0 ){
    sqlite3ErrorMsg(pOuter,
       "internal error: maintenance trigger for a materialized view "
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
** Ensure the maintenance triggers for every EAGER materialized view
** over pTab exist and are linked into pTab's trigger list.  Called
** lazily from sqlite3TriggerList() -- by the time any statement wants
** the base table's triggers, the whole schema is loaded and the
** registry is populated.  Idempotent; schema mutex is held by the
** caller's caller.  Never runs during schema load or inside its own
** sub-parse.
*/
void sqlite3MViewSynthTriggers(Parse *pParse, Table *pTab){
  sqlite3 *db = pParse->db;
  HashElem *he;
  Schema *pSchema = pTab->pSchema;
  int iDb;
  if( db->init.busy || pParse->bMViewTrigSynth ) return;
  if( pParse->nErr ) return;
  /* During VACUUM the rebuilt views receive their content by an
  ** explicit engine copy (see vacuum.c); maintenance triggers on the
  ** vacuum-side base tables would apply every delta twice. */
  if( db->mDbFlags & DBFLAG_Vacuum ) return;
  iDb = sqlite3SchemaToIndex(db, pSchema);
  if( NEVER(iDb<0) ) return;
  for(he=sqliteHashFirst(&pSchema->mviewHash); he;
      he=sqliteHashNext(he)){
    MViewInfo *pInfo = (MViewInfo*)sqliteHashData(he);
    Table *pView;
    int k, kb, iBase = -1;
    if( pInfo->bTrigBuilt ) continue;      /* every base's set built */
    for(kb=0; kb<pInfo->nBase; kb++){
      if( sqlite3_stricmp(pInfo->azBase[kb], pTab->zName)==0 ){
        iBase = kb;
        break;
      }
    }
    if( iBase<0 ) continue;
    if( pInfo->apTrig[3*iBase]!=0 ) continue;  /* this base's set built */
    pView = sqlite3FindTable(db, pInfo->zName, db->aDb[iDb].zDbSName);
    if( NEVER(pView==0) ) continue;
    for(k=0; k<3; k++){
      char *zSql = mviewBuildTriggerSql(db, pInfo, pView, pTab,
                                        db->aDb[iDb].zDbSName, iBase, k);
      if( zSql==0 ){
        sqlite3OomFault(db);
        return;
      }
      pInfo->apTrig[3*iBase+k] = mviewParseTrigger(pParse, zSql);
      sqlite3_free(zSql);
      if( pInfo->apTrig[3*iBase+k]==0 ) return;
      pInfo->apTrig[3*iBase+k]->bMViewMaint = 1;
    }
    /* All three built: link them into THIS base's list so every DML
    ** compile sees them (and so the xfer/truncate fast paths disable
    ** themselves, which is a correctness dependency here).  Other
    ** bases' sets build when their own first DML asks. */
    for(k=0; k<3; k++){
      pInfo->apTrig[3*iBase+k]->pNext = pTab->pTrigger;
      pTab->pTrigger = pInfo->apTrig[3*iBase+k];
    }
    for(kb=0; kb<pInfo->nBase; kb++){
      if( pInfo->apTrig[3*kb]==0 ) break;
    }
    if( kb>=pInfo->nBase ) pInfo->bTrigBuilt = 1;
  }
}

/*
** Append the fold source for a deferred view's refresh: the delta log
** grouped by the view's keys, every column weighted by ivm$w, shaped
** EXACTLY like the view's column layout so both the UPDATE-combine
** (which reads the bookkeeping parts) and the INSERT-if-absent (which
** takes the row whole) can use it as their d.
**
** HAVING count(*)>0 exists for the key-less case: a whole-log
** aggregate over an EMPTY log would produce one all-NULL row, and
** NULL folded into a count is not zero, it is corruption.
*/
static void mviewAppendFoldSource(
  sqlite3_str *pStr,
  const MViewInfo *pInfo,
  const Table *pView,
  const char *zDbName
){
  int i, nKey = 0;
  sqlite3_str_appendall(pStr, "(SELECT ");
  for(i=0; i<pInfo->nCol; i++){
    const char *zC = pView->aCol[i].zCnName;
    if( i ) sqlite3_str_append(pStr, ",", 1);
    switch( pInfo->aColKind[i] ){
      case MVIEW_COL_KEY:
        sqlite3_str_appendf(pStr, "\"%w\"", zC);
        nKey++;
        break;
      case MVIEW_COL_COUNT0:
      case MVIEW_COL_COUNT:
      case MVIEW_COL_TOTAL:
        sqlite3_str_appendf(pStr,
           "sum(\"ivm$w\"*\"%w\") AS \"%w\"", zC, zC);
        break;
      case MVIEW_COL_SUM:
        sqlite3_str_appendf(pStr,
           "CASE WHEN sum(\"ivm$w\"*\"ivm$n$%d\")=0 THEN NULL"
           " ELSE sum(\"ivm$w\"*coalesce(\"%w\",0)) END AS \"%w\"",
           i, zC, zC);
        break;
      case MVIEW_COL_MIN:
      case MVIEW_COL_MAX:
        /* A placeholder from the inserted values (the log column
        ** carries the captured collation, so min/max compare right);
        ** the repair pass overwrites this for every touched group, so
        ** it only ever seeds brand-new rows for one statement. */
        sqlite3_str_appendf(pStr,
           "%s(CASE WHEN \"ivm$w\">0 THEN \"%w\" END) AS \"%w\"",
           pInfo->aColKind[i]==MVIEW_COL_MIN ? "min" : "max", zC, zC);
        break;
      case MVIEW_COL_AVG:
        sqlite3_str_appendf(pStr,
           "CASE WHEN sum(\"ivm$w\"*\"ivm$n$%d\")=0 THEN NULL"
           " ELSE sum(\"ivm$w\"*coalesce(\"ivm$s$%d\",0))"
           "/CAST(sum(\"ivm$w\"*\"ivm$n$%d\") AS REAL) END AS \"%w\"",
           i, i, i, zC);
        break;
    }
  }
  /* The hidden columns, in the same order they were appended to the
  ** view: per-column parts ascending, then ivm$count.  Join views have
  ** no hidden columns; their liveness count(*) folded above with the
  ** other visible columns. */
  if( pInfo->nBase==1 ){
    for(i=0; i<pInfo->nCol; i++){
      if( pInfo->aColKind[i]==MVIEW_COL_SUM ){
        sqlite3_str_appendf(pStr,
           ",sum(\"ivm$w\"*\"ivm$n$%d\") AS \"ivm$n$%d\"", i, i);
      }else if( pInfo->aColKind[i]==MVIEW_COL_AVG ){
        sqlite3_str_appendf(pStr,
           ",sum(\"ivm$w\"*coalesce(\"ivm$s$%d\",0)) AS \"ivm$s$%d\""
           ",sum(\"ivm$w\"*\"ivm$n$%d\") AS \"ivm$n$%d\"", i, i, i, i);
      }
    }
    sqlite3_str_appendall(pStr,
       ",sum(\"ivm$w\"*\"ivm$count\") AS \"ivm$count\"");
  }
  sqlite3_str_appendf(pStr, " FROM \"%w\".\"sqlite_ivm_%w_log\"",
                      zDbName, pView->zName);
  if( nKey>0 ){
    int n = 0;
    sqlite3_str_appendall(pStr, " GROUP BY ");
    for(i=0; i<pInfo->nCol; i++){
      if( pInfo->aColKind[i]!=MVIEW_COL_KEY ) continue;
      sqlite3_str_appendf(pStr, "%s\"%w\"", n++ ? "," : "",
                          pView->aCol[i].zCnName);
    }
  }
  sqlite3_str_appendall(pStr, " HAVING count(*)>0)");
}

/*
** Generate code for PRAGMA view_refresh on one DEFERRED materialized
** view: fold the delta log into the stored rows, grouped -- the whole
** point of deferral is that N deltas against one group cost one
** application -- then clear the log.  All inside the pragma's own
** statement, so the fold is atomic with its log truncation.  Eager
** views have nothing pending by construction and refresh as a no-op.
*/
void sqlite3MViewCodeRefresh(
  Parse *pParse,
  const char *zDbName,
  const char *zView
){
  sqlite3 *db = pParse->db;
  MViewInfo *pInfo = 0;
  Table *pView;
  sqlite3_str *pStr;
  char *zSql;
  int i, ii, nKey = 0;

  for(ii=0; ii<db->nDb && pInfo==0; ii++){
    Schema *pSchema = db->aDb[ii].pSchema;
    if( pSchema==0 ) continue;
    if( sqlite3StrICmp(zDbName, db->aDb[ii].zDbSName)!=0 ) continue;
    pInfo = (MViewInfo*)sqlite3HashFind(&pSchema->mviewHash, zView);
  }
  if( pInfo==0 ){
    sqlite3ErrorMsg(pParse, "no such materialized view: %s", zView);
    return;
  }
  if( !pInfo->bDeferred ) return;   /* eager: nothing is ever pending */
  pView = sqlite3FindTable(db, zView, zDbName);
  if( NEVER(pView==0) ) return;
  for(i=0; i<pInfo->nCol; i++){
    if( pInfo->aColKind[i]==MVIEW_COL_KEY ) nKey++;
  }

  /* Step 1: combine the folded deltas into existing groups */
  pStr = sqlite3_str_new(db);
  sqlite3_str_appendf(pStr, "UPDATE \"%w\".\"%w\" SET ",
                      zDbName, pView->zName);
  mviewAppendCombine(pStr, pInfo, pView, '+', 1);
  sqlite3_str_appendall(pStr, " FROM ");
  mviewAppendFoldSource(pStr, pInfo, pView, zDbName);
  sqlite3_str_appendall(pStr, " AS d WHERE ");
  mviewAppendKeyMatch(pStr, pInfo, pView, "d");
  zSql = sqlite3_str_finish(pStr);
  if( zSql==0 ){ sqlite3OomFault(db); return; }
  sqlite3NestedParse(pParse, "%s", zSql);
  sqlite3_free(zSql);
  if( pParse->nErr ) return;

  /* Step 2: groups the fold knows and the view does not */
  if( nKey>0 ){
    pStr = sqlite3_str_new(db);
    sqlite3_str_appendf(pStr, "INSERT INTO \"%w\".\"%w\"(",
                        zDbName, pView->zName);
    for(i=0; i<pView->nCol; i++){
      sqlite3_str_appendf(pStr, "%s\"%w\"", i ? "," : "",
                          pView->aCol[i].zCnName);
    }
    sqlite3_str_appendall(pStr, ") SELECT dd.* FROM ");
    mviewAppendFoldSource(pStr, pInfo, pView, zDbName);
    sqlite3_str_appendall(pStr,
       " AS dd WHERE NOT EXISTS(SELECT 1 FROM ");
    sqlite3_str_appendf(pStr, "\"%w\".\"%w\" WHERE ",
                        zDbName, pView->zName);
    mviewAppendKeyMatch(pStr, pInfo, pView, "dd");
    sqlite3_str_appendall(pStr, ")");
    zSql = sqlite3_str_finish(pStr);
    if( zSql==0 ){ sqlite3OomFault(db); return; }
    sqlite3NestedParse(pParse, "%s", zSql);
    sqlite3_free(zSql);
    if( pParse->nErr ) return;

    /* Step 3: groups whose liveness count reached zero */
    sqlite3NestedParse(pParse,
       "DELETE FROM \"%w\".\"%w\" WHERE \"%w\"<=0",
       zDbName, pView->zName, mviewLiveName(pInfo, pView));
    if( pParse->nErr ) return;
  }

  /* Step 3b (Tier 2): a fold has no inverse for MIN/MAX, so extremum
  ** columns are REPAIRED per touched group -- one definition re-run
  ** each, filtered to the group, against the base as it now stands.
  ** Runs before the log truncation because "touched" is read from it. */
  {
    int nExt = 0;
    for(i=0; i<pInfo->nCol; i++){
      if( pInfo->aColKind[i]==MVIEW_COL_MIN
       || pInfo->aColKind[i]==MVIEW_COL_MAX ) nExt++;
    }
    if( nExt>0 ){
      char *zSql;
      int n = 0;
      pStr = sqlite3_str_new(db);
      sqlite3_str_appendf(pStr, "UPDATE \"%w\".\"%w\" SET ",
                          zDbName, pView->zName);
      for(i=0; i<pInfo->nCol; i++){
        if( pInfo->aColKind[i]!=MVIEW_COL_MIN
         && pInfo->aColKind[i]!=MVIEW_COL_MAX ) continue;
        sqlite3_str_appendf(pStr, "%s\"%w\" = ", n++ ? ", " : "",
                            pView->aCol[i].zCnName);
        mviewAppendRescan(pStr, pInfo, pView, i);
      }
      sqlite3_str_appendf(pStr,
         " WHERE EXISTS(SELECT 1 FROM \"%w\".\"sqlite_ivm_%w_log\" AS lg"
         " WHERE ", zDbName, pView->zName);
      {
        int nk = 0;
        for(i=0; i<pInfo->nCol; i++){
          if( pInfo->aColKind[i]!=MVIEW_COL_KEY ) continue;
          sqlite3_str_appendf(pStr, "%slg.\"%w\" IS \"%w\".\"%w\"",
             nk++ ? " AND " : "", pView->aCol[i].zCnName,
             pView->zName, pView->aCol[i].zCnName);
        }
        if( nk==0 ) sqlite3_str_append(pStr, "1", 1);
      }
      sqlite3_str_append(pStr, ")", 1);
      zSql = sqlite3_str_finish(pStr);
      if( zSql==0 ){ sqlite3OomFault(db); return; }
      sqlite3NestedParse(pParse, "%s", zSql);
      sqlite3_free(zSql);
      if( pParse->nErr ) return;
    }
  }

  /* Step 4: the folded deltas are applied; the log empties with them */
  sqlite3NestedParse(pParse,
     "DELETE FROM \"%w\".\"sqlite_ivm_%w_log\"", zDbName, pView->zName);
}

/*
** Append the comma-separated key-column list of pInfo, using the CTE
** column aliases mv$0..mv$N.  If the view has no key columns (a
** whole-table aggregate), append the constant 1: there is exactly one
** group, and SELECT DISTINCT 1 counts it as such.
*/
static void mviewAppendKeyList(sqlite3_str *pStr, const MViewInfo *pInfo){
  int i, n = 0;
  for(i=0; i<pInfo->nCol; i++){
    if( pInfo->aColKind[i]!=MVIEW_COL_KEY ) continue;
    if( n++ ) sqlite3_str_append(pStr, ",", 1);
    sqlite3_str_appendf(pStr, "\"mv$%d\"", i);
  }
  if( n==0 ) sqlite3_str_append(pStr, "1", 1);
}

/*
** Append an expression rendering columns as quote()d text:
**   quote("mv$0")||','||quote("mv$1")...
** With bKeysOnly, only key columns; the caller guarantees at least one
** column is rendered or supplies its own fallback.
*/
static void mviewAppendRowRender(
  sqlite3_str *pStr,
  const MViewInfo *pInfo,
  int bKeysOnly
){
  int i, n = 0;
  for(i=0; i<pInfo->nCol; i++){
    if( bKeysOnly && pInfo->aColKind[i]!=MVIEW_COL_KEY ) continue;
    if( n++ ) sqlite3_str_appendall(pStr, "||','||");
    sqlite3_str_appendf(pStr, "quote(\"mv$%d\")", i);
  }
  if( n==0 ) sqlite3_str_appendall(pStr, "NULL");
}

/*
** The index advisory (Tier 2 / PLAN-IVM2 P4).  A MIN/MAX view's
** qualifying-delete rescan re-runs the definition for one group;
** WHERE-pushdown turns that into a base seek exactly when an index
** leads with the view's key columns (ideally followed by the extremum
** arguments).  If the view has extremum columns and no such index
** exists, return the runnable CREATE INDEX statement (caller frees);
** return 0 when no advisory applies -- indexed already, no extremum
** columns, or an expression key that no index can serve.
*/
static char *mviewAdvisory(
  sqlite3 *db,
  const MViewInfo *pInfo,
  Table *pBase
){
  const char *azNeed[64];   /* key base cols, then extremum arg cols */
  int nKey = 0, nNeed = 0;
  int i, j;
  Index *pIdx;
  sqlite3_str *pStr;

  for(i=0; i<pInfo->nCol; i++){
    if( pInfo->aColKind[i]==MVIEW_COL_KEY ){
      if( pInfo->azBaseCol[i]==0 ) return 0;  /* expression key */
      if( nNeed>=(int)ArraySize(azNeed) ) return 0;
      azNeed[nNeed++] = pInfo->azBaseCol[i];
      nKey++;
    }
  }
  for(i=0; i<pInfo->nCol; i++){
    if( pInfo->aColKind[i]==MVIEW_COL_MIN
     || pInfo->aColKind[i]==MVIEW_COL_MAX ){
      int dup = 0;
      assert( pInfo->azBaseCol[i]!=0 );
      for(j=0; j<nNeed; j++){
        if( sqlite3StrICmp(azNeed[j], pInfo->azBaseCol[i])==0 ) dup = 1;
      }
      if( !dup ){
        if( nNeed>=(int)ArraySize(azNeed) ) return 0;
        azNeed[nNeed++] = pInfo->azBaseCol[i];
      }
    }
  }
  if( nNeed==nKey ) return 0;   /* no extremum columns: nothing to advise */

  /* Served already?  An index whose leading nKey columns cover the keys
  ** as a set (key-less views: whose first column is the first argument
  ** column) makes the rescan a seek. */
  for(pIdx=pBase->pIndex; pIdx; pIdx=pIdx->pNext){
    if( nKey==0 ){
      if( pIdx->nKeyCol>=1 && pIdx->aiColumn[0]>=0
       && sqlite3StrICmp(pBase->aCol[pIdx->aiColumn[0]].zCnName,
                         azNeed[0])==0 ){
        return 0;
      }
    }else if( pIdx->nKeyCol>=nKey ){
      int nHit = 0;
      for(i=0; i<nKey; i++){
        for(j=0; j<nKey; j++){
          if( pIdx->aiColumn[j]>=0
           && sqlite3StrICmp(pBase->aCol[pIdx->aiColumn[j]].zCnName,
                             azNeed[i])==0 ){
            nHit++;
            break;
          }
        }
      }
      if( nHit==nKey ) return 0;
    }
  }

  pStr = sqlite3_str_new(db);
  sqlite3_str_appendf(pStr, "CREATE INDEX \"ivmadv$%w\" ON \"%w\"(",
                      pInfo->zName, pBase->zName);
  for(i=0; i<nNeed; i++){
    sqlite3_str_appendf(pStr, "%s\"%w\"", i ? "," : "", azNeed[i]);
  }
  sqlite3_str_append(pStr, ")", 1);
  return sqlite3_str_finish(pStr);
}

/*
** Generate code for PRAGMA view_check on one materialized view: the
** stored rows diffed against a fresh recompute of the definition, BOTH
** SIDES READ INSIDE THIS ONE STATEMENT, so they share a snapshot.
**
** Output rows (view, kind, subject, detail), in kind order
** stale < diff < summary:
**   kind='diff'    -- one row per group present on only one side or
**                     disagreeing in an aggregate; subject = the group
**                     key, detail names the side and renders the row
**   kind='summary' -- ALWAYS EMITTED, LAST: 'N groups compared, M
**                     differ', so an empty diff over zero compared
**                     groups reads as the finding it is, never as a
**                     clean bill
** (kind='stale' is reserved for deferred views with pending deltas --
** a later phase; the ORDER BY already knows its place.)
**
** The SQL is compiled via sqlite3NestedParse into the pragma's own
** program: a nested SELECT's result rows are the pragma's result rows.
*/
void sqlite3MViewCodeCheck(
  Parse *pParse,
  Vdbe *v,
  const char *zDbName,    /* Schema holding the view */
  const char *zView       /* View name (known to be an mview) */
){
  sqlite3 *db = pParse->db;
  MViewInfo *pInfo = 0;
  sqlite3_str *pStr;
  char *zSql;
  int i, ii;

  UNUSED_PARAMETER(v);
  for(ii=0; ii<db->nDb && pInfo==0; ii++){
    Schema *pSchema = db->aDb[ii].pSchema;
    if( pSchema==0 ) continue;
    if( sqlite3StrICmp(zDbName, db->aDb[ii].zDbSName)!=0 ) continue;
    pInfo = (MViewInfo*)sqlite3HashFind(&pSchema->mviewHash, zView);
  }
  if( pInfo==0 ){
    sqlite3ErrorMsg(pParse, "no such materialized view: %s", zView);
    return;
  }

  pStr = sqlite3_str_new(db);

  /* WITH fresh(mv$0,..) AS (<definition>),
  **      stored(mv$0,..) AS (SELECT <cols> FROM <view>),
  **      gone  AS (fresh EXCEPT stored),   -- recomputed, not stored
  **      extra AS (stored EXCEPT fresh)    -- stored, not recomputed
  ** EXCEPT compares NULLs as equal, which is the wanted semantics for
  ** aggregate columns. */
  sqlite3_str_appendall(pStr, "WITH fresh(");
  for(i=0; i<pInfo->nCol; i++){
    sqlite3_str_appendf(pStr, "%s\"mv$%d\"", i ? "," : "", i);
  }
  sqlite3_str_appendf(pStr, ") AS (%s), stored(", pInfo->zSelDef);
  for(i=0; i<pInfo->nCol; i++){
    sqlite3_str_appendf(pStr, "%s\"mv$%d\"", i ? "," : "", i);
  }
  sqlite3_str_appendall(pStr, ") AS (SELECT ");
  {
    Table *pTab = sqlite3FindTable(db, zView, zDbName);
    if( NEVER(pTab==0) || NEVER(pTab->nCol<pInfo->nCol) ){
      sqlite3ErrorMsg(pParse, "materialized view %s could not be read",
                      zView);
      sqlite3_free(sqlite3_str_finish(pStr));
      return;
    }
    for(i=0; i<pInfo->nCol; i++){
      sqlite3_str_appendf(pStr, "%s\"%w\"", i ? "," : "",
                          pTab->aCol[i].zCnName);
    }
  }
  sqlite3_str_appendf(pStr, " FROM \"%w\".\"%w\"), ", zDbName, zView);
  sqlite3_str_appendall(pStr,
     "gone AS (SELECT * FROM fresh EXCEPT SELECT * FROM stored), "
     "extra AS (SELECT * FROM stored EXCEPT SELECT * FROM fresh) "
     "SELECT \"view\", kind, subject, detail FROM (");

  /* A deferred view with pending deltas is declared STALE before the
  ** measurement is reported: the reader learns first that the diffs
  ** below have a known, fixable cause.  The ORDER BY sorts it first. */
  if( pInfo->bDeferred ){
    sqlite3_str_appendf(pStr,
       "SELECT %Q AS \"view\", 'stale' AS kind, NULL AS subject, "
       "(SELECT count(*) FROM \"%w\".\"sqlite_ivm_%w_log\")"
       "||' deltas pending; run PRAGMA view_refresh(''%q'')' AS detail "
       "WHERE EXISTS(SELECT 1 FROM \"%w\".\"sqlite_ivm_%w_log\") "
       "UNION ALL ",
       zView, zDbName, zView, zView, zDbName, zView);
  }

  /* Rows the recompute has and the stored table lacks (or disagrees) */
  sqlite3_str_appendf(pStr,
     "SELECT %Q AS \"view\", 'diff' AS kind, ", zView);
  mviewAppendRowRender(pStr, pInfo, 1);
  sqlite3_str_appendall(pStr,
     " AS subject, 'recomputed but not stored: ('||");
  mviewAppendRowRender(pStr, pInfo, 0);
  sqlite3_str_appendall(pStr, "||')' AS detail FROM gone UNION ALL ");

  /* Rows the stored table has and the recompute lacks (or disagrees) */
  sqlite3_str_appendf(pStr, "SELECT %Q, 'diff', ", zView);
  mviewAppendRowRender(pStr, pInfo, 1);
  sqlite3_str_appendall(pStr, ", 'stored but not recomputed: ('||");
  mviewAppendRowRender(pStr, pInfo, 0);
  sqlite3_str_appendall(pStr, "||')' FROM extra UNION ALL ");

  /* The mandatory coverage summary */
  sqlite3_str_appendf(pStr,
     "SELECT %Q, 'summary', NULL, "
     "(SELECT count(*) FROM (SELECT ", zView);
  mviewAppendKeyList(pStr, pInfo);
  sqlite3_str_appendall(pStr, " FROM fresh UNION SELECT ");
  mviewAppendKeyList(pStr, pInfo);
  sqlite3_str_appendall(pStr,
     " FROM stored))||' groups compared, '||"
     "(SELECT count(*) FROM (SELECT ");
  mviewAppendKeyList(pStr, pInfo);
  sqlite3_str_appendall(pStr, " FROM gone UNION SELECT ");
  mviewAppendKeyList(pStr, pInfo);
  sqlite3_str_appendall(pStr, " FROM extra))||' differ' ");

  /* Tier 2: the index advisory rides the same surface, last -- fires
  ** while a maintenance probe or rescan would scan, silent once the
  ** index exists (this compile-time detection expires with the schema
  ** cookie, so a CREATE INDEX makes the very next check clean).
  **
  ** Single-base views advise the rescan index (keys, then extremum
  ** arguments).  Join views advise the JOIN-PROBE indexes: one row per
  ** equality-condition column not already the btree key or the leading
  ** column of some index -- each base's delta select probes the others
  ** through exactly these.  (The rescan advisory is not attempted
  ** across tables: no advisory rather than a guessed one.) */
  if( pInfo->nBase==1 ){
    Table *pBase = sqlite3FindTable(db, pInfo->azBase[0], zDbName);
    char *zAdv = pBase ? mviewAdvisory(db, pInfo, pBase) : 0;
    if( zAdv ){
      sqlite3_str_appendf(pStr,
         "UNION ALL SELECT %Q, 'advisory', NULL, %Q ", zView, zAdv);
      sqlite3_free(zAdv);
    }
  }else{
    int m;
    for(m=0; m<pInfo->nProbe; m++){
      Table *pBase = sqlite3FindTable(db,
                        pInfo->azBase[pInfo->aProbeBase[m]], zDbName);
      const char *zCol = pInfo->azProbeCol[m];
      Index *pIdx;
      int served = 0;
      if( pBase==0 ) continue;
      if( pBase->iPKey>=0
       && sqlite3StrICmp(pBase->aCol[pBase->iPKey].zCnName, zCol)==0 ){
        served = 1;
      }
      for(pIdx=pBase->pIndex; pIdx && !served; pIdx=pIdx->pNext){
        if( pIdx->nKeyCol>=1 && pIdx->aiColumn[0]>=0
         && sqlite3StrICmp(pBase->aCol[pIdx->aiColumn[0]].zCnName,
                           zCol)==0 ){
          served = 1;
        }
      }
      if( !served ){
        char *zAdv = sqlite3MPrintf(db,
           "CREATE INDEX \"ivmadv$%w$%d$%w\" ON \"%w\"(\"%w\")",
           pInfo->zName, pInfo->aProbeBase[m], zCol,
           pBase->zName, zCol);
        if( zAdv ){
          sqlite3_str_appendf(pStr,
             "UNION ALL SELECT %Q, 'advisory', NULL, %Q ", zView, zAdv);
          sqlite3DbFree(db, zAdv);
        }
      }
    }
  }

  sqlite3_str_appendall(pStr,
     ") ORDER BY CASE kind WHEN 'stale' THEN 0 WHEN 'diff' THEN 1 "
     "WHEN 'summary' THEN 2 ELSE 3 END, subject");

  zSql = sqlite3_str_finish(pStr);
  if( zSql==0 ){
    sqlite3OomFault(db);
    return;
  }
  sqlite3NestedParse(pParse, "%s", zSql);
  sqlite3_free(zSql);
}

/*
** Generate code for PRAGMA view_list: one row per materialized view --
** (name, maintenance, pending, stale).  pending and stale are NULL
** until a capture mechanism exists to measure them: unmeasured is
** spelled NULL, never a reassuring zero.  Sorted by name within each
** schema so the output is stable rather than hash-ordered.
*/
void sqlite3MViewCodeList(Parse *pParse, Vdbe *v, const char *zDb){
  sqlite3 *db = pParse->db;
  int ii;
  pParse->nMem = 4;
  for(ii=0; ii<db->nDb; ii++){
    Schema *pSchema = db->aDb[ii].pSchema;
    HashElem *he;
    MViewInfo **apMV;
    int nMV = 0, j, k;
    if( pSchema==0 ) continue;
    if( zDb && sqlite3StrICmp(zDb, db->aDb[ii].zDbSName)!=0 ) continue;
    for(he=sqliteHashFirst(&pSchema->mviewHash); he;
        he=sqliteHashNext(he)) nMV++;
    if( nMV==0 ) continue;
    apMV = sqlite3DbMallocRawNN(db, nMV*sizeof(MViewInfo*));
    if( apMV==0 ) break;
    j = 0;
    for(he=sqliteHashFirst(&pSchema->mviewHash); he;
        he=sqliteHashNext(he)){
      apMV[j++] = (MViewInfo*)sqliteHashData(he);
    }
    for(j=1; j<nMV; j++){
      MViewInfo *pT = apMV[j];
      for(k=j; k>0 && sqlite3StrICmp(apMV[k-1]->zName, pT->zName)>0; k--){
        apMV[k] = apMV[k-1];
      }
      apMV[k] = pT;
    }
    for(j=0; j<nMV; j++){
      if( apMV[j]->bDeferred ){
        /* Deferred: pending IS the delta-log row count, and stale
        ** follows from it -- measured, not assumed */
        sqlite3NestedParse(pParse,
           "SELECT %Q, 'deferred',"
           "(SELECT count(*) FROM \"%w\".\"sqlite_ivm_%w_log\"),"
           "(SELECT count(*) FROM \"%w\".\"sqlite_ivm_%w_log\")>0",
           apMV[j]->zName,
           db->aDb[ii].zDbSName, apMV[j]->zName,
           db->aDb[ii].zDbSName, apMV[j]->zName);
      }else{
        /* Eager: maintained inside every writing statement, so zero
        ** pending and never stale -- true by construction */
        sqlite3VdbeMultiLoad(v, 1, "ssii",
           apMV[j]->zName, "eager", 0, 0);
      }
    }
    sqlite3DbFree(db, apMV);
  }
}

#endif /* !defined(SQLITE_OMIT_VIEW) */
