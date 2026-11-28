#!/bin/sh
set -e
ROOT=${1:?usage: build.sh ROOTFS ARCH CROSS REPO}
ARCH=${2:-${ARCH:-x86_64}}
REPO=${4:-$(dirname "$0")/../..}
MUSL=${MUSL:-/usr/local/musl}
OPENRC_STAGE=${OPENRC_STAGE:?set OPENRC_STAGE=\$HOME/openrc-stage}
. "$REPO/tools/rootfs-https.sh"

# ---- BusyBox -------------------------------------------------------------
BB=${BUSYBOX:-$(command -v busybox || true)}
[ -n "$BB" ] && [ -f "$BB" ] || { echo "build.sh: no BusyBox found. Set BUSYBOX=/path/to/static/busybox" >&2; exit 1; }
if command -v readelf >/dev/null 2>&1 && readelf -l "$BB" 2>/dev/null | grep -q INTERP; then
    echo "build.sh: $BB is dynamically linked; Chiffon needs a STATIC BusyBox." >&2; exit 1
fi
if command -v readelf >/dev/null 2>&1; then
    m=$(readelf -h "$BB" 2>/dev/null | sed -n 's/^ *Machine: *//p')
    case "$ARCH:$m" in
        x86_64:*X86-64*|aarch64:*AArch64*) ;;
        *) echo "build.sh: $BB is '$m' but the target is $ARCH." >&2; exit 1;;
    esac
fi

mkdir -p "$ROOT/bin" "$ROOT/sbin" "$ROOT/usr/bin" "$ROOT/usr/sbin" "$ROOT/etc" \
         "$ROOT/tmp" "$ROOT/dev" "$ROOT/proc" "$ROOT/sys" "$ROOT/root" \
         "$ROOT/run" "$ROOT/var/log" "$ROOT/var/lib/rc" "$ROOT/lib"
cp "$BB" "$ROOT/bin/busybox"; chmod 755 "$ROOT/bin/busybox" 2>/dev/null || true

APPLETS=$("$BB" --list 2>/dev/null || true)
[ -n "$APPLETS" ] || APPLETS="sh ash cat ls echo mkdir rmdir rm cp mv ln pwd ps kill sleep date uname env \
  head tail wc grep sed awk sort uniq tr cut chmod touch true false test [ expr basename dirname dmesg \
  df free uptime whoami id hostname vi less more find xargs tee mount umount mountpoint reboot poweroff halt \
  ifconfig route wget"
skipped=""
for a in $APPLETS; do
    [ "$a" = busybox ] && continue
    ln -sf busybox "$ROOT/bin/$a" 2>/dev/null || skipped="$skipped $a"
done
[ -n "$skipped" ] && echo "build.sh: warning: could not link applets (skipped:$skipped). Build from ~/, not /mnt/c." >&2
rm -f "$ROOT/bin/sh"
ln -s busybox "$ROOT/bin/sh" 2>/dev/null || cp "$ROOT/bin/busybox" "$ROOT/bin/sh"

# OpenRC's dependency cache and boot scripts need these applets
missing=""
required="awk sed grep sort cat mount umount mountpoint ifconfig route hostname"
if [ -n "${OPENSSL_BIN:-}${CA_CERTS:-}" ]; then
    required="$required wget"
fi
for a in $required; do
    echo "$APPLETS" | grep -qw "$a" || missing="$missing $a"
done
[ -z "$missing" ] || { echo "build.sh: BusyBox is missing required applets:$missing" >&2; exit 1; }

# ---- musl dynamic loader (PT_INTERP that Chiffon's proc.c loads) ---------
case $ARCH in
    x86_64)  LD=ld-musl-x86_64.so.1 ;;
    aarch64) LD=ld-musl-aarch64.so.1 ;;
esac
cp "$MUSL/lib/libc.so" "$ROOT/lib/$LD"
ln -sf "$LD" "$ROOT/lib/libc.so"

# ---- OpenRC (static, musl), staged with DESTDIR --------------------------
cp -a "$OPENRC_STAGE/." "$ROOT/"
for f in sbin/openrc sbin/openrc-run lib/rc/sh/openrc-run.sh; do
    [ -e "$ROOT/$f" ] || { echo "build.sh: OpenRC stage is missing /$f" >&2; exit 1; }
done
mkdir -p "$ROOT/lib/rc/cache" "$ROOT/run/openrc" "$ROOT/run/lock"
install_https "$ROOT"

# ---- /etc basics ---------------------------------------------------------
cat > "$ROOT/etc/profile" <<'EOP'
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
export HOME=/root
if [ -r /etc/ssl/cert.pem ]; then
    export SSL_CERT_FILE="${SSL_CERT_FILE:-/etc/ssl/cert.pem}"
