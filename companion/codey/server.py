"""HTTP 服务:GET /codey/state(用量+会话) · POST /codey/asr(语音)。供手表 LAN 拉取。"""
import json
import os
import threading
import time
from http.server import BaseHTTPRequestHandler
from urllib.parse import urlparse, parse_qs

from . import asr_history
from . import collect
from . import config
from . import ngrok_api
from .asr import WhisperManager
from .chime import ChimeState
from .state import build_state

REFRESH_MS = 2000                # 仅作默认参考;实际间隔实时读 config.get("refresh_ms")
NGROK_REFRESH_MS = 15000          # ngrok 公网地址轮询(免费档 ASR 地址随机)
STATE_PORT = int(os.environ.get("CODEY_PORT") or 8787)
ASR_PORT = int(os.environ.get("CODEY_ASR_PORT") or 8788)
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
        self.ngrok = {"state_url": "", "asr_url": ""}   # 整体替换,不就地改(见 _ngrok_loop)
        self.lock = threading.Lock()
        self.chime = ChimeState()
        self.whisper = WhisperManager()

    def start_background(self):
        self.whisper.start()
        threading.Thread(target=self._refresh_loop, daemon=True).start()
        threading.Thread(target=self._ngrok_loop, daemon=True).start()

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
            time.sleep(max(0.5, config.get("refresh_ms") / 1000))  # 实时可配,下界 0.5s

    def _ngrok_loop(self):
        while True:
            try:
                urls = ngrok_api.public_urls(ngrok_api.fetch(), STATE_PORT, ASR_PORT)
                with self.lock:
                    self.ngrok = urls                              # 整体替换(不可变更新)
            except Exception as e:                                 # fetch() 失败已返回 {};此处仅兜底 public_urls 解析异常
                print("ngrok refresh failed:", e)
            time.sleep(NGROK_REFRESH_MS / 1000)

    def state(self):
        with self.lock:
            cache, tok = self.session_cache, dict(self.tok_rate)
            chime_event = self.chime.event
            asr_url = self.ngrok.get("asr_url", "")
        return build_state(cache, tok, chime_event, asr_url=asr_url, display=config.get("display"))


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
