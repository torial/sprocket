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
** Is pExpr a Tier-1 maintainable aggregate call: COUNT(*), COUNT(x),
** SUM(x), TOTAL(x) or AVG(x), not DISTINCT, over deterministic arguments?
** If yes return 1.  If it is an aggregate but not maintainable, refuse
** with the named reason and return 0.  If it is not an aggregate call at
** all, return 0 silently (caller applies the GROUP BY rule instead).
*/
static int mviewCheckAggregate(MViewWalk *p, Expr *pExpr){
  const char *zFunc;
  if( pExpr->op!=TK_AGG_FUNCTION ) return 0;
  assert( !ExprHasProperty(pExpr, EP_IntValue) );
  zFunc = pExpr->u.zToken;
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
    mviewRefuse(p, "%s() requires rescanning a group on delete "
                   "(Tier 2, not yet supported)", zFunc);
    return 0;
  }
  if( sqlite3_stricmp(zFunc,"count")!=0
   && sqlite3_stricmp(zFunc,"sum")!=0
   && sqlite3_stricmp(zFunc,"total")!=0
   && sqlite3_stricmp(zFunc,"avg")!=0
  ){
    mviewRefuse(p, "%s() is not incrementally maintainable "
                   "(v1 supports count, sum, total, avg)", zFunc);
    return 0;
  }
  /* Arguments must be deterministic (COUNT(*) has none) */
  if( ExprUseXList(pExpr) && pExpr->x.pList ){
    int i;
    for(i=0; i<pExpr->x.pList->nExpr; i++){
      if( mviewCheckDeterministic(p, pExpr->x.pList->a[i].pExpr) ) return 0;
    }
  }
  return p->rc==SQLITE_OK;
}

