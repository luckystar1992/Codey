# Codey StopWatch UI 重构(对齐 abtop 数据模型)Design

**Date:** 2026-06-13
**Status:** Approved (brainstorming) → 待 writing-plans
**Branch base:** `feat/ngrok-remote-access`(当前 HEAD)

## Goal

把 abtop 级别的会话信息(状态 / 配额 / 上下文 / token 细分 / 任务摘要)以适合 **466×466 圆屏**、可一眼读取的方式搬上 Codey StopWatch,并把交互重构为「全触摸为主」。采用**渐进增强(方案 C)**:保留现有「主页 ⇄ 列表 ⇄ 详情」三级骨架与两个动态 mascot 动画(`drawClaude`/`drawCodex`),只升级信息密度、排序、提醒与交互。

设备仍只轮询**一个接口** `/codey/state`;所有"数据梳理"在 companion(Python)侧完成,对齐 abtop 的 `AgentSession` 数据模型语义,而非依赖 abtop 二进制(abtop 无 JSON 导出口)。

## 决策摘要(来自 brainstorming)

| 维度 | 决定 |
|---|---|
| 使用场景 | 桌面常驻 + 上手交互 两者均衡 → 首页可一眼读、详情页做厚 |
| 信息优先级 | 状态/谁在等我 + 配额 + 上下文/token 压力 + 任务内容(全选) |
| 交互 | 全触摸为主;按键只做亮屏/逐级返回/设置 |
| 提醒 | 声音(chime,已有)+ 视觉(等待横幅置顶)双提醒 |
| 双端 | 保留 Claude/Codex 双端,配置台可关某一端 |
| UI 方向 | 方案 C 渐进增强,保留两个 mascot 动画 |

---

## 架构总览

```
┌─────────────────────── companion (Python, :8787) ───────────────────────┐
│ transcript_claude.py  ── 已解析 token4 + first_prompt + ctx_tokens       │
│        │  (新增: compactions 计数)                                        │
│        ▼                                                                  │
│ build_session.py  ── session dict 新增 summary / tokens{} / compactions   │
│        │                                                                  │
│ derive.py  ── status 新增 rate_limited(provider 5h ≥100%)                │
│        ▼                                                                  │
│ state.py build_state ── provider 新增 limited;display 新增 summary/branch │
│        ▼                                                                  │
│ GET /codey/state  ── 设备唯一数据源(JSON)                               │
└──────────────────────────────────┬───────────────────────────────────────┘
                                   │  HTTP(LAN)/ HTTPS(ngrok)
                                   ▼
┌──────────────────────── firmware (codey_dash, 466 圆屏) ─────────────────┐
│ codey_ui.h  ── 纯函数(host-testable):新增 composeMetaLine / 新 sortRank │
│        ▼                                                                  │
│ codey_dash.ino                                                            │
│   renderUsagePage  ── + 等待横幅 + tok/min 速率                          │
│   renderListPage   ── 2 行 → 3 行卡片行;waiting 置顶高亮;拖动滚动        │
│   renderDetailPage ── + token4 行 + 摘要行 + ctx 压缩角标;WAITING 转橙   │
│   handleAction     ── 横幅点击直达;BtnA 短按逐级返回 / 长按设置          │
└──────────────────────────────────────────────────────────────────────────┘
```

---

## 组件 1:数据层(companion)

### 1.1 `transcript_claude.py` — 新增压缩计数(纯函数,可测)

解析每个 turn 的 `last_context_tokens` 序列时,记录相邻 turn 间上下文骤降事件:

- 维护上一 turn 的 ctx tokens;当 `cur < prev * 0.7`(掉 >30%)且 `prev > 0` 时 `compactions += 1`。
- 返回结构新增键 `"compactions": int`(默认 0)。

> 对齐 abtop `AgentSession.compaction_count`(其阈值同为 30%)。

### 1.2 `build_session.py` — session dict 契约扩展

`build_claude_session` / `build_codex_session` 返回 dict 新增:

| 键 | 值 | 来源 | 说明 |
|---|---|---|---|
| `summary` | str | `parsed["first_prompt"]` 首行,strip + 截 64 字符(UTF-8 安全) | abtop Summary 列;空则 `""` |
| `tokens` | obj | `{"in","out","cache_r","cache_w"}` | abtop token 四件套;各值取已解析的 `total_*` |
| `compactions` | int | `parsed["compactions"]` | ctx 块角标 |

`tokens_total` **保留不变**(旧固件 / 列表 Tok 列仍用),`tokens` 为新增并列字段——不破坏现有契约(不可变扩展)。Codex 端 `tokens.cache_w` 恒 0(其 rollout 无 cache_create)。

