#!/bin/bash
# Cross-build an L4T r35.6.4 (AGX Xavier, JetPack 5.1.6) PREEMPT_RT kernel on an x86 host.
# NVIDIA ships no prebuilt RT kernel for r35 (its rt-kernel apt repo covers r36 / Orin only),
# so this applies the rt-patches bundled in the public sources.
#
# usage: build-rt.sh <workdir> <board-config>
#   <board-config>: the board's running config, e.g.  ssh board 'zcat /proc/config.gz' > config
# host needs: flex bison libelf-dev bc libssl-dev
# output: <workdir>/out/arch/arm64/boot/Image, <workdir>/stage/lib/modules/5.10.216-rt*-tegra
set -euo pipefail
W=$(mkdir -p "$1" && cd "$1" && pwd)
CFG=$(realpath "$2")
L4T=https://developer.download.nvidia.com/embedded/L4T/r35_Release_v6.4/sources/public_sources.tbz2
TC=https://developer.nvidia.com/embedded/jetson-linux/bootlin-toolchain-gcc-93
cd "$W"

if [ ! -d src/kernel/kernel-5.10 ]; then
    curl -fsSL -o public_sources.tbz2 "$L4T"
    tar xjf public_sources.tbz2 Linux_for_Tegra/source/public/kernel_src.tbz2
    mkdir -p src && tar xjf Linux_for_Tegra/source/public/kernel_src.tbz2 -C src
fi
if [ ! -x toolchain/bin/aarch64-buildroot-linux-gnu-gcc ]; then
    curl -fsSL -o toolchain.tar.gz "$TC"
    mkdir -p toolchain && tar xzf toolchain.tar.gz -C toolchain
fi

K=$W/src/kernel/kernel-5.10
export CROSS_COMPILE=$W/toolchain/bin/aarch64-buildroot-linux-gnu-
M=(make -C "$K" ARCH=arm64 LOCALVERSION=-tegra O="$W/out")

if [ ! -f "$K/arch/arm64/configs/.orig.defconfig" ]; then
    (cd "$K/scripts" && ./rt-patch.sh apply-patches)
fi
mkdir -p out && cp "$CFG" out/.config
# Same switches rt-patch.sh flips on the defconfig, plus RT_GROUP_SCHED off: with it on,
# user sessions get cpu.rt_runtime_us = 0 and SCHED_FIFO fails with EPERM.
"$K/scripts/config" --file out/.config \
    --enable PREEMPT_RT --disable PREEMPT --disable DEBUG_PREEMPT --disable KVM \
    --disable CPU_IDLE_TEGRA18X --disable CPU_FREQ_GOV_INTERACTIVE \
    --disable CPU_FREQ_TIMES --disable FAIR_GROUP_SCHED --disable RT_GROUP_SCHED
"${M[@]}" olddefconfig
"${M[@]}" -j"$(nproc)" Image modules
rm -rf stage && "${M[@]}" INSTALL_MOD_PATH="$W/stage" INSTALL_MOD_STRIP=1 modules_install
rm -f stage/lib/modules/*/build stage/lib/modules/*/source
echo "built $("${M[@]}" -s kernelrelease)"
