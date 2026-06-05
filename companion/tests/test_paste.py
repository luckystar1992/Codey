from codey import paste

def test_paste_sets_clipboard_then_cmd_v(monkeypatch):
    calls = []
    monkeypatch.setattr(paste, "_pbcopy", lambda text: calls.append(("pbcopy", text)))
    monkeypatch.setattr(paste, "_osascript", lambda script: calls.append(("osa", script)))
    paste.paste_to_active_window("你好 world")
    assert calls[0] == ("pbcopy", "你好 world")
    assert calls[1][0] == "osa" and "keystroke \"v\"" in calls[1][1]

def test_paste_empty_is_noop(monkeypatch):
    calls = []
    monkeypatch.setattr(paste, "_pbcopy", lambda t: calls.append(t))
    monkeypatch.setattr(paste, "_osascript", lambda s: calls.append(s))
    paste.paste_to_active_window("")
    assert calls == []

def test_press_enter_and_clear(monkeypatch):
    scripts = []
    monkeypatch.setattr(paste, "_osascript", lambda s: scripts.append(s))
    paste.press_enter(); paste.clear_input()
    assert "keystroke return" in scripts[0]
    assert "keystroke \"a\" using command down" in scripts[1] and "key code 51" in scripts[1]
