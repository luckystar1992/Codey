# Kindle 监视页面 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** companion 新增 `GET /kindle`——面向早期 Kindle 浏览器的零 JS 服务端渲染监视页,`<meta refresh>` 自动刷新,间隔由管理台 `kindle_refresh_s` 配置。

**Architecture:** 新纯函数模块 `kindle_page.render(state, refresh_s, now=None) -> str(HTML)`;`server.py` 加 `/kindle` 路由把 `app.state()` 喂给它;`config.py` 加 `kindle_refresh_s`(默认 30,clamp 5–3600),顺手把 refresh_ms 的 int-clamp 逻辑收进 `_INT_CLAMPS` 表(DRY)。管理台配置表单是 SCHEMA 驱动,加 schema 条目即自动出现。

**Tech Stack:** Python 3 标准库(`html.escape`、`http.server`)、pytest。**零第三方依赖、页面零 JS。**

**Spec:** `docs/superpowers/specs/2026-07-07-kindle-monitor-page-design.md`

## Global Constraints

- 页面**零 JS**;自动刷新只用 `<meta http-equiv="refresh">`;白底黑字高对比(e-ink),无彩色/阴影/动画;单列流式布局不写死宽度。
- `kindle_refresh_s`:默认 **30**,clamp **[5, 3600]**,坏值回默认,永不抛错(与 `refresh_ms` 同模式)。
- `render()` 只读 state、缺键容错(`.get` + isinstance 防御)、**永不抛错**;所有用户来源字符串(summary/branch/model/name 等)一律 `html.escape`。
- 测试从 `companion/` 目录跑:`cd /Users/zyc/code/Codey/companion && python3 -m pytest tests/... -v`。
- 代码注释用中文、贴合各文件现有风格;新文件 docstring 说明职责。
- 不改任何现有路由/配置键的行为(`_INT_CLAMPS` 重构后 `refresh_ms` 行为逐位不变,由现有测试守住)。

---

### Task 1: config 新增 `kindle_refresh_s`(含 `_INT_CLAMPS` 收敛)

**Files:**
- Modify: `companion/codey/config.py`(DEFAULTS ~L26、模块常量 ~L51、`get()` ~L120-124、`_validate()` ~L181-185)
- Test: `companion/tests/test_config.py`(文件末尾追加)

**Interfaces:**
- Consumes: 无(第一个任务)
- Produces: `config.get("kindle_refresh_s") -> int`(合并取值 + clamp [5,3600],默认 30);`config.save({"kindle_refresh_s": ...})` 校验落盘。Task 3 的 server 依赖 `config.get("kindle_refresh_s")`。

- [ ] **Step 1: 写失败测试**

在 `companion/tests/test_config.py` 末尾追加:

```python
def test_kindle_refresh_default(tmp_path, monkeypatch):
    monkeypatch.setattr(cfg, "CONFIG_PATH", str(tmp_path / "config.json"))
    assert cfg.get("kindle_refresh_s") == 30


def test_kindle_refresh_save_clamps(tmp_path, monkeypatch):
    monkeypatch.setattr(cfg, "CONFIG_PATH", str(tmp_path / "config.json"))
    cfg.save({"kindle_refresh_s": 99999})
    assert cfg.get("kindle_refresh_s") == 3600      # clamp 上界
    cfg.save({"kindle_refresh_s": 1})
    assert cfg.get("kindle_refresh_s") == 5         # clamp 下界
    cfg.save({"kindle_refresh_s": "abc"})
    assert cfg.get("kindle_refresh_s") == 30        # 坏值回默认


def test_kindle_refresh_bad_file_value_falls_back(tmp_path, monkeypatch):
    p = tmp_path / "config.json"
    p.write_text(json.dumps({"kindle_refresh_s": "bogus"}))
    monkeypatch.setattr(cfg, "CONFIG_PATH", str(p))
    assert cfg.get("kindle_refresh_s") == 30        # 文件坏值不抛错,回默认
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cd /Users/zyc/code/Codey/companion && python3 -m pytest tests/test_config.py -v -k kindle`
Expected: 3 个 FAIL(`cfg.get("kindle_refresh_s")` 返回 `None`,因为键不在 DEFAULTS)

