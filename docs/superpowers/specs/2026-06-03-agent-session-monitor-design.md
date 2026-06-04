# Codey 实时 Agent 会话监视器 — 设计文档

- 日期:2026-06-03
- 状态:已通过 brainstorming,待写实现计划
- 目标设备:M5Stack StopWatch(C152, ESP32-S3, 1.75" AMOLED **触摸**圆屏 466×466)

## 1. 背景与目标

当前 Codey 只展示**账号级**用量(Claude/Codex 的 5h/周额度 % + reset + 最常用模型),由 `companion/server.js` 被动读取 statusline 文件与 codexbar 缓存,经 `GET /codey/state`(+ WebSocket 推送)送到手表。

参考开源项目 abtop(`/Users/zyc/code/abtop`,Rust TUI)的采集算法,把 Codey 升级为**实时 agent 会话监视器**:在圆屏上同时呈现此刻正在运行的每个 Claude Code / Codex 会话的基础信息。所有数据仍只读本地文件 / 进程,无 API key。

### 成功标准
- 圆屏首页一眼看到:两端额度、活跃会话数、各会话概览。
- Claude / Codex 各有一页**列出全部已启动会话**,每会话显示 name、status、model、context、token、turn。
- 会话超过一屏时可**触屏上下滑动**滚动。
- 点按某会话进入**单会话详情**,看到更全字段。
- 保留现有的吉祥物动画与表盘页。

## 2. 数据采集(移植 abtop 算法到 **Python** companion)

> 实现说明(2026-06-04 更新):companion 由 Node 改为**纯 Python 标准库**实现(`companion/codey/` 包 + `codey_companion.py` 入口),**零 pip 依赖、零联网**——删除了原 ccusage(npx)这一唯一外连。账号额度只读本地 `~/.claude/codey-usage.json`(statusline 截获)与 codexbar 本地缓存;`/codey/state` 契约与端口 8787 不变,固件零改动。原 Node 实现(server.js + lib)及其 TDD 计划(`plans/2026-06-03-companion-session-collectors.md`)已被本 Python 版取代,仅留作历史。

全部在 Mac 上读本地文件 / 跑 `ps`/`lsof`/`git`。abtop 原实现假设 Linux `/proc`,移植时改用 macOS 等价手段。

### 2.1 会话发现
- **Claude**:扫 `~/.claude*/sessions/*.json`(字段 `pid`/`sessionId`/`cwd`/`startedAt`);对应 transcript 在 `~/.claude/projects/{encoded-cwd}/{sessionId}.jsonl`。`/clear` 后 sessionId 失效 → 在该 project 目录里选 mtime ≥ startedAt(5s 宽限)且未被其他 session 占用的最新 `.jsonl`。仅保留 pid 存活且为 `claude` 进程的会话。
- **Codex**:扫 `~/.codex/sessions/YYYY/MM/DD/rollout-*.jsonl`;经 `lsof` 把运行中的 `codex` 进程映射到打开的 rollout 文件;桌面版会话取 mtime < 30 分钟且 `originator:"Codex Desktop"`;刚结束的会话取 mtime < 5 分钟。
- **进程/端口**(macOS 适配):`ps -axo pid,ppid,rss,pcpu,comm` 建进程树;`lsof -nP -iTCP -sTCP:LISTEN` 建 pid→端口映射。递归收集会话 pid 的全部后代。
- **配置目录发现**:默认 `~/.claude`;`CLAUDE_CONFIG_DIR` 环境变量;尽力从 `ps eww -p <pid>` 解析各进程的 `CLAUDE_CONFIG_DIR`(best-effort)。

### 2.2 每会话核心字段(列表页用)
| 字段 | 来源 / 算法 |
|---|---|
| **name** | 项目名 = cwd 末段;超长在固件侧用 `…` 缩略 |
| **status** | `Executing`(有后代 CPU>5% 或末轮 tool_use 未闭合) / `Thinking`(末尾是真实 user 行、尚无 assistant 回复;跳过 tool_result、`<command-name>`、`isMeta` 等合成消息) / `Waiting`(其余) |
| **model** | transcript 末条 `assistant.message.model`;固件侧美化短名(Opus4.8 / Son4.6 / Haiku…) |
| **context %** | 末轮 `input_tokens + cache_read_input_tokens`(若 cache_read==0 且 cache_creation>0 则用 input+cache_creation)÷ 上下文窗口。窗口:模型名含 `[1m]` 或 max>200k → 1_000_000,否则 200_000 |
| **token** | 累计 `input+output+cache_read+cache_create`(Codex 的 `input_tokens` 已含 cached,需减去 `cached_input_tokens` 再单列 cache_read) |
| **turn** | assistant 轮次计数 |

### 2.3 详情页额外字段
- **current_task / tool**:末轮最后一个 `tool_use` → `"{tool} {arg}"`(Read/Edit/Write 取路径末两段;Bash 取首行命令;Grep/Glob 取 pattern)。
- **git**:`git -C {cwd} rev-parse --abbrev-ref HEAD` + `git -C {cwd} diff --shortstat`(added/modified);慢 tick(~10s)缓存。
- **subagents**:`~/.claude/projects/{cwd}/{sessionId}/subagents/*.meta.json` → 名称(description 截 30)、对应 `.jsonl` 的 token 累计、status(mtime<30s=working 否则 done)。
- **ports / children**:见 2.1 进程树。
- **first prompt / effort / cache / compaction / 时长 / 内存(CLAUDE.md)**:按 abtop 算法移植(effort 走 settings 优先级链;compaction = 相邻轮 context 跌 >30% 且 cache 失效)。

### 2.4 解析与刷新策略
- **增量解析**:JSONL 按文件 identity(inode+mtime)+ offset 增量读,只解析新增行,合并 delta 进缓存;文件被替换/截断则全量重读。
- **tick**:快 tick ~2s(status、token、context、端口);慢 tick ~10s(git、内存、配置目录发现)。
- 账号额度仍走现有 statusline 文件 + codexbar 缓存(供边缘弧使用),保留 ccusage 兜底。

### 2.5 `/codey/state` JSON schema(扩展现有)
```jsonc
{
  "ts": 1780000000,
  "providers": [
    {
      "id": "claude", "name": "Claude Code",
      "session": { "used_pct": 38, "reset_epoch": 0 },   // 账号额度(边缘弧)
      "weekly":  { "used_pct": 61, "reset_epoch": 0 },
      "model": "Opus 4.8",
      "active_count": 3,
      "agg": { "dirty_repos": 4, "tokens_per_min": 94000 },
      "sessions": [
        {
          "id": "<sessionId>", "name": "Codey",
          "status": "executing",        // executing|thinking|waiting|done(done=最近完成的 Codex 会话,5min 内;不计入 active_count)
          "model": "Opus 4.8",
          "context_pct": 47, "context_tokens": 94000, "context_window": 200000,
          "tokens_total": 1234567, "turn": 23,
          "git": { "branch": "main", "added": 3, "modified": 12 },
          "current_task": "Edit server.js:218",
          "subagents": 2, "ports": [3000, 5173],
          "started_at": 1780000000, "effort": "high"
        }
      ]
    },
    { "id": "codex", "name": "Codex", "...": "同上结构(绿)" }
  ]
}
```
排序:active(executing/thinking)优先,再按 context_pct 降序。列表页缺省每页渲染若干个,其余靠触屏滚动看到全部。

## 3. 页面结构(BtnA 短按切页)

1. **仪表盘(首页)**
   - 边缘弧拆两段:左上 = Claude 额度(橙),右上 = Codex 额度(绿)。
   - 双吉祥物(橙/绿,均带 idle 动画)夹住中央活跃计数 `3·2`。
   - 会话色块网格:跨两端 top N(状态图标 ▶执行/◆思考/⏸等待 + 项目名 + ctx%),溢出显示 `+N more`。
   - 底部聚合:`⎇ 脏仓库数 · token/min`,页点。
2. **Claude 会话列表**
   - 边缘弧 = Claude 额度;头部小吉祥物 + `CLAUDE · N`。
   - **两行 / 会话**:行1 = 状态图标 + 名称(超长 `…`) + 状态词;行2 = `model · ctx% · token · turn`。
   - 超屏:**触屏上下滑动**滚动。
3. **Codex 会话列表**:同上,绿。

> 注:原「表盘(模拟时钟)」页已去掉(2026-06-04)——它依赖农历/生肖数据,而 Python companion 不再提供这些字段。

## 4. 单会话详情页(保留)
- 边缘弧 = 该会话 context%。
- 头部:provider 点 + 项目名 + 第二行(model · 已运行时长 · turn)。
- 中央**大吉祥物**(表情/动画随 status 变)+ 状态词。
- 信息行:① 当前工具 `✎` ② git `⎇ branch +a ~m` ③ `▦ ctx k/window · out k` ④ `⬡ subagents · :ports` ⑤(可选)首条 prompt。
- 底部:会话位置点(第 i / N)。

## 5. 交互模型

| 输入 | 行为 |
|---|---|
| BtnA 短按 | 切页:仪表盘 → Claude 列表 → Codex 列表(循环) |
| BtnB 短按 | 语音命令(不变) |
| 双键长按 ~0.4s | 设置页(不变) |
| **触屏上下滑动** | 列表页滚动会话(新增能力) |
| **触屏点按会话行** | 进入该会话详情(新增能力) |
| BtnA 长按 | 按键兜底:列表页进置顶/高亮会话详情;详情页返回列表 |
| 详情页 BtnA 短按 | 翻下一个会话 |

固件侧需启用 `M5.Touch`(`M5.Touch.getDetail()`),实现垂直滑动惯性滚动与点按命中检测;触摸为本次新增交付。

## 6. 固件改动概要(`sketches/codey_dash/codey_dash.ino`)
- 新增仪表盘页与两个会话列表页的渲染函数;复用现有 `drawArc` / 吉祥物 / 真黑底风格。
- 扩展 `/codey/state` JSON 解析:provider 增加 `sessions[]`、`active_count`、`agg`。
- 引入触摸输入(滚动状态、命中检测、详情进入/退出)。
- 名称缩略、model 美化短名在固件侧完成。
- 列表/网格用小字号 + 既有 M5Canvas 离屏合成,避免闪烁。

## 7. 风险与权衡
- **macOS 进程环境读取**:`CLAUDE_CONFIG_DIR` 跨进程读取受限,非默认配置目录可能漏检 → 退化为默认 `~/.claude`。
- **JSONL 体量**:大 transcript 增量解析必须用 offset 缓存,否则每 tick 全量读会拖慢。详情字段(subagents/首条prompt)较重,放慢 tick / 仅在进入详情时拉取。
- **圆屏可视区**:466 圆内可用宽约 ~330px,两行/会话约容纳 3–4 个,其余靠滚动;字号需实测。
- **触摸为新模块**:需在真机验证滑动手感与误触(与现有摇晃手势、双键不冲突)。
- **会话数为 0**:各页需有空态(吉祥物 idle + “no active sessions”)。

## 8. 不在本次范围(YAGNI)
- OpenCode 支持、文件访问审计明细、聊天回放、主题切换、host 级 CPU/内存。
