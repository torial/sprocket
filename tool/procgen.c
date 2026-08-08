/*
** procgen.c -- generate a typed C client from a database's declared procedures.
**
** THE POINT
**
** Declared result shapes are validated at CREATE PROCEDURE time and are
** introspectable through PRAGMA proc_list / PRAGMA proc_info. So the complete
** request/response contract of a database is machine-readable **without
** executing anything**. That is what makes generation possible here and not in
** a general SQL engine, where a query's shape is only known after planning.
**
** WHAT IT EMITS
**
** A single self-contained header of `static` functions -- no build wiring, no
** link step, include it and call it:
**
**     int   greet_prepare(sqlite3*, greet_stmt**);
**     int   greet_bind(greet_stmt*, const char *who, sqlite3_int64 n);
**     int   greet_step(greet_stmt*);                  -- ROW / DONE
**     const unsigned char *greet_rs1_msg(greet_stmt*);
**     sqlite3_int64        greet_rs1_cnt(greet_stmt*);
**     int   greet_next_resultset(greet_stmt*);
**     void  greet_finalize(greet_stmt*);
**
** Accessors rather than materialised structs, deliberately: it keeps SQLite's
** zero-copy semantics and raises no questions about who owns a string.
**
** Emission is DETERMINISTIC -- procedures and columns are emitted in a fixed
** order -- so regenerating an unchanged schema produces byte-identical output
** and a diff means the contract really moved.
**
** Build (from repo root, VS dev prompt):
**   cl /nologo /O2 /I. tool\procgen.c sqlite3.c /Fe:procgen.exe
** Use:
**   procgen.exe app.db > app_client.h
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif
#include "sqlite3.h"

static FILE *out;

static void die(const char *zWhat, sqlite3 *db){
  fprintf(stderr, "procgen: %s: %s\n", zWhat, db ? sqlite3_errmsg(db) : "?");
  exit(1);
}

/*
** Make a C identifier out of a SQL name.  Anything outside [A-Za-z0-9_]
** becomes '_', and a leading digit is prefixed, so the generator cannot emit
** source that fails to compile because someone named a column "order by".
*/
static void emitIdent(const char *z){
  int i;
  if( z==0 || z[0]==0 ){ fputs("_", out); return; }
  if( isdigit((unsigned char)z[0]) ) fputc('_', out);
  for(i=0; z[i]; i++){
    unsigned char c = (unsigned char)z[i];
    fputc( (isalnum(c) || c=='_') ? c : '_', out);
  }
}

/*
** SQLite decltypes are advisory, so map by affinity the way SQLite itself
** does, and fall back to the generic accessor rather than guessing.
*/
typedef enum { T_INT, T_REAL, T_TEXT, T_BLOB, T_ANY } CType;

static CType typeOf(const char *zDecl){
  char z[64];
  int i;
  if( zDecl==0 ) return T_ANY;
  for(i=0; i<(int)sizeof(z)-1 && zDecl[i]; i++) z[i] = (char)toupper((unsigned char)zDecl[i]);
  z[i] = 0;
  if( strstr(z,"INT") ) return T_INT;
  if( strstr(z,"CHAR") || strstr(z,"CLOB") || strstr(z,"TEXT") ) return T_TEXT;
  if( strstr(z,"BLOB") ) return T_BLOB;
  if( strstr(z,"REAL") || strstr(z,"FLOA") || strstr(z,"DOUB") ) return T_REAL;
  return T_ANY;
}

/*
** Two different C types are wanted for the same SQL type: sqlite3_column_text()
** yields `const unsigned char *`, but a caller binding a parameter has a
** `const char *` (a string literal, say).  Emitting the accessor type on the
** bind side made callers cast at every call, which is exactly the friction a
** generated client exists to remove.
*/
static const char *bindTypeName(CType t){
  switch( t ){
    case T_INT:  return "sqlite3_int64";
    case T_REAL: return "double";
    case T_TEXT: return "const char *";
    case T_BLOB: return "const void *";
    default:     return "sqlite3_value *";
  }
}

