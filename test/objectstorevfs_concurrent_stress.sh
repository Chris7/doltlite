#!/bin/sh
# Build and run the objectstorevfs multi-process concurrency stress test.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
db=${1:-/tmp/objectstorevfs-concurrent.db}
so=${2:-/tmp/objectstorevfs-concurrent.so}
bin=${3:-/tmp/objectstorevfs-concurrent}
dir=$(dirname "$db")
base=$(basename "$db")
rm -rf "$db" "$db.d" "$db-journal" "$db.osvfs-lock" \
  "$dir/.${base}-lock" "$dir/.${base}-lock.d"
cc -fPIC -shared -Wall -Werror -I"$root" -I"$root/src" \
  "$root/ext/misc/objectstorevfs.c" -o "$so"
make -C "$root" doltlite-lib
cc -Wall -Werror -I"$root" "$root/test/objectstorevfs_concurrent_stress.c" \
  -L"$root" -ldoltlite -Wl,-rpath,"$root" -o "$bin"
"$bin" "$db" "$so"
