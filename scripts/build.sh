#!/usr/bin/env bash
# Compile a sketch for the M5Stack StopWatch (C152, ESP32-S3).
# Usage:  ./scripts/build.sh [sketch_dir]      (default: sketches/hello_stopwatch)
set -euo pipefail
cd "$(dirname "$0")/.."

FQBN="m5stack:esp32:m5stack_stopwatch"
SKETCH="${1:-sketches/hello_stopwatch}"

FLASH_BUDGET="${FLASH_BUDGET:-90}"   # flash 占用上限(%);超出视为构建失败

echo "Compiling $SKETCH  (FQBN=$FQBN)"
if ! out=$(arduino-cli compile --fqbn "$FQBN" --libraries libs "$SKETCH" 2>&1); then
  printf '%s\n' "$out"; exit 1
fi
printf '%s\n' "$out"

# flash 预算门禁:解析 "Sketch uses N bytes (XX%) of program storage"
pct=$(printf '%s\n' "$out" | sed -nE 's/.*\(([0-9]+)%\) of program storage.*/\1/p' | head -1)
if [ -n "$pct" ]; then
  echo "Flash usage: ${pct}% (budget ${FLASH_BUDGET}%)"
  if [ "$pct" -gt "$FLASH_BUDGET" ]; then
    echo "ERROR: flash ${pct}% exceeds ${FLASH_BUDGET}% budget — shrink fonts/assets"; exit 2
  fi
fi
