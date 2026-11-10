#!/bin/bash
# On the board, as root: install the RT kernel next to the stock one and make it the default.
# The stock kernel stays as extlinux LABEL primary. Revert:
#   sudo sed -i 's/^DEFAULT rt$/DEFAULT primary/' <boot partition>/boot/extlinux/extlinux.conf
# A kernel that fails to boot can only be bypassed from the console (pick "primary" in the
# boot menu), so have HDMI + keyboard or the serial console attached before rebooting.
#
# The firmware reads extlinux.conf from the partition it boots, which need not be the root fs:
# hermes-head boots eMMC mmcblk0p1 with root=/dev/nvme0n1p1, so edits to the NVMe /boot did
# nothing for three weeks. Point BOOT_PART at the boot partition when they differ; the script
# refuses when the running kernel is not that partition's /boot/Image (FORCE=1 overrides).
#
# usage: sudo [BOOT_PART=/dev/mmcblk0p1] install-rt.sh <dir with Image and lib/modules/<release>>
set -euo pipefail
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
K=$(realpath "$1")
REL=$(ls "$K/lib/modules")
B=""
if [ -n "${BOOT_PART:-}" ]; then
    B=$(mktemp -d); trap 'umount "$B" 2>/dev/null; rmdir "$B"' EXIT; mount "$BOOT_PART" "$B"
fi
CONF=$B/boot/extlinux/extlinux.conf
# The running kernel (stock or an RT one installed before) must be one of this partition's
# images. strings goes to a variable first: grep -q in a pipe would SIGPIPE it under pipefail.
running_found() {
    local f v
    for f in "$B/boot/Image" "$B/boot/Image.rt"; do
        [ -f "$f" ] || continue
        v=$(strings "$f" | grep -m1 "Linux version" || true)
        case "$v" in *"$(uname -v)"*) return 0 ;; esac
    done
    return 1
}
if ! running_found && [ "${FORCE:-0}" != 1 ]; then
    echo "running kernel '$(uname -v)' is not in $B/boot/Image{,.rt}: the firmware boots another" >&2
    echo "partition (set BOOT_PART), or /boot/Image changed since boot (apt?). FORCE=1 to go on." >&2
    exit 1
fi

install -m 644 "$K/Image" "$B/boot/Image.rt"
rm -rf "/lib/modules/$REL"
cp -r "$K/lib/modules/$REL" /lib/modules/
chown -R root:root "/lib/modules/$REL"
depmod "$REL"

[ -f "$CONF.bak-pre-rt" ] || cp "$CONF" "$CONF.bak-pre-rt"
if ! grep -q "^LABEL rt$" "$CONF"; then
    append=$(grep -m1 "^\s*APPEND" "$CONF")
    fdt=$(grep -m1 "^\s*FDT" "$CONF")
    printf "\nLABEL rt\n      MENU LABEL PREEMPT_RT kernel (%s)\n      LINUX /boot/Image.rt\n%s\n      INITRD /boot/initrd\n%s\n" \
        "$REL" "$fdt" "$append" >> "$CONF"
fi
sed -i "s/^DEFAULT .*/DEFAULT rt/" "$CONF"
echo "OK - reboot to boot $REL"
