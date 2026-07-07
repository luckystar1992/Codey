#!/usr/bin/env python3
"""Codey Kindle 服务 —— 只跑「会话采集 + HTTP」的轻量入口,供 Kindle/浏览器看监视页。

与完整 companion 用同一份数据源(App/collect),但不启动 whisper / ASR WebSocket / USB link /
ngrok,纯 Python 标准库、零第三方依赖、无需 sherpa 模型。适合只想看 /kindle 监视页的场景。

  GET /kindle        e-ink 监视页(自带 meta refresh 自动刷新)
  GET /codey/state   归一化用量+会话 JSON(/kindle 内部数据源)
  GET /admin         Web 管理台(语音相关功能在本模式下不可用)

运行:  python3 codey_kindle.py   (或 ./deploy_kindle.sh)
"""
import os
from http.server import ThreadingHTTPServer

from codey.netinfo import lan_ip
from codey.server import App, make_handler

PORT = int(os.environ.get("CODEY_PORT") or 8787)


def main():
    app = App()
    app.start_collectors()                  # 只采集会话,不起 whisper/ASR/USB/ngrok
    httpd = ThreadingHTTPServer(("0.0.0.0", PORT), make_handler(app))
    ip = lan_ip()
    print(f"Codey Kindle    -> http://{ip}:{PORT}/kindle  (port {PORT})")
    print(f"                   http://{ip}:{PORT}/codey/state | http://{ip}:{PORT}/admin")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        httpd.server_close()


if __name__ == "__main__":
    main()
