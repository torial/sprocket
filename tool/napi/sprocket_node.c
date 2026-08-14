/*
** sprocket_node.c -- the N-API addon under the generated TypeScript
** client (PLAN-TSGEN).  Deliberately minimal: open, close, and ONE work
** function, segments(db, sql, params) -> [[{col: value}]] -- the exact
** contract of the Python runtime's _segments, so the two clients differ
** in language, never in semantics.
**
** The addon statically links the fork's amalgamation: the .node file is
** self-contained, there is no DLL to locate, and the "statically-linked
** embedder" doctrine extends to clients.  Stable ABI: plain C over
** NAPI_VERSION 8 -- one binary per platform serves every Node major
** that speaks napi 8 (Node 12.22+).
**
** Value mapping, chosen for honesty over convenience:
**   INTEGER -> number when |v| <= 2^53-1, BigInt beyond (a double
**              cannot hold 2^60; silently rounding it would be a
**              fabricated value in the UNGIT sense)
**   REAL    -> number      TEXT -> string      BLOB -> Uint8Array
**   NULL    -> null
** Binds accept number (integral binds as int64), BigInt (lossless
** int64 or refused), string, boolean, null/undefined, Uint8Array.
**
** Build: node-gyp via tool/napi/binding.gyp (see package.json).
*/
#define NAPI_VERSION 8
#include <node_api.h>
#include <string.h>
#include "sqlite3.h"

#define JS_SAFE_MAX  9007199254740991LL   /* 2^53-1 */
#define JS_SAFE_MIN (-9007199254740991LL)

static napi_value throwSqlite(napi_env env, sqlite3 *db, const char *zWhat){
  char zMsg[512];
  sqlite3_snprintf(sizeof(zMsg), zMsg, "%s: %s", zWhat,
                   db ? sqlite3_errmsg(db) : "out of memory");
  napi_throw_error(env, 0, zMsg);
  return 0;
}

/* ------------------------------------------------------------------ open --
** The external wraps a HOLDER, not the sqlite3* itself: close() closes
** the database and NULLs the holder, the GC finalizer closes only what
** close() has not, and any use after close() throws by name.  A
** deterministic close matters here -- connection lifetime is what
** releases the fork's queued-write declaration and every wal lock. */
typedef struct DbHolder { sqlite3 *db; } DbHolder;

static void dbFinalize(napi_env env, void *pData, void *pHint){
  DbHolder *h = (DbHolder*)pData;
  (void)env; (void)pHint;
  if( h->db ) sqlite3_close(h->db);
  sqlite3_free(h);
}

static napi_value snOpen(napi_env env, napi_callback_info info){
  size_t argc = 1;
  napi_value argv[1], out;
  char zPath[1024];
  size_t n = 0;
  DbHolder *h;

  napi_get_cb_info(env, info, &argc, argv, 0, 0);
  if( argc<1
   || napi_get_value_string_utf8(env, argv[0], zPath, sizeof(zPath), &n)
        !=napi_ok ){
    napi_throw_type_error(env, 0, "open(dbPath): dbPath must be a string");
    return 0;
  }
  h = (DbHolder*)sqlite3_malloc(sizeof(*h));
  if( h==0 ){ napi_throw_error(env, 0, "out of memory"); return 0; }
  h->db = 0;
  if( sqlite3_open_v2(zPath, &h->db, SQLITE_OPEN_READWRITE, 0)!=SQLITE_OK ){
    napi_value r = throwSqlite(env, h->db, "open");
    sqlite3_close(h->db);
    sqlite3_free(h);
    return r;
  }
  if( napi_create_external(env, h, dbFinalize, 0, &out)!=napi_ok ){
    sqlite3_close(h->db);
    sqlite3_free(h);
    napi_throw_error(env, 0, "open: cannot wrap handle");
    return 0;
  }
  return out;
}

static sqlite3 *getDb(napi_env env, napi_value v){
  void *p = 0;
  DbHolder *h;
  if( napi_get_value_external(env, v, &p)!=napi_ok || p==0 ){
    napi_throw_type_error(env, 0, "expected a database handle from open()");
    return 0;
  }
  h = (DbHolder*)p;
  if( h->db==0 ){
    napi_throw_error(env, 0, "database is closed");
    return 0;
  }
  return h->db;
}

