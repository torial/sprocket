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
** Delete a procedure parameter list and all of its content.
*/
void sqlite3ProcParamListDelete(sqlite3 *db, ProcParamList *pList){
  int i;
  if( pList==0 ) return;
  for(i=0; i<pList->nParam; i++){
    sqlite3DbFree(db, pList->a[i].zName);
    sqlite3DbFree(db, pList->a[i].zType);
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
  TriggerStep *pStepList, /* Body of the procedure */
  int isTemp,             /* True if the TEMP keyword is present */
  int noErr,              /* True if IF NOT EXISTS is present */
  Token *pAll             /* Text from procedure name through END */
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

  /* Resolve or reject database qualifiers inside the body */
  sqlite3FixInit(&sFix, pParse, iDb, "procedure", pName);
  if( sqlite3FixTriggerStep(&sFix, pStepList) ){
    goto proc_cleanup;
  }

  /* Build the Proc object */
  pProc = sqlite3DbMallocZero(db, sizeof(Proc));
  if( pProc==0 ) goto proc_cleanup;
  pProc->zName = zName;
  zName = 0;
  pProc->pParams = pParams;
  pParams = 0;
  pProc->pBody = pStepList;
  pStepList = 0;
  pProc->pSchema = db->aDb[iDb].pSchema;

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
  ProcPrg *pPrg             /* Records the result-column count */
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
        sqlite3Update(pParse,
          sqlite3SrcListDup(db, pStep->pSrc, 0),
          sqlite3ExprListDup(db, pStep->pExprList, 0),
          sqlite3ExprDup(db, pStep->pWhere, 0),
          pParse->eOrconf, 0, 0, 0
        );
        break;
      }
      case TK_INSERT: {
        sqlite3Insert(pParse,
          sqlite3SrcListDup(db, pStep->pSrc, 0),
          sqlite3SelectDup(db, pStep->pSelect, 0),
          sqlite3IdListDup(db, pStep->pIdList),
          pParse->eOrconf,
          sqlite3UpsertDup(db, pStep->pUpsert)
        );
        break;
      }
      case TK_DELETE: {
        sqlite3DeleteFrom(pParse,
          sqlite3SrcListDup(db, pStep->pSrc, 0),
          sqlite3ExprDup(db, pStep->pWhere, 0), 0, 0
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
      default: assert( pStep->op==TK_SELECT ); {
        SelectDest sDest;
        Select *pSelect = sqlite3SelectDup(db, pStep->pSelect, 0);
        sqlite3SelectDestInit(&sDest, SRT_Output, 0);
        sqlite3Select(pParse, pSelect, &sDest);
        if( pParse->nErr==0 && pSelect ){
          int nCol = pSelect->pEList->nExpr;
          if( pPrg->nResCol<0 ){
            pPrg->nResCol = nCol;
          }else if( pPrg->nResCol!=nCol ){
            sqlite3ErrorMsg(pParse, "all row-producing SELECT statements "
               "in procedure %s must have the same number of result columns",
               pPrg->pProc->zName);
          }
        }
        sqlite3SelectDelete(db, pSelect);
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

    /* Copy the arguments (evaluated by the caller into the register
    ** block passed as P1 of OP_Program) into fixed local cells.  All
    ** parameter references inside the body resolve to these cells. */
    sSubParse.iProcParamBase = sSubParse.nMem+1;
    sSubParse.nMem += nParam;
    for(i=0; i<nParam; i++){
      sqlite3VdbeAddOp2(v, OP_Param, i, sSubParse.iProcParamBase+i);
      VdbeComment((v, "param %s", pProc->pParams->a[i].zName));
    }

    codeProcProgram(&sSubParse, pProc->pBody, pPrg);

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
  sqlite3ParseObjectReset(&sSubParse);
  return pPrg;
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

  v = sqlite3GetVdbe(pParse);
  if( v==0 ) goto call_cleanup;

  /* The compiled body is guarded by the schema cookie of the database
  ** holding the procedure: any schema change expires the statement. */
  iDb = sqlite3SchemaToIndex(db, pProc->pSchema);
  sqlite3CodeVerifySchema(pParse, iDb);

  /* Find or create the compiled body */
  pTop = sqlite3ParseToplevel(pParse);
  for(pPrg=pTop->pProcPrg; pPrg && pPrg->pProc!=pProc; pPrg=pPrg->pNext){}
  if( pPrg==0 ){
    pPrg = codeProcBody(pParse, pProc);
    if( pPrg==0 || pParse->nErr || db->mallocFailed ) goto call_cleanup;
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
