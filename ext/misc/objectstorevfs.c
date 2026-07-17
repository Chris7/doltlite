/*
** 2026-07-16
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
******************************************************************************
**
** An opt-in VFS shim for filesystems backed by an object store. The logical
** database is split into fixed-size page blocks in <database>.d/chunks. A
** write rewrites only the touched block, rather than the whole database.
*/
#include "sqlite3ext.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
SQLITE_EXTENSION_INIT1

typedef struct ObjectStoreVfs ObjectStoreVfs;
typedef struct ObjectStoreFile ObjectStoreFile;

struct ObjectStoreVfs {
  sqlite3_vfs base;
  sqlite3_vfs *pParent;
};

struct ObjectStoreFile {
  sqlite3_file base;
  sqlite3_file *pReal;
  char *zRoot;
  sqlite3_int64 nByte;
  sqlite3_int64 iGeneration;
  sqlite3_int64 *aBlock;
  int nBlock;
  int nAlloc;
  int isBlockStore;
  int isDirty;
};

#define OSVFS_REAL(p) (((ObjectStoreFile*)(p))->pReal)
#define OSVFS_METHOD(p) (OSVFS_REAL(p)->pMethods)

#define OSVFS_BLOCK_SIZE 4096

static int osvfsIsMainDbName(const char *zName, int flags){
  int n;
  if( !zName || (flags & SQLITE_OPEN_MAIN_DB)==0 ) return 0;
  n = (int)strlen(zName);
  return n<5 || strcmp(zName+n-5, "-lock")!=0;
}

static int osvfsMkdir(const char *z){
  char *zCopy = sqlite3_mprintf("%s", z);
  char *p;
  if( !zCopy ) return SQLITE_NOMEM;
  for(p=zCopy+1; *p; p++){
    if( *p=='/' ){
      *p = 0;
      if( mkdir(zCopy,0777) && errno!=EEXIST ){ sqlite3_free(zCopy); return SQLITE_CANTOPEN; }
      *p = '/';
    }
  }
  if( mkdir(zCopy,0777) && errno!=EEXIST ){ sqlite3_free(zCopy); return SQLITE_CANTOPEN; }
  sqlite3_free(zCopy);
  return SQLITE_OK;
}

static char *osvfsPath(ObjectStoreFile *p, const char *zKind, sqlite3_int64 i){
  return i>=0 ? sqlite3_mprintf("%s/%s/%016llx",p->zRoot,zKind,(long long)i)
              : sqlite3_mprintf("%s/%s",p->zRoot,zKind);
}

static int osvfsGrow(ObjectStoreFile *p, int n){
  sqlite3_int64 *a;
  int nNew = p->nAlloc ? p->nAlloc*2 : 16;
  while( nNew<n ) nNew *= 2;
  a = sqlite3_realloc64(p->aBlock, (sqlite3_uint64)nNew*sizeof(*a));
  if( !a ) return SQLITE_NOMEM;
  memset(a+p->nAlloc, 0, (size_t)(nNew-p->nAlloc)*sizeof(*a));
  p->aBlock = a; p->nAlloc = nNew;
  return SQLITE_OK;
}

static int osvfsLoad(ObjectStoreFile *p){
  char *z = osvfsPath(p,"HEAD",-1);
  FILE *f;
  int n;
  if( !z ) return SQLITE_NOMEM;
  f = fopen(z,"rb"); sqlite3_free(z);
  if( !f ) return SQLITE_OK;
  if( fread(&p->nByte,sizeof(p->nByte),1,f)!=1
   || fread(&p->iGeneration,sizeof(p->iGeneration),1,f)!=1
   || fread(&n,sizeof(n),1,f)!=1 || n<0 ){ fclose(f); return SQLITE_CORRUPT; }
  if( osvfsGrow(p,n)!=SQLITE_OK ){ fclose(f); return SQLITE_NOMEM; }
  if( n && fread(p->aBlock,sizeof(*p->aBlock),(size_t)n,f)!=(size_t)n ){
    fclose(f); return SQLITE_CORRUPT;
  }
  p->nBlock = n; fclose(f); return SQLITE_OK;
}

