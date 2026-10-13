#!/usr/bin/env bash
# Build the Jetson NvBufSurface probe on the board (JetPack 5 / L4T r35).
set -euo pipefail
cd "$(dirname "$0")"
MMAPI=/usr/src/jetson_multimedia_api
g++ -O2 -std=c++17 -Wall nvbuf_probe.cpp -o nvbuf_probe \
    -I"$MMAPI/include" -I/usr/local/cuda/include \
    -L/usr/lib/aarch64-linux-gnu/tegra -L/usr/local/cuda/lib64 \
    -Wl,-rpath,/usr/lib/aarch64-linux-gnu/tegra \
    -lnvbufsurface -lcuda -lEGL
echo "built: $(pwd)/nvbuf_probe"
