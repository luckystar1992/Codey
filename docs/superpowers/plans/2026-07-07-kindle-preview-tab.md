# 管理台 Kindle 预览 tab Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 管理台 `admin.html` 新增「Kindle 预览」tab——Paperwhite 外壳内嵌 `<iframe src="/kindle">` 按 758px 真实视口渲染再等比缩放,让用户在 Mac 浏览器里看到 Kindle 上的实际显示效果。

**Architecture:** 纯静态增量,照现有「设备镜像」tab 的懒加载 iframe 模式,只改 `companion/web/admin.html` 一个文件(加 CSS + nav 按钮 + section + showTab 懒插分支)。无后端改动。加一个字符串级回归守卫测试锁住 tab 与 iframe 指向。

**Tech Stack:** 静态 HTML/CSS + 原生 JS(无框架);pytest(回归守卫)。

**Spec:** `docs/superpowers/specs/2026-07-07-kindle-preview-tab-design.md`

## Global Constraints

- 只改 `companion/web/admin.html`;不动任何后端、不改 `/kindle` 渲染逻辑。
- 缩放系数 **0.45**:iframe `width:758;height:1024` → 实际 341×461;`.kscreen` 固定 341×461 + `overflow:hidden`;`.kframe` 宽 = 341 + 左右各 20 padding = 381。
- 自动刷新靠 `/kindle` 自带的 `<meta refresh>`,管理台侧**不加任何轮询 JS**。
- 懒加载:照现有 `mirrorLoaded` 加 `kindleLoaded` 标志,首次进入 tab 才插 iframe。
- 同源嵌入,无跨域/CSP 处理。
- 测试从 `companion/` 目录跑:`cd /Users/zyc/code/Codey/companion && python3 -m pytest tests/... -v`。

---

### Task 1: admin.html 新增 Kindle 预览 tab + 回归守卫

**Files:**
- Modify: `companion/web/admin.html`(CSS ~L57 后、nav ~L72 后、section ~L133 后、JS state ~L152、showTab ~L164 后)
- Test: `companion/tests/test_server_routes.py`(`TestServerRoutes` 类末尾追加)

**Interfaces:**
- Consumes: 已实现的 `GET /kindle`(返回零 JS e-ink HTML,自带 meta refresh);`server.WEB_DIR`、`server.read_static`(已存在)
- Produces: 无下游任务(终态 tab)

- [ ] **Step 1: 写失败测试**

在 `companion/tests/test_server_routes.py` 的 `TestServerRoutes` 类末尾(`test_static_bytes_and_ctype` 之后、或 `/kindle` 相关测试之后)追加:

```python
    def test_admin_html_has_kindle_preview_tab(self):
        # 回归守卫:锁住管理台里的 Kindle 预览 tab 与 iframe 指向,防未来误删/改错
        body, ctype = server.read_static(os.path.join(server.WEB_DIR, "admin.html"))
        self.assertIsNotNone(body)
        text = body.decode("utf-8")
        self.assertIn('data-tab="kindle"', text)   # nav 按钮存在
        self.assertIn('src="/kindle"', text)        # iframe 指向 /kindle
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cd /Users/zyc/code/Codey/companion && python3 -m pytest tests/test_server_routes.py -v -k kindle_preview`
Expected: FAIL(`'data-tab="kindle"' not found` —— 尚未加入)

- [ ] **Step 3a: 加 CSS**

`companion/web/admin.html` 中,找到:

```css
  /* mirror + history (existing) */
  .mirror iframe{width:480px;height:520px;max-width:100%;border:0;border-radius:14px;background:#000}
```

在这一行**之后**插入:

```css
  /* kindle preview: Paperwhite 外壳 + 758px iframe 缩放 (scale .45 → 341×461) */
  .kframe{width:381px;max-width:100%;margin:4px 0;background:#3a3d42;border-radius:22px;
    padding:20px 20px 8px;box-shadow:0 6px 20px rgba(0,0,0,.4)}
  .kscreen{width:341px;height:461px;overflow:hidden;background:#fff;margin:0 auto;border-radius:2px}
  .kscreen iframe{width:758px;height:1024px;border:0;background:#fff;
    transform:scale(.45);transform-origin:top left;display:block}
  .klogo{text-align:center;color:#9aa0a6;font-size:13px;letter-spacing:1px;padding:6px 0 2px}
```

- [ ] **Step 3b: 加 nav 按钮**

找到:

```html
    <button data-tab="mirror">设备镜像</button>
    <button data-tab="history">识别历史</button>
```

替换为(在两者之间插入 Kindle 预览):

```html
    <button data-tab="mirror">设备镜像</button>
    <button data-tab="kindle">Kindle 预览</button>
    <button data-tab="history">识别历史</button>
```

