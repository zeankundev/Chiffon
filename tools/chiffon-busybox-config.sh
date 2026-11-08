#!/bin/sh
# Finalized BusyBox config for Chiffon (aarch64, static, musl).
# Usage: cd ~/busybox && sh ~/chiffon-busybox-config.sh
set -e

set_cfg() {
    sed -i "/^# $1 is not set/d; /^$1=/d" .config
    if [ "$2" = y ]; then echo "$1=y" >> .config; else echo "# $1 is not set" >> .config; fi
}

make defconfig

# linking / build behavior
set_cfg CONFIG_STATIC y
set_cfg CONFIG_WERROR n

# features Chiffon doesn't have
set_cfg CONFIG_PAM n
set_cfg CONFIG_SELINUX n
set_cfg CONFIG_FEATURE_UTMP n
set_cfg CONFIG_FEATURE_WTMP n
set_cfg CONFIG_FEATURE_SHADOWPASSWDS n
set_cfg CONFIG_TC n

# x86-only hardware SHA paths (no aarch64 implementation)
set_cfg CONFIG_SHA1_HWACCEL n
set_cfg CONFIG_SHA256_HWACCEL n

# mtd tools: headers not available under musl
set_cfg CONFIG_NANDWRITE n
set_cfg CONFIG_NANDDUMP n

yes "" | make oldconfig > /dev/null
echo "Config ready: $(grep -c '^CONFIG_.*=y' .config) applets enabled."
