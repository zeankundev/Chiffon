set -e
ENVDIR=$1; ROOT=$2; USERLAND=$3; ARCH=$4; CROSS=$5; REPO=$(pwd)
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
[ -f "$ENVDIR/build.sh" ] && ARCH=$ARCH sh "$ENVDIR/build.sh" "$ABSROOT" "$ARCH" "$CROSS" "$REPO"
exit 0
