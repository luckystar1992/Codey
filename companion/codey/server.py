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
from . import kindle_page
from . import ngrok_api
from .asr import WhisperManager
from .chime import ChimeState
from .state import build_state

REFRESH_MS = 2000                # 仅作默认参考;实际间隔实时读 config.get("refresh_ms")
NGROK_REFRESH_MS = 15000          # ngrok 公网地址轮询(免费档 ASR 地址随机)
STATE_PORT = int(os.environ.get("CODEY_PORT") or 8787)
ASR_PORT = int(os.environ.get("CODEY_ASR_PORT") or 8788)
MAX_ASR_BYTES = 8_000_000
MAX_CONFIG_BYTES = 64 * 1024      # /codey/config POST 体积上限(64KB)
HISTORY_MAX = 500

# UI 读取的配置 schema(纯数据;每项 type/options/label/min/max/restart)。
SCHEMA = {
    "asr_engine": {"type": "select", "options": ["auto", "sherpa", "doubao"],
                   "label": "识别引擎", "restart": False},
    "paste": {"type": "bool", "label": "转写后粘贴到聚焦窗口", "restart": False},
    "paste_auto_enter": {"type": "bool", "label": "粘贴后自动回车", "restart": False},
    "doubao_api_key": {"type": "password", "label": "豆包 API Key", "restart": False},
    "doubao_app_id": {"type": "password", "label": "豆包 App ID", "restart": False},
    "refresh_ms": {"type": "int", "min": 500, "max": 60000,
                   "label": "用量刷新间隔(ms)", "restart": False},
    "display": {
        "type": "group",
        "label": "设备显示项",
        "columns": {"type": "group", "label": "列表列", "fields": {
            "status": {"type": "bool", "label": "状态"},
            "model": {"type": "bool", "label": "模型"},
            "ctx": {"type": "bool", "label": "上下文"},
            "tokens": {"type": "bool", "label": "Tokens"},
            "memory": {"type": "bool", "label": "内存"},
            "turn": {"type": "bool", "label": "轮次"},
            "summary": {"type": "bool", "label": "摘要"},
            "branch": {"type": "bool", "label": "分支"},
        }},
        "providers": {"type": "group", "label": "启用端", "fields": {
            "claude": {"type": "bool", "label": "Claude Code"},
            "codex": {"type": "bool", "label": "Codex"},
        }},
        "restart": False,
    },
}


def mask_config(values):
    """遮罩密钥:SECRET_KEYS 的值替为 ""、并加 has_<key>:bool(原值)。非密钥原样保留。
    不就地改入参——返回新 dict。"""
    out = dict(values)
    for k in config.SECRET_KEYS:
        real = out.get(k, "")
        out[k] = ""
        out["has_" + k] = bool(real)
    return out


def config_get_payload():
    """GET /codey/config 的响应体:{values: 遮罩全量, schema}。"""
    return {"values": mask_config(config.all()), "schema": SCHEMA}


def config_post(body, content_length):
    """POST /codey/config 处理:返回 (status_code, response_dict)。
    - content_length 超 MAX_CONFIG_BYTES → 400
    - 坏 JSON / 非 dict → 400
    - 密钥被 POST 成 "" 视为「不改」(遮罩往返不会清空已存密钥)
    - 校验/落盘交给 config.save(永不抛错),回 {ok, values: 遮罩全量}
    """
    if content_length is not None and content_length > MAX_CONFIG_BYTES:
        return 400, {"ok": False, "error": "config body too large"}
    try:
        parsed = json.loads(body.decode("utf-8") or "{}")
    except (ValueError, UnicodeDecodeError):
        return 400, {"ok": False, "error": "invalid JSON"}
    if not isinstance(parsed, dict):
        return 400, {"ok": False, "error": "config body must be an object"}
    # 空密钥=保留现值:在落盘前剔除值为 "" 的密钥键
    clean = {k: v for k, v in parsed.items()
             if not (k in config.SECRET_KEYS and v == "")}
    config.save(clean)
    return 200, {"ok": True, "values": mask_config(config.all())}
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


