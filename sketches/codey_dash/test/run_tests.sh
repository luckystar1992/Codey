#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
g++ -std=c++17 -Wall -Wextra -o /tmp/codey_ui_test codey_ui_test.cpp
# 20s 硬超时(perl alarm,macOS 自带):即使将来某测试卡死也会被杀,不会留成占 CPU 的孤儿进程。
# 注:不要用 -fsanitize=address —— Apple clang 的 ASan 运行时在本机 macOS(Darwin 25)启动即自旋挂死。
perl -e 'alarm 20; exec @ARGV or exit 127' /tmp/codey_ui_test