### 1.3 `derive.py` + `state.py` — RateLimited

- `state.py`:provider dict 新增 `"limited": bool`——当该 provider `session.used_pct >= 100` 时为 `True`(Claude 读 5h 窗,Codex 读其 session 窗)。
- 状态字符串集**不**改(设备侧把 `limited` 与会话 status 合成显示),避免动 `derive_status` 的纯函数契约与其测试。

### 1.4 `config.py` + 配置台 — 列开关扩展

`DEFAULTS["display"]["columns"]` 新增两键:

```python
"columns": {"status": True, "model": True, "ctx": True, "tokens": True,
            "memory": True, "turn": True,
            "summary": True, "branch": True},   # 新增
```

`web/admin.html` 配置标签页同步加这两个勾选项(沿用现有 display 绑定机制)。深合并保证旧 config.json 缺这两键时默认 `True`。

### 1.5 提醒(无新代码)

`chime.py` 已在会话 `executing/thinking → waiting/done` 跃迁时下发 chime 事件(声音侧✅)。视觉侧由固件主页等待横幅承担(组件 2)。**本层不改 chime。**

---

## 组件 2:主页(`renderUsagePage`)

保留:大 mascot 动画、`usage`/`weekly` 双表、provider 边弧、WiFi 状态、页点。

新增/改:

1. **等待横幅**(mascot 上方,top≈36):跨端扫描所有 session,若有 `waiting` 会话 → 显示橙色胶囊 `● <name> 等你输入`;多个 → `N 个会话在等你`;若该 provider `limited` → `⏳ RATE LIMITED`(红)。无等待会话则不绘制(让位给 mascot)。
2. **横幅点击直达**:`detectTouchAction` 命中横幅区域 → 进入该会话详情(跨端,设置 `detailProv`/`detailIdx`)。命中检测几何与绘制几何共用常量(同 `g_listBandTop` 模式)。
3. **速率行**:原会话数行 `a/b sessions` 末尾并入 `· <tok/min> tok/min`(数据来自 state 既有 `agg.tokens_per_min`)。

---

## 组件 3:列表页(`renderListPage`)— 核心改动

### 3.1 布局:2 行 → 3 行卡片行

- `ROW_H 40 → 64`,`LIST_MAX_VIS 6 → 4`(64×4=256,圆屏可视带内)。
- 每行三行文本:
  - **行 1**:状态点 + `name` + `branch +added~modified`(灰) + 状态词(右对齐,状态色)。
  - **行 2**:`model · ctx% · tokens · turn · memory` —— **点分内联串**,由列开关决定包含哪些段。
  - **行 3**:`summary`(斜体灰);若会话 `executing` 且有 `task` → 显示 `⚙ <task>`(当前工具/任务优先)。
- 行内文本超宽 → 截断 + 省略号(不做逐行跑马灯,避免 4 行同时滚动的视觉噪声;跑马灯只保留在详情页任务行)。

### 3.2 排序:waiting 置顶

- 新 rank:`waiting(0) < executing(1) < thinking(2) < done(3)`(当前是 executing 最前、waiting 垫底)。
- waiting 行加**橙色左边条 + 呼吸高亮**;executing 行保留现有整行高亮条。
- 同 rank 内按原 tie-breaker(started_at 等)稳定排序。

### 3.3 `layoutColumns` 退役(cleanup)

行 2 改内联点分串后,**aligned-column x 定位**(`layoutColumns` / `DISP_COL_W` / `DISP_BAND_CENTER` / `DISP_STATUS_WORD_DX`)在列表页不再使用。处理:

- 列**开关** `g_disp.col[]` 继续生效(决定行 2 包含哪些段、详情页显示哪些宫格)。
- 列**定位**函数 `layoutColumns` 及其几何常量若无其他引用 → 随本次重构删除(refactor-cleaner 口径:不留死代码)。删除前用 grep 确认无引用。
- `codey_ui.h` 对应的 `layoutColumns` 主机测试一并移除;新增 `composeMetaLine` 测试替代。

### 3.4 新纯函数(`codey_ui.h`,host-testable)

```c
// 按列开关把会话指标拼成行 2 的点分串(UTF-8 安全,截断到 n-1)。
// 段顺序固定:model, ctx%, tokens, turn, memory;关掉的段跳过;首段不加前导分隔。
int composeMetaLine(const DispCfg* cfg, const char* modelShort, int ctxPct,
                    const char* tokensStr, int turn, const char* memStr,
                    char* out, size_t n);
```

`statusRank` 改为 waiting 优先(更新其单元测试期望)。

---

## 组件 4:详情页(`renderDetailPage`)

