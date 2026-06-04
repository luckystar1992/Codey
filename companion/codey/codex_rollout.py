"""解析 Codex CLI rollout JSONL -> 聚合对象(含 rate_limits)。纯函数。

注意:Codex 的 input_tokens 已含 cached_input_tokens,故 total_input 存非缓存部分,
cache_read 单列;rate_limits 按 window_minutes<=300 分 5h / 周。
"""
import json

from .util import parse_ts_ms


def codex_tool_arg(name, args_json):
    try:
        a = json.loads(args_json or "{}")
    except (ValueError, TypeError):
        return ""
    if not isinstance(a, dict):
        return ""
    if a.get("command"):
        cmd = a["command"]
        cmd = " ".join(str(x) for x in cmd) if isinstance(cmd, list) else str(cmd)
        return cmd.split("\n")[0][:40]
    if a.get("path"):
        return "/".join(str(a["path"]).split("/")[-2:])
    if a.get("pattern"):
        return str(a["pattern"])
    return ""


def _int(v):
    try:
        return int(v or 0)
    except (TypeError, ValueError):
        return 0


def parse_codex_rollout(text):
    r = {
        "session_id": "", "cwd": "", "cli_version": "", "originator": "", "model": "", "effort": "",
        "context_window": 0, "git_branch": "", "first_prompt": "", "current_task": "",
        "total_input": 0, "total_output": 0, "total_cache_read": 0, "last_context_tokens": 0,
        "turn_count": 0, "five_hour_pct": None, "five_hour_resets_at": None,
        "weekly_pct": None, "weekly_resets_at": None, "done": False,
    }
    for line in str(text if text is not None else "").split("\n"):
        s = line.strip()
        if not s:
            continue
        try:
            e = json.loads(s)
        except (ValueError, TypeError):
            continue
        if not isinstance(e, dict):
            continue
        p = e.get("payload") or {}
        et = e.get("type")
        if et == "session_meta":
            r["session_id"] = p.get("id") or r["session_id"]
            r["cwd"] = p.get("cwd") or r["cwd"]
            r["cli_version"] = p.get("cli_version") or r["cli_version"]
            r["originator"] = p.get("originator") or r["originator"]
            git = p.get("git") or {}
            if git.get("branch"):
                r["git_branch"] = git["branch"]
        elif et == "turn_context":
            if p.get("model"):
                r["model"] = p["model"]
            if p.get("effort"):
                r["effort"] = p["effort"]
            if p.get("model_context_window"):
                r["context_window"] = _int(p["model_context_window"]) or r["context_window"]
        elif et == "response_item" and p.get("type") == "function_call":
            r["current_task"] = (str(p.get("name") or "") + " "
                                 + codex_tool_arg(p.get("name"), p.get("arguments"))).strip()
        elif et == "event_msg":
            pt = p.get("type")
            if pt == "user_message":
                r["turn_count"] += 1
                if not r["first_prompt"]:
                    r["first_prompt"] = str(p.get("message") or "").strip()
            elif pt == "task_complete":
                r["done"] = True
            elif pt == "token_count":
                info = p.get("info") or {}
                tot = info.get("total_token_usage") or {}
                last = info.get("last_token_usage") or {}
                cached = _int(tot.get("cached_input_tokens"))
                r["total_input"] = max(0, _int(tot.get("input_tokens")) - cached)
                r["total_cache_read"] = cached
                r["total_output"] = _int(tot.get("output_tokens"))
                if last.get("input_tokens") is not None:
                    r["last_context_tokens"] = _int(last.get("input_tokens"))
                rl = p.get("rate_limits") or {}
                for slot in ("primary", "secondary"):
                    w = rl.get(slot)
                    if not w:
                        continue
                    ms = parse_ts_ms(w.get("resets_at")) if w.get("resets_at") else 0
                    resets = ms // 1000 if ms else None
                    try:
                        win = float(w.get("window_minutes") or 0)
                    except (TypeError, ValueError):
                        win = 0
                    pct = w.get("used_percent")
                    pct = float(pct) if pct is not None else None
                    if win <= 300:
                        r["five_hour_pct"], r["five_hour_resets_at"] = pct, resets
                    else:
                        r["weekly_pct"], r["weekly_resets_at"] = pct, resets
    if not r["context_window"]:
        r["context_window"] = 272000
    return r
