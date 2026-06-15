# companion/codey/focus.py
"""把 macOS 终端切到某会话的 tab/pane —— 设备详情页点屏「确认切换」时调用。

映射:session 的 agent PID → 控制终端 TTY(ps -o tty)→ 终端里 tty 匹配的 pane/tab。
- 主路径 Kaku(基于 wezterm,用户的终端):`kaku cli list --format json` 按 tty_name 找
  pane_id → `kaku cli activate-pane` → 前置 GUI。
- 回退 iTerm2 / Terminal.app:AppleScript 按 `tty of session/tab` 选中并前置。
仅作用于正在运行的终端(不主动拉起)。Kaku/iTerm/Terminal 任一命中即成功。
"""
import json
import os
import shutil
import subprocess


def tty_for_pid(pid):
    """PID → 控制终端路径(如 '/dev/ttys003');无控制终端返回 None。"""
    if not pid:
        return None
    try:
        out = subprocess.check_output(
            ["ps", "-o", "tty=", "-p", str(int(pid))],
            stderr=subprocess.DEVNULL, timeout=2).decode().strip()
    except Exception:
        return None
    if not out or out in ("??", "?", "-"):
        return None
    return out if out.startswith("/dev/") else "/dev/" + out


def _run(cmd, timeout=4):
    """运行命令返回 (rc, stdout);异常返回 (None, '')。"""
    try:
        p = subprocess.run(cmd, capture_output=True, timeout=timeout)
        return p.returncode, p.stdout.decode("utf-8", "ignore")
    except Exception:
        return None, ""


# ---- Kaku (wezterm-based) ----
def _kaku_bin():
    for cand in (shutil.which("kaku"),
                 "/Applications/Kaku.app/Contents/MacOS/kaku",
                 os.path.expanduser("~/Applications/Kaku.app/Contents/MacOS/kaku")):
        if cand and os.path.exists(cand):
            return cand
    return None


def _kaku_focus(tty):
    kaku = _kaku_bin()
    if not kaku:
        return False
    rc, out = _run([kaku, "cli", "list", "--format", "json"])
    if rc != 0 or not out:
        return False
    try:
        panes = json.loads(out)
    except Exception:
        return False
    pane_id = next((p.get("pane_id") for p in panes if p.get("tty_name") == tty), None)
    if pane_id is None:
        return False
    rc, _ = _run([kaku, "cli", "activate-pane", "--pane-id", str(pane_id)])
    if rc != 0:
        return False
    _run(["osascript", "-e", 'tell application "Kaku" to activate'])   # 把 Kaku 窗口前置
    return True


# ---- iTerm2 / Terminal.app fallback ----
def _app_running(app):
    rc, out = _run(["osascript", "-e", f'application "{app}" is running'], timeout=2)
    return out.strip() == "true"


def _osascript(script):
    rc, out = _run(["osascript", "-e", script])
    return out.strip()


def _iterm_focus(tty):
    if not _app_running("iTerm"):
        return False
    script = f'''
tell application "iTerm"
  repeat with w in windows
    repeat with t in tabs of w
      repeat with s in sessions of t
        if tty of s is "{tty}" then
          tell s to select
          activate
          return "ok"
        end if
      end repeat
    end repeat
  end repeat
  return "nf"
end tell'''
    return _osascript(script) == "ok"


def _terminal_focus(tty):
    if not _app_running("Terminal"):
        return False
    script = f'''
tell application "Terminal"
  repeat with w in windows
    repeat with t in tabs of w
      if tty of t is "{tty}" then
        set selected tab of w to t
        set frontmost of w to true
        activate
        return "ok"
      end if
    end repeat
  end repeat
  return "nf"
end tell'''
    return _osascript(script) == "ok"


def focus_pid(pid):
    """把终端切到 PID 所在的 pane/tab。返回 (ok, reason)。"""
    tty = tty_for_pid(pid)
    if not tty:
        return False, "no-tty"
    if _kaku_focus(tty):
        return True, "kaku"
    if _iterm_focus(tty):
        return True, "iterm"
    if _terminal_focus(tty):
        return True, "terminal"
    return False, "tab-not-found"
