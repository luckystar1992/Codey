#!/usr/bin/env bash
# Codey Mac 伴随服务启动器 —— 同一进程起 HTTP(:8787 状态/Web 管理台)+ ASR WS(:8788)。
# 用法:
#   ./deploy.sh [start]        前台启动(Ctrl-C 退出,日志直出)
#   ./deploy.sh start --bg     后台启动(nohup;日志 data/companion.log,PID data/companion.pid)
#   ./deploy.sh stop           停止后台服务
#   ./deploy.sh restart        重启(后台)
#   ./deploy.sh status         查看端口/进程状态
# 可选环境变量:CODEY_PORT(默认 8787) CODEY_ASR_PORT(默认 8788) PYTHON(默认 python3)
set -euo pipefail
cd "$(dirname "$0")"

PY="${PYTHON:-python3}"
HTTP_PORT="${CODEY_PORT:-8787}"
ASR_PORT="${CODEY_ASR_PORT:-8788}"
ENTRY="codey_companion.py"
DATA_DIR="data"
PID_FILE="$DATA_DIR/companion.pid"
LOG_FILE="$DATA_DIR/companion.log"
LOCAL_NOPROXY="127.0.0.1,localhost,::1"   # 本地回环(含 whisper-server)不走系统代理

log()  { printf '\033[36m[deploy]\033[0m %s\n' "$*"; }
warn() { printf '\033[33m[deploy] WARN:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[31m[deploy] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }

pids_on_port() { lsof -nP -iTCP:"$1" -sTCP:LISTEN -t 2>/dev/null || true; }

preflight() {
  command -v "$PY" >/dev/null 2>&1 || die "找不到 Python:$PY(设环境变量 PYTHON 指定解释器)"
  "$PY" - <<'PYEOF' || die "缺少 Python 依赖,请先安装:$PY -m pip install numpy sherpa-onnx websockets"
import importlib.util as u, sys
miss = [m for m in ("numpy", "sherpa_onnx", "websockets") if u.find_spec(m) is None]
sys.exit(1 if miss else 0)
PYEOF
  ls -d models/*streaming-zipformer*/ >/dev/null 2>&1 \
    || die "未找到 sherpa 流式模型(models/*streaming-zipformer*/),无法启动 ASR"
  [ -x "${CODEY_WHISPER_SERVER:-/opt/homebrew/bin/whisper-server}" ] \
    || warn "whisper-server 缺失,/codey/asr 批处理转写不可用(流式 ASR 不受影响)"
  if [ ! -f .env ] && [ -f .env.example ]; then
    cp .env.example .env
    log "已从 .env.example 生成 .env(用豆包云端时再填凭据)"
  fi
}

ensure_ports_free() {
  local p busy
  for p in "$HTTP_PORT" "$ASR_PORT"; do
    busy="$(pids_on_port "$p")"
    if [ -n "$busy" ]; then
      die "端口 $p 已被占用(PID:$busy)。先执行 ./deploy.sh stop 或 restart"
    fi
  done
  return 0   # 末次 [ -n ] 为假会带出非零状态,set -e 下需显式归零
}

cmd_start() {
  preflight
  ensure_ports_free
  if [ "${1:-}" = "--bg" ]; then
    mkdir -p "$DATA_DIR"
    no_proxy="$LOCAL_NOPROXY" NO_PROXY="$LOCAL_NOPROXY" nohup "$PY" "$ENTRY" >>"$LOG_FILE" 2>&1 &
    echo "$!" > "$PID_FILE"
    sleep 1
    kill -0 "$(cat "$PID_FILE")" 2>/dev/null || die "启动失败,见日志:$LOG_FILE"
    log "后台启动 PID=$(cat "$PID_FILE") | 日志 $LOG_FILE"
    log "管理台 http://localhost:$HTTP_PORT/ | 状态 /codey/state | ASR ws://localhost:$ASR_PORT"
  else
    log "前台启动(Ctrl-C 退出)…  管理台 http://localhost:$HTTP_PORT/"
    exec env no_proxy="$LOCAL_NOPROXY" NO_PROXY="$LOCAL_NOPROXY" "$PY" "$ENTRY"
  fi
}

cmd_stop() {
  local stopped=0 pid p busy
  if [ -f "$PID_FILE" ]; then
    pid="$(cat "$PID_FILE")"
    if kill -0 "$pid" 2>/dev/null; then kill "$pid" 2>/dev/null && { stopped=1; log "已停止 PID=$pid"; }; fi
    rm -f "$PID_FILE"
  fi
  for p in "$HTTP_PORT" "$ASR_PORT"; do          # 兜底:回收仍占用端口的残留进程
    busy="$(pids_on_port "$p")"
    [ -n "$busy" ] && kill $busy 2>/dev/null && { stopped=1; log "已停止端口 $p 上的进程:$busy"; }
  done
  [ "$stopped" = 1 ] || log "没有正在运行的服务"
}

cmd_status() {
  local p busy
  for p in "$HTTP_PORT" "$ASR_PORT"; do
    busy="$(pids_on_port "$p")"
    if [ -n "$busy" ]; then log "端口 $p:运行中(PID $busy)"; else log "端口 $p:未运行"; fi
  done
}

case "${1:-start}" in
  start)   shift || true; cmd_start "${1:-}" ;;
  --bg)    cmd_start --bg ;;
  stop)    cmd_stop ;;
  restart) cmd_stop; sleep 1; cmd_start --bg ;;
  status)  cmd_status ;;
  *)       die "未知命令:$1(可用:start | stop | restart | status)" ;;
esac