static const char *cTypeName(CType t){
  switch( t ){
    case T_INT:  return "sqlite3_int64";
    case T_REAL: return "double";
    case T_TEXT: return "const unsigned char *";
    case T_BLOB: return "const void *";
    default:     return "sqlite3_value *";
  }
}
static const char *columnFn(CType t){
  switch( t ){
    case T_INT:  return "sqlite3_column_int64";
    case T_REAL: return "sqlite3_column_double";
    case T_TEXT: return "sqlite3_column_text";
    case T_BLOB: return "sqlite3_column_blob";
    default:     return "sqlite3_column_value";
  }
}
static const char *bindFn(CType t){
  switch( t ){
    case T_INT:  return "sqlite3_bind_int64";
    case T_REAL: return "sqlite3_bind_double";
    case T_TEXT: return "sqlite3_bind_text";
    case T_BLOB: return "sqlite3_bind_blob";
    default:     return "sqlite3_bind_value";
  }
}

/*
** Zebra type names.  bNested survives for the C emitter's fold column; the
** Zebra emitter no longer passes it -- a nested table is emitted as
** List(ChildStruct) stitched from the @segments stream, not as JSON text.
*/
static const char *zebraTypeName(CType t, int bNested){
  if( bNested ) return "str";
  switch( t ){
    case T_INT:  return "int";
    case T_REAL: return "float";
    case T_TEXT: return "str";
    case T_BLOB: return "str";   /* no distinct bytes type in Zebra today */
    default:     return "str";
  }
}
static const char *zebraAccessor(CType t, int bNested){
  if( bNested ) return "asStr";
  switch( t ){
    case T_INT:  return "asInt";
    case T_REAL: return "asFloat";
    default:     return "asStr";
  }
}

/* A Zebra identifier with an upper-cased first letter, for type names. */
static void emitIdentCap(const char *z){
  int i;
  if( z==0 || z[0]==0 ){ fputs("_", out); return; }
  if( isdigit((unsigned char)z[0]) ) fputc('_', out);
  fputc(toupper((unsigned char)z[0]), out);
  for(i=1; z[i]; i++){
    unsigned char c = (unsigned char)z[i];
    fputc( (isalnum(c) || c=='_') ? c : '_', out);
  }
}

/* One column or parameter. */
typedef struct Col {
  int iSet;
  int iPos;
  char *zName;
  char *zDecl;
} Col;

/* Find a column by set and name; NULL if absent. */
static Col *colFind(Col *aCol, int nCol, int iSet, const char *zName){
  int i;
  for(i=0; i<nCol; i++){
    if( aCol[i].iSet==iSet && sqlite3_stricmp(aCol[i].zName, zName)==0 ){
      return &aCol[i];
    }
  }
  return 0;
}

/* PLAN-DEPTH tooling (bugbook BUG-3): the nested tree, reconstructed from
** PRAGMA proc_nested's PRE-ORDER rows.  proc_nested tells us each nested
** table's segment and keys but not its PARENT; the parent is recoverable
** because the pragma's numbering is the same pre-order every engine walk
** shares: a set's nested columns (empty decltype in proc_info) consume the
** next entries in order, each with its own subtree before its next sibling.
*/
typedef struct ZNest {
  int iSet;             /* this nested table's segment (1-based) */
  int iParentSet;       /* segment of the table that CONTAINS it */
  char *zCol;           /* declared column it hangs from */
  char *zKChild;
  char *zKParent;
} ZNest;


/* BUG-3 follow-on: a database column named like a ZEBRA KEYWORD (e.g.
** "aspect" -- kw_aspect, aspect-oriented declarations) is unusable as a
** struct field in any position.  Colliding fields are emitted with a
** trailing underscore; the ACCESSOR still reads the real column name, so
** only the client-visible spelling shifts.  The list is extracted from
** zebra-language grammar.txt (kw_* tokens, 2026-08-08) -- if zebra grows a
** keyword this list has not heard of, the failure is a LOUD parse error in
** generated code, never a silent misread; regenerate the list from the
** grammar when that happens.  (Contextual keywords are the real fix and
** are filed upstream.)
*/
static const char *azZebraKw[] = {
  "abstract","adds","allocate","and","arena","as","aspect","assert",
  "assert_eq","assert_false","assert_ne","assert_true","bool","branch",
  "break","capture","catch","char","class","const","continue","cue","def",
  "defer","else","ensure","enum","errdefer","error","except","expect",
  "export","exposing","extend","extern","false","float","for","guard","has",
  "if","implements","in","int","interface","internal","invariant","is",
  "lock","mixin","namespace","nil","not","old","on","or","orelse","pass",
  "print","private","protected","public","raise","readonly","require",
  "result","return","same","sig","static","struct","test","this","throws",
  "to","true","type","uint","union","use","using","var","vari","weaves",
  "where","while","with", 0
};
static void emitZField(const char *z){
  int i;
  emitIdent(z);
  for(i=0; azZebraKw[i]; i++){
    if( sqlite3_stricmp(z, azZebraKw[i])==0 ){ fputc('_', out); return; }
  }
}

