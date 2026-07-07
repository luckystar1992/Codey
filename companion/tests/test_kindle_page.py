"""kindle_page.render 单测:meta refresh / 内容 / 转义 / 容错。纯函数,无 I/O。"""
from codey import kindle_page


def make_state(**over):
    base = {
        "ts": 1751871600, "stale": False,
        "providers": [
            {"id": "claude", "name": "Claude Code", "limited": False,
             "session": {"used_pct": 52, "reset_epoch": 0},
             "weekly": {"used_pct": 23, "reset_epoch": 0},
             "active_count": 1,
             "agg": {"dirty_repos": 0, "tokens_per_min": 12400},
             "sessions": [{
                 "name": "Codey", "status": "executing", "model": "fable-5",
                 "context_pct": 61, "git": {"branch": "feat/x"},
                 "summary": "修复 USB 日志乱码",
             }]},
            {"id": "codex", "name": "Codex", "limited": False,
             "session": {"used_pct": 15, "reset_epoch": 0},
             "weekly": {"used_pct": 4, "reset_epoch": 0},
             "active_count": 0, "agg": {"tokens_per_min": 0},
             "sessions": []},
        ],
    }
    base.update(over)
    return base


def test_meta_refresh_uses_refresh_s():
    h = kindle_page.render(make_state(), 45)
    assert '<meta http-equiv="refresh" content="45">' in h


def test_refresh_clamped_and_bad_value_defaults():
    assert 'content="5"' in kindle_page.render(make_state(), 1)
    assert 'content="3600"' in kindle_page.render(make_state(), 999999)
    assert 'content="30"' in kindle_page.render(make_state(), None)


def test_contains_providers_quota_sessions():
    h = kindle_page.render(make_state(), 30)
    assert "Claude Code" in h and "Codex" in h
    assert "52%" in h and "23%" in h and "15%" in h
    assert "● executing" in h and "Codey" in h
    assert "ctx 61%" in h and "feat/x" in h and "fable-5" in h
    assert "修复 USB 日志乱码" in h
    assert "12.4k tok/min" in h and "1 active" in h


def test_escapes_user_strings():
    st = make_state()
    st["providers"][0]["sessions"][0]["summary"] = "<script>alert(1)</script>"
    st["providers"][0]["sessions"][0]["git"]["branch"] = "a<b>&c"
    h = kindle_page.render(st, 30)
    assert "<script>alert(1)</script>" not in h
    assert "&lt;script&gt;" in h
    assert "a&lt;b&gt;&amp;c" in h


def test_empty_sessions_placeholder():
    assert "(无活跃会话)" in kindle_page.render(make_state(), 30)


def test_stale_warning_toggle():
    assert "数据可能过期" in kindle_page.render(make_state(stale=True), 30)
    assert "数据可能过期" not in kindle_page.render(make_state(), 30)


def test_limited_marker():
    st = make_state()
    st["providers"][0]["limited"] = True
    assert "已限流" in kindle_page.render(st, 30)


def test_never_raises_on_garbage():
    assert "<html" in kindle_page.render({}, 30)
    assert "<html" in kindle_page.render(None, 30)
    assert "<html" in kindle_page.render({"providers": [None, {}]}, 30)
    assert "(无数据)" in kindle_page.render({}, 30)


def test_time_deterministic_with_now():
    h = kindle_page.render(make_state(), 30, now=0)  # epoch 0,本地时区固定输出 HH:MM
    import time as _t
    assert _t.strftime("%H:%M", _t.localtime(0)) in h


def test_no_script_tag_in_page():
    assert "<script" not in kindle_page.render(make_state(), 30)  # 零 JS 硬约束
