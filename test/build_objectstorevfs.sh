#!/bin/sh
# Build DoltLite and its loadable object-store page-block VFS extension.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out=${1:-"$root/build/objectstorevfs"}

mkdir -p "$out"
make -C "$root" doltlite-lib shell_renamed.c
cc -DSQLITE_CORE -Wall -Werror -I"$root" -I"$root/src" \
  -c "$root/ext/misc/objectstorevfs.c" -o "$out/objectstorevfs.o"
cc -Wall -I"$root" \
  "$root/test/objectstorevfs_shell.c" "$root/shell_renamed.c" \
  "$out/objectstorevfs.o" "$root/libdoltlite.a" \
  -lpthread -lm -lz -o "$out/doltlite-objectstore"

printf 'DoltLite binary: %s\n' "$out/doltlite-objectstore"
