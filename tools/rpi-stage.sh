#!/bin/sh
# Stage a Raspberry Pi boot directory for rpiboot (usbboot), with no SD card.
#   usage: rpi-stage.sh <kernel.bin> <board-dir> <firmware-dir> <out-dir>
# <firmware-dir> is the boot/ folder of a raspberrypi/firmware checkout (not included here).
set -e
KERNEL=$1; BOARD=$2; FW=$3; OUT=$4
[ -f "$KERNEL" ] || { echo "rpi-stage: no kernel image at $KERNEL" >&2; exit 1; }
[ -d "$FW" ] || { echo "rpi-stage: set FIRMWARE=<raspberrypi/firmware>/boot" >&2; exit 1; }
for f in start.elf fixup.dat bcm2710-rpi-zero-2-w.dtb overlays/dwc2.dtbo; do
    [ -f "$FW/$f" ] || { echo "rpi-stage: missing $FW/$f" >&2; exit 1; }
done
rm -rf "$OUT"; mkdir -p "$OUT/overlays"
cp "$KERNEL" "$OUT/kernel8.img"
cp "$BOARD/boot/config.txt" "$OUT/config.txt"
cp "$FW/start.elf" "$FW/fixup.dat" "$FW/bcm2710-rpi-zero-2-w.dtb" "$OUT/"
cp "$FW/overlays/dwc2.dtbo" "$OUT/overlays/"
[ -f "$FW/bootcode.bin" ] && cp "$FW/bootcode.bin" "$OUT/" || true
echo "rpi-stage: boot directory ready in $OUT"
echo "rpi-stage: connect the Pi's DATA port (not PWR), then serve this directory with rpiboot (usbboot)."
