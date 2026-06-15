# 设备点屏 → 终端 pane 自动切换：技术说明

> 在 Codey StopWatch(M5Stack 圆屏设备)的**会话详情页点一下屏幕**,Mac 上对应那个 agent 会话的**终端 pane 会自动切到前台**。本文说明其完整技术原理。

---

## 1. 一句话原理

> **会话身份(session id)→ agent 进程(PID)→ 控制终端(TTY)→ 终端里的面板(pane)**。
>
> 设备只知道「我在看哪个会话」(session id);Mac 端逐级把它解析到一个具体的终端面板并切过去。每一级都是系统里**天然存在、稳定可查**的映射,不依赖任何额外注册或约定。

---

## 2. 整体架构

```
   ┌─────────────────── M5Stack StopWatch(固件) ───────────────────┐
   │  详情页 ACT_TAP(点屏)                                          │
   │     → g_focusSid = 当前会话 id ; g_netFocusReq = true           │
   │     → netTask: wsFocus(sid)                                     │
   └───────────────────────────┬────────────────────────────────────┘
                               │  ASR WebSocket(:8788,长连)
                               │  {"type":"focus","session":"<id>"}
                               ▼
   ┌─────────────────────── Companion(Mac, Python) ─────────────────┐
   │  asr_stream.handle():  t == "focus"                            │
   │     ① id → PID     App.pid_for_session(id)   [查 session_cache] │
   │     ② PID → TTY    ps -o tty -p <pid>        → /dev/ttysNNN     │
   │     ③ TTY → pane   kaku cli list --format json(按 tty_name 配) │
   │     ④ 切 pane      kaku cli activate-pane --pane-id N           │
   │     ⑤ 前置窗口     osascript: tell app "Kaku" to activate       │
   │     → 回 {"type":"focus_ack","ok":true,"reason":"kaku"}         │
   └───────────────────────────┬────────────────────────────────────┘
                               │  wezterm mux 协议(unix socket)
                               ▼
            Kaku.app(基于 wezterm)切到 tty 对应的 pane
```

核心思想:**复用已有的 ASR WebSocket 做上行**,**复用系统进程/终端的既有映射**做定位。没有新增端口、没有新增长连、没有让 agent 主动上报任何东西。

---

## 3. 分层详解

### 3.1 设备侧:点屏 → 发一条消息

详情页的「点击」手势(`ACT_TAP`)在其它页有各自语义(主页→列表、列表→详情),在**详情页此前是空操作**,正好用作「确认切换」:

- `sketches/codey_dash/codey_dash.ino` · `handleAction()`:详情页 `ACT_TAP` 时,把当前会话(`PROV[detailProv].sess[detailIdx].id`)拷进 `g_focusSid`,置 `g_netFocusReq = true`。
- `sketches/codey_dash/codey_net.h` · `netTask`(core 0):轮询到 `g_netFocusReq` 即 `wsFocus(g_focusSid)`,然后清标志。
- `sketches/codey_dash/codey_dash.ino` · `wsFocus()`:用现有的 `g_ws`(WebSocketsClient)发一帧文本
  `{"type":"focus","session":"<id>"}`。

为何这样切核:**主 loop 绝不做阻塞网络 IO**。点屏只在主 loop 里写两个变量(单写者),真正的 WS 发送交给 core 0 的 `netTask`——与语音 `listen` 上行完全同一套「标志位 + netTask 发送」模式(`g_netListenReq`/`wsListen` 的孪生)。

> 触发选择「点屏确认」而非「横滑即自动跟随」是用户的取舍:浏览会话时不抢占 Mac 焦点,只有明确点一下才切。

### 3.2 传输:复用 ASR WebSocket

设备启动后 `netTask` 就维持一条到 companion `:8788` 的 **WebSocket 长连**(本是给流式语音 ASR 用的)。它一直在线(不只录音时),所以「focus」帧随时可发。服务端 `asr_stream.handle()` 本就按 `data["type"]` 分发(`hello`/`listen`/`submit`/`clear`),加一个 `focus` 分支即可,零新增基础设施。

### 3.3 Companion:把 session id 解析到进程

- `companion/codey/server.py` · `App.pid_for_session(sid)`:在 `session_cache`(后台线程每 ~0.5s 刷新的 collect 结果)里按 `id` 找到会话,返回它的 `pid`。
- `pid` 从何来:`companion/codey/collect.py` 采集 Claude 会话时,`discover` 已给出该会话 agent 进程的 `d["pid"]`;经 `build_session.py` 写进会话对象的 `pid` 字段(本功能新增)。
- `companion/codey_companion.py` 启动时把 `app.pid_for_session` 注入 `asr_stream.set_pid_resolver(...)`,使 ASR 服务器(同进程的后台线程)能反查。

### 3.4 Companion:进程 → 控制终端(TTY)

`companion/codey/focus.py` · `tty_for_pid(pid)`:

```
ps -o tty= -p <pid>     →     ttys000     →     /dev/ttys000
```

**为什么 TTY 是关键锚点**:每个 agent 是 `claude → zsh → Kaku` 这样起的——claude 直接跑在某个 Kaku 面板的 shell 里,**继承了那个面板的控制终端**。所以「agent 的控制 TTY」唯一对应「它所在的终端面板」。这是 Unix 进程模型里天然、稳定的关系,无需任何额外约定。

(若进程没有控制终端,如在 GUI app 里跑,`ps` 返回 `??` → 返回 `no-tty`,功能优雅放弃。)

### 3.5 Companion:TTY → 终端面板(pane)并切换

