/*
** 2026-07-17
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
** Exercise objectstorevfs with independent writer and reader processes.
*/
#include "doltlite.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef SQLITE_CORE
extern int sqlite3_objectstorevfs_register(sqlite3_vfs*);
#endif

#define N_WRITER 4
#define N_READER 4
#define N_WRITE 250
#define N_READ 600

static int retryable(int rc){
  return rc==SQLITE_BUSY || rc==SQLITE_LOCKED || rc==SQLITE_SCHEMA;
}

static void removeTree(const char *zPath){
  DIR *pDir = opendir(zPath);
  struct dirent *pEntry;
  if( !pDir ){
    unlink(zPath);
    return;
  }
  while( (pEntry=readdir(pDir))!=0 ){
    char *zChild;
    if( strcmp(pEntry->d_name,".")==0 || strcmp(pEntry->d_name,"..")==0 ) continue;
    zChild=sqlite3_mprintf("%s/%s",zPath,pEntry->d_name);
    if( zChild ){
      removeTree(zChild);
      sqlite3_free(zChild);
    }
  }
  closedir(pDir);
  rmdir(zPath);
}

static void cleanupDb(const char *zDb){
  char *z;
  unlink(zDb);
  z=sqlite3_mprintf("%s.d",zDb);
  if( z ){
    removeTree(z);
    sqlite3_free(z);
  }
  z=sqlite3_mprintf("%s.osvfs-lock",zDb);
  if( z ){
    unlink(z);
    sqlite3_free(z);
  }
}

static int execRetry(sqlite3 *db, const char *zSql){
  int i;
  for(i=0; i<1000; i++){
    char *zErr = 0;
    int rc = sqlite3_exec(db, zSql, 0, 0, &zErr);
    sqlite3_free(zErr);
    if( rc==SQLITE_OK ) return rc;
    if( !retryable(rc) ) return rc;
    sqlite3_sleep(2);
  }
  return SQLITE_BUSY;
}

static int openDb(const char *zDb, sqlite3 **ppDb){
  int rc = sqlite3_open_v2(zDb, ppDb, SQLITE_OPEN_READWRITE,
                           "objectstorevfs");
  if( rc==SQLITE_OK ) sqlite3_busy_timeout(*ppDb, 5000);
  return rc;
}

static int writer(const char *zDb, int iWriter){
  sqlite3 *db = 0;
  int i;
  int rc = openDb(zDb, &db);
  if( rc!=SQLITE_OK ){
    fprintf(stderr, "writer %d open: %s\n", iWriter, db ? sqlite3_errmsg(db) : "failed");
    return 1;
  }
  for(i=0; i<N_WRITE; i++){
    char zSql[300];
    int id = iWriter*100000 + i;
    sqlite3_snprintf(sizeof(zSql), zSql,
      "BEGIN IMMEDIATE;"
      "INSERT INTO kv(id,writer,seq,payload) VALUES(%d,%d,%d,zeroblob(900));"
      "UPDATE kv SET payload=zeroblob(1700) WHERE id=%d;"
      "COMMIT;", id, iWriter, i, id);
    rc = execRetry(db, zSql);
    if( rc!=SQLITE_OK ){
      fprintf(stderr, "writer %d operation %d: %s\n",
              iWriter, i, sqlite3_errmsg(db));
      sqlite3_close(db);
      return 1;
    }
  }
  sqlite3_close(db);
  return 0;
}

static int reader(const char *zDb, int iReader){
  sqlite3 *db = 0;
  int i;
  int rc = openDb(zDb, &db);
  if( rc!=SQLITE_OK ){
    fprintf(stderr, "reader %d open: %s\n", iReader, db ? sqlite3_errmsg(db) : "failed");
    return 1;
  }
  for(i=0; i<N_READ; i++){
    sqlite3_stmt *pStmt = 0;
    sqlite3_int64 nRow;
    rc = sqlite3_prepare_v2(db,
      "SELECT count(*), coalesce(sum(length(payload)),0) FROM kv", -1, &pStmt, 0);
    if( rc==SQLITE_OK ) rc = sqlite3_step(pStmt);
    if( rc==SQLITE_ROW ){
      nRow = sqlite3_column_int64(pStmt, 0);
      if( nRow<0 || nRow>(sqlite3_int64)N_WRITER*N_WRITE ) rc = SQLITE_CORRUPT;
      else rc = SQLITE_OK;
    }
    sqlite3_finalize(pStmt);
    if( rc!=SQLITE_OK && retryable(rc) ){
      sqlite3_sleep(2);
      i--;
      continue;
    }
    if( rc!=SQLITE_OK ){
      fprintf(stderr, "reader %d iteration %d: %s\n",
              iReader, i, sqlite3_errmsg(db));
      sqlite3_close(db);
      return 1;
    }
    usleep(1000);
  }
  sqlite3_close(db);
  return 0;
}

