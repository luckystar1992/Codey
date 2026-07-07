# Kindle 监视页面 — 设计

> 状态:已批准设计,待 writing-plans 拆实施计划
> 分支:`feat/kindle-monitor-page`(基于 `feat/usb-wired-fallback`)
> 日期:2026-07-07

## 0. 背景与目标

用户有一台 Kindle Paperwhite(1–3 代,e-ink 屏,自带实验性 WebKit 浏览器)。希望它连上与
Mac 相同的局域网后,作为一块"桌面监视屏"常显 Codey companion 的使用状态,并按设定的
间隔自动刷新。

companion 现状(`companion/codey/server.py`,纯 Python 标准库,端口 8787):

| 路由 | 内容 |
|---|---|
| `GET /` `/admin` | 管理台 `web/admin.html`(schema 驱动的配置表单等) |
| `GET /sim` | 设备模拟器页面 |
| `GET /codey/state` | JSON:Claude/Codex 额度 + 会话列表 + 聚合 |
| `GET/POST /codey/config` | 配置读写(`config.py` 分层取值 + 校验 + 原子写) |
| `GET /codey/history` | ASR 识别历史 |

**目标:新增 `GET /kindle` 路由,返回一页面向早期 Kindle 浏览器的监视页。**
Kindle 上访问 `http://<Mac 局域网 IP>:8787/kindle` 收藏书签即可。

### 硬约束(Kindle Paperwhite 实验性浏览器的能力边界)
1. 老 WebKit,JS 内存小、易崩、无现代 API —— **页面零 JS**。
2. e-ink 局部刷新有鬼影 —— 用 `<meta http-equiv="refresh">` 整页刷新(正好触发全刷,画面干净),
   这是 Kindle 上最可靠的自动刷新方式。
3. 16 级灰度屏 —— 白底黑字高对比,不用彩色、阴影、动画。
4. 分辨率 758×1024(KPW1/2)或 1072×1448(KPW3),DPI 高 —— 单列流式布局 + 大字号,不写死宽度。

## 1. 方案选择

- **A(选定):服务端渲染纯 HTML + meta refresh。** 零 JS、渲染纯函数好测试、
  贴合 companion 零依赖风格。整页重传每次几 KB,局域网可忽略。
- B(否):静态页 + JS 轮询 `/codey/state`。老 WebKit JS 挂掉后页面静默停更(显示旧数据
  但用户不知道),局部 DOM 更新鬼影严重。
- C(否):服务端渲染 PNG(Kindle 天气面板经典做法)。要引入 Pillow + 中文字体渲染,
  代码量数倍,杀鸡用牛刀。

## 2. 页面内容与布局

数据源:直接复用 `app.state()`(与手表同一份数据,零新增采集逻辑)。

```
┌─────────────────────────────┐
│ CODEY MONITOR    14:32 (30s)│   ← 渲染时刻 + 当前刷新间隔,一眼判断数据新旧
├─────────────────────────────┤
│ CLAUDE CODE                 │
│ 5h  ████████░░░░░░  52%     │   ← 额度进度条:纯 CSS div 黑色填充块
│ 周  ███░░░░░░░░░░░  23%     │
│ 3 active · 12.4k tok/min    │
│─────────────────────────────│
│ ● executing  Codey          │   ← 会话状态符号:● executing ◐ thinking
│   fable-5 · ctx 61% · feat/…│     ○ waiting ✓ done
│   修复 USB 日志乱码…        │
│ ○ waiting    meme           │
│   fable-5 · ctx 12% · main  │
├─────────────────────────────┤
│ CODEX                       │
│ 5h  ██░░░░░░░░░░░░  15%     │
│ (无活跃会话)                │
└─────────────────────────────┘
```

- 每 provider 一块:名称、5h/周额度条 + 百分比、`active_count`、`tokens_per_min`;
  `limited`(限流)时额度行加粗提示。
- 会话行:状态符号 + status 文本 + 项目名;第二行 model · ctx% · branch;第三行 summary。
  无会话显示"(无活跃会话)"。
- `state["stale"]` 为真时页头显示"数据可能过期"提醒。
- **所有用户来源字符串(summary/branch/model/项目名等)一律 `html.escape`。**

## 3. 组件与数据流

### 新模块 `companion/codey/kindle_page.py`(纯函数,~150 行)
```
render(state: dict, refresh_s: int) -> str   # 完整 HTML 文档字符串
```
- 内嵌 `<style>`(白底黑字、serif 大字号、单列),`<meta http-equiv="refresh" content="{refresh_s}">`。
- 只读 `state`,不 I/O、不改入参;缺键容错(`.get` + 默认值),坏数据不抛错。

### `server.py` 改动
- `do_GET` 加分支:`path in ("/kindle", "/kindle.html")` →
  `kindle_page.render(app.state(), config.get("kindle_refresh_s"))`,
  `text/html; charset=utf-8` 返回。
- `SCHEMA` 加 `kindle_refresh_s`:`{"type": "int", "min": 5, "max": 3600,
  "label": "Kindle 刷新间隔(秒)", "restart": False}` —— 管理台配置表单 schema 驱动,自动出现。

### `config.py` 改动
- `DEFAULTS` 加 `kindle_refresh_s: 30`。
- `get()` / `_validate()` 照 `refresh_ms` 模式:int 强转 + clamp [5, 3600],坏值回默认,永不抛错。

## 4. 错误处理

- 采集失败:`app.state()` 读缓存,已有兜底;页面照常渲染 + `stale` 提示。
- `render()` 对缺键/None 容错;任何会话字段缺失显示占位 `-`。
- 配置坏值:`config.py` clamp/回默认,与现有键一致。
- Kindle 连不上 Mac:浏览器自身报错,不在本设计范围。

## 5. 测试

- `companion/tests/test_kindle_page.py`(单测,纯函数):
  - meta refresh content 等于传入 `refresh_s`;
  - 输出含两个 provider 名称、额度百分比、会话状态符号与 summary;
  - summary/branch 含 `<script>` 等字符时被转义;
  - 空会话列表 → "(无活跃会话)";缺键 state → 不抛错;
  - `stale` 为真 → 页头含过期提示。
- `test_server_routes.py`:`GET /kindle` → 200 + `text/html`。
- `test_config.py`:`kindle_refresh_s` 默认 30、clamp 上下界、坏值回默认。
- 真机验证(用户手动):Kindle 浏览器打开页面,确认字号可读、meta refresh 生效、无排版错乱。

## 6. 不做的事(YAGNI)

- 不做 URL 参数覆盖刷新间隔、页内切换链接(用户选定:仅管理台配置)。
- 不做 ASR 历史展示(用户未选)。
- 不做 PNG 渲染、不加任何第三方依赖。
- 不做 Kindle 端交互(点击会话切 tab 等)——e-ink 浏览器只做只读展示。
