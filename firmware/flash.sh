#!/bin/sh
# Compile and upload the glove firmware with the arduino-cli bundled in Arduino IDE 2.
#   firmware/flash.sh              # first /dev/cu.usbserial* port
#   firmware/flash.sh /dev/cu.usbserial-0001
# Close the Serial Monitor / calibrate.py / play.py first: only one program can hold the port.
set -e
cd "$(dirname "$0")"
CLI="/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"
[ -x "$CLI" ] || CLI=arduino-cli
PORT=${1:-$(ls /dev/cu.usbserial* 2>/dev/null | head -1)}
[ -n "$PORT" ] || { echo "no /dev/cu.usbserial* port found; is the ESP32 plugged in?"; exit 1; }
if lsof -t "$PORT" >/dev/null 2>&1; then
  echo "$PORT is in use by:"; lsof "$PORT" | awk 'NR>1 {print "  " $1 " (pid " $2 ")"}'
  echo "quit that program and run this again."; exit 1
fi
"$CLI" compile --fqbn esp32:esp32:esp32 --upload -p "$PORT" glove 2>&1 | grep -E 'Sketch uses|error|Hash of data verified|Hard resetting' | tail -3
echo "flashed $PORT"