- [ ] **Step 3: 实现**

`companion/codey/config.py` 四处修改:

(a) `DEFAULTS` 中 `"refresh_ms": 2000,` 行后加:

```python
    "kindle_refresh_s": 30,          # Kindle 页 meta refresh 间隔(秒),clamp [5, 3600]
```

(b) 模块常量区,`_REFRESH_MIN, _REFRESH_MAX = 500, 60000` 行后加:

```python
_KINDLE_MIN, _KINDLE_MAX = 5, 3600

# int 型配置键 -> clamp 区间(get/_validate 共用;新增 int 键只需在此登记)
_INT_CLAMPS = {
    "refresh_ms": (_REFRESH_MIN, _REFRESH_MAX),
    "kindle_refresh_s": (_KINDLE_MIN, _KINDLE_MAX),
}
```

(c) `get()` 中把 `refresh_ms` 专属分支:

```python
    if key == "refresh_ms":
        if "refresh_ms" in file_cfg:
            return _clamp(_coerce_int(file_cfg["refresh_ms"], DEFAULTS["refresh_ms"]),
                          _REFRESH_MIN, _REFRESH_MAX)
        return DEFAULTS["refresh_ms"]
```

替换为通用分支:

```python
    if key in _INT_CLAMPS:
        lo, hi = _INT_CLAMPS[key]
        if key in file_cfg:
            return _clamp(_coerce_int(file_cfg[key], DEFAULTS[key]), lo, hi)
        return DEFAULTS[key]
```

(d) `_validate()` 中把 `refresh_ms` 专属块:

```python
    if "refresh_ms" in partial:
        out["refresh_ms"] = _clamp(
            _coerce_int(partial["refresh_ms"], DEFAULTS["refresh_ms"]),
            _REFRESH_MIN, _REFRESH_MAX,
        )
```

替换为:

```python
    for k, (lo, hi) in _INT_CLAMPS.items():
        if k in partial:
            out[k] = _clamp(_coerce_int(partial[k], DEFAULTS[k]), lo, hi)
```

- [ ] **Step 4: 跑测试确认通过(含既有测试不回归)**

Run: `cd /Users/zyc/code/Codey/companion && python3 -m pytest tests/test_config.py -v`
Expected: 全部 PASS(既有 `refresh_ms` clamp 测试 `test_save_validates_and_roundtrips` 也 PASS,证明重构无行为变化)

- [ ] **Step 5: Commit**

```bash
cd /Users/zyc/code/Codey && git add companion/codey/config.py companion/tests/test_config.py && git commit -m "feat(companion): config 新增 kindle_refresh_s(默认 30,clamp 5-3600)+ int clamp 收敛 _INT_CLAMPS 表"
```

---

### Task 2: 渲染模块 `kindle_page.py`

**Files:**
- Create: `companion/codey/kindle_page.py`
- Test: `companion/tests/test_kindle_page.py`(新建)

**Interfaces:**
- Consumes: `util.clamp_pct(x) -> int`(0..100 取整夹取,已存在);state 形状见 `state.build_state`(providers[] 各含 name/limited/session.used_pct/weekly.used_pct/active_count/agg.tokens_per_min/sessions[],session 含 name/status/model/context_pct/git.branch/summary)
- Produces: `kindle_page.render(state: dict, refresh_s, now: float | None = None) -> str`(完整 HTML 文档;`now` 为 epoch 秒,None 取当前时间——测试传定值保证确定性)。Task 3 的 server 依赖此签名。

- [ ] **Step 1: 写失败测试**

新建 `companion/tests/test_kindle_page.py`:

```python
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
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cd /Users/zyc/code/Codey/companion && python3 -m pytest tests/test_kindle_page.py -v`
Expected: 全部 ERROR/FAIL,`ModuleNotFoundError: No module named 'codey.kindle_page'`