fi
alias ll='ls -l'
PS1='[\u@\h \w] (chiffon) \$ '

# Pid 1 is the init shell (chiffon.ini [init]). Boot OpenRC once, from it.
if [ "$$" = 1 ]; then
    openrc sysinit
    openrc boot
    openrc default
fi
echo "Welcome to Chiffon (OpenRC, musl)."
EOP

cat > "$ROOT/etc/passwd" <<'EOF'
root:x:0:0:root:/root:/bin/sh
EOF
cat > "$ROOT/etc/group" <<'EOF'
root:x:0:
EOF
cat > "$ROOT/etc/shells" <<'EOF'
/bin/sh
/bin/ash
EOF
cat > "$ROOT/etc/hostname" <<'EOF'
chiffon
EOF
cat > "$ROOT/etc/hosts" <<'EOF'
127.0.0.1   localhost localhost.localdomain chiffon
::1         localhost ip6-localhost
EOF
cat > "$ROOT/etc/resolv.conf" <<'EOF'
# QEMU user-mode network DNS first, then a public resolver.
nameserver 10.0.2.3
nameserver 1.1.1.1
options single-request
EOF
cat > "$ROOT/etc/fstab" <<'EOF'
# <device>  <mountpoint>  <type>  <options>  <dump>  <pass>
EOF

cat > "$ROOT/etc/rc.conf" <<'EOF'
rc_parallel="NO"
rc_interactive="NO"
rc_logger="NO"
rc_sys=""
rc_tmpdir="/run/openrc"
unicode="NO"
EOF

mkdir -p "$ROOT/etc/conf.d"
cat > "$ROOT/etc/conf.d/networking" <<'EOF'
# Defaults match QEMU user-mode networking. Change for real networks.
IFACE=eth0
ADDRESS=10.0.2.15
NETMASK=255.255.255.0
GATEWAY=10.0.2.2
EOF

# ---- OpenRC service scripts ---------------------------------------------
install_svc() {
    mkdir -p "$ROOT/etc/init.d"
    cat > "$ROOT/etc/init.d/$1"
    chmod 755 "$ROOT/etc/init.d/$1"
}

install_svc mountkernfs <<'EOS'
#!/sbin/openrc-run
description="Verify kernel filesystems (Chiffon provides /proc and /dev natively)"
depend() { provide mountkernfs; }
start() {
    ebegin "Checking kernel filesystems"
    mkdir -p /proc /sys /dev /run
    [ -r /proc/self/stat ] || ewarn "/proc is not readable"
    eend 0
}
EOS

install_svc hostname <<'EOS'
#!/sbin/openrc-run
description="Set the system hostname from /etc/hostname"
depend() { need mountkernfs; provide hostname; }
start() {
    ebegin "Setting hostname"
    hostname "$(cat /etc/hostname)"
    eend $?
}
EOS

install_svc bootmisc <<'EOS'
#!/sbin/openrc-run
description="Create runtime and log directories"
depend() { need mountkernfs; provide bootmisc; }
start() {
    ebegin "Preparing /run, /var and /tmp"
    mkdir -p /run/openrc /run/lock /var/log /var/lib/rc /tmp
    chmod 1777 /tmp
    eend 0
}
EOS

install_svc localmount <<'EOS'
#!/sbin/openrc-run
description="Mount filesystems listed in /etc/fstab"
depend() { need mountkernfs; provide localmount; }
start() {
    ebegin "Mounting local filesystems"
    mount -a 2>/dev/null
    eend 0
}
EOS

install_svc networking <<'EOS'
#!/sbin/openrc-run
description="Bring up the network interface"
depend() { need localmount hostname bootmisc; provide net; }
start() {
    ebegin "Configuring $IFACE"
    ifconfig "$IFACE" "$ADDRESS" netmask "$NETMASK" up &&
        route add default gw "$GATEWAY" dev "$IFACE"
    eend $?
}
stop() {
    ebegin "Bringing down $IFACE"
    ifconfig "$IFACE" down
    eend $?
}
EOS

# ---- runlevels -----------------------------------------------------------
enable() {
    mkdir -p "$ROOT/etc/runlevels/$1"
    ln -sf "/etc/init.d/$2" "$ROOT/etc/runlevels/$1/$2"
}
enable sysinit mountkernfs
enable sysinit hostname
enable sysinit bootmisc
enable boot    localmount
enable boot    networking
mkdir -p "$ROOT/etc/runlevels/default"

echo "build.sh: rootfs ready ($ARCH). Runlevels: sysinit, boot, default."
