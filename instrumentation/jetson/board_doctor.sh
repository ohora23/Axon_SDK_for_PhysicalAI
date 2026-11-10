#!/usr/bin/env bash
# board_doctor.sh — Read-only Jetson board health check (L4T r35, extlinux boot)
#
# Checks:
#   1 boot-source       Running kernel vs every ext4 candidate's boot/Image
#   2 boot-default      DEFAULT label and LINUX path on the boot partition
#   3 kernel-vs-rootfs  /boot/Image on root fs vs boot partition
#   4 modules            /lib/modules/<running kernel> exists
#   5 nic-binding       NM ethernet connections bound by MAC, not name
#   6 recent-boots       Last 6 boot IDs with kernel version and timestamp
#   7 pstore             /sys/fs/pstore files
#   8 apt-hold           nvidia-l4t-kernel held when root fs ≠ boot partition
#   9 rt                 Real-time / isolation / nvpmodel hints
#
# Run as root for the boot-partition checks (it mounts candidates read-only);
# as a user in group adm the journal checks work.
# Usage: sudo instrumentation/jetson/board_doctor.sh

set -u

# ── helpers ────────────────────────────────────────────────────────────────
is_root=0
if [ "$(id -u)" = "0" ]; then
    is_root=1
fi

exit_code=0
trap 'cleanup' EXIT

TMPMNTS=()

cleanup() {
    for mp in "${TMPMNTS[@]+"${TMPMNTS[@]}"}"; do
        if mountpoint -q "$mp" 2>/dev/null; then
            umount "$mp" 2>/dev/null || true
        fi
        rmdir "$mp" 2>/dev/null || true
    done
}

report() {
    echo "$1 $2: $3"
    if [ "$1" = "FAIL" ]; then
        exit_code=1
    fi
}

# ── 1. boot-source ─────────────────────────────────────────────────────────
RUN="$(uname -v)"

# Collect candidates: / plus every ext4 partition not mounted at /
declare -a CAND_DEVICES=()
declare -a CAND_PARTITIONS=()
declare -a CAND_MOUNTS=()

add_candidate() {
    local dev="$1"
    local part="$2"
    local mp="$3"
    CAND_DEVICES+=("$dev")
    CAND_PARTITIONS+=("$part")
    CAND_MOUNTS+=("$mp")
}

add_candidate "/" "" "/"

if [ "$is_root" = "1" ]; then
    while read -r part fstype mpoint; do
        if [ "$fstype" = "ext4" ]; then
            local_mp="$(mktemp -d)"
            mount -o ro "$part" "$local_mp" 2>/dev/null && {
                add_candidate "$part" "$part" "$local_mp"
                TMPMNTS+=("$local_mp")
            } || {
                rmdir "$local_mp" 2>/dev/null || true
            }
        fi
    done < <(lsblk -rpno NAME,FSTYPE,MOUNTPOINT 2>/dev/null | awk '$2 == "ext4" && $3 != "/" {print $1, $2, $3}')
fi

match_count=0
default_hits=0
boot_mp=""
boot_dev=""
boot_conf=""
boot_default=""
boot_matched=0

for i in "${!CAND_DEVICES[@]}"; do
    dev="${CAND_DEVICES[$i]}"
    mp="${CAND_MOUNTS[$i]}"

    img_path=""
    conf_path=""

    if [ "$dev" = "/" ]; then
        img_path="/boot/Image"
        conf_path="/boot/extlinux/extlinux.conf"
    else
        img_path="$mp/boot/Image"
        conf_path="$mp/boot/extlinux/extlinux.conf"
    fi

    label="(none)"
    if [ -f "$conf_path" ]; then
        conf_path="$(readlink -f "$conf_path" 2>/dev/null || echo "$conf_path")"
        label="$(awk '$1=="DEFAULT"{print $2; exit}' "$conf_path" 2>/dev/null)"
    fi

    # Every kernel image on the partition (Image, Image.rt, Image.bak-*), not only boot/Image:
    # the running kernel may be any extlinux entry.
    default_runs="no"
    if [ -f "$conf_path" ] && [ -n "$label" ]; then
        dl="$(awk -v l="$label" '$1=="LABEL"{sel=($2==l)} sel && $1=="LINUX"{print $2; exit}' "$conf_path" 2>/dev/null)"
        if [ -n "$dl" ]; then
            [ "$dev" = "/" ] && dimg="$dl" || dimg="$mp$dl"
            [ -f "$dimg" ] && strings "$dimg" 2>/dev/null | grep -m1 'Linux version' | grep -Fq "$RUN" && default_runs="yes"
        fi
    fi
    matched="no"
    for img in "${img_path%/Image}"/Image*; do
        case "$img" in *.sig) continue ;; esac
        [ -f "$img" ] || continue
        ver_line="$(strings "$img" 2>/dev/null | grep -m1 'Linux version' || true)"
        if echo "$ver_line" | grep -Fq "$RUN"; then
            matched="yes (${img##*/})"
            match_count=$((match_count + 1))
            # several partitions can hold the same image; prefer the one whose DEFAULT boots it
            if [ "$boot_matched" = 0 ] || [ "$default_runs" = yes ]; then
                boot_dev="$dev"; boot_conf="$conf_path"; boot_default="$label"; boot_mp="$mp"
                [ "$default_runs" = yes ] && default_hits=$((default_hits + 1))
            fi
            boot_matched=1
            break
        fi
    done

    if [ "$dev" = "/" ]; then
        dev_display="root fs (/)"
    else
        dev_display="$dev"
    fi

    report "INFO" "boot-source-candidate" "$dev_display DEFAULT=$label image_matches=$matched default_is_running=$default_runs"
