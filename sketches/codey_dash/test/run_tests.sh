#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
g++ -std=c++17 -Wall -Wextra -o /tmp/codey_ui_test codey_ui_test.cpp
/tmp/codey_ui_test