/* ----------------------------------------------------------------- close -- */
static napi_value snClose(napi_env env, napi_callback_info info){
  size_t argc = 1;
  napi_value argv[1], und;
  void *p = 0;
  napi_get_cb_info(env, info, &argc, argv, 0, 0);
  napi_get_undefined(env, &und);
  if( argc>=1 && napi_get_value_external(env, argv[0], &p)==napi_ok && p ){
    DbHolder *h = (DbHolder*)p;
    if( h->db ){
      sqlite3_close(h->db);
      h->db = 0;
    }
  }
  return und;
}

/* ------------------------------------------------------------- bind one -- */
static int bindOne(napi_env env, sqlite3_stmt *pStmt, int i, napi_value v){
  napi_valuetype t;
  napi_typeof(env, v, &t);
  switch( t ){
    case napi_null:
    case napi_undefined:
      return sqlite3_bind_null(pStmt, i);
    case napi_boolean: {
      bool b = 0;
      napi_get_value_bool(env, v, &b);
      return sqlite3_bind_int64(pStmt, i, b ? 1 : 0);
    }
    case napi_number: {
      double d = 0;
      napi_get_value_double(env, v, &d);
      if( d==(double)(sqlite3_int64)d ){
        return sqlite3_bind_int64(pStmt, i, (sqlite3_int64)d);
      }
      return sqlite3_bind_double(pStmt, i, d);
    }
    case napi_bigint: {
      sqlite3_int64 iv = 0;
      bool lossless = 0;
      napi_get_value_bigint_int64(env, v, (int64_t*)&iv, &lossless);
      if( !lossless ){
        napi_throw_range_error(env, 0, "BigInt parameter exceeds 64 bits");
        return SQLITE_MISUSE;
      }
      return sqlite3_bind_int64(pStmt, i, iv);
    }
    case napi_string: {
      size_t n = 0;
      char *z;
      int rc;
      napi_get_value_string_utf8(env, v, 0, 0, &n);
      z = (char*)sqlite3_malloc64(n+1);
      if( z==0 ) return SQLITE_NOMEM;
      napi_get_value_string_utf8(env, v, z, n+1, &n);
      rc = sqlite3_bind_text64(pStmt, i, z, n, sqlite3_free, SQLITE_UTF8);
      return rc;
    }
    default: {
      bool isTa = 0;
      napi_is_typedarray(env, v, &isTa);
      if( isTa ){
        napi_typedarray_type tt;
        size_t nEl = 0, off = 0;
        void *pData = 0;
        napi_value ab;
        napi_get_typedarray_info(env, v, &tt, &nEl, &pData, &ab, &off);
        if( tt==napi_uint8_array ){
          return sqlite3_bind_blob64(pStmt, i, pData, nEl, SQLITE_TRANSIENT);
        }
      }
      napi_throw_type_error(env, 0,
          "parameters must be number, BigInt, string, boolean, "
          "Uint8Array, or null");
      return SQLITE_MISUSE;
    }
  }
}

