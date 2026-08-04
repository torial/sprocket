/*
** 2026 July 4
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains the implementation of stored procedures, a fork
** extension to SQLite.  Stored procedures are schema objects (persisted
** as type='proc' rows in the sqlite_schema table) whose bodies are lists
** of SQL statements, executed via the CALL statement.
**
** The implementation deliberately parallels triggers: procedure bodies
** are represented as TriggerStep lists so that the DbFixer and (in a
** later phase) the trigger sub-program codegen machinery can be reused.
*/
#include "sqliteInt.h"

#ifndef SQLITE_OMIT_PROCEDURE

/*
** Append a parameter to a procedure parameter list.  Create the list if
** pList is NULL.  A copy of the name and type tokens is made.  Returns
** the (possibly new) list, or NULL on OOM (after freeing pList).
*/
/*
** Append a NESTED CHILD TABLE column to a result shape:
**
**     comments TABLE(cid INTEGER, body TEXT) KEY(post_id = id)
**
** Validated here, at CREATE time, rather than at CALL: a declaration whose KEY
** names a column the child does not have is an error the author can see while
** writing it.  Depth is limited to one -- a nested table inside a nested table
** is rejected explicitly rather than accepted and mis-handled, because each
** further level would have to be ordered by the full ancestor path and expose
** every ancestor key (PLAN-NESTED.md, "deliberately out of scope").
*/
ProcParamList *sqlite3ProcNestedAppend(
  Parse *pParse,          /* Parsing context */
  ProcParamList *pList,   /* Shape being built, or NULL */
  Token *pName,           /* Name of the nested column */
  ProcParamList *pChild,  /* The child's declared columns */
  Token *pKeyChild,       /* Child side of the correlation */
  Token *pKeyParent       /* Parent side of the correlation */
){
  sqlite3 *db = pParse->db;
  char *zChild = 0;
  int i, bFound = 0;

  if( pChild==0 ) goto nested_err;
  zChild = sqlite3NameFromToken(db, pKeyChild);
  if( zChild==0 ) goto nested_err;

  for(i=0; i<pChild->nParam; i++){
    if( pChild->a[i].pNested ){
      sqlite3ErrorMsg(pParse, "nested tables may not themselves nest: %s",
                      pChild->a[i].zName);
      goto nested_err;
    }
    if( sqlite3StrICmp(pChild->a[i].zName, zChild)==0 ) bFound = 1;
  }
  if( !bFound ){
    sqlite3ErrorMsg(pParse,
        "KEY names %s, which is not a column of the nested table", zChild);
    goto nested_err;
  }

  {
    /* ProcParamAppend dereferences pType (it reads pType->n), so a nested
    ** column -- which has no scalar type -- must pass an empty Token rather
    ** than NULL.  Passing NULL segfaulted, and the crash read as success
    ** because the shell printed nothing. */
    Token noType;
    noType.z = 0;
    noType.n = 0;
    pList = sqlite3ProcParamAppend(pParse, pList, pName, &noType);
  }
  if( pList==0 ) goto nested_err;
  {
    ProcParam *p = &pList->a[pList->nParam-1];
    p->pNested = pChild;
    p->zKeyChild = zChild;
    p->zKeyParent = sqlite3NameFromToken(db, pKeyParent);
    return pList;
  }

nested_err:
  sqlite3DbFree(db, zChild);
  sqlite3ProcParamListDelete(db, pChild);
  sqlite3ProcParamListDelete(db, pList);
  return 0;
}

ProcParamList *sqlite3ProcParamAppend(
  Parse *pParse,        /* Parsing context */
  ProcParamList *pList, /* List to append to, or NULL */
  Token *pName,         /* Parameter name token */
  Token *pType          /* Declared type token; pType->n==0 if none */
){
  sqlite3 *db = pParse->db;
  ProcParam *a;
  if( pList==0 ){
    pList = sqlite3DbMallocZero(db, sizeof(ProcParamList));
    if( pList==0 ) return 0;
  }
  a = sqlite3DbRealloc(db, pList->a, (pList->nParam+1)*sizeof(ProcParam));
  if( a==0 ){
    sqlite3ProcParamListDelete(db, pList);
    return 0;
  }
  pList->a = a;
  a = &pList->a[pList->nParam];
  memset(a, 0, sizeof(ProcParam));
  a->zName = sqlite3NameFromToken(db, pName);
  if( pType->n>0 ){
    a->zType = sqlite3DbStrNDup(db, pType->z, pType->n);
  }
  pList->nParam++;
  return pList;
}

/*
** Append a declared result shape ("RETURNS TABLE(...)") to a procedure's
** shape list.  pCols carries the declared columns.  Creates the list head
** if pList is NULL; returns the (possibly new) list, or NULL on OOM after
** freeing both inputs.
*/
ProcShape *sqlite3ProcShapeAppend(
  Parse *pParse,          /* Parsing context */
  ProcShape *pList,       /* Existing shape list, or NULL */
  ProcParamList *pCols    /* Declared columns of the new shape */
){
  sqlite3 *db = pParse->db;
  ProcShape *pNew, *p;
  pNew = sqlite3DbMallocZero(db, sizeof(ProcShape));
  if( pNew==0 ){
    sqlite3ProcParamListDelete(db, pCols);
    sqlite3ProcShapeListDelete(db, pList);
    return 0;
  }
  pNew->pCols = pCols;
  if( pList==0 ) return pNew;
  for(p=pList; p->pNext; p=p->pNext){}
  p->pNext = pNew;
  return pList;
}

/*
** Parse-time handler for "RETURNS <name>" where <name> is not TABLE.
** The only legal spelling is NOTHING (kept out of the keyword table on
** purpose).  Represented as a shape node with pCols==0; FinishProc folds
** it into Proc.eRet and enforces that it stands alone.
*/
ProcShape *sqlite3ProcShapeNothing(
  Parse *pParse,          /* Parsing context */
  ProcShape *pList,       /* Existing shape list, or NULL */
  Token *pName            /* The word following RETURNS */
){
  sqlite3 *db = pParse->db;
  if( pName->n!=7 || sqlite3StrNICmp((const char*)pName->z, "NOTHING", 7)!=0 ){
    sqlite3ErrorMsg(pParse, "expected TABLE or NOTHING after RETURNS");
    sqlite3ProcShapeListDelete(db, pList);
    return 0;
  }
  return sqlite3ProcShapeAppend(pParse, pList, 0);
}

/*
** Delete a list of declared result shapes.
*/
void sqlite3ProcShapeListDelete(sqlite3 *db, ProcShape *pList){
  while( pList ){
    ProcShape *pNext = pList->pNext;
    sqlite3ProcParamListDelete(db, pList->pCols);
    sqlite3DbFree(db, pList);
    pList = pNext;
  }
}

/*
** Conformance checking of a procedure body against its declared result
** shapes (SPROCKET_RETURNS_TABLE_SPEC.md section 4).  Runs once, at
** CREATE PROCEDURE time, on the TriggerStep tree -- never at CALL time.
**
** The walk carries a "shape cursor": the number of result sets streamed
** so far on the path being examined.  Streaming SELECTs advance it;
** every complete path must end with the cursor at nEmit.
**
** Enforced: R1 (set count per path; RAISE exempt), R2 arity, R3 (branch
** symmetry), R3L (no row-returning SELECT inside a loop), R4 (a declared
** procedure may only CALL a RETURNS NOTHING procedure) and R5 (static
** and conservative: "SELECT *" is rejected inside a declared procedure
** because its arity is not knowable without name resolution).
**
** NOT enforced here: per-column type conformance.  Declared types are
** authoritative for CALL metadata; as with an ordinary SQLite column
** declaration they express affinity and intent rather than a checked
** constraint.  Arity, ordering and set count -- the properties a static
** consumer cannot recover at runtime -- are checked strictly.
*/

/*
** One result set the body is required to emit, in wire order.
**
** A declared shape holding k nested tables expands to 1+k of these: the
** parent, then one per nested table in declaration order.  Flattening here
** rather than in the walk is what keeps nesting out of the control-flow
** machinery entirely -- IF branches, loops and RETURN still reason about a
** single integer cursor, and a procedure that nests nothing produces exactly
** the array the checker used before nesting existed, so its diagnostics are
** unchanged to the byte.
*/
typedef struct ProcEmit ProcEmit;
struct ProcEmit {
  int nCol;               /* Columns the emitting SELECT must have */
  int nNested;            /* Nested tables in this shape (parent rows only) */
  const char *zNested;    /* Nested table name, or NULL if this is the parent */
  int iKeyCol;            /* 1-based position of the correlation column */
};

typedef struct ProcConf ProcConf;
struct ProcConf {
  Parse *pParse;          /* Parse context for error reporting */
  Proc *pProc;            /* Procedure being defined (not yet in the hash) */
  const char *zName;      /* Its name, for error messages */
  int nEmit;              /* Result sets required (0 = RETURNS NOTHING) */
  ProcEmit *aEmit;        /* Those result sets, in wire order */
  const char *zWhere;     /* " in the THEN branch" etc, or "" at top level */
  int nErr;               /* Nonzero once an error has been reported */
};

/* How a walked statement list ended */
#define PROC_PATH_FALLS   0   /* Control falls out of the list normally */
#define PROC_PATH_ENDS    1   /* Path ended (RETURN/LEAVE) */
#define PROC_PATH_ABORTS  2   /* Path aborted (RAISE) -- exempt from R1 */

/*
** Return the number of result columns of pSelect, or -1 if that cannot
** be determined statically (a "*" appears in some arm of a compound).
*/
static int procSelectArity(Select *pSelect){
  Select *p;
  int nCol = -1;
  for(p=pSelect; p; p=p->pPrior){
    ExprList *pEList = p->pEList;
    int k;
    if( pEList==0 ) return -1;
    for(k=0; k<pEList->nExpr; k++){
      Expr *pE = pEList->a[k].pExpr;
      if( pE==0 ) return -1;
      if( pE->op==TK_ASTERISK ) return -1;
      if( pE->op==TK_DOT && pE->pRight && pE->pRight->op==TK_ASTERISK ){
        return -1;
      }
    }
    if( nCol<0 ) nCol = pEList->nExpr;
  }
  return nCol;
}

static int procCheckList(ProcConf*, TriggerStep*, int, int*, int);