static int osvfsSave(ObjectStoreFile *p){
  char *z = osvfsPath(p,"HEAD",-1);
  char *zTmp = sqlite3_mprintf("%s.tmp",z);
  FILE *f;
  int rc = SQLITE_OK;
  if( !z || !zTmp ){ sqlite3_free(z); sqlite3_free(zTmp); return SQLITE_NOMEM; }
  f = fopen(zTmp,"wb");
  p->iGeneration++;
  if( !f || fwrite(&p->nByte,sizeof(p->nByte),1,f)!=1
   || fwrite(&p->iGeneration,sizeof(p->iGeneration),1,f)!=1
   || fwrite(&p->nBlock,sizeof(p->nBlock),1,f)!=1
   || (p->nBlock && fwrite(p->aBlock,sizeof(*p->aBlock),(size_t)p->nBlock,f)!=(size_t)p->nBlock)) rc = SQLITE_IOERR;
  if( f ) fclose(f);
  if( rc==SQLITE_OK && rename(zTmp,z) ) rc = SQLITE_IOERR;
  if( rc!=SQLITE_OK ) unlink(zTmp);
  sqlite3_free(z); sqlite3_free(zTmp); return rc;
}

static int osvfsReadBlock(ObjectStoreFile *p, int i, unsigned char *a){
  char *z;
  FILE *f;
  memset(a,0,OSVFS_BLOCK_SIZE);
  if( i>=p->nBlock || p->aBlock[i]==0 ) return SQLITE_OK;
  z = osvfsPath(p,"chunks",p->aBlock[i]); if(!z) return SQLITE_NOMEM;
  f=fopen(z,"rb"); sqlite3_free(z);
  if(!f) return SQLITE_CORRUPT;
  if(fread(a,1,OSVFS_BLOCK_SIZE,f)!=(size_t)OSVFS_BLOCK_SIZE){ fclose(f); return SQLITE_CORRUPT; }
  fclose(f); return SQLITE_OK;
}

static int osvfsWriteBlock(ObjectStoreFile *p, int i, const unsigned char *a){
  char *z;
  char *zTmp;
  FILE *f;
  int rc;
  if( i>=p->nAlloc && (rc=osvfsGrow(p,i+1))!=SQLITE_OK ) return rc;
  if( i>=p->nBlock ) p->nBlock=i+1;
  /* Each logical page has a stable object name. Replacing one page does not
  ** rewrite any other page or the complete database image. */
  z=osvfsPath(p,"chunks",(sqlite3_int64)i+1); if(!z) return SQLITE_NOMEM;
  zTmp=sqlite3_mprintf("%s.tmp",z);
  if( !zTmp ){ sqlite3_free(z); return SQLITE_NOMEM; }
  f=fopen(zTmp,"wb");
  if(!f) rc=SQLITE_IOERR;
  else if(fwrite(a,1,OSVFS_BLOCK_SIZE,f)!=(size_t)OSVFS_BLOCK_SIZE) rc=SQLITE_IOERR;
  else rc=SQLITE_OK;
  if( f ) fclose(f);
  if( rc==SQLITE_OK && rename(zTmp,z) ) rc=SQLITE_IOERR;
  if( rc!=SQLITE_OK ) unlink(zTmp);
  sqlite3_free(zTmp);
  sqlite3_free(z);
  if( rc==SQLITE_OK ) p->aBlock[i]=(sqlite3_int64)i+1;
  return rc;
}

