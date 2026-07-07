# Kindle 显示自定义面板 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `/kindle` 页面的字号/行距/字体/配色/分区字号/加粗全部可配(嵌套 `kindle` 配置组),并在管理台 Kindle 预览 tab 右侧加一排控件实时调、即时预览;配置持久化,真机 Kindle 自动同步。

**Architecture:** 新增嵌套 `kindle` 配置组(照 `display` 组模式,含 float clamp)。`kindle_page.render(state, kindle)` 由配置算出 CSS。`/kindle` 路由传 `config.get("kindle")`。管理台预览 tab 改两列,右侧硬编码控件读/写 `kindle.*`,change 时 POST 配置并重载 iframe。`kindle_refresh_s` 顶层键并入 `kindle.refresh_s`(未合并进 main,无 back-compat)。

**Tech Stack:** Python 3 标准库;pytest;静态 HTML/CSS + 原生 JS。

**Spec:** `docs/superpowers/specs/2026-07-07-kindle-display-panel-design.md`

## Global Constraints

- 页面**零 JS**、`<meta refresh>` 自动刷新、e-ink 高对比、单列;render 纯函数、缺键/坏值容错、**永不抛错**;所有用户串 `html.escape`。
- 配置坏值一律 clamp / 回默认,永不抛错。数值范围:`font_scale [1.0,3.0]`、`line_height [1.0,2.2]`、`refresh_s [5,3600]`、`sizes.* [12,48]`;`font_family ∈ {serif,sans,mono}`、`theme ∈ {light,dark}`。
- 默认值:`refresh_s 30, font_scale 1.5, line_height 1.45, font_family serif, theme light, bold_emphasis True, sizes {title 21, provider 20, quota 19, session1 19, session2 17}`。
- kindle 设置**只在预览面板**,不进「配置」tab(从主 SCHEMA 移除 `kindle_refresh_s`)。
- 测试从 `companion/` 跑:`cd /Users/zyc/code/Codey/companion && python3 -m pytest tests/... -v`。
- 中文注释,不可变、小函数。

---

### Task 1: config.py 新增 `kindle` 配置组

**Files:**
- Modify: `companion/codey/config.py`(imports、DEFAULTS、常量、helper、get/_validate/save)
- Test: `companion/tests/test_config.py`(追加)

**Interfaces:**
- Consumes: 现有 `_clamp`/`_coerce_int`/`_coerce_bool`
- Produces:
  - `config.get("kindle") -> dict`(解析后的 kindle 组,全字段就绪)。
  - `config.save({"kindle": {...}})` 深合并落盘(含 `sizes` 子 dict 深合并)。
  - 顶层 `kindle_refresh_s` **不再存在**。Task 2/3 依赖 `get("kindle")`。

- [ ] **Step 1: 写失败测试**

在 `companion/tests/test_config.py` 末尾追加:

```python
def test_kindle_group_defaults(tmp_path, monkeypatch):
    monkeypatch.setattr(cfg, "CONFIG_PATH", str(tmp_path / "config.json"))
    k = cfg.get("kindle")
    assert k["refresh_s"] == 30 and k["font_scale"] == 1.5 and k["line_height"] == 1.45
    assert k["font_family"] == "serif" and k["theme"] == "light" and k["bold_emphasis"] is True
    assert k["sizes"]["title"] == 21 and k["sizes"]["session2"] == 17
    assert "kindle_refresh_s" not in cfg.all()          # 顶层键已并入组


def test_kindle_group_clamps_and_coerces(tmp_path, monkeypatch):
    monkeypatch.setattr(cfg, "CONFIG_PATH", str(tmp_path / "config.json"))
    cfg.save({"kindle": {"font_scale": 9, "line_height": 0.1, "refresh_s": 1,
                         "font_family": "bogus", "theme": "dark", "bold_emphasis": "no",
                         "sizes": {"title": 999, "quota": 5}}})
    k = cfg.get("kindle")
    assert k["font_scale"] == 3.0 and k["line_height"] == 1.0    # float clamp 上/下界
    assert k["refresh_s"] == 5                                    # int clamp 下界
    assert k["font_family"] == "serif"                           # 非法枚举回默认
    assert k["theme"] == "dark"                                  # 合法枚举保留
    assert k["bold_emphasis"] is False                          # "no" -> False
    assert k["sizes"]["title"] == 48 and k["sizes"]["quota"] == 12   # size clamp
    assert k["sizes"]["provider"] == 20                          # 未改分区保留默认(深合并)


def test_kindle_group_bad_types_fall_back(tmp_path, monkeypatch):
    p = tmp_path / "config.json"
    p.write_text(json.dumps({"kindle": {"font_scale": "abc", "sizes": "nope"}}))
    monkeypatch.setattr(cfg, "CONFIG_PATH", str(p))
    k = cfg.get("kindle")
    assert k["font_scale"] == 1.5 and k["sizes"]["title"] == 21   # 坏值不抛错,回默认
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cd /Users/zyc/code/Codey/companion && python3 -m pytest tests/test_config.py -v -k kindle_group`
Expected: FAIL(`get("kindle")` 返回 None / KeyError)

