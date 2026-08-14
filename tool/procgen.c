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

/* ---------------------------------------------------------------------
** Python emitter helpers.  The client is typed dataclasses over a ctypes
** binding of THE FORK'S OWN sqlite3.dll -- stock SQLite cannot execute
** CALL, and DB-API has no reach to the segment APIs, so a self-contained
** runtime is the only spelling that does not lie about its requirements.
** --------------------------------------------------------------------- */
static const char *pyTypeName(CType t){
  switch( t ){
    case T_INT:  return "int";
    case T_REAL: return "float";
    case T_TEXT: return "str";
    case T_BLOB: return "bytes";
    default:     return "object";
  }
}

/* The python keyword shield, same mechanism as the zebra one: a declared
** column named `class` or `import` must not emit an uncompilable field. */
static int pyKeyword(const char *z){
  static const char *azKw[] = {
    "False","None","True","and","as","assert","async","await","break",
    "class","continue","def","del","elif","else","except","finally","for",
    "from","global","if","import","in","is","lambda","nonlocal","not","or",
    "pass","raise","return","try","while","with","yield",
    /* not keywords, but names the generated scope already owns: */
    "db","field","dataclass","ctypes","list","dict","ProcError","Db"
  };
  int i;
  for(i=0; i<(int)(sizeof(azKw)/sizeof(azKw[0])); i++){
    if( strcmp(z, azKw[i])==0 ) return 1;
  }
  return 0;
}
static void emitPyIdentBuf(char *zBuf, int nBuf, const char *z){
  int i, j = 0;
  if( z==0 || z[0]==0 ){ sqlite3_snprintf(nBuf, zBuf, "_"); return; }
  if( isdigit((unsigned char)z[0]) && j<nBuf-1 ) zBuf[j++] = '_';
  for(i=0; z[i] && j<nBuf-2; i++){
    unsigned char c = (unsigned char)z[i];
    zBuf[j++] = (isalnum(c) || c=='_') ? (char)c : '_';
  }
  zBuf[j] = 0;
  if( pyKeyword(zBuf) && j<nBuf-2 ){ zBuf[j] = '_'; zBuf[j+1] = 0; }
}
static void emitPyIdent(const char *z){
  char zBuf[128];
  emitPyIdentBuf(zBuf, sizeof(zBuf), z);
  fputs(zBuf, out);
}
static void emitPyCap(const char *z){
  char zBuf[128];
  emitPyIdentBuf(zBuf, sizeof(zBuf), z);
  if( zBuf[0]>='a' && zBuf[0]<='z' ) zBuf[0] = (char)(zBuf[0]-'a'+'A');
  fputs(zBuf, out);
}
/* A python string literal of the RAW column name -- the runtime row dicts
** are keyed by what sqlite3_column_name() returns, not by the sanitized
** field spelling. */
static void emitPyStr(const char *z){
  int i;
  fputc('"', out);
  for(i=0; z && z[i]; i++){
    if( z[i]=='"' || z[i]=='\\' ) fputc('\\', out);
    fputc(z[i], out);
  }
  fputc('"', out);
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

/* ---------------------------------------------------------------------
** Python emitter bodies.  Same introspection as zebra, different stitch
** shape: gather every segment into row-dicts first, then build buckets
** deepest-first (pre-order guarantees a child's set index exceeds its
** parent's), then assemble parents by bucket lookup.  Every dataclass
** field carries a default because declaration order may interleave value
** and nested columns, and python refuses non-default fields after
** defaulted ones.
** --------------------------------------------------------------------- */
static void pynestStructs(Col *aCol, int nCol, ZNest *aN, int nN,
                          const char *zProc, int iSet){
  int k, i;
  for(k=0; k<nN; k++){
    if( aN[k].iParentSet!=iSet ) continue;
    pynestStructs(aCol, nCol, aN, nN, zProc, aN[k].iSet);
    fputs("@dataclass\nclass ", out);
    emitPyCap(zProc); emitPyCap(aN[k].zCol);
    fputs(":\n", out);
    for(i=0; i<nCol; i++){
      if( aCol[i].iSet!=aN[k].iSet ) continue;
      fputs("    ", out); emitPyIdent(aCol[i].zName);
      if( aCol[i].zDecl && aCol[i].zDecl[0] ){
        fprintf(out, ": %s | None = None\n", pyTypeName(typeOf(aCol[i].zDecl)));
      }else{
        fputs(": list[", out); emitPyCap(zProc); emitPyCap(aCol[i].zName);
        fputs("] = field(default_factory=list)\n", out);
      }
    }
    fputc('\n', out);
  }
}

/* Constructor arguments for one set's rows: value columns read from the
** row dict by their RAW names, nested members looked up in the child
** bucket keyed by this set's own parent-key column. */
static void pynestCtorArgs(Col *aCol, int nCol, ZNest *aN, int nN, int iSet){
  int i, nSeen = 0;
  for(i=0; i<nCol; i++){
    if( aCol[i].iSet!=iSet ) continue;
    if( nSeen++ ) fputs(", ", out);
    emitPyIdent(aCol[i].zName); fputc('=', out);
    if( aCol[i].zDecl && aCol[i].zDecl[0] ){
      fputs("_r[", out); emitPyStr(aCol[i].zName); fputs("]", out);
    }else{
      int j;
      for(j=0; j<nN; j++){
        if( aN[j].iParentSet==iSet
         && sqlite3_stricmp(aN[j].zCol, aCol[i].zName)==0 ) break;
      }
      if( j<nN ){
        fprintf(out, "_b%d.get(_r[", aN[j].iSet);
        emitPyStr(aN[j].zKParent);
        fputs("], [])", out);
      }else{
        fputs("[]", out);   /* proc_nested disagreed with proc_info; the
                            ** conformance walk makes this unreachable */
      }
    }
  }
}


/* ---------------------------------------------------------------------
** TypeScript emitter helpers (PLAN-TSGEN).  The client is typed
** interfaces over the N-API addon (tool/napi), whose segments() has the
** exact contract of the Python runtime's _segments -- the two clients
** differ in language, never in semantics.  INTEGER types as
** `number | bigint`: the addon returns BigInt past 2^53-1 because a
** double cannot hold 2^60, and the type must say so.
** --------------------------------------------------------------------- */
static const char *tsTypeName(CType t){
  switch( t ){
    case T_INT:  return "number | bigint";
    case T_REAL: return "number";
    case T_TEXT: return "string";
    case T_BLOB: return "Uint8Array";
    default:     return "unknown";
  }
}

static int tsKeyword(const char *z){
  static const char *azKw[] = {
    "break","case","catch","class","const","continue","debugger","default",
    "delete","do","else","enum","export","extends","false","finally","for",
    "function","if","import","in","instanceof","new","null","return","super",
    "switch","this","throw","true","try","typeof","var","void","while","with",
    "let","static","yield","await","implements","interface","package",
    "private","protected","public",
    /* names the generated scope already owns: */
    "connect","Db","ProcError","SqlValue","db","undefined"
  };
  int i;
  for(i=0; i<(int)(sizeof(azKw)/sizeof(azKw[0])); i++){
    if( strcmp(z, azKw[i])==0 ) return 1;
  }
  return 0;
}
static void emitTsIdentBuf(char *zBuf, int nBuf, const char *z){
  int i, j = 0;
  if( z==0 || z[0]==0 ){ sqlite3_snprintf(nBuf, zBuf, "_"); return; }
  if( isdigit((unsigned char)z[0]) && j<nBuf-1 ) zBuf[j++] = '_';
  for(i=0; z[i] && j<nBuf-2; i++){
    unsigned char c = (unsigned char)z[i];
    zBuf[j++] = (isalnum(c) || c=='_') ? (char)c : '_';
  }
  zBuf[j] = 0;
  if( tsKeyword(zBuf) && j<nBuf-2 ){ zBuf[j] = '_'; zBuf[j+1] = 0; }
}
static void emitTsIdent(const char *z){
  char zBuf[128];
  emitTsIdentBuf(zBuf, sizeof(zBuf), z);
  fputs(zBuf, out);
}
static void emitTsCap(const char *z){
  char zBuf[128];
  emitTsIdentBuf(zBuf, sizeof(zBuf), z);
  if( zBuf[0]>='a' && zBuf[0]<='z' ) zBuf[0] = (char)(zBuf[0]-'a'+'A');
  fputs(zBuf, out);
}

/* Interfaces, POST-ORDER so every Child[] names one declared above. */
static void tsnestStructs(Col *aCol, int nCol, ZNest *aN, int nN,
                          const char *zProc, int iSet){
  int k, i;
  for(k=0; k<nN; k++){
    if( aN[k].iParentSet!=iSet ) continue;
    tsnestStructs(aCol, nCol, aN, nN, zProc, aN[k].iSet);
    fputs("export interface ", out);
    emitTsCap(zProc); emitTsCap(aN[k].zCol);
    fputs(" {\n", out);
    for(i=0; i<nCol; i++){
      if( aCol[i].iSet!=aN[k].iSet ) continue;
      fputs("  ", out); emitTsIdent(aCol[i].zName);
      if( aCol[i].zDecl && aCol[i].zDecl[0] ){
        fprintf(out, ": %s | null;\n", tsTypeName(typeOf(aCol[i].zDecl)));
      }else{
        fputs(": ", out); emitTsCap(zProc); emitTsCap(aCol[i].zName);
        fputs("[];\n", out);
      }
    }
    fputs("}\n\n", out);
  }
}

/* Object-literal members for one set's rows: value columns read from
** the row record by RAW name with a narrowing assertion, nested members
** looked up in the child bucket keyed by this set's parent-key column. */
static void tsnestCtorArgs(Col *aCol, int nCol, ZNest *aN, int nN, int iSet){
  int i, nSeen = 0;
  for(i=0; i<nCol; i++){
    if( aCol[i].iSet!=iSet ) continue;
    if( nSeen++ ) fputs(", ", out);
    emitTsIdent(aCol[i].zName); fputs(": ", out);
    if( aCol[i].zDecl && aCol[i].zDecl[0] ){
      fputs("_r[", out); emitPyStr(aCol[i].zName);
      fprintf(out, "] as %s | null", tsTypeName(typeOf(aCol[i].zDecl)));
    }else{
      int j;
      for(j=0; j<nN; j++){
        if( aN[j].iParentSet==iSet
         && sqlite3_stricmp(aN[j].zCol, aCol[i].zName)==0 ) break;
      }
      if( j<nN ){
        fprintf(out, "_b%d.get(_r[", aN[j].iSet);
        emitPyStr(aN[j].zKParent);
        fputs("]) ?? []", out);
      }else{
        fputs("[]", out);   /* unreachable; see pynestCtorArgs */
      }
    }
  }
}

int main(int argc, char **argv){
  sqlite3 *db = 0;
  sqlite3_stmt *pList = 0;
  const char *zDb;
  const char *zLang = "c";

  /* The database is the first NON-OPTION argument, so options may come
  ** before or after it.  Before this, "procgen --lang zebra app.db" read
  ** "--lang" as the database and died with "unable to open database file"
  ** -- a refusal that named neither the reason nor the fix. */
  zDb = 0;
  out = stdout;
  {
    int a;
    for(a=1; a<argc; a++){
      if( strcmp(argv[a], "--lang")==0 ){
        if( a+1>=argc ){
          fprintf(stderr, "procgen: --lang needs a value (c, zebra, python, or ts)\n");
          return 1;
        }
        zLang = argv[++a];
      }else if( argv[a][0]=='-' ){
        fprintf(stderr, "procgen: unknown option %s\n"
                        "usage: procgen [--lang c|zebra|python|ts] DATABASE [> out.h]\n",
                        argv[a]);
        return 1;
      }else if( zDb==0 ){
        zDb = argv[a];
      }else{
        fprintf(stderr, "procgen: unexpected argument %s (database is %s)\n"
                        "usage: procgen [--lang c|zebra|python|ts] DATABASE [> out.h]\n",
                        argv[a], zDb);
        return 1;
      }
    }
  }
  if( zDb==0 ){
    fprintf(stderr, "usage: procgen [--lang c|zebra|python|ts] DATABASE [> out.h]\n");
    return 1;
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

  if( strcmp(zLang, "python")==0 ){
    fprintf(out,
      "# GENERATED by tool/procgen.c from \"%s\" -- do not edit.\n"
      "# Regenerating an unchanged schema produces byte-identical output, so a\n"
      "# diff here means the database's procedure contract actually moved.\n"
      "#\n", zDb);
    fputs(
      "# Runtime: ctypes over THE FORK'S OWN sqlite3.dll.  Stock SQLite cannot\n"
      "# execute CALL, and DB-API bindings cannot reach the segment APIs, so a\n"
      "# self-contained runtime is the only spelling that does not lie about\n"
      "# its requirements.  connect() takes the dll path EXPLICITLY -- loading\n"
      "# whatever sqlite3.dll happens to be on PATH would be exactly the\n"
      "# ambient-state failure this family of tools refuses.\n"
      "\n"
      "from __future__ import annotations\n"
      "import ctypes\n"
      "from dataclasses import dataclass, field\n"
      "\n"
      "_OK = 0\n"
      "_ROW = 100\n"
      "_DONE = 101\n"
      "_TRANSIENT = ctypes.c_void_p(-1)\n"
      "\n"
      "\n"
      "class ProcError(Exception):\n"
      "    pass\n"
      "\n"
      "\n"
      "class Db:\n"
      "    def __init__(self, db_path: str, dll_path: str):\n"
      "        lib = ctypes.CDLL(dll_path)\n"
      "        P = ctypes.POINTER\n"
      "        V = ctypes.c_void_p\n"
      "        lib.sqlite3_open_v2.argtypes = [ctypes.c_char_p, P(V), ctypes.c_int, ctypes.c_char_p]\n"
      "        lib.sqlite3_close.argtypes = [V]\n"
      "        lib.sqlite3_errmsg.argtypes = [V]\n"
      "        lib.sqlite3_errmsg.restype = ctypes.c_char_p\n"
      "        lib.sqlite3_prepare_v2.argtypes = [V, ctypes.c_char_p, ctypes.c_int, P(V), V]\n"
      "        lib.sqlite3_step.argtypes = [V]\n"
      "        lib.sqlite3_finalize.argtypes = [V]\n"
      "        lib.sqlite3_column_count.argtypes = [V]\n"
      "        lib.sqlite3_column_name.argtypes = [V, ctypes.c_int]\n"
      "        lib.sqlite3_column_name.restype = ctypes.c_char_p\n"
      "        lib.sqlite3_column_type.argtypes = [V, ctypes.c_int]\n"
      "        lib.sqlite3_column_int64.argtypes = [V, ctypes.c_int]\n"
      "        lib.sqlite3_column_int64.restype = ctypes.c_longlong\n"
      "        lib.sqlite3_column_double.argtypes = [V, ctypes.c_int]\n"
      "        lib.sqlite3_column_double.restype = ctypes.c_double\n"
      "        lib.sqlite3_column_text.argtypes = [V, ctypes.c_int]\n"
      "        lib.sqlite3_column_text.restype = ctypes.c_char_p\n"
      "        lib.sqlite3_column_blob.argtypes = [V, ctypes.c_int]\n"
      "        lib.sqlite3_column_blob.restype = V\n"
      "        lib.sqlite3_column_bytes.argtypes = [V, ctypes.c_int]\n"
      "        lib.sqlite3_bind_int64.argtypes = [V, ctypes.c_int, ctypes.c_longlong]\n"
      "        lib.sqlite3_bind_double.argtypes = [V, ctypes.c_int, ctypes.c_double]\n"
      "        lib.sqlite3_bind_text.argtypes = [V, ctypes.c_int, ctypes.c_char_p, ctypes.c_int, V]\n"
      "        lib.sqlite3_bind_blob.argtypes = [V, ctypes.c_int, ctypes.c_char_p, ctypes.c_int, V]\n"
      "        lib.sqlite3_bind_null.argtypes = [V, ctypes.c_int]\n"
      "        lib.sqlite3_proc_next_resultset.argtypes = [V]\n"
      "        self._lib = lib\n"
      "        h = V()\n"
      "        rc = lib.sqlite3_open_v2(db_path.encode(\"utf-8\"), ctypes.byref(h), 1, None)\n"
      "        if rc != _OK:\n"
      "            raise ProcError(f\"cannot open {db_path!r} ({rc})\")\n"
      "        self._h = h\n"
      "\n"
      "    def close(self) -> None:\n"
      "        if self._h:\n"
      "            self._lib.sqlite3_close(self._h)\n"
      "            self._h = None\n"
      "\n"
      "    def _errmsg(self) -> str:\n"
      "        m = self._lib.sqlite3_errmsg(self._h)\n"
      "        return m.decode(\"utf-8\", \"replace\") if m else \"?\"\n"
      "\n"
      "    def _segments(self, sql: str, params=()) -> list[list[dict]]:\n"
      "        lib = self._lib\n"
      "        stmt = ctypes.c_void_p()\n"
      "        rc = lib.sqlite3_prepare_v2(self._h, sql.encode(\"utf-8\"), -1,\n"
      "                                    ctypes.byref(stmt), None)\n"
      "        if rc != _OK:\n"
      "            raise ProcError(f\"prepare failed ({rc}): {self._errmsg()}: {sql}\")\n"
      "        try:\n"
      "            for i, v in enumerate(params, start=1):\n"
      "                if v is None:\n"
      "                    rc = lib.sqlite3_bind_null(stmt, i)\n"
      "                elif isinstance(v, bool):\n"
      "                    rc = lib.sqlite3_bind_int64(stmt, i, int(v))\n"
      "                elif isinstance(v, int):\n"
      "                    rc = lib.sqlite3_bind_int64(stmt, i, v)\n"
      "                elif isinstance(v, float):\n"
      "                    rc = lib.sqlite3_bind_double(stmt, i, v)\n"
      "                elif isinstance(v, str):\n"
      "                    b = v.encode(\"utf-8\")\n"
      "                    rc = lib.sqlite3_bind_text(stmt, i, b, len(b), _TRANSIENT)\n"
      "                elif isinstance(v, (bytes, bytearray)):\n"
      "                    b = bytes(v)\n"
      "                    rc = lib.sqlite3_bind_blob(stmt, i, b, len(b), _TRANSIENT)\n"
      "                else:\n"
      "                    raise ProcError(\n"
      "                        f\"cannot bind parameter {i}: {type(v).__name__}\")\n"
      "                if rc != _OK:\n"
      "                    raise ProcError(f\"bind {i} failed ({rc}): {self._errmsg()}\")\n"
      "            segs: list[list[dict]] = []\n"
      "            while True:\n"
      "                names: list[str] | None = None\n"
      "                rows: list[dict] = []\n"
      "                while True:\n"
      "                    rc = lib.sqlite3_step(stmt)\n"
      "                    if rc == _ROW:\n"
      "                        if names is None:\n"
      "                            n = lib.sqlite3_column_count(stmt)\n"
      "                            names = [lib.sqlite3_column_name(stmt, j)\n"
      "                                     .decode(\"utf-8\") for j in range(n)]\n"
      "                        row = {}\n"
      "                        for j, nm in enumerate(names):\n"
      "                            t = lib.sqlite3_column_type(stmt, j)\n"
      "                            if t == 1:\n"
      "                                row[nm] = lib.sqlite3_column_int64(stmt, j)\n"
      "                            elif t == 2:\n"
      "                                row[nm] = lib.sqlite3_column_double(stmt, j)\n"
      "                            elif t == 3:\n"
      "                                s = lib.sqlite3_column_text(stmt, j)\n"
      "                                row[nm] = s.decode(\"utf-8\") if s is not None else None\n"
      "                            elif t == 4:\n"
      "                                nb = lib.sqlite3_column_bytes(stmt, j)\n"
      "                                p = lib.sqlite3_column_blob(stmt, j)\n"
      "                                row[nm] = ctypes.string_at(p, nb) if p else b\"\"\n"
      "                            else:\n"
      "                                row[nm] = None\n"
      "                        rows.append(row)\n"
      "                    elif rc == _DONE:\n"
      "                        break\n"
      "                    else:\n"
      "                        raise ProcError(f\"step failed ({rc}): {self._errmsg()}\")\n"
      "                segs.append(rows)\n"
      "                rc = lib.sqlite3_proc_next_resultset(stmt)\n"
      "                if rc == _DONE:\n"
      "                    break\n"
      "                if rc != _OK:\n"
      "                    raise ProcError(\n"
      "                        f\"next_resultset failed ({rc}): {self._errmsg()}\")\n"
      "            return segs\n"
      "        finally:\n"
      "            lib.sqlite3_finalize(stmt)\n"
      "\n"
      "\n"
      "def connect(db_path: str, dll_path: str) -> Db:\n"
      "    return Db(db_path, dll_path)\n"
      "\n\n", out);
  }else if( strcmp(zLang, "ts")==0 ){
    fprintf(out,
      "// GENERATED by tool/procgen.c from \"%s\" -- do not edit.\n"
      "// Regenerating an unchanged schema produces byte-identical output, so a\n"
      "// diff here means the database's procedure contract actually moved.\n"
      "//\n"
      "// Runs on Node over the N-API addon (tool/napi), which statically\n"
      "// links the fork -- stock SQLite cannot execute CALL, so a\n"
      "// self-contained substrate is the only spelling that does not lie\n"
      "// about its requirements.  Dependencies are ARGUMENTS:\n"
      "// connect(dbPath, addonPath).  Nothing is discovered from ambient\n"
      "// state.\n"
      "\n"
      "import { createRequire } from \"node:module\";\n"
      "\n"
      "export type SqlValue = number | bigint | string | Uint8Array | null;\n"
      "type Row = Record<string, SqlValue>;\n"
      "interface Addon {\n"
      "  open(path: string): unknown;\n"
      "  close(h: unknown): void;\n"
      "  segments(h: unknown, sql: string, params?: unknown[]): Row[][];\n"
      "}\n"
      "\n"
      "export class ProcError extends Error {}\n"
      "\n"
      "export class Db {\n"
      "  private h: unknown;\n"
      "  private addon: Addon;\n"
      "  constructor(dbPath: string, addonPath: string) {\n"
      "    const req = createRequire(import.meta.url);\n"
      "    this.addon = req(addonPath) as Addon;\n"
      "    this.h = this.addon.open(dbPath);\n"
      "  }\n"
      "  _segments(sql: string, params: unknown[] = []): Row[][] {\n"
      "    return this.addon.segments(this.h, sql, params);\n"
      "  }\n"
      "  close(): void { this.addon.close(this.h); }\n"
      "}\n"
      "\n"
      "export function connect(dbPath: string, addonPath: string): Db {\n"
      "  return new Db(dbPath, addonPath);\n"
      "}\n"
      "\n", zDb);
  }else if( strcmp(zLang, "zebra")==0 ){
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

      /* The query.  Nested procs use the REAL query_segments method -- the
      ** no-param form for none, the _p form for params.  The "@segments "
      ** marker string is gone (UNGIT scar #2 closed 2026-08-08): Opus's fix
      ** to deferred-call bind-list lowering means a param'd deferred method
      ** builds its []_SqliteParam exactly like the known query() path, so the
      ** incantation is no longer needed.  RETURNING projects the fold columns
      ** away: an explicit value-column list at depth 1 (kept for regen
      ** stability of existing clients), and RETURNING * past one level --
      ** DOCKET 3i -- which declines the folds of EVERY segment, so deep
      ** clients no longer pay for columns they stitch from segments. */
      if( nNest && nParam==0 ){
        fprintf(out, "    var rows = d.query_segments(\"CALL %s(", zProc);
      }else if( nNest ){
        fprintf(out, "    var rows = d.query_segments_p(\"CALL %s(", zProc);
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
      }else if( nNest ){
        fputs(" RETURNING *", out);
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

    /* ---------------------------------------------------------------------
    ** PYTHON EMITTER.  Same introspection and the same RETURNING policy as
    ** the Zebra emitter (explicit value-column list at depth 1, RETURNING *
    ** past it, plain CALL for procedures that nest nothing); the stitch is
    ** bucket-based (see pynestStructs above).  Only shape 1 is emitted,
    ** as in the other emitters.
    ** --------------------------------------------------------------------- */
    if( strcmp(zLang, "ts")==0 ){
      ZNest aNest[32];
      int nNest = 0, iN;
      int nEmitted = 0, bDeep = 0;
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
      znestAssign(aCol, nCol, aNest, nNest, 1, 0);
      for(iN=0; iN<nNest; iN++){
        if( aNest[iN].iParentSet>1 ) bDeep = 1;
      }

      fprintf(out, "// ---- procedure %s ----\n", zProc);
      tsnestStructs(aCol, nCol, aNest, nNest, zProc, 1);

      fputs("export interface ", out); emitTsCap(zProc); fputs("Row {\n", out);
      for(i=0; i<nCol; i++){
        if( aCol[i].iSet!=1 ) continue;
        fputs("  ", out); emitTsIdent(aCol[i].zName);
        if( aCol[i].zDecl && aCol[i].zDecl[0] ){
          fprintf(out, ": %s | null;\n", tsTypeName(typeOf(aCol[i].zDecl)));
        }else{
          fputs(": ", out); emitTsCap(zProc); emitTsCap(aCol[i].zName);
          fputs("[];\n", out);
        }
        nEmitted++;
      }
      if( nEmitted==0 ){
        fputs("  _empty?: null;\n", out);       /* RETURNS NOTHING */
      }
      fputs("}\n\n", out);

      fputs("export function ", out); emitTsIdent(zProc); fputs("(db: Db", out);
      for(i=0; i<nCol; i++){
        if( aCol[i].iSet!=0 ) continue;
        fputs(", ", out); emitTsIdent(aCol[i].zName);
        fprintf(out, ": %s | null", tsTypeName(typeOf(aCol[i].zDecl)));
      }
      fputs("): ", out); emitTsCap(zProc); fputs("Row[] {\n", out);

      fprintf(out, "  const _segs = db._segments(\"CALL %s(", zProc);
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
      }else if( nNest ){
        fputs(" RETURNING *", out);
      }
      fputs("\"", out);
      if( nParam>0 ){
        int nSeen = 0;
        fputs(", [", out);
        for(i=0; i<nCol; i++){
          if( aCol[i].iSet!=0 ) continue;
          if( nSeen++ ) fputs(", ", out);
          emitTsIdent(aCol[i].zName);
        }
        fputc(']', out);
      }
      fputs(");\n", out);

      if( nSet>0 ){
        fputs("  if (_segs.length !== ", out);
        fprintf(out, "%d) {\n", nSet);
        fprintf(out,
          "    throw new ProcError(`%s: expected %d segments, "
          "got ${_segs.length}`);\n", zProc, nSet);
        fputs("  }\n", out);
      }else{
        fputs("  return [];\n}\n\n", out);      /* RETURNS NOTHING */
        goto ts_done;
      }

      /* Buckets, deepest first: pre-order set numbering guarantees every
      ** child bucket exists before the set that consumes it. */
      for(iN=nNest-1; iN>=0; iN--){
        fprintf(out, "  const _b%d = new Map<SqlValue, ", aNest[iN].iSet);
        emitTsCap(zProc); emitTsCap(aNest[iN].zCol);
        fputs("[]>();\n", out);
        fprintf(out, "  for (const _r of _segs[%d]) {\n", aNest[iN].iSet-1);
        fputs("    const _k = _r[", out); emitPyStr(aNest[iN].zKChild);
        fputs("];\n", out);
        fprintf(out, "    let _a = _b%d.get(_k);\n", aNest[iN].iSet);
        fprintf(out,
          "    if (_a === undefined) { _a = []; _b%d.set(_k, _a); }\n",
          aNest[iN].iSet);
        fputs("    _a.push({ ", out);
        tsnestCtorArgs(aCol, nCol, aNest, nNest, aNest[iN].iSet);
        fputs(" });\n  }\n", out);
      }
      fputs("  const _out: ", out); emitTsCap(zProc);
      fputs("Row[] = [];\n", out);
      fputs("  for (const _r of _segs[0]) {\n    _out.push({ ", out);
      tsnestCtorArgs(aCol, nCol, aNest, nNest, 1);
      fputs(" });\n  }\n  return _out;\n}\n\n", out);

ts_done:
      for(iN=0; iN<nNest; iN++){
        sqlite3_free(aNest[iN].zCol);
        sqlite3_free(aNest[iN].zKChild);
        sqlite3_free(aNest[iN].zKParent);
      }
      for(i=0; i<nCol; i++){ sqlite3_free(aCol[i].zName); sqlite3_free(aCol[i].zDecl); }
      free(aCol);
      continue;
    }

    if( strcmp(zLang, "python")==0 ){
      ZNest aNest[32];
      int nNest = 0, iN;
      int nEmitted = 0, bDeep = 0;
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
      znestAssign(aCol, nCol, aNest, nNest, 1, 0);
      for(iN=0; iN<nNest; iN++){
        if( aNest[iN].iParentSet>1 ) bDeep = 1;
      }

      fprintf(out, "# ---- procedure %s ----\n", zProc);
      pynestStructs(aCol, nCol, aNest, nNest, zProc, 1);

      fputs("@dataclass\nclass ", out); emitPyCap(zProc); fputs("Row:\n", out);
      for(i=0; i<nCol; i++){
        if( aCol[i].iSet!=1 ) continue;
        fputs("    ", out); emitPyIdent(aCol[i].zName);
        if( aCol[i].zDecl && aCol[i].zDecl[0] ){
          fprintf(out, ": %s | None = None\n",
                  pyTypeName(typeOf(aCol[i].zDecl)));
        }else{
          fputs(": list[", out); emitPyCap(zProc); emitPyCap(aCol[i].zName);
          fputs("] = field(default_factory=list)\n", out);
        }
        nEmitted++;
      }
      if( nEmitted==0 ){
        fputs("    _empty: int | None = None\n", out);   /* RETURNS NOTHING */
      }
      fputc('\n', out);

      fputs("def ", out); emitPyIdent(zProc); fputs("(db: Db", out);
      for(i=0; i<nCol; i++){
        if( aCol[i].iSet!=0 ) continue;
        fputs(", ", out); emitPyIdent(aCol[i].zName);
        fprintf(out, ": %s | None", pyTypeName(typeOf(aCol[i].zDecl)));
      }
      fputs(") -> list[", out); emitPyCap(zProc); fputs("Row]:\n", out);

      fprintf(out, "    _segs = db._segments(\"CALL %s(", zProc);
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
      }else if( nNest ){
        fputs(" RETURNING *", out);
      }
      fputs("\"", out);
      if( nParam>0 ){
        int nSeen = 0;
        fputs(", [", out);
        for(i=0; i<nCol; i++){
          if( aCol[i].iSet!=0 ) continue;
          if( nSeen++ ) fputs(", ", out);
          emitPyIdent(aCol[i].zName);
        }
        fputc(']', out);
      }
      fputs(")\n", out);

      if( nSet>0 ){
        fprintf(out, "    if len(_segs) != %d:\n", nSet);
        fputs("        raise ProcError(\n", out);
        fprintf(out,
          "            f\"%s: expected %d segments, got {len(_segs)}\")\n",
          zProc, nSet);
      }else{
        fputs("    return []\n\n", out);       /* RETURNS NOTHING */
        goto python_done;
      }

      /* Buckets, deepest first: pre-order set numbering guarantees every
      ** child bucket exists before the set that consumes it. */
      for(iN=nNest-1; iN>=0; iN--){
        fprintf(out, "    _b%d: dict = {}\n", aNest[iN].iSet);
        fprintf(out, "    for _r in _segs[%d]:\n", aNest[iN].iSet-1);
        fprintf(out, "        _b%d.setdefault(_r[", aNest[iN].iSet);
        emitPyStr(aNest[iN].zKChild);
        fputs("], []).append(", out);
        emitPyCap(zProc); emitPyCap(aNest[iN].zCol);
        fputc('(', out);
        pynestCtorArgs(aCol, nCol, aNest, nNest, aNest[iN].iSet);
        fputs("))\n", out);
      }
      fputs("    _out: list[", out); emitPyCap(zProc);
      fputs("Row] = []\n", out);
      fputs("    for _r in _segs[0]:\n        _out.append(", out);
      emitPyCap(zProc); fputs("Row(", out);
      pynestCtorArgs(aCol, nCol, aNest, nNest, 1);
      fputs("))\n    return _out\n\n", out);

python_done:
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

  if( strcmp(zLang, "c")==0 ){
    /* The C header guard.  Python tolerated it as a comment for a
    ** while; TypeScript rightly refused (TS18016). */
    fputs("#endif /* PROCGEN_CLIENT_H */\n", out);
  }
  return 0;
}
