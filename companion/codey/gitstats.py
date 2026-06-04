"""git diff --shortstat 解析 + 读取(本地子进程,无网络)。"""
import re
import subprocess


def parse_git_shortstat(text):
    s = str(text if text is not None else "")
    ins = re.search(r"(\d+) insertion", s)
    dele = re.search(r"(\d+) deletion", s)
    return {
        "added": int(ins.group(1)) if ins else 0,
        "modified": int(dele.group(1)) if dele else 0,
    }


def _git(cwd, args, timeout=4):
    try:
        r = subprocess.run(["git", "-C", cwd, *args],
                           capture_output=True, text=True, timeout=timeout)
        return r.stdout.strip() if r.returncode == 0 else ""
    except Exception:
        return ""


def read_git_stats(cwd):
    if not cwd:
        return {"branch": "", "added": 0, "modified": 0}
    branch = _git(cwd, ["rev-parse", "--abbrev-ref", "HEAD"])
    stat = _git(cwd, ["diff", "--shortstat"])
    return {"branch": branch or "", **parse_git_shortstat(stat)}