- [ ] **Step 3: 实现**

`companion/codey/config.py` 修改:

(a) 顶部 import 加 `math`:

```python
import copy
import json
import math
import os
import threading
```

(b) `DEFAULTS` 中把 `"kindle_refresh_s": 30,` 那一行**删除**,并在 `"display": {...}` 组**之后**(闭合大括号后)加 `kindle` 组:

```python
    "kindle": {
        "refresh_s": 30,             # int, clamp [5, 3600]
        "font_scale": 1.5,           # float, clamp [1.0, 3.0]
        "line_height": 1.45,         # float, clamp [1.0, 2.2]
        "font_family": "serif",      # serif | sans | mono
        "theme": "light",            # light(黑字白底) | dark(白字黑底)
        "bold_emphasis": True,
        "sizes": {"title": 21, "provider": 20, "quota": 19,
                  "session1": 19, "session2": 17},   # 各区块基准 px,渲染再 × font_scale
    },
```

(c) `_INT_CLAMPS` 中删除 `"kindle_refresh_s": (_KINDLE_MIN, _KINDLE_MAX),` 一行(保留 `_KINDLE_MIN/_KINDLE_MAX` 常量,kindle 组仍用);在其后加 kindle 组用的常量:

```python
_FONT_SCALE_MIN, _FONT_SCALE_MAX = 1.0, 3.0
_LINE_HEIGHT_MIN, _LINE_HEIGHT_MAX = 1.0, 2.2
_KSIZE_MIN, _KSIZE_MAX = 12, 48
_KINDLE_FONTS = ("serif", "sans", "mono")
_KINDLE_THEMES = ("light", "dark")
```

(d) 在 `_coerce_int` 之后加 `_coerce_float`:

```python
def _coerce_float(v, default):
    try:
        f = float(v)
    except (TypeError, ValueError):
        return default
    return f if math.isfinite(f) else default
```

(e) 在 `_deep_merge_display` 之后加 `_clean_kindle` 与 `_merge_kindle`:

```python
def _clean_kindle(kp):
    """从 partial kindle dict 提取合法字段成干净 dict(数值 clamp / 枚举校验 / bool 强转 /
    sizes 逐键 clamp)。坏值/非法枚举丢弃。get 与 _validate 共用。"""
    out = {}
    if not isinstance(kp, dict):
        return out
    d = DEFAULTS["kindle"]
    if "refresh_s" in kp:
        out["refresh_s"] = _clamp(_coerce_int(kp["refresh_s"], d["refresh_s"]), _KINDLE_MIN, _KINDLE_MAX)
    if "font_scale" in kp:
        out["font_scale"] = _clamp(_coerce_float(kp["font_scale"], d["font_scale"]), _FONT_SCALE_MIN, _FONT_SCALE_MAX)
    if "line_height" in kp:
        out["line_height"] = _clamp(_coerce_float(kp["line_height"], d["line_height"]), _LINE_HEIGHT_MIN, _LINE_HEIGHT_MAX)
    if "font_family" in kp:
        s = str(kp["font_family"]).strip().lower()
        if s in _KINDLE_FONTS:
            out["font_family"] = s
    if "theme" in kp:
        s = str(kp["theme"]).strip().lower()
        if s in _KINDLE_THEMES:
            out["theme"] = s
    if "bold_emphasis" in kp:
        out["bold_emphasis"] = _coerce_bool(kp["bold_emphasis"])
    if isinstance(kp.get("sizes"), dict):
        szout = {}
        for key in d["sizes"]:
            if key in kp["sizes"]:
                szout[key] = _clamp(_coerce_int(kp["sizes"][key], d["sizes"][key]), _KSIZE_MIN, _KSIZE_MAX)
        if szout:
            out["sizes"] = szout
    return out


def _merge_kindle(file_kindle):
    """以 DEFAULTS['kindle'] 深拷贝为底,叠加 file 中合法字段(sizes 深合并保留其余分区)。"""
    merged = copy.deepcopy(DEFAULTS["kindle"])
    for k, v in _clean_kindle(file_kindle).items():
        if k == "sizes":
            merged["sizes"].update(v)
        else:
            merged[k] = v
    return merged
```

