"""账号级额度:Claude 读 statusline 截获的本地文件,Codex 读 codexbar 本地缓存。零联网。"""
import json
import os
import time
from datetime import datetime, timedelta

from .util import clamp_pct, parse_ts_ms

REAL_FRESH_MS = 30 * 60 * 1000
REAL_USAGE_FILE = os.path.join(os.path.expanduser("~"), ".claude", "codey-usage.json")
CODEX_HISTORY = os.path.join(os.path.expanduser("~"), "Library", "Application Support",
                             "com.steipete.codexbar", "history", "codex.json")


def read_real_usage():
    """Claude 真实限额(由 codey-statusline.sh 写入 ~/.claude/codey-usage.json)。"""
    try:
        with open(REAL_USAGE_FILE, encoding="utf-8") as fh:
            j = json.load(fh)
    except (OSError, ValueError):
        return None
    ts = j.get("ts")
    j["fresh"] = isinstance(ts, (int, float)) and (time.time() * 1000 - ts * 1000) < REAL_FRESH_MS
    return j


def end_of_week_epoch():
    """下周一 00:00(本地)epoch 秒。"""
    now = datetime.now()
    days = (7 - now.weekday()) % 7 or 7
    nxt = now.replace(hour=0, minute=0, second=0, microsecond=0) + timedelta(days=days)
    return int(nxt.timestamp())


def read_codex_usage():
    """Codex 额度:读 codexbar 菜单栏 App 的本地缓存 history/codex.json。"""
    try:
        with open(CODEX_HISTORY, encoding="utf-8") as fh:
            j = json.load(fh)
    except (OSError, ValueError):
        return None
    try:
        acc = j["accounts"][j["preferredAccountKey"]]
    except (KeyError, TypeError):
        return None

    def pick(name):
        w = next((x for x in acc if isinstance(x, dict) and x.get("name") == name), None)
        entries = (w or {}).get("entries") or []
        if not entries:
            return None
        e = entries[-1]
        ms = parse_ts_ms(e.get("resetsAt"))
        return {"pct": clamp_pct(e.get("usedPercent")), "reset": ms // 1000 if ms else 0}

    return {"session": pick("session"), "weekly": pick("weekly")}