done

if [ "$match_count" -eq 1 ]; then
    report "PASS" "boot-source" "boot partition is $boot_dev (running kernel found)"
elif [ "$match_count" -eq 0 ]; then
    if [ "$is_root" = "1" ]; then
        report "FAIL" "boot-source" "running kernel not found in any /boot/Image*: the firmware boots something this script cannot see, or the image changed since boot"
    else
        report "WARN" "boot-source" "running kernel not found in any /boot/Image (not root — consider sudo)"
    fi
elif [ "$default_hits" -eq 1 ]; then
    report "PASS" "boot-source" "boot partition is $boot_dev ($match_count partitions hold the running kernel; only its DEFAULT entry boots it)"
else
    report "WARN" "boot-source" "$match_count candidates hold the running kernel and $default_hits of them boot it by DEFAULT: ambiguous"
fi

# ── 2. boot-default ────────────────────────────────────────────────────────
if [ -n "$boot_conf" ] && [ -f "$boot_conf" ]; then
    linux_path="$(awk -v l="$boot_default" '$1=="LABEL"{sel=($2==l)} sel && $1=="LINUX"{print $2; exit}' "$boot_conf" 2>/dev/null || true)"

    if [ -z "$linux_path" ]; then
        linux_path="(not found)"
    fi

    report "INFO" "boot-default" "DEFAULT=$boot_default LINUX=$linux_path"

    # Check if DEFAULT entry's image differs from the one that matched
    if [ -f "$boot_conf" ] && [ -n "$boot_default" ] && [ "$boot_default" != "(none)" ]; then
        if [ -n "$linux_path" ] && [ "$linux_path" != "(not found)" ]; then
            if [ -n "$boot_mp" ]; then
                matched_image="$boot_mp$linux_path"
            else
                matched_image="$linux_path"
            fi
            if [ -f "$matched_image" ]; then
                default_ver="$(strings "$matched_image" 2>/dev/null | grep -m1 'Linux version' || true)"
                if ! echo "$default_ver" | grep -Fq "$RUN"; then
                    report "WARN" "boot-default" "DEFAULT entry image differs from running kernel (next boot will run a different kernel)"
                fi
            fi
        fi
    fi
else
    report "INFO" "boot-default" "no extlinux.conf on boot partition"
fi

# Warn if root fs has its own boot and is not the boot partition
if [ "$boot_dev" != "/" ] && [ -f "/boot/extlinux/extlinux.conf" ]; then
    report "WARN" "boot-default" "root fs has its own boot/extlinux — edits to /boot/extlinux on the root fs are not read at boot"
fi

# ── 3. kernel-vs-rootfs ────────────────────────────────────────────────────
if [ "$boot_dev" != "/" ]; then
    root_img="/boot/Image"
    if [ -f "$root_img" ] && [ -n "$boot_conf" ] && [ -f "$boot_conf" ]; then
        # Resolve boot partition mount
        boot_img=""
        for ci in "${!CAND_DEVICES[@]}"; do
            if [ "${CAND_DEVICES[$ci]}" = "$boot_dev" ]; then
                boot_img="${CAND_MOUNTS[$ci]}/boot/Image"
                break
            fi
        done
        if [ -n "$boot_img" ] && [ -f "$boot_img" ]; then
            if cmp -s "$root_img" "$boot_img"; then
                report "PASS" "kernel-vs-rootfs" "boot/Image on root fs matches boot partition"
            else
                report "WARN" "kernel-vs-rootfs" "apt updated /boot on the root fs only; the booted kernel is older than /lib/modules"
            fi
        fi
    fi
else
    report "INFO" "kernel-vs-rootfs" "root fs is the boot partition — skip"
fi

# ── 4. modules ─────────────────────────────────────────────────────────────
if [ -d "/lib/modules/$(uname -r)" ]; then
    report "PASS" "modules" "/lib/modules/$(uname -r) exists"
