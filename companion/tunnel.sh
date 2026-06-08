#!/usr/bin/env bash
# Codey ngrok 隧道启动器 —— 同时暴露状态 API(:8787)与 ASR WS(:8788)到公网。
# Companion 服务本身由 deploy.sh 负责;本脚本只起 ngrok。
# 用法:
#   ./deploy.sh start --bg   先后台拉起 Companion
#   ./tunnel.sh              再起 ngrok 双隧道(前台;Ctrl-C 退出)
# 依赖:ngrok(brew install ngrok/ngrok/ngrok)+ ngrok.yml(从 ngrok.yml.example 复制填值)。
# 凭据从 .env 注入(NGROK_AUTHTOKEN / NGROK_DOMAIN / NGROK_BASIC_AUTH 供 ngrok.yml 的 ${} 展开)。
set -euo pipefail
cd "$(dirname "$0")"

log() { printf '\033[36m[tunnel]\033[0m %s\n' "$*"; }
die() { printf '\033[31m[tunnel] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }

command -v ngrok >/dev/null 2>&1 \
  || die "未找到 ngrok,请先安装:brew install ngrok/ngrok/ngrok"
[ -f ngrok.yml ] \
  || die "缺少 ngrok.yml(从 ngrok.yml.example 复制并填值)"

if [ -f .env ]; then            # 注入 NGROK_* 供 ngrok.yml 的 ${} 展开
  set -a
  . ./.env
  set +a
fi

log "起 ngrok 双隧道;本地面板 http://127.0.0.1:4040"
exec ngrok start --all --config ngrok.yml