用户的终端是 **Kaku**(`fun.tw93.kaku`),**基于 wezterm**,因此带 wezterm 的 mux 控制 CLI:

```bash
kaku cli list --format json      # 列出所有 pane,字段含 tty_name / pane_id / cwd / title …
kaku cli activate-pane --pane-id N
```

`companion/codey/focus.py` · `_kaku_focus(tty)`:

1. `kaku cli list --format json` → 解析 JSON;
2. 找 `tty_name == "/dev/ttysNNN"` 的那个 pane,取其 `pane_id`;
3. `kaku cli activate-pane --pane-id <id>` → mux 把该 pane 激活(切到它所在的 tab/window);
4. `osascript -e 'tell application "Kaku" to activate'` → 把 Kaku 窗口拉到 macOS 前台。

`kaku` 与 GUI 之间通过 wezterm 的 **unix domain socket**(`~/.local/share/kaku/gui-sock-<pid>`)通信,所以 CLI 能直接驱动正在运行的窗口。

> **走 CLI 而非 AppleScript** 的好处:① 不需要「系统设置→隐私→自动化」授权;② wezterm 的 pane 模型本就暴露 `tty_name`,匹配精确到面板。最初按 iTerm/Terminal 写的 AppleScript 因这两个终端**根本没运行**而全部 `tab-not-found`——发现真实终端是 Kaku 后才改用此法。iTerm2 / Terminal.app 的 AppleScript 路径作为**回退**保留(`_iterm_focus` / `_terminal_focus`,按 `tty of session/tab` 匹配)。

---

## 4. 端到端映射链(带实例)

| 级别 | 数据 | 由谁解析 |
|---|---|---|
| 会话身份 | `session=f95b6d99-…` | 设备发送 |
| → 进程 | `pid=44791` | `App.pid_for_session`(查 session_cache) |
| → 终端 | `tty=/dev/ttys000` | `ps -o tty -p 44791` |
| → 面板 | `pane_id=1`(Meridian) | `kaku cli list` 里 `tty_name` 匹配 |
| → 动作 | 激活 + 前置 | `kaku cli activate-pane` + osascript |

日志(`companion/data/companion.log`)对应一行:
```
[focus] session=f95b6d99-… pid=44791 -> True (kaku)
```

---

## 5. 时序

```
设备            ASR-WS(8788)        asr_stream.handle      focus.py            Kaku(wezterm)
 │  点屏 ACT_TAP                                                                    
 │  (netTask) ──{type:focus,session:id}──▶                                          
 │                                  t=="focus"                                      
 │                                  pid = resolve(id) ───▶ tty_for_pid(pid)         
 │                                                         ps -o tty ─▶ /dev/ttysN  
 │                                                         kaku cli list ──────────▶ (mux socket)
 │                                                         匹配 tty_name → pane_id  ◀── panes json
 │                                                         kaku cli activate-pane ─▶ 切到该 pane
 │                                                         osascript activate ─────▶ 窗口前置
 │              ◀──{type:focus_ack,ok:true,reason:kaku}── (executor 里跑,不卡 loop) 
```

> osascript/`kaku cli` 是阻塞子进程,放进 `run_in_executor` 线程池执行,不阻塞 asyncio 事件循环(也就不卡其它设备/语音流)。

---

## 6. 约束与边界

- **仅 Claude 会话**:Codex 会话来自 rollout 文件、无活进程 PID(`pid=0`),无法定位面板,暂不支持。
- **依赖控制 TTY**:agent 必须跑在真实终端面板里(有 ttys)。在 GUI app(如桌面版 Claude)里跑的不行。
- **终端需可控**:Kaku/wezterm(CLI)或 iTerm2/Terminal.app(AppleScript)。VS Code / Cursor 的集成终端没有「按 tty 切单个 tab」的接口,做不到精确切换。
- **共享面板退化**:若多个 agent 复用同一个终端面板(同一 tty),只能切到那个共享面板。当前每个 agent 各占独立 Kaku pane,精确到面板。
- **失败安全**:任一级解析不到都返回 `(False, reason)`(`no-tty` / `tab-not-found`),只是不切,不影响设备其它功能或语音。

---

## 7. 涉及文件

| 端 | 文件 | 职责 |
|---|---|---|
| 固件 | `sketches/codey_dash/codey_dash.ino` | `wsFocus()`、`handleAction` 详情页 tap、`g_netFocusReq`/`g_focusSid` |
| 固件 | `sketches/codey_dash/codey_net.h` | `netTask` 里发 focus |
| 固件 | `sketches/codey_dash/codey_pages.h` | 详情页「点屏 → 切到 Mac」提示 |
| companion | `companion/asr_stream.py` | WS `focus` 分发、`set_pid_resolver` |
| companion | `companion/codey/focus.py` | `tty_for_pid` + Kaku CLI 切 pane(+ iTerm/Terminal 回退) |
| companion | `companion/codey/server.py` | `App.pid_for_session` |
| companion | `companion/codey/build_session.py`、`collect.py` | 会话对象补 `pid` 字段 |
| companion | `companion/codey_companion.py` | 注入 pid 解析器 |

---

## 8. 延伸:tmux 路径

设备旁有 `~/code/abtop`(读 transcript 的独立监视器),其 `jump_via_tmux(pid)` 用
`tmux list-panes -a -F "#{pane_pid} session:window.pane"` + `is_descendant_of` 按 PID 匹配,再
`tmux switch-client / select-window / select-pane`。若将来改用 tmux 起 agent,可照此再加一条
tmux 切换路径(与 Kaku CLI 并列),原理同样是「PID/TTY → 面板 → 激活」。