(f) `get(key)` 中,在 `if key == "display":` 分支**之前**加:

```python
    if key == "kindle":
        return _merge_kindle(file_cfg.get("kindle"))
```

(g) `_validate(partial)` 中,在 `if "display" in partial ...` 块**之后**加:

```python
    if "kindle" in partial:
        kclean = _clean_kindle(partial["kindle"])
        if kclean:
            out["kindle"] = kclean
```

(h) `save(partial)` 的 `for k, v in clean.items():` 循环里,把 `if k == "display":` 改为 `if`/`elif`,新增 kindle 分支。找到:

```python
        for k, v in clean.items():
            if k == "display":
```

在 `display` 分支的 `merged["display"] = base` 之后、`else:` 之前插入:

```python
            elif k == "kindle":
                base = merged.get("kindle")
                base = copy.deepcopy(base) if isinstance(base, dict) else {}
                for kk, vv in v.items():
                    if kk == "sizes":
                        sz = base.get("sizes")
                        sz = copy.deepcopy(sz) if isinstance(sz, dict) else {}
                        sz.update(vv)
                        base["sizes"] = sz
                    else:
                        base[kk] = vv
                merged["kindle"] = base
```

- [ ] **Step 4: 跑测试确认通过 + 全量回归**

Run: `cd /Users/zyc/code/Codey/companion && python3 -m pytest tests/test_config.py -v`
Expected: 全部 PASS(新增 3 个 kindle_group 测试;既有 display/refresh_ms 测试不回归)

- [ ] **Step 5: Commit**

```bash
cd /Users/zyc/code/Codey && git add companion/codey/config.py companion/tests/test_config.py && git commit -m "feat(companion): config 新增嵌套 kindle 组(字号/行距/字体/配色/分区字号/加粗,float clamp),并入 kindle_refresh_s"
```

---

### Task 2: kindle_page.render 由配置生成 CSS

**Files:**
- Modify: `companion/codey/kindle_page.py`(`_CSS` 常量替为 `_build_css`;`render` 签名/实现)
- Test: `companion/tests/test_kindle_page.py`(迁移到新签名 + 新增覆盖)

**Interfaces:**
- Consumes: `util.clamp_pct`;kindle 配置 dict(结构见 Task 1)
- Produces: `render(state, kindle, now=None) -> str`,`kindle` 为配置 dict(缺字段/坏值用内置默认)。Task 3 的 server 依赖此签名。

- [ ] **Step 1: 写失败测试(整文件替换)**

用以下内容**整体替换** `companion/tests/test_kindle_page.py`:

```python
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
    on = kindle_page.render(make_state(), kcfg(bold_emphasis=True))
    off = kindle_page.render(make_state(), kcfg(bold_emphasis=False))
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
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cd /Users/zyc/code/Codey/companion && python3 -m pytest tests/test_kindle_page.py -v`
Expected: 多个 FAIL(render 旧签名不认 dict / 新 CSS 断言未满足)

- [ ] **Step 3: 实现**

`companion/codey/kindle_page.py`:

(1) 顶部常量区,把 `_STATUS_ICON` 之后的整个 `_CSS = (...)` 常量块**删除**,替换为字体/主题映射 + 默认:

