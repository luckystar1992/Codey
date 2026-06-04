"""组装 /codey/state:账号额度(本地)+ 每会话 sessions[] + 聚合。零联网。"""
import time

from .collect import aggregate_provider
from .quota import end_of_week_epoch, read_codex_usage, read_real_usage
from .util import clamp_pct

_RANK = {"executing": 0, "thinking": 1}


def _sort_sessions(arr):
    return sorted(arr, key=lambda s: (_RANK.get(s["status"], 2), -s["context_pct"]))


def _num(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool)


def build_state(session_cache, tok_rate):
    now = int(time.time())
    real = read_real_usage()
    five = real.get("five_hour") if real else None
    seven = real.get("seven_day") if real else None
    five_ok = bool(five and _num(five.get("used_percentage")))
    seven_ok = bool(seven and _num(seven.get("used_percentage")))

    claude_sessions = _sort_sessions(session_cache.get("claude", []))
    codex_sessions = _sort_sessions(session_cache.get("codex", []))
    c_agg = aggregate_provider(claude_sessions)
    x_agg = aggregate_provider(codex_sessions)

    claude = {
        "id": "claude", "name": "Claude Code",
        "session": {
            "used_pct": clamp_pct(five["used_percentage"]) if five_ok else 0,
            "reset_epoch": (five.get("resets_at") or 0) if five_ok else 0,
        },
        "weekly": {
            "used_pct": clamp_pct(seven["used_percentage"]) if seven_ok else 0,
            "reset_epoch": (seven.get("resets_at") or 0) if seven_ok else end_of_week_epoch(),
        },
        "pending_reviews": 0,
        "model": (real.get("model") if real else None) or None,
        "active_count": c_agg["active_count"],
        "agg": {"dirty_repos": c_agg["dirty_repos"], "tokens_per_min": tok_rate["claude"]["val"]},
        "sessions": claude_sessions,
        "_src": "statusline" if five_ok else "none",
    }

    cx = read_codex_usage()
    cx_sess = cx.get("session") if cx else None
    cx_week = cx.get("weekly") if cx else None
    codex = {
        "id": "codex", "name": "Codex",
        "session": {
            "used_pct": cx_sess["pct"] if cx_sess else 0,
            "reset_epoch": cx_sess["reset"] if cx_sess else now,
        },
        "weekly": {
            "used_pct": cx_week["pct"] if cx_week else 0,
            "reset_epoch": cx_week["reset"] if cx_week else end_of_week_epoch(),
        },
        "pending_reviews": 0,
        "model": (codex_sessions[0]["model"] if codex_sessions else None) or None,
        "active_count": x_agg["active_count"],
        "agg": {"dirty_repos": x_agg["dirty_repos"], "tokens_per_min": tok_rate["codex"]["val"]},
        "sessions": codex_sessions,
        "_src": "codexbar" if cx else "rollout",
    }

    return {
        "ts": now,
        "stale": not (real and real.get("fresh")) and not five_ok,
        "battery": {"pct": 0, "charging": False},   # 设备用自身真实电量
        "providers": [claude, codex],
    }