/*
** Apply the CALL rule (R4) to one CALL step.
*/
static void procCheckCall(ProcConf *p, TriggerStep *pStep){
  Proc *pCallee = 0;
  const char *zCallee = "?";
  if( pStep->pSrc && pStep->pSrc->nSrc>0 ){
    SrcItem *pItem = &pStep->pSrc->a[0];
    zCallee = pItem->zName;
    if( zCallee && sqlite3StrICmp(zCallee, p->zName)==0 ){
      pCallee = p->pProc;      /* Recursive self-call */
    }else{
      const char *zDb = pItem->fg.fixedSchema ? 0 : pItem->u4.zDatabase;
      pCallee = sqlite3FindProc(p->pParse, zCallee, zDb);
    }
  }
  if( pCallee==0 || pCallee->eRet!=PROC_RET_NOTHING ){
    sqlite3ErrorMsg(p->pParse,
      "procedure %s declares result shapes, so it may only CALL a "
      "procedure declared RETURNS NOTHING (%s is not)",
      p->zName, zCallee ? zCallee : "?");
    p->nErr = 1;
  }
}

/*
** Walk a loop body: no row-returning SELECT is permitted there (R3L),
** and nested constructs are searched too.
*/
static void procCheckLoopBody(ProcConf *p, TriggerStep *pList){
  TriggerStep *pStep;
  for(pStep=pList; pStep && p->nErr==0; pStep=pStep->pNext){
    switch( pStep->op ){
      case TK_SELECT: {
        if( pStep->pIdList==0 ){
          sqlite3ErrorMsg(p->pParse,
            "procedure %s declares result shapes, so a loop body may not "
            "contain a row-returning SELECT (SELECT ... INTO is allowed)",
            p->zName);
          p->nErr = 1;
        }
        break;
      }
      case TK_IF: {
        procCheckLoopBody(p, pStep->pThen);
        procCheckLoopBody(p, pStep->pElse);
        break;
      }
      case TK_WHILE: {
        procCheckLoopBody(p, pStep->pThen);
        break;
      }
      case TK_CALL: {
        procCheckCall(p, pStep);
        break;
      }
      default: break;
    }
  }
}

/*
** Impose the child ordering on one nested table's SELECT -- PLAN-NESTED phase
** 3, "the lowering".  The author never writes this ORDER BY, which is the
** whole point: POC 2's silent loss of 98% of child rows required an author to
** write the ordering and get it wrong, and here there is nothing to get wrong.
**
** Returns non-zero if an error was reported.
**
** This runs inside the conformance walk rather than in a pass of its own
** because the walk is the only place holding the mapping from an emission to
** the statement that produces it, and because an IF may give one emission two
** statements -- both of which need ordering.  Mutating a body whose CREATE
** then fails is harmless: the Proc is discarded.
*/
static int procLowerChild(ProcConf *p, TriggerStep *pStep, ProcEmit *pE){
  Select *pSel = pStep->pSelect;
  sqlite3 *db = p->pParse->db;

  /* An ORDER BY the author wrote is refused rather than overridden or merged.
  ** Overriding would discard something they wrote, silently, which is the
  ** exact failure class this phase exists to remove.  Merging -- prepending
  ** the correlation term so "ORDER BY created_at" becomes "ORDER BY key,
  ** created_at" -- would serve the real use of sorting within a parent, and is
  ** the documented growth path; it is not v1 because going from refuse to
  ** merge later breaks nothing, while the reverse breaks bodies. */
  if( pSel->pOrderBy ){
    sqlite3ErrorMsg(p->pParse,
      "the SELECT for nested table %s of procedure %s%s may not have its own "
      "ORDER BY -- the correlation ordering is supplied by the engine",
      pE->zNested, p->zName, p->zWhere);
    p->nErr = 1;
    return 1;
  }
  /* LIMIT is refused because its meaning under a correlation is genuinely
  ** ambiguous -- per parent, or across the whole child set? -- and an imposed
  ** ORDER BY silently changes which rows a LIMIT keeps. */
  if( pSel->pLimit ){
    sqlite3ErrorMsg(p->pParse,
      "the SELECT for nested table %s of procedure %s%s may not use LIMIT: "
      "its meaning per parent row is undefined",
      pE->zNested, p->zName, p->zWhere);
    p->nErr = 1;
    return 1;
  }

  /* A compound child SELECT is refused rather than half-handled: the term
  ** built below belongs to the leftmost arm's projection, which is not a legal
  ** ORDER BY for a compound. */
  if( pSel->pPrior ){
    sqlite3ErrorMsg(p->pParse,
      "the SELECT for nested table %s of procedure %s%s may not be a compound "
      "SELECT", pE->zNested, p->zName, p->zWhere);
    p->nErr = 1;
    return 1;
  }
  if( pSel->pEList==0 || pE->iKeyCol>pSel->pEList->nExpr ) return 0;

  /* Sort by a COPY OF THE PROJECTED EXPRESSION rather than by the ordinal
  ** "ORDER BY <n>".  A constructed TK_INTEGER is not resolved as an ordinal
  ** the way a parsed one is: it sorts by a constant, which is a no-op that
  ** still emits sorter opcodes -- so the body looked ordered to every static
  ** check while returning rows in scan order.  Measured: rows came back in
  ** rowid order with five Sorter opcodes present.  Duplicating the projection
  ** cannot drift from what the column actually yields. */
  pSel->pOrderBy = sqlite3ExprListAppend(p->pParse, 0,
      sqlite3ExprDup(db, pSel->pEList->a[pE->iKeyCol-1].pExpr, 0));
  return 0;
}

/*
** Walk one statement list with the shape cursor at iStart.  Returns the
** cursor after the list; *peEnd receives a PROC_PATH_* code.
*/
static int procCheckList(
  ProcConf *p,            /* Checker context */
  TriggerStep *pList,     /* Statements to walk */
  int iStart,             /* Shape cursor on entry */
  int *peEnd,             /* OUT: PROC_PATH_* */
  int bInLoop             /* True when walking inside a loop body */
){
  TriggerStep *pStep;
  int i = iStart;
  *peEnd = PROC_PATH_FALLS;
  for(pStep=pList; pStep && p->nErr==0; pStep=pStep->pNext){
    switch( pStep->op ){
      case TK_SELECT: {
        int nCol;
        if( pStep->pIdList!=0 ) break;   /* SELECT ... INTO streams nothing */
        if( bInLoop ) break;             /* Already reported by R3L */
        if( p->nEmit==0 ){
          sqlite3ErrorMsg(p->pParse,
            "procedure %s declares RETURNS NOTHING but contains a "
            "row-returning SELECT", p->zName);
          p->nErr = 1;
          break;
        }
        if( i>=p->nEmit ){
          sqlite3ErrorMsg(p->pParse,
            "procedure %s streams more result sets than the %d declared "
            "by its RETURNS clauses", p->zName, p->nEmit);
          p->nErr = 1;
          break;
        }
        nCol = procSelectArity(pStep->pSelect);
        if( nCol<0 ){
          sqlite3ErrorMsg(p->pParse,
            "procedure %s declares result shapes, so result set %d may "
            "not use \"*\" -- list the columns explicitly", p->zName, i+1);
          p->nErr = 1;
          break;
        }
        if( nCol!=p->aEmit[i].nCol ){
          ProcEmit *pE = &p->aEmit[i];
          if( pE->zNested ){
            /* The body's Nth SELECT is the one feeding a nested table, so
            ** name the table rather than leaving the author to count. */
            sqlite3ErrorMsg(p->pParse,
              "result set %d of procedure %s%s carries nested table %s, "
              "which declares %d column%s, but that SELECT has %d",
              i+1, p->zName, p->zWhere, pE->zNested, pE->nCol,
              pE->nCol==1 ? "" : "s", nCol);
          }else if( pE->nNested ){
            /* Distinguished from the flat message below because the RETURNS
            ** TABLE visibly holds more entries than the parent SELECT is
            ** meant to supply -- saying "declares 2" against a clause with
            ** three entries would read as a bug in the checker. */
            sqlite3ErrorMsg(p->pParse,
              "result set %d of procedure %s%s has %d column%s but its "
              "RETURNS TABLE declares %d value column%s before its %d nested "
              "table%s, which are streamed by the SELECTs that follow",
              i+1, p->zName, p->zWhere, nCol, nCol==1 ? "" : "s",
              pE->nCol, pE->nCol==1 ? "" : "s",
              pE->nNested, pE->nNested==1 ? "" : "s");
          }else{
            sqlite3ErrorMsg(p->pParse,
              "result set %d of procedure %s%s has %d column%s but its "
              "RETURNS TABLE declares %d", i+1, p->zName, p->zWhere, nCol,
              nCol==1 ? "" : "s", pE->nCol);
          }
          p->nErr = 1;
          break;
        }
        if( p->aEmit[i].zNested && procLowerChild(p, pStep, &p->aEmit[i]) ){
          break;
        }
        i++;
        break;
      }
      case TK_IF: {
        int eThen, eElse, iThen, iElse;
        const char *zSave = p->zWhere;
        p->zWhere = " in the THEN branch";
        iThen = procCheckList(p, pStep->pThen, i, &eThen, bInLoop);
        p->zWhere = pStep->pElse ? " in the ELSE branch" : zSave;
        if( p->nErr ){ p->zWhere = zSave; break; }
        iElse = procCheckList(p, pStep->pElse, i, &eElse, bInLoop);
        p->zWhere = zSave;
        if( p->nErr ) break;
        if( eThen!=PROC_PATH_FALLS && eElse!=PROC_PATH_FALLS ){
          *peEnd = (eThen==PROC_PATH_ABORTS && eElse==PROC_PATH_ABORTS)
                     ? PROC_PATH_ABORTS : PROC_PATH_ENDS;
          return i;
        }else if( eThen!=PROC_PATH_FALLS ){
          i = iElse;
        }else if( eElse!=PROC_PATH_FALLS ){
          i = iThen;
        }else if( iThen!=iElse ){
          sqlite3ErrorMsg(p->pParse,
            "branches of IF in procedure %s stream different numbers of "
            "result sets (%d and %d); every path must stream the declared "
            "sequence in order", p->zName, iThen, iElse);
          p->nErr = 1;
        }else{
          i = iThen;
        }
        break;
      }
      case TK_WHILE: {
        procCheckLoopBody(p, pStep->pThen);
        break;
      }
      case TK_CALL: {
        procCheckCall(p, pStep);
        break;
      }
      case TK_RETURN: {
        if( !bInLoop && i!=p->nEmit ){
          sqlite3ErrorMsg(p->pParse,
            "procedure %s returns after streaming %d of %d declared "
            "result sets", p->zName, i, p->nEmit);
          p->nErr = 1;
        }
        *peEnd = PROC_PATH_ENDS;
        return i;
      }
      case TK_RAISE: {
        *peEnd = PROC_PATH_ABORTS;   /* R1 exempt */
        return i;
      }
      case TK_LEAVE: {
        *peEnd = PROC_PATH_ENDS;
        return i;
      }
      default: break;
    }
  }
  return i;
}

