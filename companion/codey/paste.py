# companion/codey/paste.py
"""把转写文本打进 macOS 当前聚焦窗口(pbcopy + osascript Cmd+V)。
首次运行需在 系统设置→隐私与安全性→辅助功能 给运行 Python 的终端授权。"""
import subprocess


def _pbcopy(text):
    p = subprocess.Popen(["pbcopy"], stdin=subprocess.PIPE)
    p.communicate(text.encode("utf-8"))


def _osascript(script):
    subprocess.run(["osascript", "-e", script], check=False)


def paste_to_active_window(text):
    if not text:
        return
    _pbcopy(text)
    _osascript('tell application "System Events" to keystroke "v" using command down')


def press_enter():
    _osascript('tell application "System Events" to keystroke return')


def clear_input():
    _osascript('tell application "System Events"\n'
               '  keystroke "a" using command down\n'
               '  key code 51\n'
               'end tell')


def get_active_app():
    try:
        out = subprocess.check_output(
            ["osascript", "-e",
             'tell application "System Events" to name of first application process whose frontmost is true'],
            stderr=subprocess.DEVNULL, timeout=2)
        return out.decode("utf-8").strip()
    except Exception:
        return "?"
