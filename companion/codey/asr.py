"""语音 ASR:转发本机 whisper-server(127.0.0.1 回环),退化到 whisper-cli。无外网。

去幻觉过滤(whisper 在静音/极短/非语音音频上会臆造字幕)直接移植自原 Node 版。
"""
import http.client
import json
import os
import re
import subprocess
import tempfile
import threading
import time

_HALLUC = [
    re.compile(r"^(你好)+$"),
    re.compile(r"欢迎(收看|观看)"),
    re.compile(r"请[^ ]{0,3}(订阅|关注|点赞)"),
    re.compile(r"谢谢(大家)?(观看|收看)"),
    re.compile(r"^字幕"),
    re.compile(r"明镜|点点栏目"),
    re.compile(r"thanks? for watching", re.IGNORECASE),
    re.compile(r"^(嗯|啊|呃|哦)+$"),
]

_HOME = os.path.expanduser("~")
_PKG_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WHISPER_CLI = os.environ.get("CODEY_WHISPER", "/opt/homebrew/bin/whisper-cli")
WHISPER_SERVER = os.environ.get("CODEY_WHISPER_SERVER", "/opt/homebrew/bin/whisper-server")
MODEL = os.environ.get("CODEY_WHISPER_MODEL", os.path.join(_PKG_ROOT, "models", "ggml-small.bin"))
WS_HOST = "127.0.0.1"
WS_PORT = int(os.environ.get("CODEY_WHISPER_PORT") or 8090)


def collapse_and_deny(text):
    text = re.sub(r"\s+", " ", str(text or "")).strip()
    for unit in range(2, 5):
        u = text[:unit]
        if u and len(text) % unit == 0 and text == u * (len(text) // unit):
            text = u
            break
    for rx in _HALLUC:
        if rx.search(text):
            return ""
    return text


def clean_text(vj):
    parts = []
    for s in (vj.get("segments") or []):
        if isinstance(s.get("no_speech_prob"), (int, float)) and s["no_speech_prob"] > 0.6:
            continue
        if isinstance(s.get("avg_logprob"), (int, float)) and s["avg_logprob"] < -1.2:
            continue
        words = s.get("words")
        if isinstance(words, list) and words:
            t, prev = "", None
            for w in words:
                ww = (w.get("word") or "").strip()
                if ww and ww != prev:
                    t += ww
                prev = ww
        else:
            t = (s.get("text") or "").strip()
        if t:
            parts.append(t)
    return collapse_and_deny("".join(parts))


def _multipart(wav):
    b = "----codey" + os.urandom(8).hex()
    def field(name, val):
        return (f"--{b}\r\nContent-Disposition: form-data; name=\"{name}\"\r\n\r\n{val}\r\n").encode()
    head = (f"--{b}\r\nContent-Disposition: form-data; name=\"file\"; "
            f"filename=\"a.wav\"\r\nContent-Type: audio/wav\r\n\r\n").encode()
    body = b"".join([
        head, wav, b"\r\n",
        field("response_format", "verbose_json"),
        field("language", "zh"),
        field("temperature", "0"),
        f"--{b}--\r\n".encode(),
    ])
    return body, f"multipart/form-data; boundary={b}"


class WhisperManager:
    """尽力维持一个常驻 whisper-server;不可用时按需用 whisper-cli。全本地回环。"""

    def __init__(self):
        self.ready = False
        self._stop = False
        self._proc = None

    def start(self):
        threading.Thread(target=self._manage, daemon=True).start()

    def stop(self):
        self._stop = True
        if self._proc:
            try:
                self._proc.terminate()
            except Exception:
                pass

    def _probe(self):
        try:
            c = http.client.HTTPConnection(WS_HOST, WS_PORT, timeout=0.8)
            c.request("GET", "/")
            c.getresponse().read()
            c.close()
            return True
        except Exception:
            return False

    def _manage(self):
        backoff = 2
        while not self._stop:
            if self._probe():                         # 已有(自起或外部)server
                self.ready = True
                backoff = 2
                time.sleep(2)
                continue
            self.ready = False
            if not (os.path.exists(WHISPER_SERVER) and os.path.exists(MODEL)):
                time.sleep(2)                          # 没有 server/模型 -> 仅 cli
                continue
            args = [WHISPER_SERVER, "-m", MODEL, "-l", "zh", "-nt", "-t", "4",
                    "-bo", "1", "-sns", "--host", WS_HOST, "--port", str(WS_PORT)]
            try:
                self._proc = subprocess.Popen(args, stdout=subprocess.DEVNULL,
                                              stderr=subprocess.DEVNULL)
            except Exception:
                time.sleep(backoff)
                backoff = min(backoff * 2, 60)
                continue
            while self._proc.poll() is None and not self._stop:
                if not self.ready and self._probe():
                    self.ready = True
                    backoff = 2
                time.sleep(1)
            self.ready = False
            if not self._stop:
                time.sleep(backoff)
                backoff = min(backoff * 2, 60)

    def _via_server(self, wav):
        body, ctype = _multipart(wav)
        c = http.client.HTTPConnection(WS_HOST, WS_PORT, timeout=20)
        c.request("POST", "/inference", body, {"Content-Type": ctype, "Content-Length": str(len(body))})
        raw = c.getresponse().read().decode("utf-8", "replace")
        c.close()
        return clean_text(json.loads(raw))            # 非 JSON -> 抛错 -> 上层回退 cli

    def _via_cli(self, wav):
        fd, tmp = tempfile.mkstemp(suffix=".wav", prefix="codey_asr_")
        os.close(fd)
        try:
            with open(tmp, "wb") as fh:
                fh.write(wav)
            r = subprocess.run([WHISPER_CLI, "-m", MODEL, "-f", tmp, "-l", "zh",
                                "-nt", "-np", "-bo", "1", "-bs", "1", "-sns"],
                               capture_output=True, text=True, timeout=25)
            return collapse_and_deny(r.stdout)
        finally:
            try:
                os.unlink(tmp)
            except OSError:
                pass

    def transcribe(self, wav):
        if self.ready:
            try:
                return self._via_server(wav)
            except Exception:
                pass
        return self._via_cli(wav)
