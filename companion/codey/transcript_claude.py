"""解析 Claude Code transcript JSONL -> 聚合对象。纯函数(入参为字符串)。"""
import json

from .synthetic import is_synthetic_user_message, text_of
from .util import parse_ts_ms


def tool_arg(name, inp):
    i = inp or {}
    if name in ("Read", "Edit", "Write"):
        segs = [p for p in str(i.get("file_path") or "").split("/") if p]
        return "/".join(segs[-2:])
    if name == "Bash":
        return str(i.get("command") or "").split("\n")[0][:40]
    if name in ("Grep", "Glob"):
        return str(i.get("pattern") or "")
    return ""


def _int(v):
    try:
        return int(v or 0)
    except (TypeError, ValueError):
        return 0


def parse_claude_transcript(text):
    r = {
        "total_input": 0, "total_output": 0, "total_cache_read": 0, "total_cache_create": 0,
        "last_context_tokens": 0, "max_context_tokens": 0, "turn_count": 0,
        "model": "", "version": "", "git_branch": "", "current_task": "", "first_prompt": "",
        "last_user_ts_ms": 0, "pending_tool": False,
        "compactions": 0, "_prev_ctx": 0,
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
        t = e.get("type")
        if t == "assistant" and e.get("message"):
            msg = e["message"]
            u = msg.get("usage") or {}
            inp, out = _int(u.get("input_tokens")), _int(u.get("output_tokens"))
            cr, cc = _int(u.get("cache_read_input_tokens")), _int(u.get("cache_creation_input_tokens"))
            r["total_input"] += inp
            r["total_output"] += out
            r["total_cache_read"] += cr
            r["total_cache_create"] += cc
            ctx = (inp + cc) if (cr == 0 and cc > 0) else (inp + cr)
            if r["_prev_ctx"] > 0 and ctx < r["_prev_ctx"] * 0.7:
                r["compactions"] += 1
            r["_prev_ctx"] = ctx
            r["last_context_tokens"] = ctx
            if ctx > r["max_context_tokens"]:
                r["max_context_tokens"] = ctx
            if msg.get("model"):
                r["model"] = msg["model"]
            r["turn_count"] += 1
            task, has_tool = "", False
            for b in (msg.get("content") or []):
                if isinstance(b, dict) and b.get("type") == "tool_use":
                    has_tool = True
                    task = (str(b.get("name") or "") + " " + tool_arg(b.get("name"), b.get("input"))).strip()
            r["current_task"] = task
            r["pending_tool"] = has_tool
            r["last_user_ts_ms"] = 0
        elif t == "user":
            if e.get("version"):
                r["version"] = e["version"]
            if e.get("gitBranch"):
                r["git_branch"] = e["gitBranch"]
            synthetic = is_synthetic_user_message(e)
            if not synthetic and not r["first_prompt"]:
                r["first_prompt"] = text_of((e.get("message") or {}).get("content")).strip()
            r["pending_tool"] = False
            r["last_user_ts_ms"] = 0 if synthetic else parse_ts_ms(e.get("timestamp"))
    r.pop("_prev_ctx", None)
    return r
