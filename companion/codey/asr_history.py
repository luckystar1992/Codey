# companion/codey/asr_history.py
"""ASR 识别历史:单个滚动 JSONL(append),每行带时间码 ts(ms)+ time(ISO)。"""
import json
import os
import time
from datetime import datetime, timezone

_DEFAULT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "data", "asr_history.jsonl")


def _path():
    return os.environ.get("CODEY_ASR_HISTORY") or _DEFAULT


def append(text, engine="", pasted=False, app="", at_ms=None):
    text = (text or "").strip()
    if not text:
        return None
    at = int(at_ms) if at_ms is not None else int(time.time() * 1000)
    iso = datetime.fromtimestamp(at / 1000, tz=timezone.utc).astimezone().isoformat(timespec="seconds")
    entry = {"ts": at, "time": iso, "text": text, "engine": engine, "pasted": bool(pasted), "app": app}
    p = _path()
    try:
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "a", encoding="utf-8") as f:
            f.write(json.dumps(entry, ensure_ascii=False) + "\n")
    except OSError as e:
        print("asr_history append failed:", e)
    return entry


def recent(n=100):
    try:
        with open(_path(), encoding="utf-8") as f:
            lines = f.readlines()
    except OSError:
        return []
    out = []
    for ln in lines[-int(n):]:
        ln = ln.strip()
        if not ln:
            continue
        try:
            out.append(json.loads(ln))
        except Exception:
            pass
    return out