static int znestIsVal(Col *pC){ return pC->zDecl && pC->zDecl[0]; }

static int znestAssign(Col *aCol, int nCol, ZNest *aN, int nN,
                       int iParentSet, int iCursor){
  int i;
  for(i=0; i<nCol; i++){
    if( aCol[i].iSet!=iParentSet ) continue;
    if( znestIsVal(&aCol[i]) ) continue;          /* value column */
    if( iCursor>=nN ) return iCursor;
    aN[iCursor].iParentSet = iParentSet;
    iCursor = znestAssign(aCol, nCol, aN, nN, aN[iCursor].iSet, iCursor+1);
  }
  return iCursor;
}

static ZNest *znestOf(ZNest *aN, int nN, int iParentSet, const char *zCol){
  int k;
  for(k=0; k<nN; k++){
    if( aN[k].iParentSet==iParentSet
     && sqlite3_stricmp(aN[k].zCol, zCol)==0 ) return &aN[k];
  }
  return 0;
}

/* Struct declarations, POST-ORDER so every List(Child) names a struct
** already declared above it. */
static void znestStructs(Col *aCol, int nCol, ZNest *aN, int nN,
                         const char *zProc, int iParentSet){
  int k, i;
  for(k=0; k<nN; k++){
    if( aN[k].iParentSet!=iParentSet ) continue;
    znestStructs(aCol, nCol, aN, nN, zProc, aN[k].iSet);
    fputs("struct ", out); emitIdentCap(zProc); emitIdentCap(aN[k].zCol);
    fputc('\n', out);
    for(i=0; i<nCol; i++){
      if( aCol[i].iSet!=aN[k].iSet ) continue;
      fputs("    var ", out); emitZField(aCol[i].zName);
      if( znestIsVal(&aCol[i]) ){
        fprintf(out, ": %s\n", zebraTypeName(typeOf(aCol[i].zDecl), 0));
      }else{
        fputs(": List(", out); emitIdentCap(zProc);
        emitIdentCap(aCol[i].zName); fputs(")\n", out);
      }
    }
    fputc('\n', out);
  }
}

static void zind(int n){ int i; for(i=0;i<n;i++) fputc(' ', out); }

static const char *zvalAcc(Col *aCol, int nCol, int iSet, const char *zName){
  int i;
  for(i=0; i<nCol; i++){
    if( aCol[i].iSet==iSet && znestIsVal(&aCol[i])
     && sqlite3_stricmp(aCol[i].zName, zName)==0 ){
      return zebraAccessor(typeOf(aCol[i].zDecl), 0);
    }
  }
  return "asInt";
}

static int zvalIsText(Col *aCol, int nCol, int iSet, const char *zName){
  int i;
  for(i=0; i<nCol; i++){
    if( aCol[i].iSet==iSet && znestIsVal(&aCol[i])
     && sqlite3_stricmp(aCol[i].zName, zName)==0 ){
      return typeOf(aCol[i].zDecl)==T_TEXT;
    }
  }
  return 0;
}

