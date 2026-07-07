#!/usr/bin/env bash
# Codey Kindle 独立服务启动器 —— 只跑会话采集 + HTTP(:8787,含 /kindle 监视页)。
# 与完整 companion 用同一数据源,但不启动 ASR/whisper/USB/ngrok,纯标准库、零第三方依赖、无需 sherpa 模型。
# 用法:
#   ./deploy_kindle.sh [start]       前台启动(Ctrl-C 退出)
#   ./deploy_kindle.sh start --bg    后台启动(nohup;日志 data/kindle.log,PID data/kindle.pid)
#   ./deploy_kindle.sh stop          停止
#   ./deploy_kindle.sh restart       重启(后台)
#   ./deploy_kindle.sh status        查看状态
# 可选环境变量:CODEY_PORT(默认 8787) PYTHON(默认 python3)
# 注意:勿与完整 companion 同端口并行;二选一,或用 CODEY_PORT 换端口。
set -euo pipefail
cd "$(dirname "$0")"

PY="${PYTHON:-python3}"
HTTP_PORT="${CODEY_PORT:-8787}"
ENTRY="codey_kindle.py"
DATA_DIR="data"
PID_FILE="$DATA_DIR/kindle.pid"
LOG_FILE="$DATA_DIR/kindle.log"

log()  { printf '\033[36m[kindle]\033[0m %s\n' "$*"; }
die()  { printf '\033[31m[kindle] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }
pids_on_port() { lsof -nP -iTCP:"$1" -sTCP:LISTEN -t 2>/dev/null || true; }

preflight() {
  command -v "$PY" >/dev/null 2>&1 || die "找不到 Python:$PY(设环境变量 PYTHON 指定解释器)"
}

ensure_port_free() {
  local busy; busy="$(pids_on_port "$HTTP_PORT")"
  [ -z "$busy" ] || die "端口 $HTTP_PORT 已被占用(PID:$busy)。勿与完整 companion 同端口共存;先 stop,或用 CODEY_PORT 换端口。"
}

cmd_start() {
  preflight
  ensure_port_free
  if [ "${1:-}" = "--bg" ]; then
    mkdir -p "$DATA_DIR"
    nohup "$PY" "$ENTRY" >>"$LOG_FILE" 2>&1 &
    echo "$!" > "$PID_FILE"
    sleep 1
    kill -0 "$(cat "$PID_FILE")" 2>/dev/null || die "启动失败,见日志:$LOG_FILE"
    log "后台启动 PID=$(cat "$PID_FILE") | 日志 $LOG_FILE"
    log "Kindle 打开 http://localhost:$HTTP_PORT/kindle"
  else
    log "前台启动(Ctrl-C 退出)…  Kindle 打开 http://localhost:$HTTP_PORT/kindle"
    exec "$PY" "$ENTRY"
  fi
}

cmd_stop() {
  local stopped=0 pid busy
  if [ -f "$PID_FILE" ]; then
    pid="$(cat "$PID_FILE")"
    if kill -0 "$pid" 2>/dev/null; then kill "$pid" 2>/dev/null && { stopped=1; log "已停止 PID=$pid"; }; fi
    rm -f "$PID_FILE"
  fi
  busy="$(pids_on_port "$HTTP_PORT")"
  [ -n "$busy" ] && kill $busy 2>/dev/null && { stopped=1; log "已停止端口 $HTTP_PORT 上的进程:$busy"; }
  [ "$stopped" = 1 ] || log "没有正在运行的服务"
  return 0
}

cmd_status() {
  local busy; busy="$(pids_on_port "$HTTP_PORT")"
  if [ -n "$busy" ]; then log "端口 $HTTP_PORT:运行中(PID $busy)"; else log "端口 $HTTP_PORT:未运行"; fi
}

case "${1:-start}" in
  start)   shift || true; cmd_start "${1:-}" ;;
  --bg)    cmd_start --bg ;;
  stop)    cmd_stop ;;
  restart) cmd_stop; sleep 1; cmd_start --bg ;;
  status)  cmd_status ;;
  *)       die "未知命令:$1(可用:start | stop | restart | status)" ;;
esac
