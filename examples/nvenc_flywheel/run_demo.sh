#!/usr/bin/env bash
# axon NVENC data-flywheel demo runner — launches the frame producer and the
# NVENC recorder as two processes and reports the recorder's verdict. The
# producer publishes until the recorder has captured its frames, then is stopped.
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
PY="${VLA_PYTHON:-/home/jkyoo/.venvs/axon-vla/bin/python}"
AXON_SO_DIR="${AXON_SO_DIR:-$REPO/build-cuda/python}"
export PYTHONPATH="$AXON_SO_DIR:${PYTHONPATH:-}"
export VLA_OUT="${VLA_OUT:-/tmp/axon_flywheel.h264}"

echo "== axon NVENC data-flywheel demo (record from the shared GPU buffer) =="
echo "   python:      $PY"
echo "   axon module: $AXON_SO_DIR"
echo "   output:      $VLA_OUT"
echo

"$PY" "$HERE/frame_producer.py" &
PROD=$!
trap 'kill "$PROD" 2>/dev/null' EXIT

"$PY" "$HERE/nvenc_recorder.py"
RC=$?

kill "$PROD" 2>/dev/null
wait "$PROD" 2>/dev/null

echo
echo "== recorder exit code: $RC =="
exit "$RC"
