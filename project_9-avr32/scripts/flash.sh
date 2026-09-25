#!/usr/bin/env bash
# Flash a .hex onto the UC3-A3 Xplained over USB DFU.
# Run this in a normal macOS Terminal (NOT inside the container):
#   brew install dfu-programmer
#   ./scripts/flash.sh path/to/program.hex
# Put the board in DFU mode first (hold the bootloader button while plugging in USB;
# see the UC3-A3 Xplained user guide for which button).
set -euo pipefail

HEX="${1:?usage: $0 file.hex}"
PART=at32uc3a3256

command -v dfu-programmer >/dev/null || { echo "Install first: brew install dfu-programmer"; exit 1; }
[ -f "$HEX" ] || { echo "No such file: $HEX"; exit 1; }

echo "Erasing..."
dfu-programmer "$PART" erase --force 2>/dev/null || dfu-programmer "$PART" erase

echo "Flashing $HEX (bootloader area left untouched)..."
dfu-programmer "$PART" flash --suppress-bootloader-mem "$HEX"

echo "Starting program..."
# launch resets the chip, which drops the USB connection; dfu-programmer can then
# hang waiting for a reply that never comes. The program starts anyway (the flash
# was already validated above), so give it a few seconds and then stop waiting.
dfu-programmer "$PART" launch >/dev/null 2>&1 &
LAUNCH_PID=$!
for _ in 1 2 3 4 5; do
    kill -0 "$LAUNCH_PID" 2>/dev/null || break
    sleep 1
done
kill "$LAUNCH_PID" 2>/dev/null || true
wait "$LAUNCH_PID" 2>/dev/null || true
echo "Done. If the program isn't running, unplug and replug the board (without holding the button)."