- [ ] **Step 3c: 加 section**

找到设备镜像 section 结尾与识别历史 section 之间:

```html
  <!-- ============ 设备镜像 ============ -->
  <section id="mirror">
    <div class="card"><h2>设备镜像</h2><p class="muted">实时镜像手表圆屏(与设备同源渲染)。</p></div>
    <div class="mirror" id="mirrorbox"></div>
  </section>

  <!-- ============ 识别历史 ============ -->
```

替换为(在两个 section 之间插入 Kindle 预览 section):

```html
  <!-- ============ 设备镜像 ============ -->
  <section id="mirror">
    <div class="card"><h2>设备镜像</h2><p class="muted">实时镜像手表圆屏(与设备同源渲染)。</p></div>
    <div class="mirror" id="mirrorbox"></div>
  </section>

  <!-- ============ Kindle 预览 ============ -->
  <section id="kindle">
    <div class="card"><h2>Kindle 预览</h2>
      <p class="muted">模仿 6″ Paperwhite 显示 <code>/kindle</code> 监视页,按管理台配置的间隔自动刷新
        (iframe 内 <code>&lt;meta refresh&gt;</code>)。字号/换行按 Kindle 真实 758px 逻辑宽渲染再等比缩放。</p></div>
    <div class="kframe"><div class="kscreen" id="kscreen"></div><div class="klogo">kindle</div></div>
  </section>

  <!-- ============ 识别历史 ============ -->
```

- [ ] **Step 3d: 加 JS 懒加载状态 + 分支**

找到:

```javascript
let curTab='welcome', histTimer=null, mirrorLoaded=false, cfgLoaded=false;
```

替换为:

```javascript
let curTab='welcome', histTimer=null, mirrorLoaded=false, kindleLoaded=false, cfgLoaded=false;
```

再找到 showTab 里的设备镜像懒插分支:

```javascript
  // 设备镜像:首次进入才插入 iframe(避免每次切换重载)
  if(tab==='mirror' && !mirrorLoaded){
    document.getElementById('mirrorbox').innerHTML =
      '<iframe src="/sim?live=1" title="device"></iframe>';
    mirrorLoaded=true;
  }
```

在这个 `if` 块**之后**插入:

```javascript
  // Kindle 预览:首次进入才插入 iframe;/kindle 自带 meta refresh 自动刷新
  if(tab==='kindle' && !kindleLoaded){
    document.getElementById('kscreen').innerHTML =
      '<iframe src="/kindle" title="kindle"></iframe>';
    kindleLoaded=true;
  }
```

- [ ] **Step 4: 跑测试确认通过**

Run: `cd /Users/zyc/code/Codey/companion && python3 -m pytest tests/test_server_routes.py -v -k kindle_preview`
Expected: PASS

- [ ] **Step 5: 全量回归**

Run: `cd /Users/zyc/code/Codey/companion && python3 -m pytest tests/ -q`
Expected: 全部 PASS(160 passed)

- [ ] **Step 6: 冒烟目视(手动)**

```bash
cd /Users/zyc/code/Codey/companion && python3 codey_companion.py
```

Mac 浏览器打开 `http://127.0.0.1:8787/admin` → 点「Kindle 预览」tab,确认:出现 Paperwhite 深灰外壳 + 下巴 `kindle` logo,屏幕窗口内是真实 `/kindle` 页(白底黑字、额度条、会话列表),排版按 758px 宽缩放呈现;等一个刷新周期(默认 30s)看是否自动刷新。

- [ ] **Step 7: Commit**

```bash
cd /Users/zyc/code/Codey && git add companion/web/admin.html companion/tests/test_server_routes.py && git commit -m "feat(companion): 管理台新增 Kindle 预览 tab(Paperwhite 外壳 + iframe 758px 真实视口缩放,懒加载)"
```

---

## Self-Review 记录

- **Spec 覆盖**:§2 nav 按钮/section/kframe·kscreen·klogo/iframe 758px scale .45/懒加载 kindleLoaded → Step 3a-3d;缩放系数 0.45 与尺寸(341×461/381)→ Global Constraints + CSS;自动刷新靠 meta refresh 不加轮询 → Step 3d 注释 + section 文案;§4 回归守卫测试 → Step 1;§5 YAGNI 项均未引入。无缺口。
- **占位符**:无 TBD/TODO;每处改动给了完整前后代码块。
- **一致性**:`kscreen` id 在 section(3c)与 JS(3d)一致;`kindleLoaded` 在 state(3d)与分支(3d)一致;测试断言 `data-tab="kindle"`、`src="/kindle"` 与实际插入文本逐字对应。