/*
** The number of result sets -- SEGMENTS -- a procedure's body must stream.
**
** A shape holding k nested tables expands to 1+k segments: the parent, then
** one per nested table in declaration order.  This is what
** sqlite3_proc_next_resultset() advances through and what
** PRAGMA proc_list.nresultsets reports; it equals the number of declared
** shapes for every procedure that does not nest.
*/
int sqlite3ProcSegmentCount(Proc *pProc){
  ProcShape *pS;
  int n = 0, i;
  for(pS=pProc->pShapes; pS; pS=pS->pNext){
    if( pS->pCols==0 ) continue;
    n++;
    for(i=0; i<pS->pCols->nParam; i++){
      if( pS->pCols->a[i].pNested ) n++;
    }
  }
  return n;
}

/*
** True if any declared shape holds a nested table.
*/
static int procShapesNest(ProcShape *pShapes){
  ProcShape *pS;
  int i;
  for(pS=pShapes; pS; pS=pS->pNext){
    if( pS->pCols==0 ) continue;
    for(i=0; i<pS->pCols->nParam; i++){
      if( pS->pCols->a[i].pNested ) return 1;
    }
  }
  return 0;
}

/*
** Flatten pProc's declared shapes into the sequence of result sets the body
** must emit, and fill conf.aEmit/nEmit with it.  Returns non-zero if an error
** was reported.
**
** The parent half of each KEY is checked here rather than at the grammar
** action that builds the nested column: that action sees only the columns
** declared BEFORE the nested one, so a shape whose key column is declared
** after its nested table would be rejected wrongly.  By this point the shape
** is complete.
*/
static int procBuildEmits(Parse *pParse, Proc *pProc, ProcConf *pConf){
  sqlite3 *db = pParse->db;
  ProcShape *pS;
  int n, i, j;

  n = sqlite3ProcSegmentCount(pProc);
  pConf->nEmit = n;
  if( n==0 ) return 0;
  pConf->aEmit = sqlite3DbMallocZero(db, n*sizeof(ProcEmit));
  if( pConf->aEmit==0 ) return 1;

  n = 0;
  for(pS=pProc->pShapes; pS; pS=pS->pNext){
    ProcParamList *pCols = pS->pCols;
    int iParent, nScalar = 0, nNested = 0;
    if( pCols==0 ) continue;
    iParent = n++;
    for(i=0; i<pCols->nParam; i++){
      if( pCols->a[i].pNested ) nNested++; else nScalar++;
    }
    /* nScalar is what README-PROCS.md calls a VALUE COLUMN -- a column that
    ** carries a value directly, as opposed to a nested table.  The user-facing
    ** identity is columns = value columns + nested tables. */
    pConf->aEmit[iParent].nCol = nScalar;
    pConf->aEmit[iParent].nNested = nNested;
    /* Diagnosed before the key check below, which would otherwise report the
    ** parent column as missing and send the author looking for the wrong
    ** mistake. */
    if( nScalar==0 ){
      sqlite3ErrorMsg(pParse,
        "result set of procedure %s declares only nested tables; a result "
        "set must have at least one column of its own to correlate on",
        pProc->zName);
      return 1;
    }
    for(i=0; i<pCols->nParam; i++){
      ProcParam *pCol = &pCols->a[i];
      if( pCol->pNested==0 ) continue;
      /* The parent side of the correlation must name a scalar column of this
      ** same shape -- it is the value the child rows are matched against, and
      ** a name that resolves to nothing would only surface at CALL. */
      for(j=0; j<pCols->nParam; j++){
        if( pCols->a[j].pNested==0
         && sqlite3StrICmp(pCols->a[j].zName, pCol->zKeyParent)==0 ) break;
      }
      if( j>=pCols->nParam ){
        sqlite3ErrorMsg(pParse,
          "nested table %s of procedure %s correlates on %s, which is not a "
          "column of the result set that contains it",
          pCol->zName, pProc->zName, pCol->zKeyParent);
        return 1;
      }
      pConf->aEmit[n].nCol = pCol->pNested->nParam;
      pConf->aEmit[n].zNested = pCol->zName;
      /* Recorded as an ordinal rather than a name because the declaration is
      ** the interface: a body's SELECT need not spell its columns the way the
      ** shape does, so "ORDER BY 2" is right where "ORDER BY post_id" would
      ** depend on a coincidence. */
      for(j=0; j<pCol->pNested->nParam; j++){
        if( sqlite3StrICmp(pCol->pNested->a[j].zName, pCol->zKeyChild)==0 ){
          pConf->aEmit[n].iKeyCol = j+1;
          break;
        }
      }
      assert( pConf->aEmit[n].iKeyCol>0 );  /* phase 1 validated the name */
      n++;
    }
  }
  return 0;
}

/*
** Check a procedure body against its declared result shapes, reporting errors
** through pParse, AND impose the child ordering on every nested table.
** Procedures with no RETURNS clause are not checked (legacy dynamic behavior).
**
** The name says "and lower" because the second job changes the calling
** contract: a checker may be skipped when its answer is already known, a
** rewriter may not.  See the call site.
*/
static void procCheckAndLower(Parse *pParse, Proc *pProc){
  sqlite3 *db = pParse->db;
  ProcConf conf;
  int iEnd, eEnd;

  if( pProc->eRet==PROC_RET_UNDECLARED ) return;
  memset(&conf, 0, sizeof(conf));
  conf.pParse = pParse;
  conf.pProc = pProc;
  conf.zName = pProc->zName;
  conf.zWhere = "";
  if( procBuildEmits(pParse, pProc, &conf) ){
    sqlite3DbFree(db, conf.aEmit);
    return;
  }
  iEnd = procCheckList(&conf, pProc->pBody, 0, &eEnd, 0);
  if( conf.nErr==0 && eEnd==PROC_PATH_FALLS && iEnd!=conf.nEmit ){
    sqlite3ErrorMsg(pParse,
      "procedure %s streams %d result set%s but declares %d",
      pProc->zName, iEnd, iEnd==1 ? "" : "s", conf.nEmit);
  }
  sqlite3DbFree(db, conf.aEmit);
}

/*
** Delete a procedure parameter list and all of its content.
*/
void sqlite3ProcParamListDelete(sqlite3 *db, ProcParamList *pList){
  int i;
  if( pList==0 ) return;
  for(i=0; i<pList->nParam; i++){
    sqlite3DbFree(db, pList->a[i].zName);
    sqlite3DbFree(db, pList->a[i].zType);
    sqlite3DbFree(db, pList->a[i].zKeyChild);
    sqlite3DbFree(db, pList->a[i].zKeyParent);
    sqlite3ProcParamListDelete(db, pList->a[i].pNested);   /* recurses once */
  }
  sqlite3DbFree(db, pList->a);
  sqlite3DbFree(db, pList);
}

/*
** Delete a Proc object and all of its content.
*/
void sqlite3DeleteProc(sqlite3 *db, Proc *pProc){
  if( pProc==0 ) return;
  sqlite3DeleteTriggerStep(db, pProc->pBody);
  sqlite3ProcParamListDelete(db, pProc->pParams);
  sqlite3ProcShapeListDelete(db, pProc->pShapes);
  sqlite3DbFree(db, pProc->zName);
  sqlite3DbFree(db, pProc);
}

/*
** Locate a stored procedure by name.  If zDb is not NULL, only look in
** that database.  Otherwise search all attached databases, TEMP before
** MAIN, in the same order used for trigger name resolution.
*/
Proc *sqlite3FindProc(Parse *pParse, const char *zName, const char *zDb){
  sqlite3 *db = pParse->db;
  Proc *pProc = 0;
  int i;
  for(i=OMIT_TEMPDB; i<db->nDb; i++){
    int j = (i<2) ? i^1 : i;  /* Search TEMP before MAIN */
    if( zDb && sqlite3DbIsNamed(db, j, zDb)==0 ) continue;
    assert( sqlite3SchemaMutexHeld(db, j, 0) );
    pProc = sqlite3HashFind(&(db->aDb[j].pSchema->procHash), zName);
    if( pProc ) break;
  }
  return pProc;
}

