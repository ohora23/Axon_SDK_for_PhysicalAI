#!/bin/bash
# On the board, as root: install the RT kernel next to the stock one and make it the default.
# The stock kernel stays as extlinux LABEL primary. Revert:
#   sudo sed -i 's/^DEFAULT rt$/DEFAULT primary/' /boot/extlinux/extlinux.conf
# A kernel that fails to boot can only be bypassed from the console (pick "primary" in the
# boot menu), so have HDMI + keyboard or the serial console attached before rebooting.
#
# usage: sudo install-rt.sh <dir with Image and lib/modules/<release>>
set -euo pipefail
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
K=$(realpath "$1")
REL=$(ls "$K/lib/modules")
CONF=/boot/extlinux/extlinux.conf

install -m 644 "$K/Image" /boot/Image.rt
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
