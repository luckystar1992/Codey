"""HTTP 服务:GET /codey/state(用量+会话) · POST /codey/asr(语音)。供手表 LAN 拉取。"""
import json
import os
import threading
import time
from http.server import BaseHTTPRequestHandler
from urllib.parse import urlparse, parse_qs

from . import asr_history
from . import collect
from .asr import WhisperManager
from .chime import ChimeState
from .state import build_state

REFRESH_MS = 2000
MAX_ASR_BYTES = 8_000_000
HISTORY_MAX = 500
WEB_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "web")   # companion/web
SIM_PATH = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
                        "sim", "codey-sim.html")   # repo/sim/codey-sim.html


def parse_history_n(path):
    try:
        q = parse_qs(urlparse(path).query)
        n = int(q.get("n", ["100"])[0])
    except (ValueError, TypeError):
        return 100
    return max(1, min(HISTORY_MAX, n))


def read_static(path):
    try:
        with open(path, "rb") as f:
            body = f.read()
    except OSError:
        return None, None
    ext = os.path.splitext(path)[1].lower()
    ctype = {".html": "text/html; charset=utf-8", ".js": "text/javascript",
             ".css": "text/css", ".json": "application/json"}.get(ext, "application/octet-stream")
    return body, ctype


class App:
    def __init__(self):
        self.session_cache = {"claude": [], "codex": []}
        self.tok_rate = {"claude": {"prev": None, "val": 0}, "codex": {"prev": None, "val": 0}}
        self.lock = threading.Lock()
        self.chime = ChimeState()
        self.whisper = WhisperManager()

    def start_background(self):
        self.whisper.start()
        threading.Thread(target=self._refresh_loop, daemon=True).start()

    def _refresh_loop(self):
        while True:
            try:
                cache = collect.collect_sessions()
                with self.lock:
                    self.chime.update(prev_cache=self.session_cache, cur_cache=cache)
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
            chime_event = self.chime.event
        return build_state(cache, tok, chime_event)


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
            path = urlparse(self.path).path
            if path in ("/", "/admin", "/admin.html"):
                body, ctype = read_static(os.path.join(WEB_DIR, "admin.html"))
                self._send(200, body, ctype) if body is not None else self._send(404, b"admin.html missing", "text/plain")
            elif path == "/sim":
                body, ctype = read_static(SIM_PATH)
                self._send(200, body, ctype) if body is not None else self._send(404, b"sim missing", "text/plain")
            elif path.startswith("/codey/state"):
                self._send(200, json.dumps(app.state()).encode())
            elif path.startswith("/codey/history"):
                self._send(200, json.dumps({"entries": asr_history.recent(parse_history_n(self.path))},
                                           ensure_ascii=False).encode())
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
