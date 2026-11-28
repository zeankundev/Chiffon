#!/bin/sh

# Shared by the BusyBox and milkOS 2 image builders.
check_static_elf() {
    headers=$(LC_ALL=C readelf -h -l "$1") || return 1
    if printf '%s\n' "$headers" | grep -q '^[[:space:]]*INTERP'; then
        echo "rootfs: $1 must be statically linked." >&2
        return 1
    fi
    machine=$(printf '%s\n' "$headers" | sed -n 's/^ *Machine: *//p')
    case "${ARCH:-}:$machine" in
        :*|x86_64:*X86-64*|aarch64:*AArch64*) ;;
        *) echo "rootfs: $1 is '$machine' but the target is $ARCH." >&2; return 1;;
    esac
}

install_https() {
    [ -n "${OPENSSL_BIN:-}${CA_CERTS:-}" ] || return 0
    if [ ! -f "${OPENSSL_BIN:-}" ] || [ ! -s "${CA_CERTS:-}" ]; then
        echo "rootfs: HTTPS requires OPENSSL_BIN (static target ELF) and CA_CERTS (PEM bundle)." >&2
        return 1
    fi
    check_static_elf "$OPENSSL_BIN" || return 1
    if ! grep -q '^-----BEGIN CERTIFICATE-----' "$CA_CERTS"; then
        echo "rootfs: CA_CERTS must contain PEM certificates." >&2
        return 1
    fi
    mkdir -p "$1/usr/bin" "$1/etc/ssl"
    cp "$OPENSSL_BIN" "$1/usr/bin/openssl"
    chmod 755 "$1/usr/bin/openssl"
    cp "$CA_CERTS" "$1/etc/ssl/cert.pem"
    chmod 644 "$1/etc/ssl/cert.pem"
}