```python
_STATUS_ICON = {"executing": "●", "thinking": "◐", "waiting": "○", "done": "✓"}

_FONTS = {"serif": "Georgia, 'Times New Roman', serif",
          "sans": "Helvetica, Arial, sans-serif",
          "mono": "'Courier New', monospace"}
_THEMES = {"light": ("#fff", "#000"), "dark": ("#000", "#fff")}
_DEFAULT_SIZES = {"title": 21, "provider": 20, "quota": 19, "session1": 19, "session2": 17}


def _fnum(v, default, lo, hi):
    """float 强转 + clamp;坏值/非有限值回 default。"""
    try:
        f = float(v)
    except (TypeError, ValueError):
        return default
    if not math.isfinite(f):
        return default
    return max(lo, min(hi, f))


def _fint(v, default, lo, hi):
    """int 强转(经 float 容忍 "19"/19.0)+ clamp;坏值回 default。"""
    try:
        n = int(round(float(v)))
    except (TypeError, ValueError, OverflowError):
        return default
    return max(lo, min(hi, n))


def _build_css(kindle):
    """由 kindle 配置 dict 算出内联 CSS 字符串。缺字段/坏值用内置默认,永不抛错。"""
    kindle = kindle if isinstance(kindle, dict) else {}
    scale = _fnum(kindle.get("font_scale"), 1.5, 1.0, 3.0)
    lh = _fnum(kindle.get("line_height"), 1.45, 1.0, 2.2)
    fam = _FONTS.get(kindle.get("font_family"), _FONTS["serif"])
    bg, fg = _THEMES.get(kindle.get("theme"), _THEMES["light"])
    bold = "bold" if kindle.get("bold_emphasis", True) else "normal"
    sizes = kindle.get("sizes") if isinstance(kindle.get("sizes"), dict) else {}

    def sz(role):
        base = _fint(sizes.get(role), _DEFAULT_SIZES[role], _KSIZE_MIN, _KSIZE_MAX)
        return int(round(base * scale))

    title, prov, quota = sz("title"), sz("provider"), sz("quota")
    s1, s2 = sz("session1"), sz("session2")
    barh = max(6, int(round(quota * 0.6)))
    return (
        "body{{background:{bg};color:{fg};margin:0;padding:10px 14px;"
        "font-family:{fam};font-size:{s2}px;line-height:{lh}}}"
        ".hdr{{border-bottom:3px solid {fg};padding-bottom:6px}}"
        ".hdr b{{font-size:{title}px;letter-spacing:1px}}"
        ".hdr .t{{float:right;font-size:{s2}px}}"
        ".warn{{border:2px solid {fg};padding:4px 8px;margin:8px 0;font-weight:bold}}"
        ".prov{{border-bottom:3px solid {fg};padding:10px 0}}"
        ".prov h2{{font-size:{prov}px;margin:0 0 6px;font-weight:{bold}}}"
        ".qrow{{margin:4px 0;font-size:{quota}px}}.qrow .lbl{{display:inline-block;width:2.2em}}"
        ".bar{{display:inline-block;width:52%;height:{barh}px;border:2px solid {fg};"
        "vertical-align:middle}}.bar i{{display:block;height:100%;background:{fg}}}"
        ".pct{{font-weight:{bold}}}"
        ".agg{{font-size:{s2}px;margin:4px 0 8px}}"
        ".sess{{border-top:1px solid {fg};padding:7px 0}}"
        ".sess .l1{{font-size:{s1}px;font-weight:{bold}}}"
        ".sess .l2,.sess .l3{{font-size:{s2}px;margin-left:1.4em}}"
        ".none{{font-size:{s2}px;padding:6px 0}}"
    ).format(bg=bg, fg=fg, fam=fam, lh=lh, bold=bold, title=title,
             prov=prov, quota=quota, s1=s1, s2=s2, barh=barh)
```

(2) `render` 签名与实现改为:

```python
def render(state, kindle, now=None):
    """state + kindle 配置 dict -> 完整 HTML 文档字符串。缺键/坏值容错,永不抛错。

    kindle:见 config DEFAULTS['kindle'];缺字段用内置默认。now:epoch 秒,None 取当前时间。
    """
    state = state if isinstance(state, dict) else {}
    kindle = kindle if isinstance(kindle, dict) else {}
    r = _fint(kindle.get("refresh_s"), 30, _KINDLE_MIN, _KINDLE_MAX)
    css = _build_css(kindle)
    hhmm = time.strftime("%H:%M", time.localtime(now if now is not None else time.time()))
    providers = state.get("providers") if isinstance(state.get("providers"), list) else []
    body = ['<div class="hdr"><span class="t">{} ({}s)</span><b>CODEY MONITOR</b></div>'
            .format(hhmm, r)]
    if state.get("stale"):
        body.append('<div class="warn">⚠ 数据可能过期</div>')
    body.extend(_provider_html(p) for p in providers)
    if not providers:
        body.append('<div class="none">(无数据)</div>')
    return (
        "<!DOCTYPE html>\n"
        '<html lang="zh"><head><meta charset="utf-8">\n'
        '<meta http-equiv="refresh" content="{r}">\n'
        '<meta name="viewport" content="width=device-width,initial-scale=1">\n'
        "<title>Codey Monitor</title>\n"
        "<style>{css}</style></head>\n"
        "<body>{body}</body></html>"
    ).format(r=r, css=css, body="".join(body))
```

(3) 顶部保留 `_KINDLE_MIN, _KINDLE_MAX` 常量(render 用);新增 `_KSIZE_MIN, _KSIZE_MAX = 12, 48`。找到:

```python
_KINDLE_MIN, _KINDLE_MAX, _KINDLE_DEFAULT = 5, 3600, 30
```

替换为:

```python
_KINDLE_MIN, _KINDLE_MAX = 5, 3600
_KSIZE_MIN, _KSIZE_MAX = 12, 48
```