/*
** This is called by the parser after the entire CREATE PROCEDURE
** statement has been parsed.  Build the Proc object, and either write
** the new entry into the sqlite_schema table (normal DDL) or link the
** object into the schema hash tables (when parsing an existing schema).
**
** pAll spans from the procedure name through the final END keyword; the
** text stored in sqlite_schema is "CREATE PROCEDURE " || pAll, matching
** the convention used by CREATE TRIGGER (TEMP and IF NOT EXISTS do not
** appear in the stored text).
*/
void sqlite3FinishProc(
  Parse *pParse,          /* Parser context */
  Token *pName1,          /* First part of the procedure name */
  Token *pName2,          /* Second part of the name, if db-qualified */
  ProcParamList *pParams, /* Declared parameters, or NULL */
  ProcShape *pShapes,     /* Declared result shapes, or NULL if undeclared */
  TriggerStep *pStepList, /* Body of the procedure */
  int isTemp,             /* True if the TEMP keyword is present */
  int noErr,              /* True if IF NOT EXISTS is present */
  Token *pAll,            /* Text from procedure name through END */
  int bDefiner            /* True for SECURITY DEFINER (body unauthorized) */
){
  sqlite3 *db = pParse->db;
  Proc *pProc = 0;        /* The new procedure object */
  char *zName = 0;        /* Name of the procedure */
  int iDb;                /* Database to store the procedure in */
  Token *pName = 0;       /* Unqualified name token */
  DbFixer sFix;           /* State for fixing db references in the body */
  int i;

  if( pName1==0 || pParse->nErr ) goto proc_cleanup;

  /* Figure out which database the procedure belongs in */
  if( isTemp ){
    if( pName2->n>0 ){
      sqlite3ErrorMsg(pParse, "temporary procedure may not have qualified "
                              "name");
      goto proc_cleanup;
    }
    iDb = 1;
    pName = pName1;
  }else{
    iDb = sqlite3TwoPartName(pParse, pName1, pName2, &pName);
    if( iDb<0 ) goto proc_cleanup;
  }
  if( db->mallocFailed ) goto proc_cleanup;

  /* If this statement is being parsed out of the schema of an attached
  ** or reopened database, everything is already resolved. */
  zName = sqlite3NameFromToken(db, pName);
  if( zName==0 ) goto proc_cleanup;
  if( sqlite3CheckObjectName(pParse, zName, "proc", "") ){
    goto proc_cleanup;
  }
  assert( sqlite3SchemaMutexHeld(db, iDb, 0) );
  if( sqlite3HashFind(&(db->aDb[iDb].pSchema->procHash), zName) ){
    if( !noErr ){
      sqlite3ErrorMsg(pParse, "procedure %s already exists", zName);
    }else{
      assert( !db->init.busy );
      sqlite3CodeVerifySchema(pParse, iDb);
    }
    goto proc_cleanup;
  }

  /* Reject duplicate parameter names */
  if( pParams ){
    int j;
    for(i=1; i<pParams->nParam; i++){
      for(j=0; j<i; j++){
        if( sqlite3StrICmp(pParams->a[i].zName, pParams->a[j].zName)==0 ){
          sqlite3ErrorMsg(pParse, "duplicate parameter name: %s",
                          pParams->a[i].zName);
          goto proc_cleanup;
        }
      }
    }
  }

  /* Validate the declared result shapes, if any.  RETURNS NOTHING (a
  ** shape node with pCols==0) must stand alone; every RETURNS TABLE
  ** column must carry a type name; column names within one shape must
  ** be distinct. */
  if( pShapes ){
    ProcShape *pS;
    int haveNothing = 0, haveTable = 0, iShape = 0;
    for(pS=pShapes; pS; pS=pS->pNext, iShape++){
      if( pS->pCols==0 ){
        haveNothing = 1;
      }else{
        int j, k;
        ProcParamList *pC = pS->pCols;
        haveTable = 1;
        for(j=0; j<pC->nParam; j++){
          /* A nested child table has no scalar type of its own -- its shape IS
          ** its declaration -- so it is exempt from the type-name rule that
          ** applies to ordinary result columns. */
          if( pC->a[j].zType==0 && pC->a[j].pNested==0 ){
            sqlite3ErrorMsg(pParse,
               "RETURNS TABLE column %s of %s needs a type name",
               pC->a[j].zName, zName);
            goto proc_cleanup;
          }
          for(k=0; k<j; k++){
            if( sqlite3StrICmp(pC->a[j].zName, pC->a[k].zName)==0 ){
              sqlite3ErrorMsg(pParse,
                 "duplicate column name in RETURNS TABLE %d of %s: %s",
                 iShape+1, zName, pC->a[j].zName);
              goto proc_cleanup;
            }
          }
        }
      }
    }
    if( haveNothing && (haveTable || pShapes->pNext!=0) ){
      sqlite3ErrorMsg(pParse,
         "RETURNS NOTHING may not be combined with other RETURNS clauses");
      goto proc_cleanup;
    }
  }

  /* Resolve or reject database qualifiers inside the body */
  sqlite3FixInit(&sFix, pParse, iDb, "procedure", pName);
  if( sqlite3FixTriggerStep(&sFix, pStepList) ){
    goto proc_cleanup;
  }

  /* Build the Proc object */
  pProc = sqlite3DbMallocZero(db, sizeof(Proc));
  if( pProc==0 ) goto proc_cleanup;
  pProc->zName = zName;
  pProc->bDefiner = (u8)(bDefiner!=0);
  zName = 0;
  pProc->pParams = pParams;
  pParams = 0;
  pProc->pBody = pStepList;
  pStepList = 0;
  pProc->pSchema = db->aDb[iDb].pSchema;
  if( pShapes==0 ){
    pProc->eRet = PROC_RET_UNDECLARED;
  }else if( pShapes->pCols==0 ){
    pProc->eRet = PROC_RET_NOTHING;
    sqlite3ProcShapeListDelete(db, pShapes);   /* Nothing worth keeping */
  }else{
    pProc->eRet = PROC_RET_TABLES;
    pProc->pShapes = pShapes;
  }
  pShapes = 0;

  /* Enforce the declared shapes against the body, and impose the child
  ** ordering on any nested table (PLAN-NESTED phase 3).
  **
  ** This runs on BOTH paths, unlike the check it grew out of.  A real DDL
  ** execution builds this Proc, writes the schema row, and then re-parses it
  ** -- so the object checked here is thrown away and the one that answers
  ** CALL is built by the reparse below with db->init.busy set.  While this
  ** pass only validated, skipping it on that path was a sound optimisation.
  ** Now that it also TRANSFORMS the body, skipping it meant the lowering was
  ** applied only to the copy that gets discarded: measured as zero sorter
  ** opcodes in a body whose hand-written equivalent emits five.  A pass that
  ** rewrites a body has to run wherever that body is materialised. */
  if( pProc->eRet!=PROC_RET_UNDECLARED ){
    procCheckAndLower(pParse, pProc);
    if( pParse->nErr ) goto proc_cleanup;
  }

  if( !db->init.busy ){
    /* Normal DDL execution: write the entry into sqlite_schema, then
    ** cause the new row to be re-parsed (registering the in-memory Proc)
    ** once the write commits. */
    Vdbe *v = sqlite3GetVdbe(pParse);
    char *z;
    if( v==0 ) goto proc_cleanup;
    sqlite3BeginWriteOperation(pParse, 0, iDb);
    z = sqlite3DbStrNDup(db, (char*)pAll->z, pAll->n);
    testcase( z==0 );
    sqlite3NestedParse(pParse,
       "INSERT INTO %Q." LEGACY_SCHEMA_TABLE
       " VALUES('proc',%Q,'',0,'CREATE PROCEDURE %q')",
       db->aDb[iDb].zDbSName, pProc->zName, z);
    sqlite3DbFree(db, z);
    sqlite3ChangeCookie(pParse, iDb);
    sqlite3VdbeAddParseSchemaOp(v, iDb,
        sqlite3MPrintf(db, "type='proc' AND name='%q'", pProc->zName), 0);
  }else{
    /* Parsing the schema of an existing database: link the Proc into the
    ** procedure hash table directly. */
    Proc *pLink = pProc;
    Hash *pHash = &db->aDb[iDb].pSchema->procHash;
    assert( sqlite3SchemaMutexHeld(db, iDb, 0) );
    pProc = sqlite3HashInsert(pHash, pLink->zName, pLink);
    if( pProc ){
      sqlite3OomFault(db);
    }
  }

proc_cleanup:
  sqlite3DbFree(db, zName);
  sqlite3ProcParamListDelete(db, pParams);
  sqlite3ProcShapeListDelete(db, pShapes);
  sqlite3DeleteTriggerStep(db, pStepList);
  sqlite3DeleteProc(db, pProc);
}

/*
** Generate code to drop the given procedure: delete its sqlite_schema
** row, bump the schema cookie, and remove the in-memory object.
*/
static void codeDropProc(Parse *pParse, Proc *pProc){
  sqlite3 *db = pParse->db;
  int iDb = sqlite3SchemaToIndex(db, pProc->pSchema);
  Vdbe *v;

  assert( iDb>=0 && iDb<db->nDb );
  if( (v = sqlite3GetVdbe(pParse))!=0 ){
    sqlite3NestedParse(pParse,
       "DELETE FROM %Q." LEGACY_SCHEMA_TABLE
       " WHERE name=%Q AND type='proc'",
       db->aDb[iDb].zDbSName, pProc->zName
    );
    sqlite3ChangeCookie(pParse, iDb);
    sqlite3VdbeAddOp4(v, OP_DropProc, iDb, 0, 0, pProc->zName, 0);
  }
}

/*
** This is called by the parser for a DROP PROCEDURE statement.
*/
void sqlite3DropProc(Parse *pParse, SrcList *pName, int noErr){
  Proc *pProc = 0;
  const char *zDb;
  const char *zName;
  sqlite3 *db = pParse->db;

  if( db->mallocFailed ) goto drop_proc_cleanup;
  if( SQLITE_OK!=sqlite3ReadSchema(pParse) ){
    goto drop_proc_cleanup;
  }

  assert( pName->nSrc==1 );
  assert( pName->a[0].fg.fixedSchema==0 && pName->a[0].fg.isSubquery==0 );
  zDb = pName->a[0].u4.zDatabase;
  zName = pName->a[0].zName;
  assert( zDb!=0 || sqlite3BtreeHoldsAllMutexes(db) );
  pProc = sqlite3FindProc(pParse, zName, zDb);
  if( !pProc ){
    if( !noErr ){
      sqlite3ErrorMsg(pParse, "no such procedure: %S", pName->a);
    }else{
      sqlite3CodeVerifyNamedSchema(pParse, zDb);
    }
    pParse->checkSchema = 1;
    goto drop_proc_cleanup;
  }
  codeDropProc(pParse, pProc);

drop_proc_cleanup:
  sqlite3SrcListDelete(db, pName);
}

/*
** Remove a stored procedure from the hash tables of the sqlite* pointer.
** This is the run-time counterpart of codeDropProc(), invoked by the
** OP_DropProc opcode.
*/
void sqlite3UnlinkAndDeleteProc(sqlite3 *db, int iDb, const char *zName){
  Proc *pProc;
  assert( sqlite3SchemaMutexHeld(db, iDb, 0) );
  pProc = sqlite3HashInsert(&(db->aDb[iDb].pSchema->procHash), zName, 0);
  if( pProc ){
    sqlite3DeleteProc(db, pProc);
    db->mDbFlags |= DBFLAG_SchemaChange;
  }
}

/*
** Build a TriggerStep representing a CALL statement inside a procedure
** body.  pName is the (possibly db-qualified) procedure name; pArgs the
** argument expressions, or NULL.  This routine takes ownership of both.
*/
TriggerStep *sqlite3ProcCallStep(Parse *pParse, SrcList *pName,
                                 ExprList *pArgs){
  sqlite3 *db = pParse->db;
  TriggerStep *pStep = sqlite3DbMallocZero(db, sizeof(TriggerStep));
  if( pStep==0 ){
    sqlite3SrcListDelete(db, pName);
    sqlite3ExprListDelete(db, pArgs);
    return 0;
  }
  pStep->op = TK_CALL;
  pStep->pSrc = pName;
  pStep->pExprList = pArgs;
  pStep->orconf = OE_Default;
  return pStep;
}

/*
** Allocate a zeroed TriggerStep with the given opcode, for the PSM
** statement constructors below.
*/
static TriggerStep *procStepAlloc(Parse *pParse, int op){
  TriggerStep *p = sqlite3DbMallocZero(pParse->db, sizeof(TriggerStep));
  if( p ){
    p->op = (u8)op;
    p->orconf = OE_Default;
    p->pLast = p;
  }
  return p;
}

