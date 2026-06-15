"""编排:发现 + 解析 + 进程/端口/git/子agent -> 两端 sessions[]。含纯聚合工具。"""
import os
import time

from . import discover
from .build_session import build_claude_session, build_codex_session
from .codex_rollout import parse_codex_rollout
from .derive import derive_status
from .gitstats import read_git_stats
from .proctree import descendants_of, read_listen_ports, read_ps
from .transcript_claude import parse_claude_transcript

CODEX_FRESH_WINDOW_MS = 5 * 60 * 1000   # 仅展示 5 分钟内活跃/近期结束的 Codex 会话


def has_active_descendant(pmap, pid, threshold=5):
    for d in descendants_of(pmap, pid):
        info = pmap.get(d)
        if info and float(info["cpu"]) > threshold:
            return True
    return False


def ports_for_tree(pmap, ports_map, pid):
    s = set()
    for d in [pid, *descendants_of(pmap, pid)]:
        for p in ports_map.get(d, []):
            s.add(p)
    return sorted(s)


def memory_for_tree(pmap, pid):
    """会话进程 + 全部后代的常驻内存(RSS)合计,单位 KB。"""
    total = 0
    for d in [pid, *descendants_of(pmap, pid)]:
        info = pmap.get(d)
        if info:
            total += int(info.get("rss_kb", 0))
    return total


def aggregate_provider(sessions):
    return {
        "active_count": sum(1 for s in sessions if s["status"] in ("executing", "thinking")),
        "dirty_repos": sum(1 for s in sessions
                           if s.get("git") and (s["git"]["added"] > 0 or s["git"]["modified"] > 0)),
        "tokens_total": sum(s.get("tokens_total", 0) for s in sessions),
    }


def tokens_per_min(prev, cur):
    if not prev or not cur:
        return 0
    dt = (cur["at"] - prev["at"]) / 60000
    if dt <= 0:
        return 0
    return max(0, round((cur["tokens"] - prev["tokens"]) / dt))


def collect_sessions():
    pmap = read_ps()
    ports_map = read_listen_ports()
    aliver = set(pmap.keys())

    claude = []
    for d in discover.discover_claude_sessions(discover.default_claude_home_dirs(), aliver):
        try:
            with open(d["transcript_path"], encoding="utf-8") as fh:
                text = fh.read()
        except OSError:
            text = ""
        parsed = parse_claude_transcript(text)
        status = derive_status(
            has_active_descendant=has_active_descendant(pmap, d["pid"]),
            pending_tool=parsed["pending_tool"],
            model_generating=parsed["last_user_ts_ms"] > 0,
        )
        git = read_git_stats(d["cwd"])
        claude.append(build_claude_session(
            session_id=d["session_id"], cwd=d["cwd"], started_at=d["started_at"], parsed=parsed,
            status=status, git=git, ports=ports_for_tree(pmap, ports_map, d["pid"]),
            subagents=discover.count_subagents(d["subagents_dir"]), effort="",
            memory_kb=memory_for_tree(pmap, d["pid"]), pid=d["pid"]))

    codex = []
    seen = set()
    codex_root = os.path.join(os.path.expanduser("~"), ".codex")
    now_ms = time.time() * 1000
    for roll in discover.list_codex_rollouts(codex_root):
        try:
            with open(roll["path"], encoding="utf-8") as fh:
                text = fh.read()
        except OSError:
            text = ""
        parsed = parse_codex_rollout(text)
        if not parsed["session_id"] or parsed["session_id"] in seen:
            continue
        seen.add(parsed["session_id"])               # 标记最新 rollout 代表该会话
        if (now_ms - roll["mtime_ms"]) >= CODEX_FRESH_WINDOW_MS:
            continue
        status = derive_status(done=parsed["done"], model_generating=False)
        git = read_git_stats(parsed["cwd"])
        codex.append(build_codex_session(
            parsed=parsed, started_at=int(roll["mtime_ms"]), status=status,
            git=git, ports=[], subagents=0))

    return {"claude": claude, "codex": codex}