(`_KINDLE_DEFAULT` 不再使用,删除。)`import math` 已在文件顶部(现有 `_fmt_kilo` 用),无需再加。

- [ ] **Step 4: 跑测试确认通过**

Run: `cd /Users/zyc/code/Codey/companion && python3 -m pytest tests/test_kindle_page.py -v`
Expected: 全部 PASS

- [ ] **Step 5: Commit**

```bash
cd /Users/zyc/code/Codey && git add companion/codey/kindle_page.py companion/tests/test_kindle_page.py && git commit -m "feat(companion): kindle_page.render 由 kindle 配置生成 CSS(字号缩放/分区字号/行距/字体/配色/加粗)"
```

---

### Task 3: server.py 路由传配置 + 移除顶层 kindle_refresh_s

**Files:**
- Modify: `companion/codey/server.py`(SCHEMA、`/kindle` 路由)
- Test: `companion/tests/test_server_routes.py`(改既有 kindle 测试)

**Interfaces:**
- Consumes: `kindle_page.render(state, kindle, now=None)`(Task 2)、`config.get("kindle")`(Task 1)
- Produces: `/kindle` 200 反映配置;主 schema 不含 `kindle_refresh_s`;`values.kindle` 在 config payload

- [ ] **Step 1: 改测试(先让其表达新契约 → 失败)**

在 `companion/tests/test_server_routes.py` 中,把既有 `test_schema_and_values_include_kindle_refresh` 方法**整体替换**为:

```python
    def test_kindle_group_in_values_and_not_in_main_schema(self):
        payload = server.config_get_payload()
        self.assertNotIn("kindle_refresh_s", payload["schema"])   # 已移出「配置」tab
        self.assertIn("kindle", payload["values"])                # 组在 config.all()
        self.assertEqual(payload["values"]["kindle"]["refresh_s"], 30)
        self.assertEqual(payload["values"]["kindle"]["font_scale"], 1.5)
```

`test_kindle_route_returns_html` 里的默认刷新断言仍成立(`content="30"` 来自 `kindle.refresh_s` 默认),无需改。

- [ ] **Step 2: 跑测试确认失败**

Run: `cd /Users/zyc/code/Codey/companion && python3 -m pytest tests/test_server_routes.py -v -k "kindle_group_in_values or kindle_route"`
Expected: `test_kindle_group_in_values_and_not_in_main_schema` FAIL(schema 仍含 kindle_refresh_s);`test_kindle_route_returns_html` 可能 FAIL(render 旧签名)

- [ ] **Step 3: 实现**

`companion/codey/server.py`:

(a) `SCHEMA` 中删除:

```python
    "kindle_refresh_s": {"type": "int", "min": 5, "max": 3600,
                         "label": "Kindle 刷新间隔(秒)", "restart": False},
```

(b) `do_GET` 的 `/kindle` 分支,把:

```python
            elif path in ("/kindle", "/kindle.html"):
                page = kindle_page.render(app.state(), config.get("kindle_refresh_s"))
                self._send(200, page.encode("utf-8"), "text/html; charset=utf-8")
```

改为:

```python
            elif path in ("/kindle", "/kindle.html"):
                page = kindle_page.render(app.state(), config.get("kindle"))
                self._send(200, page.encode("utf-8"), "text/html; charset=utf-8")
```

- [ ] **Step 4: 跑测试确认通过 + 全量回归**

Run: `cd /Users/zyc/code/Codey/companion && python3 -m pytest tests/ -q`
Expected: 全部 PASS,无回归

- [ ] **Step 5: Commit**

```bash
cd /Users/zyc/code/Codey && git add companion/codey/server.py companion/tests/test_server_routes.py && git commit -m "feat(companion): /kindle 路由传 config.get(kindle);从主 schema 移除 kindle_refresh_s(改由预览面板调)"
```

---

### Task 4: 管理台预览 tab 两列 + 控件面板

**Files:**
- Modify: `companion/web/admin.html`(CSS、kindle section、JS)
- Test: `companion/tests/test_server_routes.py`(改回归守卫断言)

**Interfaces:**
- Consumes: `GET/POST /codey/config`(`values.kindle`,深合并)、`/kindle`(Task 3)
- Produces: 预览面板控件;无下游任务

- [ ] **Step 1: 改回归守卫测试(表达新标记 → 失败)**

在 `companion/tests/test_server_routes.py`,把既有 `test_admin_html_has_kindle_preview_tab` 的断言部分替换为:

```python
    def test_admin_html_has_kindle_preview_panel(self):
        # 回归守卫:锁住预览 tab、控件面板与关键控件,防未来误删/改错
        body, ctype = server.read_static(os.path.join(server.WEB_DIR, "admin.html"))
        self.assertIsNotNone(body)
        text = body.decode("utf-8")
        self.assertIn('data-tab="kindle"', text)              # nav 按钮
        self.assertIn('id="kpanel"', text)                    # 控件面板
        self.assertIn('name="kindle.font_scale"', text)       # 字号缩放控件
        self.assertIn('name="kindle.sizes.title"', text)      # 分区字号控件
        self.assertIn('name="kindle.theme"', text)            # 配色控件
```

(删除旧的 `test_admin_html_has_kindle_preview_tab`。)

- [ ] **Step 2: 跑测试确认失败**

Run: `cd /Users/zyc/code/Codey/companion && python3 -m pytest tests/test_server_routes.py -v -k kindle_preview_panel`
Expected: FAIL(`id="kpanel"` 等标记不存在)

- [ ] **Step 3a: 改 CSS**

`companion/web/admin.html`,找到 kindle 预览的 CSS 块:

```css
  /* kindle preview: Paperwhite 外壳 + 758px iframe 缩放 (scale .45 → 341×461) */
  .kframe{width:381px;max-width:100%;margin:4px 0;background:#3a3d42;border-radius:22px;
    padding:20px 20px 8px;box-shadow:0 6px 20px rgba(0,0,0,.4)}
  .kscreen{width:341px;height:461px;overflow:hidden;background:#fff;margin:0 auto;border-radius:2px}
  .kscreen iframe{width:758px;height:1024px;border:0;background:#fff;
    transform:scale(.45);transform-origin:top left;display:block}
  .klogo{text-align:center;color:#9aa0a6;font-size:13px;letter-spacing:1px;padding:6px 0 2px}
```

在其**之后**插入两列布局与面板样式:

```css
  /* kindle 预览:两列(左预览 + 右控件面板) */
  .kwrap{display:flex;gap:18px;flex-wrap:wrap;align-items:flex-start}
  .kpanel{flex:1;min-width:240px;background:var(--panel);border:1px solid var(--line);
    border-radius:14px;padding:16px}
  .kpanel h3{margin:14px 0 8px;font-size:13px;color:var(--green)} .kpanel h3:first-child{margin-top:0}
  .kfield{margin:0 0 12px} .kfield label{display:block;font-size:13px;margin-bottom:5px}
  .kfield input[type=range]{width:100%;accent-color:var(--green)}
  .kfield input[type=number]{width:80px} .kfield select{min-width:160px}
  .kfield label span{color:var(--green);font-weight:700;margin-left:6px}
  .kgrid{display:grid;grid-template-columns:repeat(auto-fit,minmax(120px,1fr));gap:8px 12px}
  .kgrid label{display:flex;align-items:center;justify-content:space-between;font-size:12px;
    background:#0a0b0e;border:1px solid var(--line);border-radius:8px;padding:6px 10px}
  .kgrid input{width:56px}
```

- [ ] **Step 3b: 改 kindle section 结构**

找到现有 kindle section:

```html
  <!-- ============ Kindle 预览 ============ -->
  <section id="kindle">
    <div class="card"><h2>Kindle 预览</h2>
      <p class="muted">模仿 6″ Paperwhite 显示 <code>/kindle</code> 监视页,按管理台配置的间隔自动刷新
        (iframe 内 <code>&lt;meta refresh&gt;</code>)。字号/换行按 Kindle 真实 758px 逻辑宽渲染再等比缩放。</p></div>
    <div class="kframe"><div class="kscreen" id="kscreen"></div><div class="klogo">kindle</div></div>
  </section>
```

整体替换为:

```html
  <!-- ============ Kindle 预览 ============ -->
  <section id="kindle">
    <div class="card"><h2>Kindle 预览</h2>
      <p class="muted">左侧模仿 6″ Paperwhite 显示 <code>/kindle</code> 监视页(真实 758px 逻辑宽渲染再缩放);
        右侧调节字号/行距/字体/配色等,改动即时应用并同步到真机 Kindle(下次刷新生效)。</p></div>
    <div class="kwrap">
      <div class="kframe"><div class="kscreen" id="kscreen"></div><div class="klogo">kindle</div></div>
      <div class="kpanel" id="kpanel">
        <h3>显示设置</h3>
        <div class="kfield"><label>刷新间隔(秒)</label>
          <input type="number" name="kindle.refresh_s" min="5" max="3600" step="5"></div>
        <div class="kfield"><label>字号缩放 <span id="v_scale"></span></label>
          <input type="range" name="kindle.font_scale" min="1" max="3" step="0.1"></div>
        <div class="kfield"><label>行距 <span id="v_lh"></span></label>
          <input type="range" name="kindle.line_height" min="1" max="2.2" step="0.05"></div>
        <div class="kfield"><label>字体</label>
          <select name="kindle.font_family">
            <option value="serif">衬线</option><option value="sans">无衬线</option><option value="mono">等宽</option>
          </select></div>
        <div class="kfield"><label>配色</label>
          <select name="kindle.theme">
            <option value="light">浅(黑字白底)</option><option value="dark">深(白字黑底)</option>
          </select></div>
        <div class="kfield"><label class="chk"><input type="checkbox" name="kindle.bold_emphasis"><span>重点加粗</span></label></div>
        <h3>分区字号(px)</h3>
        <div class="kgrid">
          <label>页头<input type="number" name="kindle.sizes.title" min="12" max="48"></label>
          <label>端名<input type="number" name="kindle.sizes.provider" min="12" max="48"></label>
          <label>额度<input type="number" name="kindle.sizes.quota" min="12" max="48"></label>
          <label>会话主<input type="number" name="kindle.sizes.session1" min="12" max="48"></label>
          <label>会话次<input type="number" name="kindle.sizes.session2" min="12" max="48"></label>
        </div>
        <div class="kfield" style="margin-top:14px"><button class="btn" type="button" id="kReset">恢复默认</button></div>
        <div class="toast" id="ktoast"></div>
      </div>
    </div>
  </section>
```

- [ ] **Step 3c: 改 JS(懒加载分支 + 面板逻辑)**

找到 showTab 里的 kindle 懒加载分支:

```javascript
  // Kindle 预览:首次进入才插入 iframe;/kindle 自带 meta refresh 自动刷新
  if(tab==='kindle' && !kindleLoaded){
    document.getElementById('kscreen').innerHTML =
      '<iframe src="/kindle" title="kindle"></iframe>';
    kindleLoaded=true;
  }
```

替换为:

```javascript
  // Kindle 预览:首次进入拉配置填控件 + 载预览;/kindle 自带 meta refresh 自动刷新
  if(tab==='kindle' && !kindleLoaded){ kindleLoaded=true; loadKindle(); }
```

在 `<script>` 末尾、`showTab('welcome');` **之前**加入面板逻辑:

```javascript
/* ---------- Kindle 预览面板 ---------- */
const KDEFAULT={refresh_s:30,font_scale:1.5,line_height:1.45,font_family:'serif',theme:'light',
  bold_emphasis:true,sizes:{title:21,provider:20,quota:19,session1:19,session2:17}};
const KSIZES=['title','provider','quota','session1','session2'];
function kctl(name){ return document.querySelector('#kpanel [name="'+name+'"]'); }
function kLabels(){
  document.getElementById('v_scale').textContent=kctl('kindle.font_scale').value+'×';
  document.getElementById('v_lh').textContent=kctl('kindle.line_height').value;
}
function fillKindle(k){
  k=k||KDEFAULT; const sz=k.sizes||{};
  kctl('kindle.refresh_s').value=k.refresh_s!=null?k.refresh_s:30;
  kctl('kindle.font_scale').value=k.font_scale!=null?k.font_scale:1.5;
  kctl('kindle.line_height').value=k.line_height!=null?k.line_height:1.45;
  kctl('kindle.font_family').value=k.font_family||'serif';
  kctl('kindle.theme').value=k.theme||'light';
  kctl('kindle.bold_emphasis').checked=k.bold_emphasis!==false;
  KSIZES.forEach(s=>{ kctl('kindle.sizes.'+s).value=(sz[s]!=null?sz[s]:KDEFAULT.sizes[s]); });
  kLabels();
}
function collectKindle(){
  const sizes={}; KSIZES.forEach(s=>{ sizes[s]=Number(kctl('kindle.sizes.'+s).value)||KDEFAULT.sizes[s]; });
  return { kindle:{
    refresh_s:Number(kctl('kindle.refresh_s').value)||30,
    font_scale:Number(kctl('kindle.font_scale').value)||1.5,
    line_height:Number(kctl('kindle.line_height').value)||1.45,
    font_family:kctl('kindle.font_family').value,
    theme:kctl('kindle.theme').value,
    bold_emphasis:kctl('kindle.bold_emphasis').checked,
    sizes:sizes,
  }};
}
function reloadKindlePreview(){
  // 加时间戳强制 iframe 重新取,服务端按新配置重渲染
  document.getElementById('kscreen').innerHTML='<iframe src="/kindle?t='+Date.now()+'" title="kindle"></iframe>';
}
function ktoast(kind,msg){ const t=document.getElementById('ktoast'); if(!t)return; t.className='toast '+kind; t.textContent=msg; }
async function saveKindle(){
  kLabels();
  try{
    const r=await fetch('/codey/config',{method:'POST',headers:{'Content-Type':'application/json'},
      body:JSON.stringify(collectKindle())});
    const j=await r.json().catch(()=>({}));
    if(!r.ok || !j.ok) throw new Error((j&&j.error)||('HTTP '+r.status));
    ktoast('ok','已应用 ✓ 真机 Kindle 下次刷新同步'); reloadKindlePreview();
  }catch(err){ ktoast('err','保存失败:'+esc(err.message)); }
}
let kindleBound=false;
async function loadKindle(){
  try{ const r=await fetch('/codey/config'); const j=await r.json(); fillKindle(j.values&&j.values.kindle); }
  catch(err){ fillKindle(null); }
  reloadKindlePreview();
  if(!kindleBound){
    kindleBound=true;
    document.querySelectorAll('#kpanel [name^="kindle."]').forEach(el=>{
      el.addEventListener('change', saveKindle);
      if(el.type==='range') el.addEventListener('input', kLabels);
    });
    document.getElementById('kReset').addEventListener('click',()=>{ fillKindle(KDEFAULT); saveKindle(); });
  }
}
```