/*
** Build a DECLARE step.  The declared type is currently documentation
** only (values keep the dynamic types of whatever is assigned to them);
** the DEFAULT expression, if any, is evaluated when execution reaches
** the DECLARE statement.
*/
TriggerStep *sqlite3ProcDeclareStep(
  Parse *pParse,
  Token *pName,          /* Variable name */
  Token *pType,          /* Declared type token (may be empty) */
  Expr *pDflt            /* DEFAULT expression, or NULL */
){
  TriggerStep *p = procStepAlloc(pParse, TK_DECLARE);
  sqlite3 *db = pParse->db;
  if( p==0 ){
    sqlite3ExprDelete(db, pDflt);
    return 0;
  }
  p->zVar = sqlite3NameFromToken(db, pName);
  p->pWhere = pDflt;
  UNUSED_PARAMETER(pType);
  return p;
}

/*
** Build a SET step: assign an expression to a variable or parameter.
*/
TriggerStep *sqlite3ProcSetStep(Parse *pParse, Token *pName, Expr *pValue){
  TriggerStep *p = procStepAlloc(pParse, TK_SET);
  sqlite3 *db = pParse->db;
  if( p==0 ){
    sqlite3ExprDelete(db, pValue);
    return 0;
  }
  p->zVar = sqlite3NameFromToken(db, pName);
  p->pWhere = pValue;
  return p;
}

/*
** Build an IF step.  ELSEIF chains arrive as an IF step in pElse.
*/
TriggerStep *sqlite3ProcIfStep(
  Parse *pParse,
  Expr *pCond,           /* The IF condition */
  TriggerStep *pThen,    /* Statements when the condition is true */
  TriggerStep *pElse     /* ELSE branch, ELSEIF chain, or NULL */
){
  TriggerStep *p = procStepAlloc(pParse, TK_IF);
  sqlite3 *db = pParse->db;
  if( p==0 ){
    sqlite3ExprDelete(db, pCond);
    sqlite3DeleteTriggerStep(db, pThen);
    sqlite3DeleteTriggerStep(db, pElse);
    return 0;
  }
  p->pWhere = pCond;
  p->pThen = pThen;
  p->pElse = pElse;
  return p;
}

/*
** Build a WHILE step.  A NULL condition is an unconditional LOOP,
** exited only by LEAVE, RETURN, or RAISE.
*/
TriggerStep *sqlite3ProcWhileStep(Parse *pParse, Expr *pCond,
                                  TriggerStep *pBody){
  TriggerStep *p = procStepAlloc(pParse, TK_WHILE);
  sqlite3 *db = pParse->db;
  if( p==0 ){
    sqlite3ExprDelete(db, pCond);
    sqlite3DeleteTriggerStep(db, pBody);
    return 0;
  }
  p->pWhere = pCond;
  p->pThen = pBody;
  return p;
}

/*
** Build a bodiless step: LEAVE or RETURN.
*/
TriggerStep *sqlite3ProcSimpleStep(Parse *pParse, int op){
  return procStepAlloc(pParse, op);
}

/*
** Build a RAISE step.  onError is OE_Ignore, OE_Rollback, OE_Abort or
** OE_Fail; pMsg is the error message expression (NULL for OE_Ignore).
*/
TriggerStep *sqlite3ProcRaiseStep(Parse *pParse, int onError, Expr *pMsg){
  TriggerStep *p = procStepAlloc(pParse, TK_RAISE);
  sqlite3 *db = pParse->db;
  if( p==0 ){
    sqlite3ExprDelete(db, pMsg);
    return 0;
  }
  p->orconf = (u8)onError;
  p->pWhere = pMsg;
  return p;
}

/*
** Build a SELECT ... INTO step.  Represented as a TK_SELECT step whose
** pIdList holds the target variable names.
*/
TriggerStep *sqlite3ProcSelectIntoStep(
  Parse *pParse,
  Select *pSelect,       /* The SELECT statement */
  IdList *pInto,         /* Target variable names */
  const char *zStart,    /* Start of SQL text */
  const char *zEnd       /* End of SQL text */
){
  TriggerStep *p = procStepAlloc(pParse, TK_SELECT);
  sqlite3 *db = pParse->db;
  if( p==0 ){
    sqlite3SelectDelete(db, pSelect);
    sqlite3IdListDelete(db, pInto);
    return 0;
  }
  p->pSelect = pSelect;
  p->pIdList = pInto;
  p->zSpan = sqlite3DbSpanDup(db, zStart, zEnd);
  return p;
}

/*
** Append zName to the variable table being built for a procedure body,
** reporting an error on duplicates.
*/
static void procVarAdd(Parse *pParse, ProcParamList **ppList,
                       const char *zName){
  sqlite3 *db = pParse->db;
  ProcParamList *pList = *ppList;
  ProcParam *a;
  int i;
  if( pParse->nErr || db->mallocFailed || zName==0 ) return;
  if( pList==0 ){
    pList = sqlite3DbMallocZero(db, sizeof(ProcParamList));
    if( pList==0 ) return;
    *ppList = pList;
  }
  for(i=0; i<pList->nParam; i++){
    if( sqlite3StrICmp(pList->a[i].zName, zName)==0 ){
      sqlite3ErrorMsg(pParse, "duplicate variable name: %s", zName);
      return;
    }
  }
  a = sqlite3DbRealloc(db, pList->a, (pList->nParam+1)*sizeof(ProcParam));
  if( a==0 ) return;
  pList->a = a;
  memset(&a[pList->nParam], 0, sizeof(ProcParam));
  a[pList->nParam].zName = sqlite3DbStrDup(db, zName);
  pList->nParam++;
}

/*
** Recursively collect the names of all DECLAREd variables in a step
** tree.  Variables are hoisted: one flat scope per procedure body.
*/
static void procCollectVars(Parse *pParse, ProcParamList **ppList,
                            TriggerStep *pStep){
  for(; pStep; pStep=pStep->pNext){
    if( pStep->op==TK_DECLARE ){
      procVarAdd(pParse, ppList, pStep->zVar);
    }
    procCollectVars(pParse, ppList, pStep->pThen);
    procCollectVars(pParse, ppList, pStep->pElse);
  }
}

/*
** Return the memory cell holding variable zName, or 0 if there is no
** such variable.
*/
static int procVarCell(Parse *pParse, const char *zName){
  ProcParamList *pL = pParse->pProcVars;
  int i;
  if( pL ){
    for(i=0; i<pL->nParam; i++){
      if( sqlite3StrICmp(pL->a[i].zName, zName)==0 ){
        return pParse->iProcParamBase + i;
      }
    }
  }
  return 0;
}

/*
** Mark every subquery in the given expression and/or SELECT as
** EP_VarSelect (i.e. "not constant").  Uncorrelated subqueries are
** normally wrapped in OP_Once, which caches their result for the
** lifetime of one frame invocation -- inside a WHILE/LOOP body that
** would return stale values on every iteration after the first.
*/
static int procMarkVarSelectExpr(Walker *pWalker, Expr *pExpr){
  UNUSED_PARAMETER(pWalker);
  if( pExpr->op==TK_SELECT || pExpr->op==TK_EXISTS ){
    ExprSetProperty(pExpr, EP_VarSelect);
  }
  return WRC_Continue;
}
static void procMarkVarSelect(Parse *pParse, Expr *pExpr, Select *pSelect){
  Walker w;
  memset(&w, 0, sizeof(w));
  w.pParse = pParse;
  w.xExprCallback = procMarkVarSelectExpr;
  w.xSelectCallback = sqlite3SelectWalkNoop;
  if( pExpr ) sqlite3WalkExpr(&w, pExpr);
  if( pSelect ) sqlite3WalkSelect(&w, pSelect);
}

/*
** Duplicate, mark, and resolve one PSM-managed expression, then code it
** into the given cell (iTarget>0), or evaluate it for control flow via
** the returned duplicate.  Returns the resolved duplicate (caller must
** delete) or NULL on error.
*/
static Expr *procDupResolveExpr(Parse *pParse, Expr *pExpr){
  sqlite3 *db = pParse->db;
  Expr *pDup = sqlite3ExprDup(db, pExpr, 0);
  NameContext sNC;
  if( pDup==0 ) return 0;
  procMarkVarSelect(pParse, pDup, 0);
  memset(&sNC, 0, sizeof(sNC));
  sNC.pParse = pParse;
  if( sqlite3ResolveExprNames(&sNC, pDup) ){
    sqlite3ExprDelete(db, pDup);
    return 0;
  }
  return pDup;
}

/*
** Move a parse error from the sub-parse used to compile a procedure body
** up into the parent parse.  (Same logic as trigger.c:transferParseError,
** which is file-static there.)
*/
static void procTransferParseError(Parse *pTo, Parse *pFrom){
  assert( pFrom->zErrMsg==0 || pFrom->nErr );
  assert( pTo->zErrMsg==0 || pTo->nErr );
  if( pTo->nErr==0 ){
    pTo->zErrMsg = pFrom->zErrMsg;
    pTo->nErr = pFrom->nErr;
    pTo->rc = pFrom->rc;
  }else{
    sqlite3DbFree(pFrom->db, pFrom->zErrMsg);
  }
  pFrom->zErrMsg = 0;
}