- [ ] **Step 3: 实现**

新建 `companion/codey/kindle_page.py`(完整文件):

```python
"""Kindle 监视页:把 /codey/state 渲染成零 JS 纯 HTML。纯函数。

面向早期 Kindle 实验性浏览器(老 WebKit):零 JS、<meta refresh> 整页刷新、
白底黑字高对比(e-ink)、单列大字号。只读 state,缺键容错,永不抛错。
"""
import html
import time

from .util import clamp_pct

_REFRESH_MIN, _REFRESH_MAX, _REFRESH_DEFAULT = 5, 3600, 30
_STATUS_ICON = {"executing": "●", "thinking": "◐", "waiting": "○", "done": "✓"}

_CSS = (
    "body{background:#fff;color:#000;margin:0;padding:10px 14px;"
    "font-family:Georgia,serif;font-size:19px;line-height:1.45}"
    ".hdr{border-bottom:3px solid #000;padding-bottom:6px}"
    ".hdr b{font-size:21px;letter-spacing:1px}"
    ".hdr .t{float:right;font-size:17px}"
    ".warn{border:2px solid #000;padding:4px 8px;margin:8px 0;font-weight:bold}"
    ".prov{border-bottom:3px solid #000;padding:10px 0}"
    ".prov h2{font-size:20px;margin:0 0 6px}"
    ".qrow{margin:4px 0}.qrow .lbl{display:inline-block;width:2.2em}"
    ".bar{display:inline-block;width:52%;height:13px;border:2px solid #000;"
    "vertical-align:middle}.bar i{display:block;height:100%;background:#000}"
    ".pct{font-weight:bold}"
    ".agg{font-size:17px;margin:4px 0 8px}"
    ".sess{border-top:1px solid #000;padding:7px 0}"
    ".sess .l1{font-weight:bold}"
    ".sess .l2,.sess .l3{font-size:17px;margin-left:1.4em}"
    ".none{font-size:17px;padding:6px 0}"
)


def _esc(v):
    return html.escape(str(v if v is not None else ""), quote=True)


def _fmt_kilo(n):
    """1234 -> '1.2k';999 -> '999';坏值 -> '0'。"""
    try:
        f = float(n)
    except (TypeError, ValueError):
        return "0"
    if f >= 1000:
        return "{:.1f}k".format(f / 1000.0)
    return str(int(f))


def _bar_html(pct):
    p = clamp_pct(pct)
    return ('<span class="bar"><i style="width:{p}%"></i></span> '
            '<span class="pct">{p}%</span>'.format(p=p))


def _quota_html(label, quota):
    quota = quota if isinstance(quota, dict) else {}
    return ('<div class="qrow"><span class="lbl">{}</span>{}</div>'
            .format(_esc(label), _bar_html(quota.get("used_pct", 0))))


def _session_html(s):
    s = s if isinstance(s, dict) else {}
    icon = _STATUS_ICON.get(s.get("status"), "○")
    git = s.get("git") if isinstance(s.get("git"), dict) else {}
    l2 = " · ".join(x for x in (
        _esc(s.get("model")),
        "ctx {}%".format(clamp_pct(s.get("context_pct"))),
        _esc(git.get("branch")),
    ) if x)
    parts = ['<div class="sess"><div class="l1">{} {}  {}</div>'.format(
        icon, _esc(s.get("status") or "-"), _esc(s.get("name") or "-"))]
    parts.append('<div class="l2">{}</div>'.format(l2 or "-"))
    summary = _esc(s.get("summary"))
    if summary:
        parts.append('<div class="l3">{}</div>'.format(summary))
    parts.append("</div>")
    return "".join(parts)


def _provider_html(p):
    p = p if isinstance(p, dict) else {}
    agg = p.get("agg") if isinstance(p.get("agg"), dict) else {}
    parts = ['<div class="prov"><h2>{}{}</h2>'.format(
        _esc(p.get("name") or p.get("id") or "?"),
        " —— 已限流" if p.get("limited") else "")]
    parts.append(_quota_html("5h", p.get("session")))
    parts.append(_quota_html("周", p.get("weekly")))
    try:
        active = int(p.get("active_count") or 0)
    except (TypeError, ValueError):
        active = 0
    parts.append('<div class="agg">{} active · {} tok/min</div>'.format(
        active, _fmt_kilo(agg.get("tokens_per_min", 0))))
    sessions = p.get("sessions") if isinstance(p.get("sessions"), list) else []
    if sessions:
        parts.extend(_session_html(s) for s in sessions)
    else:
        parts.append('<div class="none">(无活跃会话)</div>')
    parts.append("</div>")
    return "".join(parts)


def render(state, refresh_s, now=None):
    """state + 刷新秒数 -> 完整 HTML 文档字符串。缺键/坏值容错,永不抛错。

    now:epoch 秒,None 取当前时间(测试传定值保证确定性)。
    """
    state = state if isinstance(state, dict) else {}
    try:
        r = int(refresh_s)
    except (TypeError, ValueError):
        r = _REFRESH_DEFAULT
    r = max(_REFRESH_MIN, min(_REFRESH_MAX, r))
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
    ).format(r=r, css=_CSS, body="".join(body))
```

