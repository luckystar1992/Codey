"""kindle_page.render 单测:配置驱动 CSS(字号/行距/字体/配色/加粗)+ 内容 / 转义 / 容错。纯函数。"""
from codey import kindle_page


def kcfg(**over):
    base = {"refresh_s": 30, "font_scale": 1.5, "line_height": 1.45,
            "font_family": "serif", "theme": "light", "bold_emphasis": True,
            "sizes": {"title": 21, "provider": 20, "quota": 19,
                      "session1": 19, "session2": 17}}
    if "sizes" in over:
        base["sizes"] = {**base["sizes"], **over.pop("sizes")}
    base.update(over)
    return base


def make_state(**over):
    base = {
        "ts": 1751871600, "stale": False,
        "providers": [
            {"id": "claude", "name": "Claude Code", "limited": False,
             "session": {"used_pct": 52}, "weekly": {"used_pct": 23},
             "active_count": 1, "agg": {"tokens_per_min": 12400},
             "sessions": [{"name": "Codey", "status": "executing", "model": "fable-5",
                           "context_pct": 61, "git": {"branch": "feat/x"},
                           "summary": "修复 USB 日志乱码"}]},
            {"id": "codex", "name": "Codex", "limited": False,
             "session": {"used_pct": 15}, "weekly": {"used_pct": 4},
             "active_count": 0, "agg": {"tokens_per_min": 0}, "sessions": []},
        ],
    }
    base.update(over)
    return base


def test_meta_refresh_from_config():
    assert '<meta http-equiv="refresh" content="45">' in kindle_page.render(make_state(), kcfg(refresh_s=45))
    assert 'content="5"' in kindle_page.render(make_state(), kcfg(refresh_s=1))       # clamp 下界
    assert 'content="3600"' in kindle_page.render(make_state(), kcfg(refresh_s=99999))# clamp 上界


def test_font_scale_multiplies_sizes():
    # session2 基准 17,scale 2.0 → 34px;scale 1.0 → 17px
    assert "font-size:34px" in kindle_page.render(make_state(), kcfg(font_scale=2.0))
    assert "font-size:17px" in kindle_page.render(make_state(), kcfg(font_scale=1.0))


def test_per_section_size_independent():
    # title 基准调到 40,scale 1.0 → 40px 出现在 .hdr b
    h = kindle_page.render(make_state(), kcfg(font_scale=1.0, sizes={"title": 40}))
    assert "font-size:40px" in h


def test_line_height_applied():
    assert "line-height:1.8" in kindle_page.render(make_state(), kcfg(line_height=1.8))


def test_font_family_mapping():
    assert "Georgia" in kindle_page.render(make_state(), kcfg(font_family="serif"))
    assert "Helvetica" in kindle_page.render(make_state(), kcfg(font_family="sans"))
    assert "Courier New" in kindle_page.render(make_state(), kcfg(font_family="mono"))


def test_theme_inverts_colors():
    light = kindle_page.render(make_state(), kcfg(theme="light"))
    dark = kindle_page.render(make_state(), kcfg(theme="dark"))
    assert "background:#fff;color:#000" in light
    assert "background:#000;color:#fff" in dark


def test_bold_emphasis_toggle():
    on = kindle_page.render(make_state(), kcfg(bold_emphasis=True, font_scale=1.0))
    off = kindle_page.render(make_state(), kcfg(bold_emphasis=False, font_scale=1.0))
    assert ".sess .l1{font-size:19px;font-weight:bold}" in on
    assert ".sess .l1{font-size:19px;font-weight:normal}" in off


def test_contains_providers_quota_sessions():
    h = kindle_page.render(make_state(), kcfg())
    assert "Claude Code" in h and "Codex" in h
    assert "52%" in h and "23%" in h and "15%" in h
    assert "● executing" in h and "Codey" in h
    assert "ctx 61%" in h and "feat/x" in h and "fable-5" in h
    assert "修复 USB 日志乱码" in h
    assert "12.4k tok/min" in h and "1 active" in h


def test_escapes_user_strings():
    st = make_state()
    sess = st["providers"][0]["sessions"][0]
    sess["summary"] = "<script>alert(1)</script>"
    sess["git"]["branch"] = "a<b>&c"
    sess["model"] = "<img src=x onerror=1>"
    sess["name"] = "<i>proj</i>"
    st["providers"][0]["name"] = "<b>Claude</b>"
    h = kindle_page.render(st, kcfg())
    assert "<script>alert(1)</script>" not in h and "&lt;script&gt;" in h
    assert "a&lt;b&gt;&amp;c" in h
    assert "&lt;img src=x onerror=1&gt;" in h
    assert "&lt;i&gt;proj&lt;/i&gt;" in h
    assert "&lt;b&gt;Claude&lt;/b&gt;" in h


def test_empty_sessions_placeholder():
    assert "(无活跃会话)" in kindle_page.render(make_state(), kcfg())


def test_stale_warning_toggle():
    assert "数据可能过期" in kindle_page.render(make_state(stale=True), kcfg())
    assert "数据可能过期" not in kindle_page.render(make_state(), kcfg())


def test_limited_marker():
    st = make_state()
    st["providers"][0]["limited"] = True
    assert "已限流" in kindle_page.render(st, kcfg())


def test_never_raises_on_garbage():
    assert "<html" in kindle_page.render({}, {})
    assert "<html" in kindle_page.render(None, None)
    assert "<html" in kindle_page.render({"providers": [None, {}]}, {"font_scale": "x", "sizes": "y"})
    assert "(无数据)" in kindle_page.render({}, {})


def test_never_raises_on_non_finite_numbers():
    inf, nan = float("inf"), float("nan")
    st = make_state()
    p = st["providers"][0]
    p["session"]["used_pct"] = inf
    p["weekly"]["used_pct"] = nan
    p["agg"]["tokens_per_min"] = inf
    p["sessions"][0]["context_pct"] = nan
    assert "<html" in kindle_page.render(st, kcfg(font_scale=inf))
    assert "<html" in kindle_page.render(st, kcfg(refresh_s=nan))


def test_no_script_tag_in_page():
    assert "<script" not in kindle_page.render(make_state(), kcfg())


def test_time_deterministic_with_now():
    import time as _t
    h = kindle_page.render(make_state(), kcfg(), now=0)
    assert _t.strftime("%H:%M", _t.localtime(0)) in h
