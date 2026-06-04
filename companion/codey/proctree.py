"""进程树 / 监听端口:ps、lsof 解析 + 后代收集(macOS 本地子进程,无网络)。"""
import re
import subprocess


def parse_ps(text):
    """`ps -axo pid,ppid,rss,pcpu,comm` 输出 -> {pid: info}。表头/空行跳过。"""
    out = {}
    for line in str(text if text is not None else "").split("\n"):
        m = re.match(r"^\s*(\d+)\s+(\d+)\s+(\d+)\s+([\d.]+)\s+(.*)$", line)
        if not m:
            continue
        pid = int(m.group(1))
        out[pid] = {
            "pid": pid, "ppid": int(m.group(2)), "rss_kb": int(m.group(3)),
            "cpu": float(m.group(4)), "comm": m.group(5).strip(),
        }
    return out


def descendants_of(pmap, root_pid):
    """递归收集 root_pid 的全部后代 pid(不含自身)。"""
    children = {}
    for info in pmap.values():
        children.setdefault(info["ppid"], []).append(info["pid"])
    out, seen, stack = [], {root_pid}, list(children.get(root_pid, []))
    while stack:
        pid = stack.pop()
        if pid in seen:
            continue
        seen.add(pid)
        out.append(pid)
        stack.extend(children.get(pid, []))
    return out


def parse_lsof_listen(text):
    """`lsof -nP -iTCP -sTCP:LISTEN` 输出 -> {pid: [ports]}。"""
    ports = {}
    for line in str(text if text is not None else "").split("\n"):
        if "(LISTEN)" not in line:
            continue
        pidm = re.match(r"^\S+\s+(\d+)", line)
        portm = re.search(r":(\d+)\s+\(LISTEN\)", line)
        if not pidm or not portm:
            continue
        pid, port = int(pidm.group(1)), int(portm.group(1))
        ports.setdefault(pid, [])
        if port not in ports[pid]:
            ports[pid].append(port)
    return ports


def _run(cmd, timeout=4):
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return r.stdout if r.returncode == 0 else ""
    except Exception:
        return ""


def read_ps():
    return parse_ps(_run(["ps", "-axo", "pid,ppid,rss,pcpu,comm"]))


def read_listen_ports():
    return parse_lsof_listen(_run(["lsof", "-nP", "-iTCP", "-sTCP:LISTEN"]))
