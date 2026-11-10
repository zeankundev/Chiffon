
ENVDIR=$1; ROOT=$2; USERLAND=$3; ARCH=$4; CROSS=$5; REPO=$(pwd)
set -e                                           # a failing env build.sh must fail the build, not ship a bad rootfs
rm -rf "$ROOT"; mkdir -p "$ROOT"
[ -d "$ENVDIR/rootfs" ] && cp -R "$ENVDIR/rootfs/." "$ROOT/"
if [ -d "$USERLAND" ]; then
    for f in "$USERLAND"/*; do
        [ -f "$f" ] || continue
        n=$(basename "$f")
        if [ "$n" = init ]; then mkdir -p "$ROOT/sbin"; cp "$f" "$ROOT/sbin/init"; chmod 755 "$ROOT/sbin/init" 2>/dev/null || true
        else mkdir -p "$ROOT/bin"; cp "$f" "$ROOT/bin/$n"; chmod 755 "$ROOT/bin/$n" 2>/dev/null || true; fi
    done
fi
case $ROOT in /*) ABSROOT=$ROOT;; *) ABSROOT=$REPO/$ROOT;; esac
if [ -f "$ENVDIR/build.sh" ]; then ARCH=$ARCH sh "$ENVDIR/build.sh" "$ABSROOT" "$ARCH" "$CROSS" "$REPO" || { echo "mkrootfs: $ENVDIR/build.sh failed" >&2; exit 1; }; fi
exit 0
