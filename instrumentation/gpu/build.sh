#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Build the dczc GPU sidecar demo. Requires CUDA (nvcc) + a built libdczc.
#
#   cmake -S . -B build && cmake --build build -j    # builds libdczc.a first
#   instrumentation/gpu/build.sh
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${REPO}/build/gpu_sidecar_demo"

LIB=""
for cand in "${REPO}/build/libdczc.a" "${REPO}/build-iox2/libdczc.a"; do
    [ -f "$cand" ] && LIB="$cand" && break
done
if [ -z "$LIB" ]; then
    echo "libdczc.a not found — run: cmake -S . -B build && cmake --build build -j" >&2
    exit 1
fi

build_one() {  # $1 = source basename (no .cu), $2 = output name
    nvcc -O2 -std=c++17 -Wno-deprecated-gpu-targets \
        -I "${REPO}/include" \
        "${REPO}/instrumentation/gpu/$1.cu" "$LIB" \
        -lcuda -lrt -lpthread \
        -o "${REPO}/build/$2"
    echo "built: ${REPO}/build/$2"
}

build_one gpu_sidecar_demo   gpu_sidecar_demo
build_one vlm_handoff_bench   vlm_handoff_bench
