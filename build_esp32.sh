#!/bin/bash
# Build build/esp32/: everything that goes on the ESP32 board, with every
# module precompiled to .mpy (saves the RAM that compiling on the board
# would need).  main.py stays as source, since only main.py auto-runs.
# Upload the contents of build/esp32/ to the board's root.
#
# Needs mpy-cross matching the firmware's .mpy version (pip install
# mpy-cross); a mismatch shows up on the board as
# "ValueError: incompatible .mpy file".

set -euo pipefail
cd "$(dirname "$0")"

OUT=build/esp32
MODULES="pins.py display.py ssd1306.py settings.py wifi.py web.py
         microdot/__init__.py microdot/microdot.py microdot/helpers.py"

command -v mpy-cross >/dev/null || { echo "mpy-cross not found"; exit 1; }
echo "Using $(mpy-cross --version)"

rm -rf "$OUT"
mkdir -p "$OUT/microdot"

for src in $MODULES; do
    mpy-cross -o "$OUT/${src%.py}.mpy" "$src"
done
cp main.py "$OUT/"

# Credentials are gitignored and so is build/; copy them as source so they
# stay editable on the board.
if [ -f wifi_credentials.py ]; then
    cp wifi_credentials.py "$OUT/"
else
    echo "Note: no wifi_credentials.py; the board will use saved settings or the setup AP"
fi

echo
echo "Contents of $OUT:"
(cd "$OUT" && find . -type f | sort | sed 's|^\./|  |')
