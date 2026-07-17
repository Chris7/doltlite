#include "doltlite.h"
#include <stdio.h>
#include <stdlib.h>

static void sql(sqlite3 *db, const char *z){
  char *zErr = 0;
  if( sqlite3_exec(db, z, 0, 0, &zErr)!=SQLITE_OK ){
    fprintf(stderr, "%s: %s\n", z, zErr ? zErr : sqlite3_errmsg(db));
    sqlite3_free(zErr);
    exit(1);
  }
}

static void expectCount(sqlite3 *db, int expected){
  sqlite3_stmt *pStmt = 0;
  if( sqlite3_prepare_v2(db, "select count(*) from t", -1, &pStmt, 0)!=SQLITE_OK
   || sqlite3_step(pStmt)!=SQLITE_ROW
   || sqlite3_column_int(pStmt, 0)!=expected ){
    fprintf(stderr, "expected %d merged rows\n", expected);
    sqlite3_finalize(pStmt);
    exit(1);
  }
  sqlite3_finalize(pStmt);
}

int main(int argc, char **argv){
  sqlite3 *db = 0;
  sqlite3 *loader = 0;
  const char *zDb = argc>1 ? argv[1] : "/tmp/objectstorevfs-smoke.db";
  const char *zExt = argc>2 ? argv[2] : "/tmp/objectstorevfs.so";
  if( sqlite3_open(":memory:", &loader)!=SQLITE_OK ) return 1;
  sqlite3_enable_load_extension(loader, 1);
  if( sqlite3_load_extension(loader, zExt, "sqlite3_objectstorevfs_init", 0)!=SQLITE_OK ){
    fprintf(stderr, "cannot load %s: %s\n", zExt, sqlite3_errmsg(loader));
    return 1;
  }
  if( sqlite3_open_v2(zDb, &db, SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE,
                      "objectstorevfs")!=SQLITE_OK ){
    fprintf(stderr, "cannot open %s: %s\n", zDb, sqlite3_errmsg(db));
    return 1;
  }
  sql(db, "create table t(id integer primary key, v text)");
  sql(db, "insert into t values(1,'main')");
  sql(db, "select dolt_commit('-A','-m','initial')");
  sql(db, "select dolt_branch('feature')");
  sql(db, "select dolt_checkout('feature')");
  sql(db, "insert into t values(2,'feature')");
  sql(db, "select dolt_commit('-A','-m','feature')");
  sql(db, "select dolt_checkout('main')");
  sql(db, "insert into t values(3,'main')");
  sql(db, "select dolt_commit('-A','-m','main')");
  sql(db, "select dolt_merge('feature')");
  sqlite3_close(db);
  if( sqlite3_open_v2(zDb, &db, SQLITE_OPEN_READONLY, "objectstorevfs")!=SQLITE_OK ){
    fprintf(stderr, "cannot reopen %s: %s\n", zDb, sqlite3_errmsg(db));
    return 1;
  }
  expectCount(db, 3);
  sqlite3_close(db);
  sqlite3_close(loader);
  return 0;
}
