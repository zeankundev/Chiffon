#!/bin/sh
# Create a BIOS El Torito ISO for the x86_64 Multiboot kernel.
# grub-mkrescue supplies the El Torito boot image; the kernel itself is loaded
# using GRUB's Multiboot 1 command.
set -eu

if [ "$#" -ne 2 ]; then
    echo "usage: $0 KERNEL_ELF OUTPUT_ISO" >&2
    exit 2
fi

kernel=$1
output=$2
grub_mkrescue=${GRUB_MKRESCUE:-grub-mkrescue}

if ! command -v "$grub_mkrescue" >/dev/null 2>&1; then
    echo "error: $grub_mkrescue is required (install grub-pc-bin and xorriso in WSL)" >&2
    exit 127
fi
if [ ! -f "$kernel" ]; then
    echo "error: kernel not found: $kernel" >&2
    exit 1
fi

stage=$(mktemp -d "${TMPDIR:-/tmp}/chiffon-iso.XXXXXX")
trap 'rm -rf "$stage"' EXIT HUP INT TERM
mkdir -p "$stage/boot/grub" "$(dirname "$output")"
cp "$kernel" "$stage/boot/chiffon.elf"

cat > "$stage/boot/grub/grub.cfg" <<'EOF'
set timeout=0
set default=0

# Request a pixel framebuffer from the firmware/VESA before handing off to
# the kernel.  This is essential on machines that do not forward legacy VGA
# text mode (0xB8000) to the display, such as VirtualBox (VMSVGA/VBoxSVGA),
# VMware, most modern laptops with UEFI GOP, and hybrid-graphics systems.
#
# GRUB will try the preferred resolution first; if the display does not
# support it, it falls back down the list automatically.  The Multiboot
# information structure will carry the framebuffer address, dimensions and
# pixel format in its framebuffer tag (MBI bit 12), which the kernel's
# mb_fb driver adopts without needing a chipset-specific display driver.
insmod all_video

if loadfont /boot/grub/fonts/unicode.pf2; then
    insmod gfxterm
    terminal_output gfxterm
fi

set gfxpayload=1024x768x32,1024x768x24,800x600x32,800x600x24,640x480x32,640x480x24

menuentry "Chiffon x86_64" {
    insmod all_video
    set gfxpayload=keep
    multiboot /boot/chiffon.elf root=initramfs
    boot
}
EOF

"$grub_mkrescue" -o "$output" "$stage"
echo "Created bootable El Torito ISO: $output"
