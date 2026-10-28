#!/bin/sh
set -e
OUT=build/chiffon.img; FS=ext4; SIZE=; MBR=0; LABEL=chiffon; ROOT=
usage() { sed -n '2,10p' "$0" >&2; exit 1; }
while getopts r:o:f:s:pl: o; do
    case $o in r) ROOT=$OPTARG;; o) OUT=$OPTARG;; f) FS=$OPTARG;; s) SIZE=$OPTARG;; p) MBR=1;; l) LABEL=$OPTARG;; *) usage;; esac
done
[ -d "$ROOT" ] || usage
mkdir -p "$(dirname "$OUT")"
case $FS in
  cpio) ${MKCPIO:-build/mkcpio} "$ROOT" "$OUT"; echo "mkimage: wrote $OUT (initramfs archive)"; exit 0;;
  ext2|ext3|ext4) ;;
  *) echo "mkimage: filesystem '$FS' has no Chiffon kernel driver yet"; exit 1;;
esac
MKE2FS=$(command -v mke2fs || command -v /sbin/mke2fs || command -v /usr/sbin/mke2fs || true)
[ -n "$MKE2FS" ] || { echo "mkimage: mke2fs not found. Install e2fsprogs (apt install e2fsprogs / pkg install e2fsprogs)." >&2; exit 1; }
if [ -z "$SIZE" ]; then
    used=$(du -sk "$ROOT" | cut -f1)
    SIZE=$(( used / 1024 * 2 + 32 ))M
fi
case $SIZE in *[Gg]) mb=$(( ${SIZE%[Gg]} * 1024 ));; *[Mm]) mb=${SIZE%[Mm]};; *) mb=$SIZE;; esac
OPTS="-q -F -t $FS -L $LABEL -d $ROOT"
[ "$FS" = ext4 ] && [ -z "$KEEP_JOURNAL" ] && OPTS="$OPTS -O ^has_journal"   # kernel mounts read-only; journal is dead weight
rm -f "$OUT"
if [ "$MBR" = 1 ]; then
    truncate -s $(( mb + 1 ))M "$OUT"
    $MKE2FS $OPTS -E offset=1048576 "$OUT" ${mb}M
    # MBR: partition 1, type 0x83, starts at LBA 2048
    le32() { printf "\\$(printf %03o $(($1 & 255)))\\$(printf %03o $(($1 >> 8 & 255)))\\$(printf %03o $(($1 >> 16 & 255)))\\$(printf %03o $(($1 >> 24 & 255)))"; }
    { printf '\000\000\000\000\203\376\377\377'; le32 2048; le32 $(( mb * 2048 )); } > "$OUT.mbr"
    dd if="$OUT.mbr" of="$OUT" bs=1 seek=446 conv=notrunc 2>/dev/null
    printf '\125\252' | dd of="$OUT" bs=1 seek=510 conv=notrunc 2>/dev/null
    rm -f "$OUT.mbr"
else
    truncate -s ${mb}M "$OUT"
    $MKE2FS $OPTS "$OUT"
fi
echo "mkimage: wrote $OUT ($FS, ${mb}M$( [ "$MBR" = 1 ] && echo ', MBR partition 1'))"