/*
** Generate VDBE code (in the sub-parse pParse) for the list of statements
** that make up a procedure body.
**
** Bare SELECT statements compile with SRT_Output: their rows stream out
** of the sub-frame to the caller of the CALL statement via the normal
** OP_ResultRow / SQLITE_ROW mechanism.  Every row-producing SELECT in one
** procedure must have the same number of result columns; pPrg->nResCol
** records that count (-1 until the first row-producing SELECT is seen).
*/
static void codeProcProgram(
  Parse *pParse,            /* Sub-parse context for the body */
  TriggerStep *pStepList,   /* List of statements in the body */
  ProcPrg *pPrg,            /* Records the result-column count */
  int lblLeave              /* Jump target for LEAVE; 0 if not in a loop */
){
  TriggerStep *pStep;
  Vdbe *v = pParse->pVdbe;
  sqlite3 *db = pParse->db;

  assert( pParse->pToplevel && pParse->pProcCoding );
  assert( v!=0 );
  for(pStep=pStepList; pStep && pParse->nErr==0; pStep=pStep->pNext){
    pParse->eOrconf = pStep->orconf;
    assert( pParse->okConstFactor==0 );

#ifndef SQLITE_OMIT_TRACE
    if( pStep->zSpan ){
      sqlite3VdbeAddOp4(v, OP_Trace, 0x7fffffff, 1, 0,
                        sqlite3MPrintf(db, "-- %s", pStep->zSpan),
                        P4_DYNAMIC);
    }
#endif

    /* A failure part-way through a multi-statement body must undo the
    ** whole CALL, not just the failing statement.  Trigger bodies get a
    ** statement journal because pParse->pTriggerTab is set when their
    ** steps compile; procedure bodies have no trigger table, so request
    ** the journal explicitly for every write step. */
    if( pStep->op!=TK_SELECT ){
      sqlite3MultiWrite(pParse);
      sqlite3MayAbort(pParse);
    }

    switch( pStep->op ){
      case TK_UPDATE: {
        ExprList *pSet = sqlite3ExprListDup(db, pStep->pExprList, 0);
        Expr *pWhere = sqlite3ExprDup(db, pStep->pWhere, 0);
        if( lblLeave ){
          /* Inside a loop: subqueries must re-evaluate every iteration */
          int i;
          procMarkVarSelect(pParse, pWhere, 0);
          for(i=0; pSet && i<pSet->nExpr; i++){
            procMarkVarSelect(pParse, pSet->a[i].pExpr, 0);
          }
        }
        sqlite3Update(pParse,
          sqlite3SrcListDup(db, pStep->pSrc, 0),
          pSet, pWhere, pParse->eOrconf, 0, 0, 0
        );
        break;
      }
      case TK_INSERT: {
        Select *pSel = sqlite3SelectDup(db, pStep->pSelect, 0);
        if( lblLeave ) procMarkVarSelect(pParse, 0, pSel);
        sqlite3Insert(pParse,
          sqlite3SrcListDup(db, pStep->pSrc, 0),
          pSel,
          sqlite3IdListDup(db, pStep->pIdList),
          pParse->eOrconf,
          sqlite3UpsertDup(db, pStep->pUpsert)
        );
        break;
      }
      case TK_DELETE: {
        Expr *pWhere = sqlite3ExprDup(db, pStep->pWhere, 0);
        if( lblLeave ) procMarkVarSelect(pParse, pWhere, 0);
        sqlite3DeleteFrom(pParse,
          sqlite3SrcListDup(db, pStep->pSrc, 0),
          pWhere, 0, 0
        );
        break;
      }
      case TK_CALL: {
        sqlite3CallProc(pParse,
          sqlite3SrcListDup(db, pStep->pSrc, 0),
          sqlite3ExprListDup(db, pStep->pExprList, 0)
        );
        break;
      }
      case TK_DECLARE: {
        /* The variable cell was allocated (and NULL-initialized) by the
        ** body prologue; all that executes here is the DEFAULT, if any. */
        if( pStep->pWhere ){
          int iCell = procVarCell(pParse, pStep->zVar);
          Expr *pDflt = procDupResolveExpr(pParse, pStep->pWhere);
          assert( iCell>0 || pParse->nErr );
          if( pDflt ){
            sqlite3ExprCode(pParse, pDflt, iCell);
            sqlite3ExprDelete(db, pDflt);
          }
        }
        break;
      }
      case TK_SET: {
        int iCell = procVarCell(pParse, pStep->zVar);
        if( iCell==0 ){
          sqlite3ErrorMsg(pParse, "no such variable: %s", pStep->zVar);
        }else{
          Expr *pVal = procDupResolveExpr(pParse, pStep->pWhere);
          if( pVal ){
            sqlite3ExprCode(pParse, pVal, iCell);
            sqlite3ExprDelete(db, pVal);
          }
        }
        break;
      }
      case TK_IF: {
        int lblElse = sqlite3VdbeMakeLabel(pParse);
        int lblEnd = sqlite3VdbeMakeLabel(pParse);
        Expr *pCond = procDupResolveExpr(pParse, pStep->pWhere);
        if( pCond ){
          sqlite3ExprIfFalse(pParse, pCond, lblElse, SQLITE_JUMPIFNULL);
          sqlite3ExprDelete(db, pCond);
        }
        codeProcProgram(pParse, pStep->pThen, pPrg, lblLeave);
        sqlite3VdbeGoto(v, lblEnd);
        sqlite3VdbeResolveLabel(v, lblElse);
        codeProcProgram(pParse, pStep->pElse, pPrg, lblLeave);
        sqlite3VdbeResolveLabel(v, lblEnd);
        break;
      }
      case TK_WHILE: {
        /* A NULL condition is an unconditional LOOP */
        int addrTop = sqlite3VdbeCurrentAddr(v);
        int lblEnd = sqlite3VdbeMakeLabel(pParse);
        if( pStep->pWhere ){
          Expr *pCond = procDupResolveExpr(pParse, pStep->pWhere);
          if( pCond ){
            sqlite3ExprIfFalse(pParse, pCond, lblEnd, SQLITE_JUMPIFNULL);
            sqlite3ExprDelete(db, pCond);
          }
        }
        codeProcProgram(pParse, pStep->pThen, pPrg, lblEnd);
        sqlite3VdbeGoto(v, addrTop);
        sqlite3VdbeResolveLabel(v, lblEnd);
        break;
      }
      case TK_LEAVE: {
        if( lblLeave==0 ){
          sqlite3ErrorMsg(pParse, "LEAVE used outside of a LOOP or WHILE");
        }else{
          sqlite3VdbeGoto(v, lblLeave);
        }
        break;
      }
      case TK_RETURN: {
        sqlite3VdbeAddOp0(v, OP_Halt);
        break;
      }
      case TK_RAISE: {
        if( pStep->orconf==OE_Ignore ){
          sqlite3VdbeAddOp2(v, OP_Halt, SQLITE_OK, OE_Ignore);
        }else{
          Expr *pMsg = procDupResolveExpr(pParse, pStep->pWhere);
          int r1 = ++pParse->nMem;
          if( pMsg ){
            sqlite3ExprCode(pParse, pMsg, r1);
            sqlite3ExprDelete(db, pMsg);
          }
          if( pStep->orconf==OE_Abort ){
            sqlite3MayAbort(pParse);
          }
          sqlite3VdbeAddOp3(v, OP_Halt, SQLITE_ERROR, pStep->orconf, r1);
        }
        break;
      }
      default: assert( pStep->op==TK_SELECT ); {
        if( pStep->pIdList ){
          /* SELECT ... INTO var-list.  Compiled as a row-value subquery
          ** (first row wins; all-NULL if the query returns no rows),
          ** then copied into the target variable cells. */
          Expr *pSub;
          Select *pSelect = sqlite3SelectDup(db, pStep->pSelect, 0);
          IdList *pInto = pStep->pIdList;
          int nInto = pInto->nId;
          NameContext sNC;
          int k, nCol, rReg;
          pSub = sqlite3PExpr(pParse, TK_SELECT, 0, 0);
          if( pSub==0 ){
            sqlite3SelectDelete(db, pSelect);
            break;
          }
          sqlite3PExprAddSelect(pParse, pSub, pSelect);
          /* Never cache across loop iterations */
          procMarkVarSelect(pParse, pSub, 0);
          memset(&sNC, 0, sizeof(sNC));
          sNC.pParse = pParse;
          if( sqlite3ResolveExprNames(&sNC, pSub) ){
            sqlite3ExprDelete(db, pSub);
            break;
          }
          assert( ExprUseXSelect(pSub) );
          nCol = pSub->x.pSelect->pEList->nExpr;
          if( nCol!=nInto ){
            sqlite3ErrorMsg(pParse, "SELECT INTO has %d result column%s "
               "but %d target variable%s", nCol, nCol==1?"":"s",
               nInto, nInto==1?"":"s");
            sqlite3ExprDelete(db, pSub);
            break;
          }
          rReg = sqlite3CodeSubselect(pParse, pSub);
          for(k=0; k<nInto && pParse->nErr==0; k++){
            int iCell = procVarCell(pParse, pInto->a[k].zName);
            if( iCell==0 ){
              sqlite3ErrorMsg(pParse, "no such variable: %s",
                              pInto->a[k].zName);
            }else{
              sqlite3VdbeAddOp2(v, OP_Copy, rReg+k, iCell);
            }
          }
          sqlite3ExprDelete(db, pSub);
        }else{
          SelectDest sDest;
          Select *pSelect = sqlite3SelectDup(db, pStep->pSelect, 0);
          if( lblLeave ){
            /* Inside a loop: subqueries must re-evaluate every iteration */
            procMarkVarSelect(pParse, 0, pSelect);
          }
          sqlite3SelectDestInit(&sDest, SRT_Output, 0);
          sqlite3Select(pParse, pSelect, &sDest);
          if( pParse->nErr==0 && pSelect ){
            int nCol = pSelect->pEList->nExpr;
            Proc *pProcC = pPrg->pProc;
            if( pProcC && pProcC->eRet==PROC_RET_TABLES ){
              /* A declared procedure delimits its sets explicitly.  Each
              ** set was checked against its own declared shape at CREATE
              ** time, so widths may differ between sets; the boundary
              ** opcode switches the statement's metadata between them. */
              sqlite3VdbeAddOp1(v, OP_ProcSetEnd, pPrg->nProcSet);
              pPrg->nProcSet++;
              if( pPrg->nResCol<nCol ) pPrg->nResCol = nCol;
            }else if( pPrg->nResCol<0 ){
              pPrg->nResCol = nCol;
            }else if( pPrg->nResCol!=nCol ){
              sqlite3ErrorMsg(pParse, "all row-producing SELECT statements "
                 "in procedure %s must have the same number of result "
                 "columns", pPrg->pProc->zName);
            }
          }
          sqlite3SelectDelete(db, pSelect);
        }
        break;
      }
    }
  }
}

