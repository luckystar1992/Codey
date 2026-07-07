# 管理台 Kindle 预览 tab — 设计

> 状态:已批准设计,待 writing-plans 拆实施计划
> 分支:`feat/kindle-monitor-page`(接在 Kindle 监视页面之后)
> 日期:2026-07-07

## 0. 背景与目标

Kindle 监视页面(`GET /kindle`,零 JS 服务端渲染 e-ink 页)已实现。现希望在 Mac 端的
companion 管理台(`companion/web/admin.html`)里能**直接预览 Kindle 上的实际显示效果**,
外观模仿 Kindle Paperwhite,免得每次都走到真机前看。

管理台现有 tab:欢迎 / 教学 / 配置 / 设备镜像 / 识别历史。其中「设备镜像」tab 已是
**懒加载 iframe 模式**(切到该 tab 时插入 `<iframe src="/sim?live=1">`),Kindle 预览完全照此套路。

## 1. 方案选择

- **A(选定):Paperwhite 外壳 + 真实视口缩放。** 画 6″ Paperwhite 深灰黑外壳,内嵌
  `<iframe src="/kindle">` 按 Kindle 真实逻辑宽 758px 渲染,再 `transform:scale` 等比缩小放入外壳。
  换行/字体比例与真机一致(能提前看出排版问题),一眼可辨"这是 Kindle 视图"。
- B(否):简单灰框屏幕。实现最简但看不出是 Kindle。
- C(否):外壳 + PW1/2·PW3 尺寸切换按钮。两者都是 6″ 3:4,单一 758px 已够,多余。

## 2. 组件与数据流

**唯一改动文件:`companion/web/admin.html`**(现 314 行,加约 40 行 CSS+HTML,不新增 JS 框架)。

- **nav**:在「设备镜像」按钮后加 `<button data-tab="kindle">Kindle 预览</button>`。
- **新 section** `<section id="kindle">`:
  - `.kframe`——Paperwhite 外壳:深灰黑(如 `#3a3d42`)圆角矩形 bezel,内边距形成边框,
    下巴区居中小写 `kindle` 浅色 logo 文字。
  - `.kscreen`——屏幕窗口:固定 **758×1024 逻辑像素**乘缩放系数的实际尺寸,`overflow:hidden`,
    白底(未加载时的占位背景)。
  - iframe:`width:758;height:1024`,CSS `transform:scale(0.45);transform-origin:top left`,
    实际占位 341×461。内容超一屏在屏幕窗口内滚动(与真机一致)。
- **数据流**:`/kindle` 与 `/admin` 同源同服务,iframe 嵌入无跨域/CSP 问题。
  `/kindle` 自带 `<meta http-equiv="refresh" content="{kindle_refresh_s}">`,iframe 会**自行按
  管理台配置的间隔整页刷新**,预览自动跟随,无需管理台侧任何轮询 JS。
- **懒加载**:照现有 `mirrorLoaded` 模式加 `kindleLoaded` 标志,`showTab('kindle')` 首次进入才
  插 iframe,避免每次切 tab 重载。

### 缩放系数
默认 `0.45`(预览约 341×461,与现有 mirror iframe 的 480×520 尺寸相当,舒适落在管理台面板内)。

## 3. 错误处理

- iframe 加载失败(服务未起等)→ 浏览器自身错误页,隔离在屏幕窗口内,不影响管理台其余 tab。
- `/kindle` 服务端已容错(永不抛错),此处无新增错误路径。

## 4. 测试

admin.html 是静态文件(经 `server.read_static` 原样返回),项目无 JS 测试框架,现有测试不校验其内容。

- `companion/tests/test_server_routes.py` 加一个轻量回归守卫:读取实际
  `os.path.join(server.WEB_DIR, "admin.html")`,断言正文含 `data-tab="kindle"` 与 `src="/kindle"`
  (锁住预览 tab 与 iframe 指向,防未来误删/改错)。
- 视觉效果(外壳观感、缩放比例、自动刷新)由用户在 Mac 浏览器打开 `/admin` → Kindle 预览 tab 目视确认。

## 5. 不做的事(YAGNI)

- 不做 PW3 1072px 切换按钮。
- 不做截图/导出、拟真高光阴影特效。
- 不改任何后端;不动 `/kindle` 渲染逻辑。