/* One nested table's collection: declare the list, loop the rows of its
** segment, match the correlation against the ENCLOSING scope's row var,
** collect its own descendants first (recursion), construct, add.  Loop
** variables are numbered by a GLOBAL counter -- two siblings live in the
** same scope, so depth alone would redeclare. */
static void znestCollect(Col *aCol, int nCol, ZNest *aN, int nN,
                         const char *zProc, ZNest *pE,
                         const char *zRowVar, int nInd, int *pCtr){
  char zVar[24];
  int i, me = (*pCtr)++;
  sqlite3_snprintf(sizeof(zVar), zVar, "c%d", me);

  zind(nInd); fputs("var v_", out); emitIdent(pE->zCol);
  fputs(": List(", out); emitIdentCap(zProc); emitIdentCap(pE->zCol);
  fputs(") = []\n", out);
  zind(nInd); fprintf(out, "for %s in rows\n", zVar);
  zind(nInd+4);
  fprintf(out, "if %s.asInt(\"_segment\") == %d\n", zVar, pE->iSet-1);
  if( zvalIsText(aCol, nCol, pE->iSet, pE->zKChild) ){
    /* TEXT keys: rows from the deferred path are untyped to Zebra, so a
    ** bare == on two asStr() calls emits a raw Zig slice compare, which is
    ** not allowed.  Binding to ANNOTATED locals first tells Zebra both
    ** sides are str, and it lowers real string equality.  Integer keys keep
    ** the single-line form (and the byte-identity of every existing
    ** generated client). */
    zind(nInd+8);
    fprintf(out, "var ka%d: str = %s.asStr(\"%s\")\n", me, zVar, pE->zKChild);
    zind(nInd+8);
    fprintf(out, "var kb%d: str = %s.asStr(\"%s\")\n", me, zRowVar,
            pE->zKParent);
    zind(nInd+8);
    fprintf(out, "if ka%d == kb%d\n", me, me);
  }else{
    zind(nInd+8);
    fprintf(out, "if %s.%s(\"%s\") == %s.%s(\"%s\")\n",
            zVar, zvalAcc(aCol, nCol, pE->iSet, pE->zKChild), pE->zKChild,
            zRowVar, zvalAcc(aCol, nCol, pE->iParentSet, pE->zKParent),
            pE->zKParent);
  }
  /* descendants first, inside the correlation match */
  for(i=0; i<nCol; i++){
    ZNest *pE2;
    if( aCol[i].iSet!=pE->iSet || znestIsVal(&aCol[i]) ) continue;
    pE2 = znestOf(aN, nN, pE->iSet, aCol[i].zName);
    if( pE2 ){
      znestCollect(aCol, nCol, aN, nN, zProc, pE2, zVar, nInd+12, pCtr);
    }
  }
  zind(nInd+12); fputs("v_", out); emitIdent(pE->zCol);
  fputs(".add(", out); emitIdentCap(zProc); emitIdentCap(pE->zCol);
  fputc('(', out);
  {
    int nSeen = 0;
    for(i=0; i<nCol; i++){
      if( aCol[i].iSet!=pE->iSet ) continue;
      if( nSeen++ ) fputs(", ", out);
      emitZField(aCol[i].zName); fputs(": ", out);
      if( znestIsVal(&aCol[i]) ){
        fprintf(out, "%s.%s(\"%s\")", zVar,
                zebraAccessor(typeOf(aCol[i].zDecl), 0), aCol[i].zName);
      }else{
        fputs("v_", out); emitIdent(aCol[i].zName);
      }
    }
  }
  fputs("))\n", out);
}