else
    report "FAIL" "modules" "/lib/modules/$(uname -r) missing"
fi

# ── 5. nic-binding ─────────────────────────────────────────────────────────
if ! command -v nmcli >/dev/null 2>&1; then
    report "INFO" "nic-binding" "nmcli not found — skipping"
else
    eth_count="$(nmcli -t -f DEVICE,TYPE dev 2>/dev/null | grep -c ':ethernet$' || echo 0)"
    if [ "$eth_count" -lt 2 ]; then
        report "PASS" "nic-binding" "$eth_count ethernet device(s) found (need 2+ to check)"
    else
        found_name_only=0
        while IFS=: read -r cn ct; do
            [ "$ct" = "802-3-ethernet" ] || continue
            iface="$(nmcli -g connection.interface-name con show "$cn" 2>/dev/null || true)"
            mac="$(nmcli -g 802-3-ethernet.mac-address con show "$cn" 2>/dev/null || true)"
            report "INFO" "nic-binding" "$cn iface=$iface mac=$mac"
            if [ -n "$iface" ] && [ -z "$mac" ]; then
                found_name_only=1
            fi
        done < <(nmcli -t -f NAME,TYPE con show 2>/dev/null)

        if [ "$found_name_only" -eq 1 ]; then
            report "WARN" "nic-binding" "bound by name: eth0/eth1 can swap between boots; bind by MAC"
        else
            report "PASS" "nic-binding" "all ethernet connections have MAC addresses or no interface-name constraint"
        fi
    fi
fi

# ── 6. recent-boots ────────────────────────────────────────────────────────
boot_ids="$(journalctl -F _BOOT_ID 2>/dev/null || true)"
if [ -n "$boot_ids" ]; then
    count=0
    while IFS= read -r bid; do
        if [ "$count" -ge 6 ]; then
            break
        fi
        first_line="$(journalctl -q -b "$bid" -k -o short-iso --no-pager 2>/dev/null | grep -m1 'Linux version' || true)"
        if [ -n "$first_line" ]; then
            kernel_token="$(echo "$first_line" | awk '{for(i=1;i<=NF;i++){if($i ~ /^Linux$/ && $(i+1) == "version"){print $(i+2);exit}}}' || true)"
            ts="$(echo "$first_line" | awk '{print $1, $2}' || true)"
            report "INFO" "recent-boots" "$bid $kernel_token $ts (clock may jump between boots; order not reliable)"
        else
            report "INFO" "recent-boots" "$bid (no kernel message found)"
        fi
        count=$((count + 1))
    done <<< "$boot_ids"
else
    report "WARN" "recent-boots" "no system journal access (run as root or add the user to group adm)"
fi

# ── 7. pstore ──────────────────────────────────────────────────────────────
if [ -d "/sys/fs/pstore" ] && [ "$(ls -A /sys/fs/pstore 2>/dev/null)" ]; then
    pstore_detail=""
    for f in /sys/fs/pstore/*; do
        if [ -e "$f" ]; then
            fname="$(basename "$f")"
            fsize="$(stat -c%s "$f" 2>/dev/null || echo '?')"
            fmtime="$(stat -c%y "$f" 2>/dev/null | cut -d. -f1 || echo '?')"
            pstore_detail="${pstore_detail}${fname}(${fsize},${fmtime} ) "
        fi
    done
    report "INFO" "pstore" "$pstore_detail"
else
    report "INFO" "pstore" "pstore not readable (root only)"
fi

# ── 8. apt-hold ────────────────────────────────────────────────────────────
if [ "$boot_dev" != "/" ]; then
    held="$(apt-mark showhold 2>/dev/null | grep 'nvidia-l4t-kernel' || true)"
    if [ -z "$held" ]; then
        report "WARN" "apt-hold" "apt kernel upgrades write the root fs /boot, not the boot partition"
    else
        report "PASS" "apt-hold" "nvidia-l4t-kernel is held"
    fi
else
    report "INFO" "apt-hold" "root fs is the boot partition — skip"
fi

# ── 9. rt ──────────────────────────────────────────────────────────────────
rt_detail=""
if [ -f "/sys/kernel/realtime" ]; then
    rt_val="$(cat /sys/kernel/realtime 2>/dev/null || echo '?')"
    rt_detail="realtime=$rt_val "
fi
if [ -f "/sys/devices/system/cpu/isolated" ]; then
    iso_val="$(cat /sys/devices/system/cpu/isolated 2>/dev/null || echo '?')"
    rt_detail="${rt_detail}isolated=$iso_val "
fi
nvp="$(nvpmodel -q 2>/dev/null | head -1 || true)"
if [ -n "$nvp" ]; then
    rt_detail="${rt_detail}nvpmodel=$nvp"
fi
report "INFO" "rt" "${rt_detail:-no real-time indicators found}"

exit "$exit_code"