/*
** Compile the body of pProc into a SubProgram, wrapped in a new ProcPrg
** object linked into the toplevel Parse.  The structure of this function
** deliberately mirrors trigger.c:codeRowTrigger().
**
** The ProcPrg is linked into the list *before* the body is coded so that
** a recursive CALL inside the body finds the in-progress entry (whose
** SubProgram.aOp is populated at the end of this function) instead of
** recursing forever.
*/
static ProcPrg *codeProcBody(Parse *pParse, Proc *pProc){
  Parse *pTop = sqlite3ParseToplevel(pParse);
  sqlite3 *db = pParse->db;
  ProcPrg *pPrg;
  Parse sSubParse;
  Vdbe *v;
  SubProgram *pProgram = 0;
  int nParam = pProc->pParams ? pProc->pParams->nParam : 0;
  int nDepth;
  Parse *pP;

  /* Guard against pathologically deep compile-time nesting of CALLs
  ** (runtime recursion is bounded separately, by the frame-depth check
  ** in OP_Program). */
  for(nDepth=0, pP=pParse; pP->pOuterParse; pP=pP->pOuterParse, nDepth++){}
  if( nDepth>=db->aLimit[SQLITE_LIMIT_TRIGGER_DEPTH] ){
    sqlite3ErrorMsg(pParse, "procedure calls nested too deeply");
    return 0;
  }
  assert( pTop->pVdbe );

  pPrg = sqlite3DbMallocZero(db, sizeof(ProcPrg));
  if( !pPrg ) return 0;
  pPrg->pNext = pTop->pProcPrg;
  pTop->pProcPrg = pPrg;
  pPrg->pProgram = pProgram = sqlite3DbMallocZero(db, sizeof(SubProgram));
  if( !pProgram ) return 0;
  pProgram->nRef = 1;   /* owned by pTop->pVdbe's pProgram list */
  sqlite3VdbeLinkSubProgram(pTop->pVdbe, pProgram);
  pPrg->pProc = pProc;
  pPrg->nResCol = -1;

  /* Allocate and populate a new Parse context for coding the body */
  sqlite3ParseObjectInit(&sSubParse, db);
  sSubParse.pToplevel = pTop;
  sSubParse.zAuthContext = pProc->zName;
  sSubParse.nQueryLoop = pParse->nQueryLoop;
  sSubParse.prepFlags = pParse->prepFlags;
  sSubParse.pProcCoding = pProc;

  v = sqlite3GetVdbe(&sSubParse);
  if( v ){
    int i;
    VdbeComment((v, "Start: proc %s", pProc->zName));
#ifndef SQLITE_OMIT_TRACE
    sqlite3VdbeChangeP4(v, -1,
      sqlite3MPrintf(db, "-- PROCEDURE %s", pProc->zName), P4_DYNAMIC
    );
#endif

    /* Build the flat variable table for the body: parameters first,
    ** then every DECLAREd variable (hoisted; one scope per body). */
    {
      int nVar;
      for(i=0; i<nParam; i++){
        procVarAdd(&sSubParse, &sSubParse.pProcVars,
                   pProc->pParams->a[i].zName);
      }
      procCollectVars(&sSubParse, &sSubParse.pProcVars, pProc->pBody);
      nVar = sSubParse.pProcVars ? sSubParse.pProcVars->nParam : 0;
      sSubParse.iProcParamBase = sSubParse.nMem+1;
      sSubParse.nMem += nVar;

      /* Prologue: copy the arguments (evaluated by the caller into the
      ** register block passed as P1 of OP_Program) into their local
      ** cells, and NULL-initialize the DECLAREd variables (frame cells
      ** start out MEM_Undefined, not NULL). */
      for(i=0; i<nParam; i++){
        sqlite3VdbeAddOp2(v, OP_Param, i, sSubParse.iProcParamBase+i);
        VdbeComment((v, "param %s", pProc->pParams->a[i].zName));
      }
      if( nVar>nParam ){
        sqlite3VdbeAddOp3(v, OP_Null, 0, sSubParse.iProcParamBase+nParam,
                          sSubParse.iProcParamBase+nVar-1);
        VdbeComment((v, "init %d local variable(s)", nVar-nParam));
      }
    }

    /* SECURITY DEFINER: compile the body with the authorizer detached, so its
    ** statements are not checked against the caller's policy.  That is the
    ** whole feature -- the procedure may touch what its caller may not.
    **
    ** Safe because the CALL itself was already authorized (SQLITE_CALL fires
    ** in sqlite3CallProc before the body is compiled or fetched from cache),
    ** so an unchecked body is reachable only through a gate the application
    ** explicitly opened.  Restored unconditionally, including on the error
    ** paths below, because the save/restore brackets only this call.
    **
    ** SECURITY INVOKER -- the default, and everything written before this
    ** feature existed -- takes the untouched path. */
    {
      sqlite3_xauth xAuthSave = db->xAuth;
      if( pProc->bDefiner ) db->xAuth = 0;
      codeProcProgram(&sSubParse, pProc->pBody, pPrg, 0);
      db->xAuth = xAuthSave;
    }

    sqlite3VdbeAddOp0(v, OP_Halt);
    VdbeComment((v, "End: proc %s", pProc->zName));
    procTransferParseError(pParse, &sSubParse);

    if( pParse->nErr==0 ){
      assert( db->mallocFailed==0 );
      pProgram->aOp = sqlite3VdbeTakeOpArray(v, &pProgram->nOp, &pTop->nMaxArg);
      /* If this CALL is itself the top-level statement, its prepared
      ** statement reports the procedure's result columns. */
      if( pParse->pToplevel==0 && pPrg->nResCol>0 ){
        sqlite3VdbeTransferColumnNames(pParse->pVdbe, v, pPrg->nResCol);
      }
    }
    pProgram->nMem = sSubParse.nMem;
    pProgram->nCsr = sSubParse.nTab;
    pProgram->token = (void*)pProc;
    sqlite3VdbeDelete(v);
  }else{
    procTransferParseError(pParse, &sSubParse);
  }

  assert( !sSubParse.pTriggerPrg && !sSubParse.nMaxArg );
  assert( !sSubParse.pProcPrg );
  sqlite3ProcParamListDelete(db, sSubParse.pProcVars);
  sSubParse.pProcVars = 0;
  sqlite3ParseObjectReset(&sSubParse);
  return pPrg;
}

/*
** Free one entry of the per-connection compiled-body cache.
*/
static void procCacheEntryFree(sqlite3 *db, ProcCacheEntry *pE){
  sqlite3SubProgramUnref(db, pE->pProg);
  if( pE->azColName ){
    int i;
    for(i=0; i<pE->nResCol; i++) sqlite3DbFree(db, pE->azColName[i]);
    sqlite3DbFree(db, pE->azColName);
  }
  sqlite3DbFree(db, pE->zProc);
  sqlite3DbFree(db, pE);
}

/*
** Discard every cached compiled procedure body on the connection.
** Called whenever prepared statements are mass-expired (new function
** or collation registrations, schema resets), on DETACH, and at
** connection close.
*/
void sqlite3ProcCacheFlush(sqlite3 *db){
  ProcCacheEntry *pE, *pNext;
  for(pE=db->pProcCache; pE; pE=pNext){
    pNext = pE->pNext;
    procCacheEntryFree(db, pE);
  }
  db->pProcCache = 0;
}

/*
** Look for a valid cached compiled body for pProc.  A hit requires the
** same database slot, the same Schema (pointer identity), an unchanged
** schema cookie, and a name match.
*/
static ProcCacheEntry *procCacheFind(Parse *pParse, Proc *pProc){
  sqlite3 *db = pParse->db;
  int iDb = sqlite3SchemaToIndex(db, pProc->pSchema);
  ProcCacheEntry *pE;
  if( iDb==1 ) return 0;   /* TEMP procedures are never cached */
  for(pE=db->pProcCache; pE; pE=pE->pNext){
    if( pE->iDb==iDb
     && pE->pSchema==pProc->pSchema
     && pE->cookie==(u32)pProc->pSchema->schema_cookie
     && sqlite3StrICmp(pE->zProc, pProc->zName)==0
    ){
      return pE;
    }
  }
  return 0;
}

/*
** After successfully compiling pPrg for a top-level CALL of pProc,
** stash the compiled body in the connection cache if it is
** self-contained enough to be safely reused by other statements:
**
**  - not a TEMP procedure (TEMP bodies may reference any database, so
**    a single schema cookie cannot guard them);
**  - not in shared-cache mode (the Schema, and hence the Proc, may be
**    shared across connections; this cache is strictly per-connection);
**  - the body did not touch AUTOINCREMENT tables (autoinc opcodes
**    address counters in the toplevel frame by register number, which
**    differs between statements);
**  - the body embeds no other sub-programs (trigger programs and
**    nested CALLs are owned by the compiling statement; sharing them
**    would require cycle-aware reference counting).
*/
static void procCachePopulate(
  Parse *pParse,        /* The (toplevel) parse of the CALL statement */
  Proc *pProc,          /* The procedure that was compiled */
  ProcPrg *pPrg,        /* Its compiled body */
  void *pAincBefore     /* Value of pParse->pAinc before compilation */
){
  sqlite3 *db = pParse->db;
  int iDb = sqlite3SchemaToIndex(db, pProc->pSchema);
  ProcCacheEntry *pE, **pp;
  SubProgram *pProg = pPrg->pProgram;
  int i;

  assert( pParse->pToplevel==0 );
  if( iDb<0 || iDb==1 ) return;
  if( db->aDb[iDb].pBt==0 || sqlite3BtreeSharable(db->aDb[iDb].pBt) ) return;
  if( (void*)pParse->pAinc!=pAincBefore ) return;
  if( pProg==0 || pProg->aOp==0 ) return;
  for(i=0; i<pProg->nOp; i++){
    if( pProg->aOp[i].p4type==P4_SUBPROGRAM ) return;
  }

  /* Replace any existing (now stale) entry for this procedure */
  for(pp=&db->pProcCache; (pE=*pp)!=0; pp=&pE->pNext){
    if( pE->iDb==iDb && sqlite3StrICmp(pE->zProc, pProc->zName)==0 ){
      *pp = pE->pNext;
      procCacheEntryFree(db, pE);
      break;
    }
  }

  pE = sqlite3DbMallocZero(db, sizeof(*pE));
  if( pE==0 ) return;
  pE->zProc = sqlite3DbStrDup(db, pProc->zName);
  if( pE->zProc==0 ){
    sqlite3DbFree(db, pE);
    return;
  }
  pE->iDb = iDb;
  pE->pSchema = pProc->pSchema;
  pE->cookie = (u32)pProc->pSchema->schema_cookie;
  pE->pProg = pProg;
  pProg->nRef++;
  pE->nResCol = pPrg->nResCol;
  pE->nMaxArg = pParse->nMaxArg;
  if( DbMaskTest(pParse->writeMask, iDb) ) pE->flags |= PROCCACHE_WRITES;
  if( pParse->mayAbort ) pE->flags |= PROCCACHE_MAYABORT;
  if( pE->nResCol>0 && pParse->pVdbe ){
    pE->azColName = sqlite3VdbeCaptureColumnNames(pParse->pVdbe, pE->nResCol);
  }
  pE->pNext = db->pProcCache;
  db->pProcCache = pE;
}

