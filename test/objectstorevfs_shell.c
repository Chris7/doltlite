#include "doltlite.h"
#include <stdio.h>

extern int sqlite3_shell_main(int, char**);
extern int sqlite3_objectstorevfs_register(sqlite3_vfs*);

int main(int argc, char **argv){
  sqlite3_vfs *pVfs;
  int rc = sqlite3_objectstorevfs_register(0);
  if( rc!=SQLITE_OK ){
    fprintf(stderr, "cannot register objectstorevfs: %d\n", rc);
    return 1;
  }
  pVfs = sqlite3_vfs_find("objectstorevfs");
  if( !pVfs || sqlite3_vfs_register(pVfs, 1)!=SQLITE_OK ){
    fprintf(stderr, "cannot make objectstorevfs the default VFS\n");
    return 1;
  }
  return sqlite3_shell_main(argc, argv);
}