def safe_content_length(headers, limit):
    """解析 Content-Length:非法/负数/超限 -> None(调用方拒绝);否则返回非负长度。
    防 rfile.read(-1) 把整条流读进内存(负数 Content-Length 的内存 DoS)。"""
    try:
        n = int(headers.get("Content-Length") or 0)
    except (TypeError, ValueError):
        return None
    return None if (n < 0 or n > limit) else n


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
        self.start_collectors()
        threading.Thread(target=self._ngrok_loop, daemon=True).start()

    def start_collectors(self):
        """只起会话采集后台线程(填 session_cache/tok_rate);/kindle 与 /codey/state 依赖它。返回该线程。"""
        t = threading.Thread(target=self._refresh_loop, daemon=True)
        t.start()
        return t

    def _collect_once(self):
        """采集一次并加锁更新 session_cache / tok_rate / chime。"""
        cache = collect.collect_sessions()
        with self.lock:
            self.chime.update(prev_cache=self.session_cache, cur_cache=cache)
            self.session_cache = cache
            for pid in ("claude", "codex"):
                total = sum(s.get("tokens_total", 0) for s in cache.get(pid, []))
                cur = {"tokens": total, "at": time.time() * 1000}
                prev = self.tok_rate[pid]["prev"]
                self.tok_rate[pid] = {"prev": cur, "val": collect.tokens_per_min(prev, cur)}

    def _refresh_loop(self):
        while True:
            try:
                self._collect_once()
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
            chime_event = self.chime.last      # 持久最新完成(非瞬时 event);设备 30s 轮询据 seq 增量响铃
            asr_url = self.ngrok.get("asr_url", "")
        return build_state(cache, tok, chime_event, asr_url=asr_url, display=config.get("display"))

    def pid_for_session(self, sid):
        """会话 id → agent PID(设备「切到此会话」时 companion 据此找终端 tab);找不到返回 0。"""
        if not sid:
            return 0
        with self.lock:
            for agent in ("claude", "codex"):
                for s in self.session_cache.get(agent, []):
                    if s.get("id") == sid:
                        return s.get("pid") or 0
        return 0

    def status_for_session(self, sid):
        """会话 id → status(executing/thinking/waiting/done);语音流式同步仅在 waiting(空闲于提示符)时注入。"""
        if not sid:
            return ""
        with self.lock:
            for agent in ("claude", "codex"):
                for s in self.session_cache.get(agent, []):
                    if s.get("id") == sid:
                        return s.get("status") or ""
        return ""


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
            elif path in ("/kindle", "/kindle.html"):
                page = kindle_page.render(app.state(), config.get("kindle"))
                self._send(200, page.encode("utf-8"), "text/html; charset=utf-8")
            elif path.startswith("/codey/config"):
                self._send(200, json.dumps(config_get_payload(), ensure_ascii=False).encode())
            elif path.startswith("/codey/state"):
                self._send(200, json.dumps(app.state()).encode())
            elif path.startswith("/codey/history"):
                self._send(200, json.dumps({"entries": asr_history.recent(parse_history_n(self.path))},
                                           ensure_ascii=False).encode())
            else:
                self._send(404, b"not found", "text/plain")

        def do_POST(self):
            if self.path.startswith("/codey/config"):
                length = safe_content_length(self.headers, MAX_CONFIG_BYTES)
                if length is None:                                # 非法/负数/超限 -> 400
                    code, resp = config_post(b"", MAX_CONFIG_BYTES + 1)
                else:
                    body = self.rfile.read(length) if length else b""
                    code, resp = config_post(body, length)
                self._send(code, json.dumps(resp, ensure_ascii=False).encode())
                return
            if self.path.startswith("/codey/asr"):
                length = safe_content_length(self.headers, MAX_ASR_BYTES)
                if length is None:                                # 非法/负数/超限 -> 413
                    self._send(413, json.dumps({"text": "", "error": "audio too large"}).encode())
                    return
                wav = self.rfile.read(length) if length else b""
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