保留:边缘弧=ctx%、中号 mascot 动画(0.62)、三宫格(CTX/TURN/TOKENS)、任务跑马灯、git/subagents/ports 行、底部位置点。

新增/改:

1. **token 四件套行**(三宫格下方,top≈196):`in <a> · out <b> · cR <c> · cW <d>`,值用现有 `fmtTokens` 压缩(K/W/B)。数据来自 state 新增 `tokens{}`;Codex 的 cW 省略。
2. **摘要行**(token4 下方,top≈216):`summary` 斜体灰,截断省略号。
3. **CTX 宫格角标**:`compactions > 0` 时在 CTX 块右上角标 `<n>c`(橙)。
4. **WAITING 转橙**:状态词 WAITING 由灰(`0x8b9097`)改橙(`0xffa94d`),与主页等待横幅、列表高亮一致(视觉语言统一)。
5. 头部第二行 `model · 时长` 末尾并入 `· <branch>`(若 BRANCH 列开)。

Sess 结构(`session_store.h`)新增字段以承接:`char summary[64]`、`long tokIn/tokOut/tokCacheR/tokCacheW`(与现有 `tokTotal` 同为 `long`,继承其 int32 上限——非本次回归)、`int compactions`。`MAX_SESS` 不变(12/端)。

---

## 组件 5:交互(`handleAction` / 按键)

触摸手势**不变**:横滑切端 / 详情翻会话、上滑进列表、下滑返回、点行下钻、拖动滚动。

新增/改:

- **新**:主页等待横幅点击 → 跨端直达该会话详情。
- **改**:`btnAShort` = 逐级返回(详情→列表→主页;**息屏时先亮屏**);`btnALong` = 进入设置页。(原:短按切端、长按返回——切端已由横滑承担。)
- 语音键 `BtnB` 与现有语音桥逻辑**不动**。
- 摇晃手势:保留现状(不在本次范围)。

---

## 数据流

1. companion 每 tick:`collect → build_session(+summary/tokens/compactions) → build_state(+limited/display) → /codey/state`。
2. 设备 `netTask` 轮询 state → 解析进 `Sess[]` + `Prov`(新增字段)→ 主 loop `render()` 按页绘制。
3. chime 事件经 state `chime` 字段下发(已有)→ 设备响铃;视觉提醒由主页横幅(本地从 `Sess[].status` 计算,无需新 state 字段)。

---

## 错误处理

- companion:`summary`/`tokens` 解析失败 → 安全默认(`""` / 全 0);不抛到 state 构建。沿用现有 `_int`/`clamp_pct` 容错。
- 固件:state 缺新字段(旧 companion)→ 字段默认(summary 空、tokens 全 0、compactions 0、limited false),UI 退化为"不显示该增量",不崩。ArduinoJson 取键用带默认的 `| 0` / `| ""`。
- 横幅点击命中越界 / 目标会话已消失 → 回退主页(`detailIdx` clamp)。

---

## 测试

遵循 host-testable 纯函数优先:

| 层 | 测试 | 文件 |
|---|---|---|
| companion | compaction 计数(掉 >30% / 边界 / prev=0) | `tests/test_transcript_claude.py` |
| companion | build_session 含 summary/tokens/compactions;Codex cW=0 | `tests/test_build_session.py`(或现有) |
| companion | state 含 provider.limited;display 含 summary/branch | `tests/test_state.py` |
| companion | config 默认/合并新增两列开关 | `tests/test_config.py` |
| firmware | `composeMetaLine` 列开关组合 / 截断 / UTF-8 | `sketches/codey_dash/test/codey_ui_test.cpp` |
| firmware | `statusRank` waiting 优先 | 同上 |
| firmware | 编译过 + 主机测试全绿 | `scripts/build.sh` + `run_tests.sh` |

真机验证(USB 接上后,与现有分支一并):刷机 → 三页观感 / 等待横幅 / 横幅点击直达 / token4 行 / 排序。

---

## 范围边界(YAGNI)

**做**:上述 5 个组件的增量。
**不做**(本次明确排除):工具调用时间线可视化、chat 尾巴、文件访问审计、主页改雷达/表圈(那是方案 A/B)、摇晃手势重做、新设备页(MCP servers / orphan ports 独立页)。

## Self-Review 待办(写完文档后执行)

- [ ] 占位扫描:无 TBD/TODO
- [ ] 一致性:`layoutColumns` 退役与"列开关保留"不矛盾(开关留、定位删)——已澄清
- [ ] 范围:单一实现计划可覆盖,无需拆子项目
- [ ] 歧义:行 2 内联串 vs 列对齐——已明确选内联串
