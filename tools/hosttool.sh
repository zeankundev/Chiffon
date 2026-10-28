#!/bin/sh
BUILD=${1:-build}; CC=${2:-cc}
SRC=$(dirname "$0")/mkcpio.c
try() {
    d=$1
    mkdir -p "$d" 2>/dev/null || return 1
    if [ ! -f "$d/mkcpio" ] || [ "$SRC" -nt "$d/mkcpio" ]; then
        rm -f "$d/mkcpio"
        $CC -O2 -o "$d/mkcpio" "$SRC" >/dev/null 2>&1 || return 1
    fi
    "$d/mkcpio" >/dev/null 2>&1                       # no arguments: prints usage, exits 1
    rc=$?
    [ $rc -ne 126 ] && [ $rc -ne 127 ] && [ -f "$d/mkcpio" ]
}
if try "$BUILD"; then echo "$BUILD/mkcpio"; exit 0; fi
alt=${TMPDIR:-/tmp}/chiffon-host-$(id -u 2>/dev/null || echo 0)
if try "$alt"; then echo "$alt/mkcpio"; exit 0; fi
echo "error: failed to build mkcpio" >&2
exit 1