- [ ] **Step 4: 跑测试确认通过**

Run: `cd /Users/zyc/code/Codey/companion && python3 -m pytest tests/test_kindle_page.py -v`
Expected: 全部 PASS

- [ ] **Step 5: Commit**

```bash
cd /Users/zyc/code/Codey && git add companion/codey/kindle_page.py companion/tests/test_kindle_page.py && git commit -m "feat(companion): kindle_page.render——state 渲染零 JS 纯 HTML(e-ink 高对比 + meta refresh,转义+容错)"
```

---

### Task 3: server 路由 `/kindle` + SCHEMA 条目

**Files:**
- Modify: `companion/codey/server.py`(imports ~L9-15、SCHEMA ~L33 后、`do_GET` ~L214-216 `/sim` 分支后)
- Test: `companion/tests/test_server_routes.py`(文件末尾追加)

**Interfaces:**
- Consumes: `kindle_page.render(state, refresh_s, now=None) -> str`(Task 2);`config.get("kindle_refresh_s") -> int`(Task 1)
- Produces: `GET /kindle` 与 `GET /kindle.html` → 200 `text/html; charset=utf-8`;`GET /codey/config` 的 schema/values 含 `kindle_refresh_s`(管理台表单自动出现)

- [ ] **Step 1: 写失败测试**

在 `companion/tests/test_server_routes.py` 的 `TestServerRoutes` 类末尾追加两个方法:

```python
    # --- /kindle ---

    def test_schema_and_values_include_kindle_refresh(self):
        payload = server.config_get_payload()
        self.assertIn("kindle_refresh_s", payload["schema"])
        self.assertEqual(payload["schema"]["kindle_refresh_s"]["type"], "int")
        self.assertEqual(payload["schema"]["kindle_refresh_s"]["min"], 5)
        self.assertEqual(payload["schema"]["kindle_refresh_s"]["max"], 3600)
        self.assertEqual(payload["values"]["kindle_refresh_s"], 30)   # config.all() 自动带上

    def test_kindle_route_returns_html(self):
        # 起真实 HTTPServer 服务一次请求,验证 do_GET 分支真的接上了
        import threading, urllib.request
        from http.server import HTTPServer
        app = server.App()                       # 不 start_background,不起后台线程
        httpd = HTTPServer(("127.0.0.1", 0), server.make_handler(app))
        port = httpd.server_address[1]
        t = threading.Thread(target=httpd.handle_request, daemon=True)
        t.start()
        try:
            with urllib.request.urlopen("http://127.0.0.1:%d/kindle" % port, timeout=5) as resp:
                self.assertEqual(resp.status, 200)
                self.assertIn("text/html", resp.headers.get("Content-Type", ""))
                body = resp.read().decode("utf-8")
        finally:
            t.join(timeout=5)
            httpd.server_close()
        self.assertIn("CODEY MONITOR", body)
        self.assertIn('http-equiv="refresh" content="30"', body)     # 默认 30s
        self.assertNotIn("<script", body)                             # 零 JS
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cd /Users/zyc/code/Codey/companion && python3 -m pytest tests/test_server_routes.py -v -k kindle`
Expected: 2 个 FAIL(schema 无 `kindle_refresh_s`;`/kindle` 返回 404)

