#!/usr/bin/env bash
# board_doctor.sh: read-only health check for a Jetson board (L4T r35, extlinux boot).
# Run it before and after changing the boot config, after `apt upgrade`, and after the board
# was unreachable. One line per check, `PASS|WARN|FAIL|INFO <check>: <detail>`; exit 1 on FAIL.
#
#   boot-source       which partition holds the running kernel (every boot/Image* on every ext4
#                     partition; several holding it are told apart by whose DEFAULT boots it)
#   boot-default      that partition's DEFAULT entry, and whether the root fs has its own extlinux
#   kernel-vs-rootfs  the DEFAULT image on the root fs /boot vs the boot partition's
#   modules           /lib/modules/$(uname -r)
#   nic-binding       NetworkManager ethernet profiles bound by name instead of MAC
#   recent-boots      kernel of the last 6 boots (the board clock may jump between boots)
#   pstore            leftover panic/console dumps
#   apt-hold          nvidia-l4t-kernel held when the boot partition is not the root fs
#   rt                PREEMPT_RT, isolated CPUs, power mode
#
# Root is needed for the boot-partition checks: it mounts each candidate with `-o ro,noload`
# (noload: no journal replay, so nothing is written even after a crash). As a user in group adm
# only the root fs and the journal are checked.
#   usage: sudo instrumentation/jetson/board_doctor.sh
set -u

rc=0
report() { echo "$1 $2: $3"; [ "$1" = FAIL ] && rc=1; return 0; }
is_root=0; [ "$(id -u)" = 0 ] && is_root=1
RUN=$(uname -v)

MNTS=()
cleanup() {
    for m in "${MNTS[@]+"${MNTS[@]}"}"; do
        if mountpoint -q "$m"; then umount "$m" || echo "WARN cleanup: $m is still mounted" >&2; fi
        rmdir "$m" 2>/dev/null
    done
}
trap cleanup EXIT

kver() { grep -a -m1 -o 'Linux version [[:print:]]*' "$1" 2>/dev/null; }   # no binutils needed
runs() { kver "$1" | grep -Fq "$RUN"; }
# extlinux: case-insensitive keywords, CRLF tolerated, comments ignored
xl_default() { awk '{sub(/\r$/,"")} toupper($1)=="DEFAULT"{print $2; exit}' "$1"; }
xl_linux() { awk -v l="$2" '{sub(/\r$/,"")} toupper($1)=="LABEL"{s=($2==l)} s && toupper($1)=="LINUX"{print $2; exit}' "$1"; }

# ── candidates: the root fs, plus (root only) every other ext4 partition, mounted ro,noload ──
NAMES=("/"); ROOTS=("")
mount_failed=0
if [ "$is_root" = 1 ]; then
    while read -r part; do
        d=$(mktemp -d); MNTS+=("$d")
        if mount -t ext4 -o ro,noload "$part" "$d" 2>/dev/null; then
            NAMES+=("$part"); ROOTS+=("$d")
        else
            report WARN boot-source-candidate "$part: mount failed"; mount_failed=1
        fi
    done < <(lsblk -rpno NAME,FSTYPE,MOUNTPOINT | awk '$2=="ext4" && $3!="/" {print $1}')
fi