int main(int argc, char **argv){
  sqlite3 *db = 0;
  sqlite3_stmt *pList = 0;
  const char *zDb;
  const char *zLang = "c";

  if( argc<2 ){
    fprintf(stderr, "usage: procgen DATABASE [> out.h]\n");
    return 1;
  }
  zDb = argv[1];
  out = stdout;
  {
    int a;
    for(a=2; a<argc-1; a++){
      if( strcmp(argv[a], "--lang")==0 ) zLang = argv[a+1];
    }
  }
#ifdef _WIN32
  /* Emit LF, not CRLF.  Windows text mode silently translates every '\n' into
  ** "\r\n", and the Zebra compiler REJECTS CRLF outright:
  **
  **     4:1: unexpected '\r' (CRLF line endings - convert to LF)
  **
  ** So on Windows the generator produced output that read perfectly and would
  ** not compile.  Caught only by compiling the generated file, which is why the
  ** contract for a generator is "generate, compile, and call" rather than
  ** "generate and eyeball". */
  _setmode(_fileno(stdout), _O_BINARY);
#endif

  if( sqlite3_open_v2(zDb, &db, SQLITE_OPEN_READONLY, 0)!=SQLITE_OK ){
    die("open", db);
  }

  if( strcmp(zLang, "zebra")==0 ){
    fprintf(out,
      "# GENERATED by tool/procgen.c from \"%s\" -- do not edit.\n"
      "# Regenerating an unchanged schema produces byte-identical output, so a\n"
      "# diff here means the database's procedure contract actually moved.\n\n",
      zDb);
  }else
  fprintf(out,
    "/* GENERATED by tool/procgen.c from \"%s\" -- do not edit.\n"
    "** Regenerating an unchanged schema produces byte-identical output, so a\n"
    "** diff here means the database's procedure contract actually moved.\n"
    "*/\n"
    "#ifndef PROCGEN_CLIENT_H\n"
    "#define PROCGEN_CLIENT_H\n"
    "#include \"sqlite3.h\"\n\n", zDb);

  /* Deterministic order: by name.  proc_list's natural order is hash order,
  ** which would make the output churn for no reason. */
  if( sqlite3_prepare_v2(db,
        "SELECT name, nparams, nresultsets FROM pragma_proc_list"
        " WHERE declared ORDER BY name", -1, &pList, 0)!=SQLITE_OK ){
    die("prepare proc_list", db);
  }

  while( sqlite3_step(pList)==SQLITE_ROW ){
    const char *zProc = (const char*)sqlite3_column_text(pList, 0);
    int nParam = sqlite3_column_int(pList, 1);
    int nSet   = sqlite3_column_int(pList, 2);
    sqlite3_stmt *pInfo = 0;
    Col *aCol = 0;
    int nCol = 0, nAlloc = 0;
    int i, iSet, nNestSeen = 0;

    /* Collect the whole signature in one query: set 0 = parameters,
    ** 1..n = declared result shapes. */
    if( sqlite3_prepare_v2(db,
          "SELECT resultset_index, position, name, decltype"
          "  FROM pragma_proc_info(?1)"
          " ORDER BY resultset_index, position", -1, &pInfo, 0)!=SQLITE_OK ){
      die("prepare proc_info", db);
    }
    sqlite3_bind_text(pInfo, 1, zProc, -1, SQLITE_TRANSIENT);
    while( sqlite3_step(pInfo)==SQLITE_ROW ){
      if( nCol==nAlloc ){
        nAlloc = nAlloc ? nAlloc*2 : 8;
        aCol = (Col*)realloc(aCol, nAlloc*sizeof(Col));
        if( aCol==0 ){ fprintf(stderr,"OOM\n"); return 1; }
      }
      aCol[nCol].iSet = sqlite3_column_int(pInfo, 0);
      aCol[nCol].iPos = sqlite3_column_int(pInfo, 1);
      aCol[nCol].zName = sqlite3_mprintf("%s", sqlite3_column_text(pInfo,2));
      aCol[nCol].zDecl = sqlite3_mprintf("%s", sqlite3_column_text(pInfo,3));
      nCol++;
    }
    sqlite3_finalize(pInfo);

    /* ---------------------------------------------------------------------
    ** ZEBRA EMITTER.  Typed nested structs over the @segments stream.
    **
    ** A nested table becomes List(ChildStruct), stitched client-side: the
    ** query runs once with the "@segments " prefix (the zebra-sprocket
    ** preamble seam), every row carries a leading _segment discriminator,
    ** and the KEY correlation -- which no generator can guess -- comes from
    ** PRAGMA proc_nested.  proc_nested's resultset_index is 1-based (set 0 is
    ** the parameters) while _segment is 0-based, so the emitted comparison is
    ** resultset_index-1.
    **
    ** Only shape 1 is emitted, as before.  A procedure with no nested tables
    ** emits a plain CALL with no marker, byte-identical in behavior to the
    ** previous generator.
    ** --------------------------------------------------------------------- */
    if( strcmp(zLang, "zebra")==0 ){
      ZNest aNest[32];
      int nNest = 0, iN;
      int nEmitted = 0, bDeep = 0, nCtr = 0;
      sqlite3_stmt *pNest = 0;
      if( sqlite3_prepare_v2(db,
            "SELECT resultset_index, \"column\", key_child, key_parent"
            "  FROM pragma_proc_nested(?1) ORDER BY resultset_index",
            -1, &pNest, 0)!=SQLITE_OK ){
        die("prepare proc_nested", db);
      }
      sqlite3_bind_text(pNest, 1, zProc, -1, SQLITE_TRANSIENT);
      while( sqlite3_step(pNest)==SQLITE_ROW && nNest<32 ){
        aNest[nNest].iSet     = sqlite3_column_int(pNest, 0);
        aNest[nNest].iParentSet = 0;
        aNest[nNest].zCol     = sqlite3_mprintf("%s", sqlite3_column_text(pNest,1));
        aNest[nNest].zKChild  = sqlite3_mprintf("%s", sqlite3_column_text(pNest,2));
        aNest[nNest].zKParent = sqlite3_mprintf("%s", sqlite3_column_text(pNest,3));
        nNest++;
      }
      sqlite3_finalize(pNest);
      /* Rebuild the containment tree from the shared pre-order (BUG-3):
      ** iParentSet answers "whose child am I", which proc_nested's rows do
      ** not carry directly. */
      znestAssign(aCol, nCol, aNest, nNest, 1, 0);
      for(iN=0; iN<nNest; iN++){
        if( aNest[iN].iParentSet>1 ) bDeep = 1;
      }

      fprintf(out, "# ---- procedure %s ----\n", zProc);

      /* Child structs, POST-ORDER: innermost first, so every List(Child)
      ** names a struct already on the page. */
      znestStructs(aCol, nCol, aNest, nNest, zProc, 1);

      /* Parent struct: scalars as themselves, nested as List(ChildStruct). */
      fputs("struct ", out); emitIdentCap(zProc); fputs("Row\n", out);
      for(i=0; i<nCol; i++){
        int bNested;
        if( aCol[i].iSet!=1 ) continue;
        bNested = (aCol[i].zDecl==0 || aCol[i].zDecl[0]==0);
        fputs("    var ", out); emitZField(aCol[i].zName);
        if( bNested ){
          fputs(": List(", out); emitIdentCap(zProc); emitIdentCap(aCol[i].zName);
          fputs(")\n", out);
        }else{
          fprintf(out, ": %s\n", zebraTypeName(typeOf(aCol[i].zDecl), 0));
        }
        nEmitted++;
      }
      if( nEmitted==0 ){
        fputs("    var _empty: int\n", out);   /* RETURNS NOTHING */
      }
      fputc('\n', out);

      fputs("def ", out); emitIdent(zProc); fputs("(d: SqliteDb", out);
      for(i=0; i<nCol; i++){
        if( aCol[i].iSet!=0 ) continue;
        fputs(", ", out); emitIdent(aCol[i].zName);
        fprintf(out, ": %s", zebraTypeName(typeOf(aCol[i].zDecl), 0));
      }
      fputs("): List(", out); emitIdentCap(zProc); fputs("Row)\n", out);
      fputs("    var out: List(", out); emitIdentCap(zProc);
      fputs("Row) = []\n", out);

      /* The query.  No-param nested procs use the real query_segments
      ** method (UNGIT scar #2's open half only affects param'd procs, which
      ** keep the marker until deferred-call bind lists are uniform).
      ** RETURNING projects the fold columns away -- but only at depth 1:
      ** past one level the engine refuses the composition, so deep clients
      ** carry the fold columns and simply never read them (the cost is
      ** noted here because it is paid here). */
      if( nNest && nParam==0 ){
        fprintf(out, "    var rows = d.query_segments(\"CALL %s(", zProc);
      }else if( nNest ){
        fprintf(out, "    var rows = d.query(\"@segments CALL %s(", zProc);
      }else{
        fprintf(out, "    var rows = d.query(\"CALL %s(", zProc);
      }
      for(i=0; i<nParam; i++) fprintf(out, "%s?", i?",":"");
      fputs(")", out);
      if( nNest && !bDeep ){
        int nSeen = 0;
        fputs(" RETURNING ", out);
        for(i=0; i<nCol; i++){
          if( aCol[i].iSet!=1 ) continue;
          if( aCol[i].zDecl==0 || aCol[i].zDecl[0]==0 ) continue;
          if( nSeen++ ) fputs(", ", out);
          emitIdent(aCol[i].zName);
        }
      }
      fputs("\"", out);
      if( nParam>0 ){
        int nSeen = 0;
        fputs(", [", out);
        for(i=0; i<nCol; i++){
          if( aCol[i].iSet!=0 ) continue;
          if( nSeen++ ) fputs(", ", out);
          emitIdent(aCol[i].zName);
        }
        fputc(']', out);
      }
      fputs(")\n", out);

      fputs("    for row in rows\n", out);
      if( nNest ){
        fputs("        if row.asInt(\"_segment\") == 0\n", out);
      }
      {
        int nInd = nNest ? 12 : 8;
        for(iN=0; iN<nNest; iN++){
          if( aNest[iN].iParentSet!=1 ) continue;
          znestCollect(aCol, nCol, aNest, nNest, zProc, &aNest[iN],
                       "row", nInd, &nCtr);
        }
        zind(nInd); fputs("out.add(", out); emitIdentCap(zProc);
        fputs("Row(", out);
        {
          int nSeen = 0;
          for(i=0; i<nCol; i++){
            int bNested;
            if( aCol[i].iSet!=1 ) continue;
            bNested = (aCol[i].zDecl==0 || aCol[i].zDecl[0]==0);
            if( nSeen++ ) fputs(", ", out);
            emitZField(aCol[i].zName); fputs(": ", out);
            if( bNested ){
              fputs("v_", out); emitIdent(aCol[i].zName);
            }else{
              fprintf(out, "row.%s(\"%s\")",
                      zebraAccessor(typeOf(aCol[i].zDecl), 0), aCol[i].zName);
            }
          }
        }
        fputs("))\n", out);
      }
      fputs("    return out\n\n", out);

      for(iN=0; iN<nNest; iN++){
        sqlite3_free(aNest[iN].zCol);
        sqlite3_free(aNest[iN].zKChild);
        sqlite3_free(aNest[iN].zKParent);
      }
      for(i=0; i<nCol; i++){ sqlite3_free(aCol[i].zName); sqlite3_free(aCol[i].zDecl); }
      free(aCol);
      continue;
    }

    fprintf(out, "/* ---- procedure %s: %d parameter%s, %d result set%s ---- */\n",
            zProc, nParam, nParam==1?"":"s", nSet, nSet==1?"":"s");

    /* opaque handle */
    fputs("typedef struct ", out); emitIdent(zProc);
    fputs("_stmt { sqlite3_stmt *pStmt; } ", out); emitIdent(zProc);
    fputs("_stmt;\n\n", out);

    /* prepare */
    fputs("static int ", out); emitIdent(zProc);
    fputs("_prepare(sqlite3 *db, ", out); emitIdent(zProc);
    fputs("_stmt **pp){\n", out);
    fputs("  ", out); emitIdent(zProc);
    fputs("_stmt *p = (", out); emitIdent(zProc);
    fprintf(out, "_stmt*)sqlite3_malloc(sizeof(*p));\n");
    fputs("  int rc;\n"
          "  if( p==0 ) return SQLITE_NOMEM;\n"
          "  p->pStmt = 0;\n", out);
    fprintf(out, "  rc = sqlite3_prepare_v2(db, \"CALL %s(", zProc);
    for(i=0; i<nParam; i++) fprintf(out, "%s?", i?",":"");
    fprintf(out, ");\", -1, &p->pStmt, 0);\n");
    fputs("  if( rc!=SQLITE_OK ){ sqlite3_free(p); return rc; }\n"
          "  *pp = p;\n  return SQLITE_OK;\n}\n\n", out);

    /* bind, typed */
    fputs("static int ", out); emitIdent(zProc); fputs("_bind(", out);
    emitIdent(zProc); fputs("_stmt *p", out);
    for(i=0; i<nCol; i++){
      if( aCol[i].iSet!=0 ) continue;
      fputs(", ", out);
      fputs(bindTypeName(typeOf(aCol[i].zDecl)), out);
      if( bindTypeName(typeOf(aCol[i].zDecl))[strlen(bindTypeName(typeOf(aCol[i].zDecl)))-1]!='*' ){
        fputc(' ', out);
      }
      emitIdent(aCol[i].zName);
      if( typeOf(aCol[i].zDecl)==T_TEXT || typeOf(aCol[i].zDecl)==T_BLOB ){
        fputs(", int n", out); emitIdent(aCol[i].zName);
      }
    }
    fputs("){\n  int rc = SQLITE_OK;\n", out);
    for(i=0; i<nCol; i++){
      CType t;
      if( aCol[i].iSet!=0 ) continue;
      t = typeOf(aCol[i].zDecl);
      fprintf(out, "  if( rc==SQLITE_OK ) rc = %s(p->pStmt, %d, ",
              bindFn(t), aCol[i].iPos + 1);
      emitIdent(aCol[i].zName);
      if( t==T_TEXT || t==T_BLOB ){
        fputs(", n", out); emitIdent(aCol[i].zName);
        fputs(", SQLITE_TRANSIENT", out);
      }
      fputs(");\n", out);
    }
    fputs("  return rc;\n}\n\n", out);

    /* step / advance / finalize / reset */
    fputs("static int ", out); emitIdent(zProc);
    fputs("_step(", out); emitIdent(zProc);
    fputs("_stmt *p){ return sqlite3_step(p->pStmt); }\n", out);

    if( nSet>1 ){
      fputs("static int ", out); emitIdent(zProc);
      fputs("_next_resultset(", out); emitIdent(zProc);
      fputs("_stmt *p){ return sqlite3_proc_next_resultset(p->pStmt); }\n", out);
    }
    fputs("static int ", out); emitIdent(zProc);
    fputs("_reset(", out); emitIdent(zProc);
    fputs("_stmt *p){ return sqlite3_reset(p->pStmt); }\n", out);
    fputs("static void ", out); emitIdent(zProc);
    fputs("_finalize(", out); emitIdent(zProc);
    fputs("_stmt *p){ if(p){ sqlite3_finalize(p->pStmt); sqlite3_free(p); } }\n\n", out);

    /* typed column accessors, per result set */
    for(iSet=1; iSet<=nSet; iSet++){
      nNestSeen = 0;
      for(i=0; i<nCol; i++){
        CType t;
        if( aCol[i].iSet!=iSet ) continue;
        t = typeOf(aCol[i].zDecl);
        fprintf(out, "static %s", cTypeName(t));
        if( cTypeName(t)[strlen(cTypeName(t))-1]!='*' ) fputc(' ', out);
        emitIdent(zProc); fprintf(out, "_rs%d_", iSet); emitIdent(aCol[i].zName);
        fputs("(", out); emitIdent(zProc);
        fprintf(out, "_stmt *p){ return %s(p->pStmt, %d); }\n",
                columnFn(t), aCol[i].iPos);
        if( t==T_BLOB ){
          fputs("static int ", out); emitIdent(zProc);
          fprintf(out, "_rs%d_", iSet); emitIdent(aCol[i].zName);
          fputs("_bytes(", out); emitIdent(zProc);
          fprintf(out, "_stmt *p){ return sqlite3_column_bytes(p->pStmt, %d); }\n",
                  aCol[i].iPos);
        }
        /* A NESTED TABLE.  Recognised by an EMPTY decltype, unambiguous
        ** because CREATE requires a type on every scalar column.  Its own
        ** columns are the next result set, in declaration order -- and that
        ** ordering is the only link between the two, so the generated header
        ** states it rather than leaving a reader to rediscover it. */
        if( aCol[i].zDecl==0 || aCol[i].zDecl[0]==0 ){
          fprintf(out,
            "/* %s: NESTED TABLE.  Its rows are result set %d -- reach them\n"
            "** with %s_next_resultset().  The accessor above yields the whole\n"
            "** table as JSON for a flat client; a typed client ignores it. */\n",
            aCol[i].zName, iSet + 1 + nNestSeen, zProc);
          fputs("static int ", out); emitIdent(zProc);
          fprintf(out, "_rs%d_", iSet); emitIdent(aCol[i].zName);
          fputs("_count(", out); emitIdent(zProc);
          fprintf(out,
            "_stmt *p){ return sqlite3_proc_child_count(p->pStmt, %d); }\n",
            nNestSeen);
          fputs("/* ^ children of the CURRENT parent row.  -1 unless the CALL\n"
                "** was prepared WITH COUNTS; 0 means genuinely childless. */\n",
                out);
          nNestSeen++;
        }
      }
    }
    fputs("\n", out);

    for(i=0; i<nCol; i++){ sqlite3_free(aCol[i].zName); sqlite3_free(aCol[i].zDecl); }
    free(aCol);
  }
  sqlite3_finalize(pList);
  sqlite3_close(db);

  if( strcmp(zLang, "zebra")!=0 ){
    fputs("#endif /* PROCGEN_CLIENT_H */\n", out);
  }
  return 0;
}
