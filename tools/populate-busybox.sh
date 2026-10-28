set -e
ROOT=${1:?usage: populate-busybox.sh ROOTFS}
BB=${BUSYBOX:-$(command -v busybox || true)}
[ -n "$BB" ] && [ -f "$BB" ] || { echo "populate-busybox: no BusyBox found. Set BUSYBOX=/path/to/static/busybox (see README)." >&2; exit 1; }
if command -v readelf >/dev/null 2>&1 && readelf -l "$BB" 2>/dev/null | grep -q INTERP; then
    echo "populate-busybox: $BB is dynamically linked; Chiffon needs a STATIC BusyBox." >&2; exit 1
fi
if [ -n "$ARCH" ] && command -v readelf >/dev/null 2>&1; then
    m=$(readelf -h "$BB" 2>/dev/null | sed -n 's/^ *Machine: *//p')
    case "$ARCH:$m" in
        x86_64:*X86-64*|aarch64:*AArch64*) ;;
        *) echo "populate-busybox: $BB is '$m' but the target is $ARCH. Provide a matching static BusyBox via BUSYBOX=." >&2; exit 1;;
    esac
fi
mkdir -p "$ROOT/bin" "$ROOT/sbin" "$ROOT/usr/bin" "$ROOT/etc" "$ROOT/tmp" "$ROOT/dev" "$ROOT/root"
cp "$BB" "$ROOT/bin/busybox"; chmod 755 "$ROOT/bin/busybox" 2>/dev/null || true
APPLETS=$("$BB" --list 2>/dev/null || true)       # works when host arch == target arch
[ -n "$APPLETS" ] || APPLETS="sh ash cat ls echo mkdir rmdir rm cp mv ln pwd ps kill sleep date uname env \
  head tail wc grep sed sort uniq tr cut chmod touch true false test [ expr basename dirname dmesg \
  df free uptime whoami id hostname vi less more find xargs tee mount umount reboot poweroff halt"
for a in $APPLETS; do
    [ "$a" = busybox ] && continue
    ln -sf busybox "$ROOT/bin/$a"
done
[ -f "$ROOT/etc/profile" ] || cat > "$ROOT/etc/profile" <<'EOP'
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
export HOME=/root
alias ll='ls -l'
PS1='[\u@\h \w] (chiffon) \$ '
echo "Welcome to Chiffon! Type 'help' for a list of commands."
EOP
[ -f "$ROOT/etc/passwd" ] || echo "root:x:0:0:root:/root:/bin/sh" > "$ROOT/etc/passwd"
[ -f "$ROOT/etc/group" ] || echo "root:x:0:" > "$ROOT/etc/group"