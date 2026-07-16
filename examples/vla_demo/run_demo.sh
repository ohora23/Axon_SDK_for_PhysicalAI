#!/usr/bin/env bash
# axon VLA demo runner — launches the vision producer and the LLM consumer as
# two separate processes and reports the consumer's verdict. The producer keeps
# publishing until the consumer finishes, then it is stopped.
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
PY="${VLA_PYTHON:-/home/jkyoo/.venvs/axon-vla/bin/python}"
AXON_SO_DIR="${AXON_SO_DIR:-$REPO/build-cuda/python}"
export PYTHONPATH="$AXON_SO_DIR:${PYTHONPATH:-}"

echo "== axon VLA demo (vision -> LLM zero-copy handoff) =="
echo "   python:     $PY"
echo "   axon module: $AXON_SO_DIR"
echo

"$PY" "$HERE/vision_producer.py" &
PROD=$!
trap 'kill "$PROD" 2>/dev/null' EXIT

"$PY" "$HERE/llm_consumer.py"
RC=$?

kill "$PROD" 2>/dev/null
wait "$PROD" 2>/dev/null

echo
echo "== consumer exit code: $RC =="
exit "$RC"