# ── 1. boot-source ──
hits=0; dhits=0; bi=-1
for i in "${!NAMES[@]}"; do
    r=${ROOTS[$i]}; conf="$r/boot/extlinux/extlinux.conf"
    label="(none)"; dimg=""
    if [ -f "$conf" ]; then
        label=$(xl_default "$conf")
        label=${label:-$(awk '{sub(/\r$/,"")} toupper($1)=="LABEL"{print $2; exit}' "$conf")}   # no DEFAULT: first entry
        label=${label:-"(none)"}
        dl=$(xl_linux "$conf" "$label"); [ -n "$dl" ] && dimg="$r$dl"
    fi
    held=""
    for img in "$r"/boot/Image*; do
        case "$img" in *.sig) continue ;; esac
        [ -f "$img" ] && runs "$img" && { held=${img##*/}; break; }
    done
    druns=no; [ -n "$dimg" ] && [ -f "$dimg" ] && runs "$dimg" && druns=yes
    report INFO boot-source-candidate "${NAMES[$i]} DEFAULT=$label holds_running=${held:-no} default_boots_it=$druns"
    if [ -n "$held" ]; then
        hits=$((hits + 1))
        if [ "$druns" = yes ]; then dhits=$((dhits + 1)); bi=$i; elif [ "$bi" -lt 0 ]; then bi=$i; fi
    fi
done

known=0
if [ "$is_root" = 0 ]; then
    report WARN boot-source "only the root fs was checked (run with sudo to check the other partitions)"
elif [ "$mount_failed" = 1 ]; then
    report WARN boot-source "a candidate failed to mount; the boot partition may be among them"
elif [ "$hits" = 0 ]; then
    report FAIL boot-source "running kernel is in no boot/Image*: the firmware boots something not checked here, or the image changed since boot"
elif [ "$hits" = 1 ] || [ "$dhits" = 1 ]; then
    report PASS boot-source "boot partition is ${NAMES[$bi]}"; known=1
else
    report WARN boot-source "$hits partitions hold the running kernel and $dhits boot it by DEFAULT: ambiguous"
fi

if [ "$known" = 1 ]; then
    B=${ROOTS[$bi]}; conf="$B/boot/extlinux/extlinux.conf"; dl=""
    # ── 2. boot-default ──
    if [ -f "$conf" ]; then
        label=$(xl_default "$conf"); dl=$(xl_linux "$conf" "${label:-}")
        report INFO boot-default "DEFAULT=${label:-(none)} LINUX=${dl:-(not found)}"
        if [ -n "$dl" ] && [ ! -f "$B$dl" ]; then
            report WARN boot-default "DEFAULT image $dl is missing on the boot partition: the next boot may fail"
        elif [ -n "$dl" ] && ! runs "$B$dl"; then
            report WARN boot-default "the DEFAULT entry boots a different kernel than the one running now"
        fi
    fi
    if [ -n "$B" ] && [ -f /boot/extlinux/extlinux.conf ]; then
        report WARN boot-default "the root fs has its own /boot/extlinux; edits there are not read at boot"
    fi
    # ── 3. kernel-vs-rootfs ──
    if [ -n "$B" ]; then
        if [ -z "$dl" ] || [ ! -f "/$dl" ] || [ ! -f "$B$dl" ]; then
            report INFO kernel-vs-rootfs "DEFAULT image missing on one side (${dl:-none})"
        elif cmp -s "/$dl" "$B$dl"; then
            report PASS kernel-vs-rootfs "$dl is the same on the root fs and the boot partition"
        else
            report WARN kernel-vs-rootfs "$dl differs: root fs $(kver "/$dl" | cut -c15-60) / boot partition $(kver "$B$dl" | cut -c15-60)"
        fi
    else
        report INFO kernel-vs-rootfs "the root fs is the boot partition"
    fi
    # ── 8. apt-hold ── (reported here, needs the boot partition)
    if [ -n "$B" ]; then
        if apt-mark showhold 2>/dev/null | grep -qx nvidia-l4t-kernel; then
            report PASS apt-hold "nvidia-l4t-kernel is held"
        else
            report WARN apt-hold "nvidia-l4t-kernel is not held: apt writes the root fs /boot, not the boot partition"
        fi
    else
        report INFO apt-hold "the root fs is the boot partition"
    fi
else
    report INFO boot-default "boot partition unknown, skipped (also kernel-vs-rootfs, apt-hold)"
fi

# ── 4. modules ──
if [ -d "/lib/modules/$(uname -r)" ]; then
    report PASS modules "/lib/modules/$(uname -r)"
else
    report FAIL modules "/lib/modules/$(uname -r) missing"
fi

# ── 5. nic-binding ──
if ! command -v nmcli >/dev/null; then
    report INFO nic-binding "nmcli not found"
elif ! nmcli -t general status >/dev/null 2>&1; then
    report INFO nic-binding "NetworkManager not running"
else
    n_eth=$(nmcli -t -f DEVICE,TYPE dev 2>/dev/null | grep -c ':ethernet$'); n_eth=${n_eth:-0}
    by_name=0
    while IFS=: read -r uuid type; do
        [ "$type" = 802-3-ethernet ] || continue
        name=$(nmcli -e no -g connection.id con show uuid "$uuid")
        iface=$(nmcli -e no -g connection.interface-name con show uuid "$uuid")
        mac=$(nmcli -e no -g 802-3-ethernet.mac-address con show uuid "$uuid")
        report INFO nic-binding "'$name' interface=${iface:--} mac=${mac:--}"
        [ -n "$iface" ] && [ -z "$mac" ] && by_name=1
    done < <(nmcli -t -f UUID,TYPE con show 2>/dev/null)
    if [ "$by_name" = 1 ] && [ "$n_eth" -ge 2 ]; then
        report WARN nic-binding "$n_eth ethernet devices and a profile bound by name: eth0/eth1 can swap between boots; bind it by MAC"
    else
        report PASS nic-binding "$n_eth ethernet device(s); no name-bound profile at risk"
    fi
fi

# ── 6. recent-boots ──
boots=$(journalctl --list-boots --no-legend --no-pager 2>/dev/null | tail -n 6)
if [ -n "$boots" ] && [ "$(journalctl -q -b 0 -k -n 1 --no-pager 2>/dev/null | wc -l)" -gt 0 ]; then
    while read -r off _; do
        k=$(journalctl -q -b "$off" -k --no-pager 2>/dev/null | grep -m1 -o 'Linux version [^ ]*' | cut -d' ' -f3)
        report INFO recent-boots "boot $off: ${k:-no kernel log}"
    done <<< "$boots"
    report INFO recent-boots "order is by first timestamp; the board clock can jump between boots"
else
    report WARN recent-boots "no system journal access (run as root or add the user to group adm)"
fi

# ── 7. pstore ──
if [ ! -r /sys/fs/pstore ] || [ ! -x /sys/fs/pstore ]; then
    report INFO pstore "not readable (root only)"
elif [ -z "$(ls -A /sys/fs/pstore)" ]; then
    report PASS pstore "empty"
else
    report WARN pstore "dumps from an earlier boot: $(cd /sys/fs/pstore && ls -l --time-style=+%F_%T | awk 'NR>1{printf "%s(%s %s) ", $NF, $5, $6}')"
fi

# ── 9. rt ──
rt="realtime=$(cat /sys/kernel/realtime 2>/dev/null || echo 0) isolated=$(cat /sys/devices/system/cpu/isolated 2>/dev/null)"
report INFO rt "$rt power=$(nvpmodel -q 2>/dev/null | head -1 | sed 's/^NV Power Mode: //')"

exit "$rc"
