# Kindle 显示自定义面板 — 设计

> 状态:已批准设计,待 writing-plans 拆实施计划
> 分支:`feat/kindle-monitor-page`(接在独立 Kindle 服务之后)
> 日期:2026-07-07

## 0. 背景与目标

`/kindle` 监视页在真机 Kindle Paperwhite(212–300ppi)上字号偏小看不清。用户要求:能在
**Mac 管理台的 Kindle 预览 tab 右侧加一排控件**,实时调节字号、行距、字体、配色、刷新间隔,
以及**各显示区块单独调字号**,并把重点信息加粗;调节后左侧预览即时反映效果。

所有设置持久化到配置 → **真机 Kindle 下次 meta refresh 自动同步**(在 Mac 上校准,Kindle 生效)。

背景约束不变:目标浏览器是早期 Kindle 实验性 WebKit —— 页面零 JS、`<meta refresh>` 自动刷新、
e-ink 高对比、单列布局;渲染纯函数、缺键/坏值容错、永不抛错。

`kindle_refresh_s` 目前是顶层配置键且**尚未合并进 main**,本设计将其并入新的 `kindle` 配置组
(`kindle.refresh_s`),统一管理,无 back-compat 负担。

## 1. 配置模型:嵌套 `kindle` 组

照现有 `display` 组的嵌套模式,在 `config.DEFAULTS` 新增:

```python
"kindle": {
    "refresh_s": 30,            # int, clamp [5, 3600]
    "font_scale": 1.5,         # float, clamp [1.0, 3.0]
    "line_height": 1.45,       # float, clamp [1.0, 2.2]
    "font_family": "serif",    # select: serif | sans | mono
    "theme": "light",          # select: light(黑字白底) | dark(白字黑底)
    "bold_emphasis": True,     # bool
    "sizes": {                 # 各区块基准字号(px),渲染时再 × font_scale
        "title": 21,           # 页头(标题+时间)
        "provider": 20,        # provider 名(Claude/Codex)
        "quota": 19,           # 额度行(5h/周 + %)
        "session1": 19,        # 会话主行(状态 + 项目名)
        "session2": 17,        # 会话次行(模型/ctx/分支/摘要)+ 聚合行
    },
}
```

各 `sizes.*` clamp `[12, 48]`。坏值/非法枚举一律回默认,永不抛错。

## 2. 组件与数据流

### 2.1 `codey/config.py`
- `DEFAULTS` 加 `kindle` 组;删顶层 `kindle_refresh_s` 与 `_INT_CLAMPS` 里的对应项。
- 新增 `_coerce_float(v, default)` 与 `_FLOAT_CLAMPS`(供 `font_scale`/`line_height`)。
- 新增 `_merge_kindle(file_kindle)`:以 `DEFAULTS["kindle"]` 深拷贝为底,叠加 file 中合法字段
  (数值 clamp、select 校验、bool 强转、`sizes` 逐键 clamp);未知键忽略。
- `get("kindle")` 走 `_merge_kindle`;`get("refresh_ms")` 等其余不变。
- `_validate` 增 `kindle` 分支:从 partial 提取合法子集(同样 clamp/校验)。
- `save` 增 `k == "kindle"` 深合并分支:标量字段覆盖、`sizes` 子 dict 深合并(保留其余分区字号)。

### 2.2 `codey/kindle_page.py`
- 签名改为 `render(state, kindle, now=None)`,`kindle` 为解析后的配置 dict;缺字段用内置默认。
- CSS 由配置算出:
  - 各区块字号 = `round(sizes[区块] × font_scale)` px;`line-height = line_height`。
  - `font_family`:serif→`Georgia,serif`;sans→`Helvetica,Arial,sans-serif`;mono→`"Courier New",monospace`。
  - `theme`:light→`bg #fff / fg #000`;dark→`bg #000 / fg #fff`;边框/进度条填充用 fg。
  - `bold_emphasis` 开→provider 名、额度 %、会话主行 `font-weight:bold`;关→常规。
  - `<meta refresh content=refresh_s>`。
- 纯函数、缺键容错、永不抛错、零 JS 不变。

### 2.3 `codey/server.py`
- `/kindle` 路由:`kindle_page.render(app.state(), config.get("kindle"))`。
- 主 `SCHEMA` 移除 `kindle_refresh_s`(kindle 设置只在预览面板调,不进「配置」tab)。

### 2.4 `web/admin.html` — Kindle 预览 tab 两列
```
┌ Kindle 预览 ───────────────────────────────┐
│ ╭Paperwhite╮  │ 显示设置                    │
│ │ /kindle  │  │ 刷新[30]s  字号●—1.5×       │
│ │ iframe   │  │ 行距●—1.45  字体[衬线▾]     │
│ │ 实时预览 │  │ 配色[浅▾]   重点加粗[✓]     │
│ ╰──────────╯  │ 分区字号 页头[21]端名[20]…  │
│               │ [恢复默认]                  │
└───────────────┴─────────────────────────────┘
```
- 控件值来自 `GET /codey/config` 的 `values.kindle`(config.all() 自带该组;非密钥不遮罩)。
- 控件 `change`(滑块松手触发,不刷屏)→ `POST /codey/config` 提交 `{kindle:{...}}`(后端深合并)
  → 成功后重载左侧 iframe(`kscreen` 内 iframe 重设 `src` 触发服务端重渲染)→ 即时显示。
- 「恢复默认」:POST 一份 `kindle` 默认值并重载。
- 控件在预览 tab 内**硬编码**(带实时数值标签的滑块/下拉,UX 优于通用 schema 渲染);窄屏自动上下堆叠。

### 数据流
控件改动 → POST /codey/config(复用现有接口,深合并 kindle 组)→ 重载预览 iframe → 服务端按新
`kindle` 配置重渲染 → 即时显示。真机 Kindle 靠自身 meta refresh 在下个周期拿到同一份配置。

## 3. 错误处理

- 所有数值 clamp、select 非法回默认、bool 强转:config 层,永不抛错。
- POST 失败:面板显示提示,预览不动(不重载)。
- `render` 对 `kindle` 缺字段/坏值用内置默认,永不抛错。

## 4. 测试

- `test_config.py`:kindle 组各字段 clamp(font_scale/line_height/refresh_s/sizes.* 上下界)、
  select 非法回默认、bool 强转、深合并保留未改分区字号;`kindle_refresh_s` 顶层键已移除。
- `test_kindle_page.py`:迁移到新签名 `render(state, kindle)`;断言不同 font_scale/分区字号 → 对应 px;
  line_height、font_family 映射、theme 反色(dark 出现 `#000`/`#fff` 对调)、bold 开/关的 `font-weight`;
  边界与缺键容错;仍零 `<script>`。
- `test_server_routes.py`:`/kindle` 200 且反映默认配置(`content="30"`);主 schema 不再含 `kindle_refresh_s`;
  `values.kindle` 存在于配置 payload。
- `test_server_routes.py` 回归守卫:admin.html 含预览面板控件标记(如 `name="kindle.font_scale"`)。
- 真机字号/配色是否舒服由用户在 Kindle 上滑动校准。

## 5. 不做的事(YAGNI)

- 不做字间距、边距、每会话逐条字号等更细粒度(分区五档已够)。
- 不做 URL 参数覆盖、不新增预览专属路由。
- 配色仅 light/dark 两档(e-ink 灰阶,深色反色会有全刷闪烁,已足够)。
- kindle 设置不进「配置」tab(只在预览面板),避免双入口。
