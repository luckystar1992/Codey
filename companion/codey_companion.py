#!/usr/bin/env python3
"""Codey Companion 入口 —— 纯本地 Agent 用量/会话监视服务(零联网)。

  GET  /codey/state   归一化的 Claude/Codex 账号额度 + 每会话信息(sessions[])
  POST /codey/asr     16k/16-bit/mono WAV -> { text }(本机 whisper)

数据全部读本地文件 / 进程(ps/lsof/git/transcript),不直连任何在线 API。
运行:  python3 codey_companion.py   (运行期间保持开启供手表拉取)
"""
import os
import socket
import threading
from http.server import ThreadingHTTPServer

import asr_stream
from codey import envcfg
from codey.server import App, make_handler

PORT = int(os.environ.get("CODEY_PORT") or 8787)


def lan_ip():
    """本机 LAN IP(仅做主机名解析,不对外发包)。"""
    try:
        for ip in socket.gethostbyname_ex(socket.gethostname())[2]:
            if not ip.startswith("127."):
                return ip
    except Exception:
        pass
    return "127.0.0.1"


def main():
    app = App()
    app.start_background()
    asr_stream.set_pid_resolver(app.pid_for_session)                      # 设备「切到此会话」→ session id→PID→终端 tab
    threading.Thread(target=asr_stream.run_server, daemon=True).start()   # ASR WS :8788(同进程)
    print(f"Codey ASR       -> ws://{lan_ip()}:8788  (engine={envcfg.select_engine()})")
    httpd = ThreadingHTTPServer(("0.0.0.0", PORT), make_handler(app))
    print(f"Codey companion -> http://{lan_ip()}:{PORT}/codey/state  (port {PORT})")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        app.whisper.stop()
        httpd.server_close()


if __name__ == "__main__":
    main()