/* -------------------------------------------------------------- segments -- */
static napi_value snSegments(napi_env env, napi_callback_info info){
  size_t argc = 3;
  napi_value argv[3], aSegs, und;
  sqlite3 *db;
  sqlite3_stmt *pStmt = 0;
  char *zSql = 0;
  size_t nSql = 0;
  unsigned iSeg = 0;
  int rc;

  napi_get_undefined(env, &und);
  napi_get_cb_info(env, info, &argc, argv, 0, 0);
  if( argc<2 ){
    napi_throw_type_error(env, 0, "segments(db, sql, params?)");
    return 0;
  }
  db = getDb(env, argv[0]);
  if( db==0 ) return 0;
  napi_get_value_string_utf8(env, argv[1], 0, 0, &nSql);
  zSql = (char*)sqlite3_malloc64(nSql+1);
  if( zSql==0 ){ napi_throw_error(env, 0, "out of memory"); return 0; }
  napi_get_value_string_utf8(env, argv[1], zSql, nSql+1, &nSql);

  rc = sqlite3_prepare_v2(db, zSql, -1, &pStmt, 0);
  sqlite3_free(zSql);
  if( rc!=SQLITE_OK ) return throwSqlite(env, db, "prepare");
  if( pStmt==0 ){
    napi_throw_error(env, 0, "prepare: empty SQL produced no statement");
    return 0;
  }

  if( argc>=3 ){
    bool isArr = 0;
    napi_is_array(env, argv[2], &isArr);
    if( isArr ){
      unsigned nP = 0, i;
      napi_get_array_length(env, argv[2], &nP);
      for(i=0; i<nP; i++){
        napi_value pv;
        napi_get_element(env, argv[2], i, &pv);
        rc = bindOne(env, pStmt, (int)i+1, pv);
        if( rc!=SQLITE_OK ){
          bool pending = 0;
          napi_is_exception_pending(env, &pending);
          sqlite3_finalize(pStmt);
          if( !pending ) throwSqlite(env, db, "bind");
          return 0;
        }
      }
    }
  }

  napi_create_array(env, &aSegs);
  for(;;){
    napi_value aRows;
    unsigned iRow = 0;
    int nCol = -1;
    napi_create_array(env, &aRows);
    for(;;){
      rc = sqlite3_step(pStmt);
      if( rc==SQLITE_ROW ){
        napi_value row;
        int j;
        if( nCol<0 ) nCol = sqlite3_column_count(pStmt);
        napi_create_object(env, &row);
        for(j=0; j<nCol; j++){
          const char *zName = sqlite3_column_name(pStmt, j);
          napi_value val;
          switch( sqlite3_column_type(pStmt, j) ){
            case SQLITE_INTEGER: {
              sqlite3_int64 v = sqlite3_column_int64(pStmt, j);
              if( v>=JS_SAFE_MIN && v<=JS_SAFE_MAX ){
                napi_create_double(env, (double)v, &val);
              }else{
                napi_create_bigint_int64(env, v, &val);
              }
              break;
            }
            case SQLITE_FLOAT:
              napi_create_double(env, sqlite3_column_double(pStmt, j), &val);
              break;
            case SQLITE_TEXT:
              napi_create_string_utf8(env,
                  (const char*)sqlite3_column_text(pStmt, j),
                  (size_t)sqlite3_column_bytes(pStmt, j), &val);
              break;
            case SQLITE_BLOB: {
              int nB = sqlite3_column_bytes(pStmt, j);
              const void *pB = sqlite3_column_blob(pStmt, j);
              void *pOut = 0;
              napi_value ab;
              napi_create_arraybuffer(env, (size_t)nB, &pOut, &ab);
              if( nB>0 ) memcpy(pOut, pB, (size_t)nB);
              napi_create_typedarray(env, napi_uint8_array, (size_t)nB,
                                     ab, 0, &val);
              break;
            }
            default:
              napi_get_null(env, &val);
              break;
          }
          napi_set_named_property(env, row, zName, val);
        }
        napi_set_element(env, aRows, iRow++, row);
      }else if( rc==SQLITE_DONE ){
        break;
      }else{
        sqlite3_finalize(pStmt);
        return throwSqlite(env, db, "step");
      }
    }
    napi_set_element(env, aSegs, iSeg++, aRows);
    rc = sqlite3_proc_next_resultset(pStmt);
    if( rc==SQLITE_DONE ) break;
    if( rc!=SQLITE_OK ){
      sqlite3_finalize(pStmt);
      return throwSqlite(env, db, "next_resultset");
    }
  }
  sqlite3_finalize(pStmt);
  return aSegs;
}

/* ------------------------------------------------------------------ init -- */
static napi_value Init(napi_env env, napi_value exports){
  napi_value f;
  napi_create_function(env, "open", NAPI_AUTO_LENGTH, snOpen, 0, &f);
  napi_set_named_property(env, exports, "open", f);
  napi_create_function(env, "close", NAPI_AUTO_LENGTH, snClose, 0, &f);
  napi_set_named_property(env, exports, "close", f);
  napi_create_function(env, "segments", NAPI_AUTO_LENGTH, snSegments, 0, &f);
  napi_set_named_property(env, exports, "segments", f);
  return exports;
}
NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)
