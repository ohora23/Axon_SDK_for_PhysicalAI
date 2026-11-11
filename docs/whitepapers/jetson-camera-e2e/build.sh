#!/bin/bash
# Rebuild the figures, numbers.json and the PDF of this white paper (needs python3 + matplotlib
# and Google Chrome / Chromium for the headless print).
set -eu
cd "$(dirname "$0")"
python3 make_figures.py > /dev/null
CHROME=$(command -v google-chrome || command -v chromium || command -v chromium-browser) || { echo "need google-chrome or chromium" >&2; exit 1; }
"$CHROME" --headless=new --disable-gpu --no-pdf-header-footer \
  --print-to-pdf=../axon-jetson-camera-inference-e2e.pdf "file://$PWD/whitepaper.html"
echo "../axon-jetson-camera-inference-e2e.pdf"