- [ ] **Step 3: 实现**

`companion/codey/server.py` 三处修改:

(a) imports 区(`from .chime import ChimeState` 附近)加:

```python
from . import kindle_page
```

(b) `SCHEMA` 中 `"refresh_ms": {...}` 条目后加:

```python
    "kindle_refresh_s": {"type": "int", "min": 5, "max": 3600,
                         "label": "Kindle 刷新间隔(秒)", "restart": False},
```

(c) `do_GET` 中 `/sim` 分支(`elif path == "/sim":` 块)之后、`/codey/config` 分支之前加:

```python
            elif path in ("/kindle", "/kindle.html"):
                page = kindle_page.render(app.state(), config.get("kindle_refresh_s"))
                self._send(200, page.encode("utf-8"), "text/html; charset=utf-8")
```

- [ ] **Step 4: 跑测试确认通过**

Run: `cd /Users/zyc/code/Codey/companion && python3 -m pytest tests/test_server_routes.py -v`
Expected: 全部 PASS

- [ ] **Step 5: 全量回归**

Run: `cd /Users/zyc/code/Codey/companion && python3 -m pytest tests/ -q`
Expected: 全部 PASS,无回归

- [ ] **Step 6: Commit**

```bash
cd /Users/zyc/code/Codey && git add companion/codey/server.py companion/tests/test_server_routes.py && git commit -m "feat(companion): GET /kindle 监视页路由 + 管理台 kindle_refresh_s 配置项"
```

---

### Task 4: 冒烟验证(手动,非自动化)

**Files:** 无代码改动

- [ ] **Step 1: 本机起服务看渲染**

```bash
cd /Users/zyc/code/Codey/companion && python3 codey_companion.py
```

另开终端:`curl -s http://127.0.0.1:8787/kindle | head -30`
Expected: 完整 HTML,含 `CODEY MONITOR`、meta refresh、真实 provider 数据;Mac 浏览器打开 `http://127.0.0.1:8787/kindle` 目视排版。

- [ ] **Step 2: 管理台确认配置项出现**

Mac 浏览器打开 `http://127.0.0.1:8787/admin` → 配置 tab → 出现「Kindle 刷新间隔(秒)」,改为 60 保存,再 `curl -s http://127.0.0.1:8787/kindle | grep refresh` 确认 `content="60"`。

- [ ] **Step 3: Kindle 真机验证(用户执行)**

Kindle 连同一 WiFi,实验性浏览器打开 `http://<Mac 局域网 IP>:8787/kindle`(IP 用 `ipconfig getifaddr en0` 查),确认:字号可读、进度条显示正常、按间隔自动整页刷新、无排版错乱。收藏书签。

---

## Self-Review 记录

- **Spec 覆盖**:§2 页面内容(额度条/agg/会话行/stale/limited/转义)→ Task 2;§3 kindle_page/server/config 改动 → Task 2/3/1;§4 错误处理(render 容错、config clamp)→ Task 1/2 测试;§5 测试清单逐条对应;§6 YAGNI 项均未引入。无缺口。
- **占位符**:无 TBD/TODO;每个代码步骤给了完整代码。
- **类型一致性**:`render(state, refresh_s, now=None)` 在 Task 2 定义、Task 3 消费一致;`config.get("kindle_refresh_s")` Task 1 定义、Task 3 消费一致;`_INT_CLAMPS` 仅 config.py 内部。