static int osvfsClose(sqlite3_file *pFile){
  ObjectStoreFile *p = (ObjectStoreFile*)pFile;
  int rc = p->isBlockStore && p->isDirty ? osvfsSave(p) : SQLITE_OK;
  if( rc==SQLITE_OK ) rc = OSVFS_METHOD(p)->xClose(p->pReal);
  sqlite3_free(p->zRoot);
  sqlite3_free(p->aBlock);
  p->pReal = 0;
  return rc;
}
static int osvfsRead(sqlite3_file *p, void *z, int n, sqlite3_int64 o){
  ObjectStoreFile *f=(ObjectStoreFile*)p;
  sqlite3_int64 iEnd = o+n;
  unsigned char a[OSVFS_BLOCK_SIZE]; int i, nCopy, rc;
  if( !f->isBlockStore ) return OSVFS_METHOD(p)->xRead(OSVFS_REAL(p),z,n,o);
  if( o+n>f->nByte ) memset(z,0,(size_t)n);
  for(i=0; n>0; i+=nCopy,o+=nCopy,n-=nCopy){
    nCopy=OSVFS_BLOCK_SIZE-(int)(o%OSVFS_BLOCK_SIZE); if(nCopy>n)nCopy=n;
    if( o>=f->nByte ) break;
    if( o+nCopy>f->nByte ) nCopy=(int)(f->nByte-o);
    rc=osvfsReadBlock(f,(int)(o/OSVFS_BLOCK_SIZE),a); if(rc!=SQLITE_OK)return rc;
    memcpy((unsigned char*)z+i,a+(o%OSVFS_BLOCK_SIZE),(size_t)nCopy);
  }
  return iEnd>f->nByte ? SQLITE_IOERR_SHORT_READ : SQLITE_OK;
}
static int osvfsWrite(sqlite3_file *p, const void *z, int n, sqlite3_int64 o){
  ObjectStoreFile *f=(ObjectStoreFile*)p;
  unsigned char a[OSVFS_BLOCK_SIZE]; int i,nCopy,rc;
  if(!f->isBlockStore) return OSVFS_METHOD(p)->xWrite(OSVFS_REAL(p),z,n,o);
  for(i=0;n>0;i+=nCopy,o+=nCopy,n-=nCopy){
    nCopy=OSVFS_BLOCK_SIZE-(int)(o%OSVFS_BLOCK_SIZE);if(nCopy>n)nCopy=n;
    rc=osvfsReadBlock(f,(int)(o/OSVFS_BLOCK_SIZE),a);if(rc!=SQLITE_OK)return rc;
    memcpy(a+(o%OSVFS_BLOCK_SIZE),(const unsigned char*)z+i,(size_t)nCopy);
    rc=osvfsWriteBlock(f,(int)(o/OSVFS_BLOCK_SIZE),a);if(rc!=SQLITE_OK)return rc;
  }
  if( o>f->nByte ) f->nByte=o;
  f->isDirty = 1;
  return SQLITE_OK;
}
static int osvfsTruncate(sqlite3_file *p, sqlite3_int64 n){
  if(((ObjectStoreFile*)p)->isBlockStore){
    ((ObjectStoreFile*)p)->nByte=n;
    ((ObjectStoreFile*)p)->isDirty=1;
    return SQLITE_OK;
  }
  return OSVFS_METHOD(p)->xTruncate(OSVFS_REAL(p),n);
}
static int osvfsSync(sqlite3_file *p, int f){
  if(((ObjectStoreFile*)p)->isBlockStore){
    ObjectStoreFile *f=(ObjectStoreFile*)p;
    int rc=f->isDirty ? osvfsSave(f) : SQLITE_OK;
    if(rc==SQLITE_OK) f->isDirty=0;
    return rc;
  }
  return OSVFS_METHOD(p)->xSync(OSVFS_REAL(p),f);
}
static int osvfsFileSize(sqlite3_file *p, sqlite3_int64 *n){
  if(((ObjectStoreFile*)p)->isBlockStore){*n=((ObjectStoreFile*)p)->nByte;return SQLITE_OK;}
  return OSVFS_METHOD(p)->xFileSize(OSVFS_REAL(p),n);
}
static int osvfsLock(sqlite3_file *p, int e){
  ObjectStoreFile *f = (ObjectStoreFile*)p;
  int rc;
  if( e==SQLITE_LOCK_EXCLUSIVE ){
    rc = OSVFS_METHOD(p)->xLock(OSVFS_REAL(p), SQLITE_LOCK_SHARED);
    if( rc!=SQLITE_OK ) return rc;
  }
  rc = OSVFS_METHOD(p)->xLock(OSVFS_REAL(p),e);
  /* xOpen happens before SQLite has obtained the shared lock. Reload the
  ** manifest after that lock is in place so a connection cannot use the
  ** block count from before another writer committed. */
  if( rc==SQLITE_OK && f->isBlockStore && e==SQLITE_LOCK_SHARED && !f->isDirty ){
    rc = osvfsLoad(f);
  }
  return rc;
}
static int osvfsUnlock(sqlite3_file *p, int e){
  return OSVFS_METHOD(p)->xUnlock(OSVFS_REAL(p),e);
}
static int osvfsCheckReservedLock(sqlite3_file *p, int *r){
  return OSVFS_METHOD(p)->xCheckReservedLock(OSVFS_REAL(p),r);
}
static int osvfsFileControl(sqlite3_file *p, int op, void *a){
  ObjectStoreFile *f = (ObjectStoreFile*)p;
  if( f->isBlockStore ){
    if( op==SQLITE_FCNTL_HAS_MOVED ){
      char *z = osvfsPath(f,"HEAD",-1);
      FILE *in = z ? fopen(z,"rb") : 0;
      sqlite3_int64 nByte;
      sqlite3_int64 iGeneration;
      int rc = SQLITE_OK;
      if( !z ) return SQLITE_NOMEM;
      if( !in || fread(&nByte,sizeof(nByte),1,in)!=1
       || fread(&iGeneration,sizeof(iGeneration),1,in)!=1 ) rc=SQLITE_IOERR;
      if( in ) fclose(in);
      sqlite3_free(z);
      if( rc!=SQLITE_OK ) return rc;
      *(int*)a = iGeneration!=f->iGeneration;
      return SQLITE_OK;
    }
    if( op==SQLITE_FCNTL_SIZE_HINT ) return SQLITE_OK;
  }
  return OSVFS_METHOD(p)->xFileControl(OSVFS_REAL(p),op,a);
}
static int osvfsSectorSize(sqlite3_file *p){
  return OSVFS_METHOD(p)->xSectorSize(OSVFS_REAL(p));
}
static int osvfsDeviceCharacteristics(sqlite3_file *p){
  return OSVFS_METHOD(p)->xDeviceCharacteristics(OSVFS_REAL(p));
}
static int osvfsShmMap(sqlite3_file *p, int i, int n, int w, void volatile **a){
  return OSVFS_METHOD(p)->xShmMap(OSVFS_REAL(p),i,n,w,a);
}
static int osvfsShmLock(sqlite3_file *p, int o, int n, int f){
  return OSVFS_METHOD(p)->xShmLock(OSVFS_REAL(p),o,n,f);
}
static void osvfsShmBarrier(sqlite3_file *p){ OSVFS_METHOD(p)->xShmBarrier(OSVFS_REAL(p)); }
static int osvfsShmUnmap(sqlite3_file *p, int d){
  return OSVFS_METHOD(p)->xShmUnmap(OSVFS_REAL(p),d);
}
static int osvfsFetch(sqlite3_file *p, sqlite3_int64 o, int n, void **a){
  return OSVFS_METHOD(p)->xFetch(OSVFS_REAL(p),o,n,a);
}
static int osvfsUnfetch(sqlite3_file *p, sqlite3_int64 o, void *a){
  return OSVFS_METHOD(p)->xUnfetch(OSVFS_REAL(p),o,a);
}

