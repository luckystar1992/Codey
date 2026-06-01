#!/usr/bin/env bash
# Serial monitor for the StopWatch @ 115200 baud.
# Usage:  ./scripts/monitor.sh [/dev/cu.xxx]
# With no argument it auto-detects the first USB serial port.
set -euo pipefail
cd "$(dirname "$0")/.."

CFG="--config-file ./arduino-cli.yaml"
PORT="${1:-$(arduino-cli $CFG board list | awk '/usbmodem|usbserial/{print $1; exit}')}"

if [ -z "${PORT:-}" ]; then
  echo "No serial port found."
  echo "Plug in the StopWatch and enter download mode (long-press reset ~2s until the green LED lights), then retry."
  exit 1
fi

echo "Monitoring $PORT @115200 (Ctrl-C to exit)"
exec arduino-cli $CFG monitor -p "$PORT" -c baudrate=115200
