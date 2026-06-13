"""把解析结果 + 进程/git/端口/子agent 组装成对外 session 对象(契约形状)。纯函数。"""
from .context_window import context_window_for
from .util import clamp_pct, project_name


def _summary(first_prompt):
    """首条用户提示截到 64 字节(UTF-8 安全,不切半个字)。"""
    s = " ".join((first_prompt or "").split())   # 折叠换行/多空格
    b = s.encode("utf-8")
    if len(b) <= 64:
        return s
    return b[:64].decode("utf-8", "ignore")


def build_claude_session(*, session_id, cwd, started_at, parsed, status,
                         git, ports, subagents, effort, memory_kb=0):
    cw = context_window_for(parsed.get("model"), parsed.get("max_context_tokens"))
    ctx_tok = parsed.get("last_context_tokens") or 0
    return {
        "id": session_id,
        "name": project_name(cwd),
        "status": status,
        "model": parsed.get("model") or "",
        "context_pct": clamp_pct((ctx_tok / cw) * 100),
        "context_tokens": ctx_tok,
        "context_window": cw,
        "tokens_total": (parsed.get("total_input") or 0) + (parsed.get("total_output") or 0)
        + (parsed.get("total_cache_read") or 0) + (parsed.get("total_cache_create") or 0),
        "turn": parsed.get("turn_count") or 0,
        "git": git or {"branch": parsed.get("git_branch") or "", "added": 0, "modified": 0},
        "current_task": parsed.get("current_task") or "",
        "subagents": subagents or 0,
        "ports": ports or [],
        "started_at": (started_at or 0) // 1000,
        "effort": effort or "",
        "memory": memory_kb or 0,            # 进程树常驻内存(KB);0=未知
        "summary": _summary(parsed.get("first_prompt")),
        "tokens": {
            "in": parsed.get("total_input") or 0,
            "out": parsed.get("total_output") or 0,
            "cache_r": parsed.get("total_cache_read") or 0,
            "cache_w": parsed.get("total_cache_create") or 0,
        },
        "compactions": parsed.get("compactions") or 0,
    }


def build_codex_session(*, parsed, started_at, status, git, ports, subagents, memory_kb=0):
    cw = parsed.get("context_window") or 272000
    ctx_tok = parsed.get("last_context_tokens") or 0
    return {
        "id": parsed.get("session_id"),
        "name": project_name(parsed.get("cwd")),
        "status": status,
        "model": parsed.get("model") or "",
        "context_pct": clamp_pct((ctx_tok / cw) * 100),
        "context_tokens": ctx_tok,
        "context_window": cw,
        "tokens_total": (parsed.get("total_input") or 0) + (parsed.get("total_output") or 0)
        + (parsed.get("total_cache_read") or 0),
        "turn": parsed.get("turn_count") or 0,
        "git": git or {"branch": parsed.get("git_branch") or "", "added": 0, "modified": 0},
        "current_task": parsed.get("current_task") or "",
        "subagents": subagents or 0,
        "ports": ports or [],
        "started_at": (started_at or 0) // 1000,
        "effort": parsed.get("effort") or "",
        "memory": memory_kb or 0,
        "summary": _summary(parsed.get("first_prompt")),
        "tokens": {
            "in": parsed.get("total_input") or 0,
            "out": parsed.get("total_output") or 0,
            "cache_r": parsed.get("total_cache_read") or 0,
            "cache_w": 0,
        },
        "compactions": parsed.get("compactions") or 0,
    }
