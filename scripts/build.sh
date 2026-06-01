#!/usr/bin/env bash
# Compile a sketch for the M5Stack StopWatch (C152, ESP32-S3).
# Usage:  ./scripts/build.sh [sketch_dir]      (default: sketches/hello_stopwatch)
set -euo pipefail
cd "$(dirname "$0")/.."

FQBN="m5stack:esp32:m5stack_stopwatch"
SKETCH="${1:-sketches/hello_stopwatch}"

echo "Compiling $SKETCH  (FQBN=$FQBN)"
arduino-cli compile --fqbn "$FQBN" --libraries libs "$SKETCH"