- [ ] **Step 4: 跑测试确认通过 + 全量回归**

Run: `cd /Users/zyc/code/Codey/companion && python3 -m pytest tests/ -q`
Expected: 全部 PASS

- [ ] **Step 5: 冒烟目视(手动)**

```bash
cd /Users/zyc/code/Codey/companion && CODEY_PORT=8799 python3 codey_kindle.py &
sleep 2
# 改字号缩放到 2.5 并读回配置
curl -s -X POST http://127.0.0.1:8799/codey/config -H 'Content-Type: application/json' -d '{"kindle":{"font_scale":2.5,"theme":"dark"}}' | head -c 120; echo
curl -s http://127.0.0.1:8799/kindle | grep -o "background:#000;color:#fff" | head -1     # dark 生效
curl -s http://127.0.0.1:8799/kindle | grep -o "font-size:[0-9]*px" | head -3             # 放大后的字号
kill %1 2>/dev/null
```
Expected: POST 回 `{"ok": true,...}`;`/kindle` 含 `background:#000;color:#fff`(dark)与放大字号(如 session2 17×2.5≈43px)。
另在 Mac 浏览器打开 `http://127.0.0.1:8799/admin` → Kindle 预览:右侧控件齐全,拖动字号滑块左侧预览即时变大,配色切换生效,「恢复默认」复位。

- [ ] **Step 6: 恢复默认配置(避免污染)+ Commit**

```bash
cd /Users/zyc/code/Codey/companion && curl -s -X POST http://127.0.0.1:8799/codey/config -H 'Content-Type: application/json' -d '{"kindle":{"font_scale":1.5,"theme":"light"}}' >/dev/null 2>&1 || true
cd /Users/zyc/code/Codey && git add companion/web/admin.html companion/tests/test_server_routes.py && git commit -m "feat(companion): 管理台 Kindle 预览两列布局 + 显示设置控件(字号/行距/字体/配色/分区字号/加粗,change 即时应用并重载预览)"
```

---

## Self-Review 记录

- **Spec 覆盖**:§1 kindle 组模型 → Task 1;§2.1 config → Task 1;§2.2 render 由配置生成 CSS(字号缩放/分区字号/行距/字体映射/theme 反色/bold)→ Task 2;§2.3 server 路由 + 移除 kindle_refresh_s → Task 3;§2.4 admin 两列面板 + change→POST→重载 → Task 4;§3 错误处理(clamp/回默认、POST 失败提示、render 容错)→ Task 1/2/4;§4 测试逐条对应 Task 1-4。无缺口。
- **占位符**:无 TBD/TODO;每处改动给完整代码块。
- **一致性**:`render(state, kindle, now=None)` 在 Task 2 定义、Task 3 消费一致;`config.get("kindle")` Task 1 产出、Task 3 消费;控件 `name="kindle.*"` 与 collectKindle/fillKindle 及守卫测试断言逐一对应;分区字号五键 `title/provider/quota/session1/session2` 在 config 默认、render `_DEFAULT_SIZES`、admin KSIZES、测试 kcfg 中完全一致;数值范围(1.0–3.0 / 1.0–2.2 / 5–3600 / 12–48)各处一致;`_KSIZE_MIN/MAX` 在 config 与 kindle_page 同为 12/48。