static const sqlite3_io_methods osvfs_io_methods = {
  3, osvfsClose, osvfsRead, osvfsWrite, osvfsTruncate, osvfsSync,
  osvfsFileSize, osvfsLock, osvfsUnlock, osvfsCheckReservedLock,
  osvfsFileControl, osvfsSectorSize, osvfsDeviceCharacteristics,
  osvfsShmMap, osvfsShmLock, osvfsShmBarrier, osvfsShmUnmap,
  osvfsFetch, osvfsUnfetch
};

static int osvfsOpen(sqlite3_vfs *pVfs, const char *zName,
                     sqlite3_file *pFile, int flags, int *pOutFlags){
  ObjectStoreVfs *p = (ObjectStoreVfs*)pVfs;
  ObjectStoreFile *pOut = (ObjectStoreFile*)pFile;
  char *zCopy = 0;
  char *zBacking = 0;
  const char *zOpen = zName;
  int rc;
  memset(pOut, 0, sizeof(*pOut));
  pOut->pReal = (sqlite3_file*)&pOut[1];
  /* Main-db names passed between VFS layers must retain SQLite's second
  ** nul byte.  URI parsing may have reduced the incoming name to one. */
  if( osvfsIsMainDbName(zName, flags) ){
    int n = (int)strlen(zName);
    zCopy = sqlite3_malloc(n+2);
    if( !zCopy ) return SQLITE_NOMEM;
    memcpy(zCopy, zName, n+1);
    zCopy[n+1] = 0;
    zOpen = zCopy;
    {
      int nBacking = (int)strlen(zName) + (int)sizeof(".osvfs-lock");
      zBacking = sqlite3_malloc(nBacking+1);
      if( zBacking ){
        sqlite3_snprintf(nBacking, zBacking, "%s.osvfs-lock", zName);
        zBacking[nBacking] = 0;
      }
    }
    if( !zBacking ){
      sqlite3_free(zCopy);
      return SQLITE_NOMEM;
    }
    zOpen = zBacking;
  }
  rc = p->pParent->xOpen(p->pParent, zOpen, pOut->pReal, flags, pOutFlags);
  sqlite3_free(zCopy);
  sqlite3_free(zBacking);
  if( rc==SQLITE_OK && osvfsIsMainDbName(zName, flags) ){
    char *zChunks;
    pOut->zRoot = sqlite3_mprintf("%s.d", zName);
    zChunks = pOut->zRoot ? osvfsPath(pOut,"chunks",-1) : 0;
    if( !pOut->zRoot || !zChunks
     || osvfsMkdir(pOut->zRoot)!=SQLITE_OK || osvfsMkdir(zChunks)!=SQLITE_OK ){
      sqlite3_free(zChunks); OSVFS_METHOD(pOut)->xClose(pOut->pReal); return SQLITE_CANTOPEN;
    }
    sqlite3_free(zChunks);
    pOut->isBlockStore = 1;
    rc = osvfsLoad(pOut);
  }
  if( rc==SQLITE_OK ) pOut->base.pMethods = &osvfs_io_methods;
  return rc;
}
static int osvfsDelete(sqlite3_vfs *p, const char *z, int s){
  return ((ObjectStoreVfs*)p)->pParent->xDelete(((ObjectStoreVfs*)p)->pParent,z,s);
}
static int osvfsAccess(sqlite3_vfs *p, const char *z, int f, int *r){
  if( z && f==SQLITE_ACCESS_EXISTS ){
    char *zHead = sqlite3_mprintf("%s.d/HEAD", z);
    struct stat st;
    if( !zHead ) return SQLITE_NOMEM;
    *r = stat(zHead, &st)==0;
    sqlite3_free(zHead);
    return SQLITE_OK;
  }
  return ((ObjectStoreVfs*)p)->pParent->xAccess(((ObjectStoreVfs*)p)->pParent,z,f,r);
}
static int osvfsFullPathname(sqlite3_vfs *p, const char *z, int n, char *o){
  return ((ObjectStoreVfs*)p)->pParent->xFullPathname(((ObjectStoreVfs*)p)->pParent,z,n,o);
}

