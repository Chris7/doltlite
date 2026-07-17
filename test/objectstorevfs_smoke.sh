#!/bin/sh
# Verify page-block persistence through objectstorevfs.
set -eu
root=${1:-/tmp/objectstorevfs-smoke.db}
so=${2:-/tmp/objectstorevfs.so}
driver=${3:-/tmp/objectstorevfs-smoke}
dir=$(dirname "$root")
base=$(basename "$root")
rm -rf "$root" "$root.d" "$root-journal" "$root-journal.d" \
  "$root.osvfs-lock" "$dir/.${base}-lock" \
  "$dir/.${base}-lock.d"
cc -fPIC -shared -Wall -Werror -I. -Isrc ext/misc/objectstorevfs.c -o "$so"
make doltlite-lib
cc -Wall -Werror -I. test/objectstorevfs_smoke.c -L. -ldoltlite \
  -Wl,-rpath,"$(pwd)" -o "$driver"
"$driver" "$root" "$so"
test -s "$root.d/HEAD"
test "$(find "$root.d/chunks" -type f | wc -l | tr -d ' ')" -gt 0
printf 'object-store layout:\n'
find "$root.d" -type f -printf '%P %s bytes\n' | sort