/*
** This is called by the parser for a CALL statement.
**
** The procedure body is compiled (once per statement; recursive CALLs
** share the entry linked into Parse.pProcPrg) into a VDBE SubProgram.
** The generated code evaluates the arguments into a contiguous register
** block and invokes the body via OP_Program.  Rows produced by bare
** SELECT statements in the body stream out of the sub-frame through the
** ordinary sqlite3_step() interface.
*/
void sqlite3CallProc(Parse *pParse, SrcList *pName, ExprList *pArgs){
  sqlite3 *db = pParse->db;
  Proc *pProc = 0;
  ProcPrg *pPrg;
  Parse *pTop;
  Vdbe *v;
  const char *zDb;
  const char *zName;
  int nArg;
  int nParam;
  int rBase;
  int iMem;
  int addr;
  int iDb;
  int i;

  if( db->mallocFailed ) goto call_cleanup;
  if( SQLITE_OK!=sqlite3ReadSchema(pParse) ){
    goto call_cleanup;
  }

  assert( pName->nSrc==1 );
  assert( pName->a[0].fg.isSubquery==0 );
  if( pName->a[0].fg.fixedSchema ){
    /* A CALL step inside a stored procedure body: the DbFixer pinned the
    ** name to the schema containing the calling procedure. */
    int iDbFix = sqlite3SchemaToIndex(db, pName->a[0].u4.pSchema);
    zDb = db->aDb[iDbFix].zDbSName;
  }else{
    zDb = pName->a[0].u4.zDatabase;
  }
  zName = pName->a[0].zName;
  pProc = sqlite3FindProc(pParse, zName, zDb);
  if( !pProc ){
    sqlite3ErrorMsg(pParse, "no such procedure: %S", pName->a);
    pParse->checkSchema = 1;
    goto call_cleanup;
  }

  nArg = pArgs ? pArgs->nExpr : 0;
  nParam = pProc->pParams ? pProc->pParams->nParam : 0;
  if( nArg!=nParam ){
    sqlite3ErrorMsg(pParse,
       "procedure %s expects %d argument%s but %d %s supplied",
       pProc->zName, nParam, nParam==1 ? "" : "s",
       nArg, nArg==1 ? "was" : "were");
    goto call_cleanup;
  }

#ifndef SQLITE_OMIT_AUTHORIZATION
  /* Authorize the INVOCATION, distinct from anything the body touches.
  **
  ** Body statements are already authorized -- they are compiled through the
  ** trigger machinery with pParse->zAuthContext set to the procedure name, so
  ** an application can already write policy like "deny reads of `secret` when
  ** the context is `leaky`".  What that cannot express is "you may not call
  ** this procedure at all", independently of which tables its body happens to
  ** use.  That is the gate a procedure needs to be a privilege boundary rather
  ** than a macro.
  **
  ** Placed here deliberately: after the procedure is resolved and its arity
  ** checked (so a denial cannot be used to probe for existence or signature
  ** beyond what resolution already reveals), and BEFORE the body is compiled
  ** or fetched from the cache. A future security level that runs a trusted
  ** body without authorizing its statements must still pass through this
  ** point, which is then the only gate.
  **
  ** The database name passed is the one the procedure actually lives in,
  ** resolved from its schema rather than from the (possibly unqualified)
  ** name the caller wrote -- otherwise `CALL p()` and `CALL main.p()` would
  ** present differently to the same policy. */
  if( sqlite3AuthCheck(pParse, SQLITE_CALL, pProc->zName, 0,
                       db->aDb[sqlite3SchemaToIndex(db, pProc->pSchema)].zDbSName)
  ){
    goto call_cleanup;
  }
#endif

  /* Nested result shapes are declarable and conformance-checked (PLAN-NESTED
  ** phases 1-2) but not yet streamable, so CALL is refused rather than served
  ** wrong.  Measured before this guard existed: sqlite3_column_count reported
  ** the 3 DECLARED columns while the parent SELECT wrote 2 result registers,
  ** so the third column read a register the body never assigned and handed the
  ** client a fabricated value.  Reporting the scalar count instead would be
  ** quieter and no more honest -- it would drop the nested table with no
  ** indication it was declared.  Phase 5 removes this by materialising the
  ** nested column; until then the feature is absent at CALL rather than
  ** half-present.  See test/proc6.test section 6. */
  if( pProc->eRet==PROC_RET_TABLES && procShapesNest(pProc->pShapes) ){
    sqlite3ErrorMsg(pParse,
      "procedure %s declares a nested result table, which cannot yet be "
      "streamed by CALL", pProc->zName);
    goto call_cleanup;
  }

  v = sqlite3GetVdbe(pParse);
  if( v==0 ) goto call_cleanup;

  /* The compiled body is guarded by the schema cookie of the database
  ** holding the procedure: any schema change expires the statement. */
  iDb = sqlite3SchemaToIndex(db, pProc->pSchema);
  sqlite3CodeVerifySchema(pParse, iDb);

  /* Find the compiled body: already coded in this statement, cached on
  ** the connection from an earlier statement, or compiled fresh. */
  pTop = sqlite3ParseToplevel(pParse);
  for(pPrg=pTop->pProcPrg; pPrg && pPrg->pProc!=pProc; pPrg=pPrg->pNext){}
  if( pPrg==0 ){
    ProcCacheEntry *pE = procCacheFind(pParse, pProc);
    if( pE ){
      /* Cache hit: reuse the compiled body, replaying the toplevel
      ** bookkeeping that compiling it would have performed. */
      pPrg = sqlite3DbMallocZero(db, sizeof(ProcPrg));
      if( pPrg==0 ) goto call_cleanup;
      pPrg->pNext = pTop->pProcPrg;
      pTop->pProcPrg = pPrg;
      pPrg->pProc = pProc;
      pPrg->pProgram = pE->pProg;
      pPrg->nResCol = pE->nResCol;
      if( sqlite3VdbeAttachSubProgram(pTop->pVdbe, pE->pProg) ){
        goto call_cleanup;
      }
      if( pTop->nMaxArg<pE->nMaxArg ) pTop->nMaxArg = pE->nMaxArg;
      if( pE->flags & PROCCACHE_WRITES ){
        sqlite3BeginWriteOperation(pParse, 1, pE->iDb);
        sqlite3MultiWrite(pParse);
      }
      if( pE->flags & PROCCACHE_MAYABORT ){
        sqlite3MayAbort(pParse);
      }
      if( pParse->pToplevel==0 && pE->nResCol>0 ){
        int k;
        sqlite3VdbeSetNumCols(v, pE->nResCol);
        for(k=0; k<pE->nResCol; k++){
          const char *z = pE->azColName ? pE->azColName[k] : 0;
          if( z ){
            sqlite3VdbeSetColName(v, k, COLNAME_NAME, z, SQLITE_TRANSIENT);
          }else{
            char *zGen = sqlite3MPrintf(db, "column%d", k+1);
            sqlite3VdbeSetColName(v, k, COLNAME_NAME, zGen, SQLITE_DYNAMIC);
          }
        }
      }
    }else{
      void *pAincBefore = (void*)pTop->pAinc;
      pPrg = codeProcBody(pParse, pProc);
      if( pPrg==0 || pParse->nErr || db->mallocFailed ) goto call_cleanup;
      if( pParse->pToplevel==0 ){
        procCachePopulate(pParse, pProc, pPrg, pAincBefore);
      }
    }
  }

  /* Declared result shapes (RETURNS TABLE / RETURNS NOTHING): a toplevel
  ** prepared CALL reports the FIRST declared shape through the standard
  ** column-metadata interfaces, exactly as if it were a SELECT with those
  ** columns -- declared names and types win over whatever the body's
  ** first SELECT happened to be spelled as.  This intentionally runs
  ** after all three body-acquisition paths above so it supersedes both
  ** the fresh-compile column-name transfer and the cache-hit replay.
  ** Undeclared procedures keep the legacy behavior untouched. */
  if( pParse->pToplevel==0 && pProc->eRet!=PROC_RET_UNDECLARED ){
    Vdbe *vTop = pParse->pVdbe;
    if( pProc->eRet==PROC_RET_NOTHING ){
      sqlite3VdbeSetNumCols(vTop, 0);
    }else{
      /* Copies every declared shape onto the statement, sizes the column
      ** name array for the widest one, and applies set 1.  Advancing
      ** between sets at run time then swaps metadata without allocating. */
      sqlite3VdbeSetProcShapes(vTop, pProc->pShapes);
    }
  }

  /* If this CALL occurs inside another procedure's body, rows produced by
  ** the callee stream out through the caller's frame too, so the callee's
  ** result shape must be merged into (and agree with) the caller's.
  ** (For an in-progress recursive compile the callee's count is not yet
  ** known; its own SELECTs will establish the shape instead.) */
  if( pParse->pProcCoding && pPrg->nResCol>=0 ){
    ProcPrg *pPrgCaller;
    for(pPrgCaller=pTop->pProcPrg;
        pPrgCaller && pPrgCaller->pProc!=pParse->pProcCoding;
        pPrgCaller=pPrgCaller->pNext){}
    if( pPrgCaller ){
      if( pPrgCaller->nResCol<0 ){
        pPrgCaller->nResCol = pPrg->nResCol;
      }else if( pPrgCaller->nResCol!=pPrg->nResCol ){
        sqlite3ErrorMsg(pParse, "all row-producing SELECT statements "
           "in procedure %s must have the same number of result columns",
           pParse->pProcCoding->zName);
        goto call_cleanup;
      }
    }
  }

  /* Resolve names in the argument expressions.  There is no row context
  ** at a CALL site, so column references fail here -- except references
  ** to the parameters of an enclosing procedure body, which the resolver
  ** handles via Parse.pProcCoding. */
  if( nArg>0 ){
    NameContext sNC;
    memset(&sNC, 0, sizeof(sNC));
    sNC.pParse = pParse;
    if( sqlite3ResolveExprListNames(&sNC, pArgs) ) goto call_cleanup;
  }

  /* Evaluate the arguments into a contiguous register block */
  rBase = pParse->nMem+1;
  pParse->nMem += nArg;
  for(i=0; i<nArg; i++){
    sqlite3ExprCode(pParse, pArgs->a[i].pExpr, rBase+i);
  }

  /* Invoke the body.  P2 (the RAISE(IGNORE) jump) is the instruction
  ** after OP_Program.  P5 is left clear: recursive invocation is allowed
  ** and bounded by the frame-depth check inside OP_Program. */
  iMem = ++pParse->nMem;
  addr = sqlite3VdbeAddOp4(v, OP_Program, rBase, 0, iMem,
                           (const char*)pPrg->pProgram, P4_SUBPROGRAM);
  sqlite3VdbeJumpHere(v, addr);
  VdbeComment((v, "Call: %s", pProc->zName));

call_cleanup:
  sqlite3SrcListDelete(db, pName);
  sqlite3ExprListDelete(db, pArgs);
}

#endif /* !defined(SQLITE_OMIT_PROCEDURE) */
