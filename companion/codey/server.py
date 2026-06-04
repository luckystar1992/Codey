"""HTTP 服务:GET /codey/state(用量+会话) · POST /codey/asr(语音)。供手表 LAN 拉取。"""
import json
import threading
import time
from http.server import BaseHTTPRequestHandler

from . import collect
from .asr import WhisperManager
from .state import build_state

REFRESH_MS = 2000
MAX_ASR_BYTES = 8_000_000


class App:
    def __init__(self):
        self.session_cache = {"claude": [], "codex": []}
        self.tok_rate = {"claude": {"prev": None, "val": 0}, "codex": {"prev": None, "val": 0}}
        self.lock = threading.Lock()
        self.whisper = WhisperManager()

    def start_background(self):
        self.whisper.start()
        threading.Thread(target=self._refresh_loop, daemon=True).start()

    def _refresh_loop(self):
        while True:
            try:
                cache = collect.collect_sessions()
                with self.lock:
                    self.session_cache = cache
                    for pid in ("claude", "codex"):
                        total = sum(s.get("tokens_total", 0) for s in cache.get(pid, []))
                        cur = {"tokens": total, "at": time.time() * 1000}
                        prev = self.tok_rate[pid]["prev"]
                        self.tok_rate[pid] = {"prev": cur, "val": collect.tokens_per_min(prev, cur)}
            except Exception as e:                                 # 单次失败不影响服务
                print("collect_sessions failed:", e)
            time.sleep(REFRESH_MS / 1000)

    def state(self):
        with self.lock:
            cache, tok = self.session_cache, dict(self.tok_rate)
        return build_state(cache, tok)


def make_handler(app):
    class Handler(BaseHTTPRequestHandler):
        def _send(self, code, body, ctype="application/json"):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            if self.path.startswith("/codey/state"):
                self._send(200, json.dumps(app.state()).encode())
            else:
                self._send(404, b"not found", "text/plain")

        def do_POST(self):
            if self.path.startswith("/codey/asr"):
                length = int(self.headers.get("Content-Length") or 0)
                if length > MAX_ASR_BYTES:
                    self._send(413, json.dumps({"text": "", "error": "audio too large"}).encode())
                    return
                wav = self.rfile.read(length)
                text, err = "", None
                try:
                    text = app.whisper.transcribe(wav)
                except Exception as e:
                    err = str(e)
                self._send(200 if not err else 500, json.dumps({"text": text, "error": err}).encode())
            else:
                self._send(404, b"not found", "text/plain")

        def log_message(self, *_):                                # 静音默认访问日志
            pass

    return Handler
