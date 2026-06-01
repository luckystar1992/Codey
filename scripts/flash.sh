#!/usr/bin/env bash
# Compile and flash a sketch to the M5Stack StopWatch (C152, ESP32-S3).
# Usage:  ./scripts/flash.sh [sketch_dir] [port]
#   sketch_dir  defaults to sketches/hello_stopwatch
#   port        autodetects the first USB serial device if omitted
set -euo pipefail
cd "$(dirname "$0")/.."

FQBN="m5stack:esp32:m5stack_stopwatch"
SKETCH="${1:-sketches/hello_stopwatch}"
PORT="${2:-$(arduino-cli board list | awk '/usbmodem|usbserial/{print $1; exit}')}"

if [ -z "${PORT:-}" ]; then
  echo "No serial port found."
  echo "Plug in the StopWatch (shows up as /dev/cu.usbmodem*)."
  echo "If it still won't connect, enter download mode: long-press reset ~2s until the green LED lights."
  exit 1
fi

echo "Flashing $SKETCH -> $PORT  (FQBN=$FQBN)"
arduino-cli compile --fqbn "$FQBN" --libraries libs --upload --port "$PORT" "$SKETCH"
echo
echo "Flashed. Watch serial output with:  ./scripts/monitor.sh"
