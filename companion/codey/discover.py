"""文件系统发现会话(读 ~/.claude*/sessions、transcript、~/.codex/sessions)。"""
import json
import os
import re


def pick_latest_transcript(files, started_at_ms, claimed_sids):
    """选 mtime>=startedAt(含 5s 宽限)且 sid 未占用的最新 jsonl。"""
    grace = started_at_ms - 5000
    cands = [f for f in files if f["mtime_ms"] >= grace and f["sid"] not in claimed_sids]
    cands.sort(key=lambda f: f["mtime_ms"], reverse=True)
    return cands[0] if cands else None


def list_transcripts(proj_dir):
    try:
        names = [n for n in os.listdir(proj_dir) if n.endswith(".jsonl")]
    except OSError:
        return []
    out = []
    for n in names:
        p = os.path.join(proj_dir, n)
        try:
            out.append({"sid": n[:-6], "path": p, "mtime_ms": os.stat(p).st_mtime * 1000})
        except OSError:
            continue
    return out


def encode_cwd(cwd):
    return re.sub(r"[^a-zA-Z0-9]", "-", cwd)


def count_subagents(subagents_dir):
    try:
        return sum(1 for n in os.listdir(subagents_dir) if n.endswith(".meta.json"))
    except OSError:
        return 0


def discover_claude_sessions(home_dirs, aliver_pids):
    out, claimed = [], set()
    for claude in home_dirs:
        sess_dir = os.path.join(claude, "sessions")
        try:
            files = [n for n in os.listdir(sess_dir) if n.endswith(".json")]
        except OSError:
            continue
        for fname in files:
            try:
                with open(os.path.join(sess_dir, fname), encoding="utf-8") as fh:
                    meta = json.load(fh)
            except (OSError, ValueError):
                continue
            pid = meta.get("pid")
            if not pid or pid not in aliver_pids:
                continue
            proj_dir = os.path.join(claude, "projects", encode_cwd(meta.get("cwd") or ""))
            transcripts = list_transcripts(proj_dir)
            direct = next((t for t in transcripts if t["sid"] == meta.get("sessionId")), None)
            chosen = direct or pick_latest_transcript(transcripts, meta.get("startedAt") or 0, claimed)
            if not chosen:
                continue
            claimed.add(chosen["sid"])
            out.append({
                "config_root": claude, "pid": pid, "session_id": chosen["sid"],
                "cwd": meta.get("cwd") or "", "started_at": meta.get("startedAt") or 0,
                "transcript_path": chosen["path"],
                "subagents_dir": os.path.join(proj_dir, chosen["sid"], "subagents"),
            })
    return out


def default_claude_home_dirs():
    home = os.path.expanduser("~")
    dirs = []
    env = os.environ.get("CLAUDE_CONFIG_DIR")
    if env:
        dirs.append(env)
    try:
        for n in os.listdir(home):
            if n.startswith(".claude"):
                p = os.path.join(home, n)
                try:
                    if os.path.isdir(os.path.join(p, "sessions")) and p not in dirs:
                        dirs.append(p)
                except OSError:
                    pass
    except OSError:
        pass
    if not dirs:
        dirs.append(os.path.join(home, ".claude"))
    return dirs


def list_codex_rollouts(codex_root, limit=40):
    root = os.path.join(codex_root, "sessions")
    out = []

    def walk(d, depth=0):
        if depth > 5:
            return
        try:
            entries = list(os.scandir(d))
        except OSError:
            return
        for e in entries:
            try:
                if e.is_symlink():
                    continue
                if e.is_dir():
                    walk(e.path, depth + 1)
                elif re.match(r"^rollout-.*\.jsonl$", e.name):
                    out.append({"path": e.path, "mtime_ms": e.stat().st_mtime * 1000})
            except OSError:
                continue

    walk(root, 0)
    out.sort(key=lambda x: x["mtime_ms"], reverse=True)
    return out[:limit]