static int integrityCheck(sqlite3 *db){
  sqlite3_stmt *pStmt = 0;
  int rc = sqlite3_prepare_v2(db, "PRAGMA integrity_check", -1, &pStmt, 0);
  if( rc==SQLITE_OK ) rc = sqlite3_step(pStmt);
  if( rc==SQLITE_ROW && sqlite3_stricmp((const char*)sqlite3_column_text(pStmt,0),"ok")==0 ){
    rc = SQLITE_OK;
  }else if( rc==SQLITE_ROW ){
    fprintf(stderr, "integrity_check: %s\n", sqlite3_column_text(pStmt,0));
    rc = SQLITE_CORRUPT;
  }
  sqlite3_finalize(pStmt);
  return rc;
}

int main(int argc, char **argv){
  const char *zDb = argc>1 ? argv[1] : "/tmp/objectstorevfs-concurrent.db";
#ifndef SQLITE_CORE
  const char *zExt = argc>2 ? argv[2] : "/tmp/objectstorevfs.so";
  sqlite3 *loader = 0;
#endif
  sqlite3 *db = 0;
  pid_t aPid[N_WRITER+N_READER];
  int i;
  int rc;
  int status;

#ifdef SQLITE_CORE
  rc = sqlite3_objectstorevfs_register(0);
  if( rc!=SQLITE_OK ){
    fprintf(stderr, "cannot register objectstorevfs: %d\n", rc);
    return 1;
  }
#else
  rc = sqlite3_open(":memory:", &loader);
  if( rc!=SQLITE_OK ) return 1;
  sqlite3_enable_load_extension(loader, 1);
  rc = sqlite3_load_extension(loader, zExt, "sqlite3_objectstorevfs_init", 0);
  if( rc!=SQLITE_OK ){
    fprintf(stderr, "cannot load %s: %s\n", zExt, sqlite3_errmsg(loader));
    return 1;
  }
#endif
  cleanupDb(zDb);
  rc = sqlite3_open_v2(zDb, &db, SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE,
                       "objectstorevfs");
  if( rc==SQLITE_OK ) rc = execRetry(db,
    "CREATE TABLE kv("
    "id INTEGER PRIMARY KEY, writer INTEGER NOT NULL, seq INTEGER NOT NULL,"
    "payload BLOB NOT NULL);"
    "SELECT dolt_commit('-A','-m','concurrency setup')");
  if( rc!=SQLITE_OK ){
    fprintf(stderr, "setup: %s\n", db ? sqlite3_errmsg(db) : "open failed");
    sqlite3_close(db);
#ifndef SQLITE_CORE
    sqlite3_close(loader);
#endif
    return 1;
  }
  sqlite3_close(db);

  for(i=0; i<N_WRITER+N_READER; i++){
    aPid[i] = fork();
    if( aPid[i]==0 ) _exit(i<N_WRITER ? writer(zDb,i) : reader(zDb,i-N_WRITER));
    if( aPid[i]<0 ) return 1;
  }
  for(i=0; i<N_WRITER+N_READER; i++){
    if( waitpid(aPid[i], &status, 0)<0 || !WIFEXITED(status) || WEXITSTATUS(status)!=0 ){
      if( WIFSIGNALED(status) ) fprintf(stderr, "child %d terminated by signal %d\n", i, WTERMSIG(status));
      else fprintf(stderr, "child %d failed\n", i);
#ifndef SQLITE_CORE
      sqlite3_close(loader);
#endif
      return 1;
    }
  }

  rc = openDb(zDb, &db);
  if( rc==SQLITE_OK ) rc = execRetry(db, "SELECT dolt_commit('-A','-m','concurrency complete')");
  if( rc==SQLITE_OK ) rc = integrityCheck(db);
  if( rc==SQLITE_OK ){
    sqlite3_stmt *pStmt = 0;
    rc = sqlite3_prepare_v2(db, "SELECT count(*) FROM kv", -1, &pStmt, 0);
    if( rc==SQLITE_OK && sqlite3_step(pStmt)==SQLITE_ROW
     && sqlite3_column_int(pStmt,0)==N_WRITER*N_WRITE ) rc = SQLITE_OK;
    else{
      if( pStmt ) fprintf(stderr, "final row count: %d\n", sqlite3_column_int(pStmt,0));
      rc = SQLITE_CORRUPT;
    }
    sqlite3_finalize(pStmt);
  }
  if( rc!=SQLITE_OK ) fprintf(stderr, "final verification: %s\n", sqlite3_errmsg(db));
  sqlite3_close(db);
#ifndef SQLITE_CORE
  sqlite3_close(loader);
#endif
  if( rc==SQLITE_OK ) printf("objectstorevfs concurrent stress: %d writes, %d reader queries\n",
                             N_WRITER*N_WRITE, N_READER*N_READ);
  return rc==SQLITE_OK ? 0 : 1;
}
