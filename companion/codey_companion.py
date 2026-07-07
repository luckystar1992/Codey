#!/usr/bin/env python3
"""Codey Companion 入口 —— 纯本地 Agent 用量/会话监视服务(零联网)。

  GET  /codey/state   归一化的 Claude/Codex 账号额度 + 每会话信息(sessions[])
  POST /codey/asr     16k/16-bit/mono WAV -> { text }(本机 whisper)

数据全部读本地文件 / 进程(ps/lsof/git/transcript),不直连任何在线 API。
运行:  python3 codey_companion.py   (运行期间保持开启供手表拉取)
"""
import os
import threading
from http.server import ThreadingHTTPServer

import asr_stream
from codey.netinfo import lan_ip
from codey.server import App, make_handler

PORT = int(os.environ.get("CODEY_PORT") or 8787)


def maybe_fallback_engine():
    """启动期探云端 ASR(tencent/doubao):不可达 或 网络延时 > 阈值(默认 200ms)→ 本运行强制本地 sherpa。
    只在生效引擎是云端时才探(本就本地则跳过)。"""
    eng = asr_stream.effective_engine()
    if eng == "tencent":
        from codey import asr_tencent as cloud
        thresh = cloud.TENCENT_MAX_LATENCY_MS
    elif eng == "doubao":
        from codey import asr_doubao as cloud
        thresh = cloud.DOUBAO_MAX_LATENCY_MS
    else:
        return
    ok, lat, why = cloud.health_check()
    if not ok:
        asr_stream.set_engine_override("sherpa")
        print(f"[asr] {eng} 不可达({why})→ 回落本地 sherpa")
    elif lat > thresh:
        asr_stream.set_engine_override("sherpa")
        print(f"[asr] {eng} 延时 {lat:.0f}ms > {thresh:.0f}ms → 回落本地 sherpa")
    else:
        print(f"[asr] {eng} 可达,延时 {lat:.0f}ms → 用 {eng}")


def main():
    app = App()
    app.start_background()
    asr_stream.set_pid_resolver(app.pid_for_session)                      # 设备「切到此会话」→ session id→PID→终端 tab
    asr_stream.set_status_resolver(app.status_for_session)                # 语音流式同步:仅 waiting(空闲)会话才注入
    maybe_fallback_engine()                                               # 豆包不可达/延时高 → 本运行回落本地 sherpa
    threading.Thread(target=asr_stream.run_server, daemon=True).start()   # ASR WS :8788(同进程)
    print(f"Codey ASR       -> ws://{lan_ip()}:8788  (engine={asr_stream.effective_engine()})")
    from codey import usb_link
    threading.Thread(target=usb_link.run, args=(app, asr_stream.make_backend), daemon=True).start()
    print(f"Codey USB link  -> /dev/cu.usbmodem* (有线兜底,优先)")
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