/*
** Expression-walker: refuse any aggregate that is not Tier-1.  Used for
** HAVING, where aggregates appear nested inside comparisons.
*/
static int mviewHavingExprCb(Walker *pWalker, Expr *pExpr){
  MViewWalk *p = (MViewWalk*)pWalker->u.pMViewWalk;
  if( p->rc!=SQLITE_OK ) return WRC_Abort;
  if( pExpr->op==TK_AGG_FUNCTION ){
    mviewCheckAggregate(p, pExpr);
    return p->rc==SQLITE_OK ? WRC_Prune : WRC_Abort;
  }
  return mviewDetExprCb(pWalker, pExpr);
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
  Select *pSelect          /* The prepared definition */
){
  MViewWalk sWalk;
  SrcItem *pItem;
  Table *pBase;
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

  /* Exactly one base, and it must be an ordinary table */
  assert( pSelect->pSrc!=0 );
  if( pSelect->pSrc->nSrc!=1 ){
    mviewRefuse(&sWalk, "joins are not yet supported (Tier 2)");
    return sWalk.rc;
  }
  pItem = &pSelect->pSrc->a[0];
  if( pItem->fg.isSubquery || pItem->pSTab==0 ){
    mviewRefuse(&sWalk, "subqueries in FROM are not yet supported");
    return sWalk.rc;
  }
  pBase = pItem->pSTab;
  if( IsView(pBase) ){
    mviewRefuse(&sWalk, "the base must be an ordinary table "
                        "(%s is a view)", pBase->zName);
    return sWalk.rc;
  }
  if( IsVirtual(pBase) ){
    mviewRefuse(&sWalk, "the base must be an ordinary table "
                        "(%s is a virtual table)", pBase->zName);
    return sWalk.rc;
  }
  if( IsMView(pBase) ){
    mviewRefuse(&sWalk, "the base must be an ordinary table (%s is "
                        "itself a materialized view; cascading "
                        "maintenance is not yet supported)", pBase->zName);
    return sWalk.rc;
  }

  /* Result columns: each is either a Tier-1 aggregate call or an
  ** expression covered by GROUP BY.  (Bare columns outside GROUP BY are
  ** refused: which row the engine picks for them is unspecified, and a
  ** maintained view may not depend on it.) */
  assert( pSelect->pEList!=0 );
  for(i=0; i<pSelect->pEList->nExpr; i++){
    Expr *pE = pSelect->pEList->a[i].pExpr;
    if( pE->op==TK_AGG_FUNCTION ){
      if( !mviewCheckAggregate(&sWalk, pE) ) return sWalk.rc;
      continue;
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

  /* HAVING: deterministic, and any aggregate inside it must be Tier-1 */
  if( pSelect->pHaving ){
    Walker w;
    memset(&w, 0, sizeof(w));
    w.xExprCallback = mviewHavingExprCb;
    w.xSelectCallback = mviewDetSelectCb;
    w.u.pMViewWalk = &sWalk;
    sqlite3WalkExpr(&w, pSelect->pHaving);
    if( sWalk.rc ) return sWalk.rc;
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
  const char *zBase,       /* Name of the base table */
  int bDeferred,
  Select *pSelect,         /* Resolved definition (for the key flags) */
  const char *zSelDef,     /* Definition SELECT text */
  int nSelDef              /* Length of zSelDef */
){
  MViewInfo *pInfo, *pOld;
  sqlite3 *db = pParse->db;
  sqlite3_int64 nName = sqlite3Strlen30(p->zName)+1;
  sqlite3_int64 nBase = sqlite3Strlen30(zBase)+1;
  int nCol = pSelect->pEList->nExpr;
  int i;
  assert( db->init.busy );
  assert( sqlite3SchemaMutexHeld(db, 0, p->pSchema) );
  /* One allocation: struct, strings, then the key-flag array.  The hash
  ** keys off pInfo->zName rather than the Table's own name so the entry
  ** never points into memory the table teardown frees. */
  pInfo = sqlite3MallocZero(sizeof(MViewInfo)+nName+nBase+nSelDef+1+nCol);
  if( pInfo==0 ){
    sqlite3OomFault(db);
    return;
  }
  pInfo->zName = (char*)&pInfo[1];
  memcpy(pInfo->zName, p->zName, nName);
  pInfo->zBase = pInfo->zName + nName;
  memcpy(pInfo->zBase, zBase, nBase);
  pInfo->zSelDef = pInfo->zBase + nBase;
  memcpy(pInfo->zSelDef, zSelDef, nSelDef);
  pInfo->zSelDef[nSelDef] = 0;
  pInfo->aIsKey = (u8*)(pInfo->zSelDef + nSelDef + 1);
  pInfo->nCol = nCol;
  for(i=0; i<nCol; i++){
    pInfo->aIsKey[i] =
        (u8)(pSelect->pEList->a[i].pExpr->op!=TK_AGG_FUNCTION);
  }
  pInfo->bDeferred = (u8)(bDeferred!=0);
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
** Free every MViewInfo in an mviewHash and reset the hash.  Called from
** the schema-clearing paths in callback.c.
*/
void sqlite3MViewHashClear(Hash *pHash){
  HashElem *pElem;
  for(pElem=sqliteHashFirst(pHash); pElem; pElem=sqliteHashNext(pElem)){
    sqlite3_free(sqliteHashData(pElem));
  }
  sqlite3HashClear(pHash);
}

/*
** Remove one view's registry entry, if present.  Called when the table
** object is unlinked (see sqlite3UnlinkAndDeleteTable in build.c), which
** covers every drop path.
*/
void sqlite3UnlinkAndDeleteMView(sqlite3 *db, int iDb, const char *zName){
  Hash *pHash;
  MViewInfo *pInfo;
  assert( sqlite3SchemaMutexHeld(db, iDb, 0) );
  pHash = &db->aDb[iDb].pSchema->mviewHash;
  pInfo = sqlite3HashInsert(pHash, zName, 0);
  sqlite3_free(pInfo);
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
    if( sqlite3_stricmp(pInfo->zBase, zBase)==0 ){
      return pInfo->zName;
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
  const char *zBase = 0;

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

  /* Tier-1 conformance: everything outside the maintainable subset is
  ** refused by name, with the fix, before anything is written. */
  if( mviewCheckSelect(pParse, p->zName, pSelect) ) goto create_mview_fail;
  assert( pSelect->pSrc->nSrc==1 && pSelect->pSrc->a[0].pSTab!=0 );
  zBase = pSelect->pSrc->a[0].pSTab->zName;

  /* Population: run the definition into the new root page. */
  if( !db->init.busy ){
    if( IN_SPECIAL_PARSE ){
      pParse->rc = SQLITE_ERROR;
      pParse->nErr++;
      goto create_mview_fail;
    }
    mviewCodePopulate(pParse, p, pSelect);
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
    mviewRegister(pParse, p, zBase, (p->tabFlags & TF_MViewDeferred)!=0,
                  pSelect, zSelDef, nSelDef);
  }

create_mview_fail:
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
  }

exit_drop_mview:
  sqlite3SrcListDelete(db, pName);
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
    if( !pInfo->aIsKey[i] ) continue;
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
    if( bKeysOnly && !pInfo->aIsKey[i] ) continue;
    if( n++ ) sqlite3_str_appendall(pStr, "||','||");
    sqlite3_str_appendf(pStr, "quote(\"mv$%d\")", i);
  }
  if( n==0 ) sqlite3_str_appendall(pStr, "NULL");
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
  sqlite3_str_appendall(pStr,
     " FROM extra))||' differ' "
     ") ORDER BY CASE kind WHEN 'stale' THEN 0 WHEN 'diff' THEN 1 "
     "ELSE 2 END, subject");

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
      sqlite3VdbeMultiLoad(v, 1, "ssss",
         apMV[j]->zName,
         apMV[j]->bDeferred ? "deferred" : "eager",
         (const char*)0,      /* pending: unmeasured until capture */
         (const char*)0);     /* stale:   unmeasured until capture */
    }
    sqlite3DbFree(db, apMV);
  }
}

#endif /* !defined(SQLITE_OMIT_VIEW) */