static ObjectStoreVfs osvfs;

SQLITE_API int sqlite3_objectstorevfs_register(sqlite3_vfs *pParent){
  if( pParent==0 ) pParent = sqlite3_vfs_find(0);
  if( pParent==0 ) return SQLITE_NOTFOUND;
  if( sqlite3_vfs_find("objectstorevfs") ) return SQLITE_OK;
  memset(&osvfs, 0, sizeof(osvfs));
  osvfs.pParent = pParent;
  osvfs.base.iVersion = pParent->iVersion;
  osvfs.base.szOsFile = (int)(sizeof(ObjectStoreFile) + pParent->szOsFile);
  osvfs.base.mxPathname = pParent->mxPathname;
  osvfs.base.zName = "objectstorevfs";
  osvfs.base.xOpen = osvfsOpen;
  osvfs.base.xDelete = osvfsDelete;
  osvfs.base.xAccess = osvfsAccess;
  osvfs.base.xFullPathname = osvfsFullPathname;
  osvfs.base.xDlOpen = pParent->xDlOpen;
  osvfs.base.xDlError = pParent->xDlError;
  osvfs.base.xDlSym = pParent->xDlSym;
  osvfs.base.xDlClose = pParent->xDlClose;
  osvfs.base.xRandomness = pParent->xRandomness;
  osvfs.base.xSleep = pParent->xSleep;
  osvfs.base.xCurrentTime = pParent->xCurrentTime;
  osvfs.base.xGetLastError = pParent->xGetLastError;
  if( pParent->iVersion>=2 ) osvfs.base.xCurrentTimeInt64 = pParent->xCurrentTimeInt64;
  if( pParent->iVersion>=3 ){
    osvfs.base.xSetSystemCall = pParent->xSetSystemCall;
    osvfs.base.xGetSystemCall = pParent->xGetSystemCall;
    osvfs.base.xNextSystemCall = pParent->xNextSystemCall;
  }
  return sqlite3_vfs_register(&osvfs.base, 0);
}

#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_objectstorevfs_init(
  sqlite3 *db, char **pzErrMsg, const sqlite3_api_routines *pApi
){
  int rc;
  (void)db;
  SQLITE_EXTENSION_INIT2(pApi);
  rc = sqlite3_objectstorevfs_register(0);
  if( rc!=SQLITE_OK && pzErrMsg ){
    *pzErrMsg = sqlite3_mprintf("objectstorevfs registration failed: %d", rc);
  }
  return rc;
}
