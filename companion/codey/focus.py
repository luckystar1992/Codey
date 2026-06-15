# companion/codey/focus.py
"""把 macOS 终端切到某会话的 tab —— 设备详情页点屏「确认切换」时调用。

映射:session 的 agent PID → 控制终端 TTY(ps -o tty)→ iTerm2/Terminal.app 里
tty 匹配的 tab(AppleScript 选中并前置)。仅作用于正在运行的终端 app(不主动拉起)。
需在 系统设置→隐私与安全性→自动化/辅助功能 授权运行 Python 的进程。
"""
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


def _app_running(app):
    try:
        out = subprocess.check_output(
            ["osascript", "-e", f'application "{app}" is running'],
            stderr=subprocess.DEVNULL, timeout=2).decode().strip()
        return out == "true"
    except Exception:
        return False


def _osascript(script):
    """运行 AppleScript,返回 stdout 文本(失败/异常返回 '')。"""
    try:
        out = subprocess.check_output(["osascript", "-e", script],
                                      stderr=subprocess.DEVNULL, timeout=4)
        return out.decode("utf-8", "ignore").strip()
    except Exception:
        return ""


def _iterm_focus(tty):
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
    """把终端切到 PID 所在的 tab。返回 (ok, reason)。"""
    tty = tty_for_pid(pid)
    if not tty:
        return False, "no-tty"
    if _app_running("iTerm") and _iterm_focus(tty):
        return True, "iterm"
    if _app_running("Terminal") and _terminal_focus(tty):
        return True, "terminal"
    return False, "tab-not-found"
