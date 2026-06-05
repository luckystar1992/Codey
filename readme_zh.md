![Codey 仪表盘](asserts/result.png)

# Codey

Codey 是一个面向 M5Stack StopWatch（SKU C152，ESP32-S3）的 Claude Code 与 Codex
使用量仪表盘项目。手表端用于展示会话额度、周额度、重置倒计时、设备状态，以及为圆形
466 x 466 AMOLED 屏幕定制的动画界面。

English documentation: [readme.md](readme.md).

## 功能

- 在 M5 StopWatch 上展示 Claude Code 与 Codex 使用量。
- 展示 5 小时 / 会话额度、周额度百分比，以及对应重置倒计时。
- 从 Claude Code statusline 获取当前模型信息。
- 从本机 CodexBar 历史缓存读取 Codex 使用量。
- 提供 Claude、Codex、机械表盘三个页面。
- 通过 `Codey-Setup` 热点完成 Wi-Fi 配网。
- 手表每 30 秒从局域网 Companion 服务拉取标准化使用量数据。
- 使用 StopWatch 真实电量、RTC / 时间、按键、麦克风和 IMU 数据。
- 内置设置页，可调整 Wi-Fi、亮度和音量。
- 包含基于 WebSocket 的流式语音识别实验。

## 当前进展

已完成：

- 基于 Arduino CLI 的 M5Stack StopWatch 编译、烧录、串口监视工作流。
- `hello_stopwatch` 硬件和工具链冒烟测试程序。
- `stopwatch` 独立秒表程序，支持计圈和圆形进度环。
- `codey_dash` 主仪表盘固件，包含 Claude / Codex 动画页面。
- Companion Node.js 服务，提供 `GET /codey/state`。
- 通过 `companion/codey-statusline.sh` 捕获 Claude Code 真实额度数据。
- 当 statusline 数据不可用时，通过 `ccusage` 作为 Claude 使用量兜底来源。
- 读取 CodexBar 本地历史缓存，展示 Codex 使用量。
- Companion 侧计算农历、生肖等表盘信息。
- `companion/asr_stream.py` 流式 ASR 原型。

进行中 / 已知不足：

- `pending_reviews` 目前还是 Companion 响应里的占位字段。
- `sketches/codey_dash/codey_dash.ino` 中仍有硬编码的 Mac hostname 和 fallback IP。
- 语音输入目前主要完成转写，尚未接入具体命令执行。
- Codex 使用量依赖 CodexBar 的 macOS 缓存路径；没有该缓存时 Codex 使用量显示为 0。
- Companion 默认假设本机代理为 `http://127.0.0.1:7892`，可通过 `CODEY_PROXY` 覆盖。

## 项目结构

```text
Codey/
├── arduino-cli.yaml             # 项目级 Arduino 板管理配置
├── asserts/
│   └── result.png               # 当前仪表盘预览图
├── companion/
│   ├── server.js                # 提供给手表访问的局域网使用量 API
│   ├── asr_stream.py            # 流式 ASR WebSocket 服务
│   ├── codey-statusline.sh      # Claude Code statusline 使用量捕获脚本
│   ├── models/                  # Whisper / sherpa-onnx 模型文件
│   └── package.json             # Node Companion 依赖
├── docs/
│   └── 开发的基础方法.md          # StopWatch 开发说明
├── libs/
│   └── WebSockets/              # 随仓库携带的 Arduino WebSockets 库
├── scripts/
│   ├── build.sh                 # 编译 sketch
│   ├── flash.sh                 # 编译并烧录 sketch
│   └── monitor.sh               # 115200 波特率串口监视
└── sketches/
    ├── hello_stopwatch/         # 最小 StopWatch 验证程序
    ├── stopwatch/               # 独立秒表程序
    └── codey_dash/              # Codey 主仪表盘固件
```

## 架构

```text
Claude Code statusline ─┐
                        ├─ companion (codey_companion.py) ─── HTTP /codey/state ── M5 StopWatch
ccusage fallback ───────┤
                        ├─ WebSocket /ws（ASR）──────────── M5 麦克风 PCM
CodexBar history ───────┤
                        └─ Web 管理台（http://localhost:8787）── 设备镜像 + 历史
```

手表连接运行在 Mac 上的统一 Companion 进程。Companion 把各来源额度数据标准化为统一 JSON，
处理 WebSocket 上的语音转写，并提供 Web 管理台用于监控和查看识别历史。

## 依赖

硬件：

- M5Stack StopWatch，SKU C152，ESP32-S3。
- USB-C 数据线。
- Mac 与 StopWatch 位于同一局域网。

固件工具链：

- `arduino-cli`
- M5Stack ESP32 开发板包
- Arduino 库：`M5Unified`、`M5GFX`、`WiFiManager`、`ArduinoJson`
- 本仓库随带的 `libs/WebSockets`

Companion 服务：

- Node.js
- npm
- 可选：`bun` 与 `claude-hud`，用于保留原 Claude statusline 渲染
- 可选：通过 `npx` 调用 `ccusage`，作为 Claude token 使用量兜底来源
- 可选：CodexBar，用于读取 Codex 使用量缓存

语音识别实验：

- Python 3
- `numpy`、`websockets`、`sherpa-onnx`
- `companion/models/` 下的 Streaming Zipformer 模型文件
- 可选：`whisper.cpp` 工具，用于 `server.js` 中的旧版 HTTP ASR 接口

## 初始化

安装 Arduino 平台和依赖库：

```bash
brew install arduino-cli
arduino-cli config init
arduino-cli config add board_manager.additional_urls https://static-cdn.m5stack.com/resource/arduino/package_m5stack_index.json
arduino-cli core update-index
arduino-cli core install m5stack:esp32
arduino-cli lib install M5Unified M5GFX WiFiManager ArduinoJson
```

安装 Companion 依赖：

```bash
cd companion
npm install
```

如果需要获取 Claude Code 真实额度百分比，请把 Claude Code 的 statusline command 指向本项目
的包装脚本：

```bash
chmod +x companion/codey-statusline.sh
```

脚本路径：

```text
/Users/zyc/code/Codey/companion/codey-statusline.sh
```

该脚本会把最近一次 Claude 使用量写入：

```text
~/.claude/codey-usage.json
```

## 运行

启动统一 Companion：

```bash
cd companion
python3 codey_companion.py
```

检查状态 API：

```bash
curl http://127.0.0.1:8787/codey/state
```

打开 Web 管理台：

```bash
open http://127.0.0.1:8787/
```

编译主仪表盘固件：

```bash
./scripts/build.sh sketches/codey_dash
```

烧录到 StopWatch：

```bash
./scripts/flash.sh sketches/codey_dash
```

打开串口监视：

```bash
./scripts/monitor.sh
```

首次启动时，手表会创建名为 `Codey-Setup` 的 Wi-Fi 配网热点。用手机或电脑连接该热点，
打开 `192.168.4.1`，选择与运行 Companion 的 Mac 相同的局域网。

## Companion 服务

启动统一的 Companion（单一进程，一体化）：

```bash
cd companion
python3 codey_companion.py
```

启动后包含：
- HTTP 状态 API（`:8787`，对应 `GET /codey/state`）
- ASR WebSocket（`:8788`，接收 16 kHz 单声道 PCM）
- Web 管理台（`http://<mac>:8787/`，设备镜像 + 识别历史）

Web 管理台展示：
- **设备镜像**（`/sim`）：实时轮询 `/codey/state` 显示手表状态
- **识别历史**（`/codey/history`）：从 `companion/data/asr_history.jsonl` 读取识别记录

ASR 识别历史存储为行分隔 JSON 文件（每行一条记录，带 ISO 时间码）：

```bash
cat companion/data/asr_history.jsonl
```

开发时 `asr_stream.py` 仍可单独运行于 `:8788`：

```bash
cd companion
python3 asr_stream.py
```

### 语音输入桥

`asr_stream.py` 是手表连接的流式 ASR 服务（端口 `:8788`）。启动时打印已选引擎，并自动加载
`companion/.env`（将 `companion/.env.example` 复制为 `.env` 并填写凭据）。

**引擎** — 由 `CODEY_ASR_ENGINE` 控制（默认 `auto`）：

- `sherpa` — 本地 sherpa-onnx Zipformer；离线可用。
- `doubao` — 火山引擎流式语音识别大模型 2.0（中文准确，内置标点/ITN）；需要
  `DOUBAO_API_KEY` + `DOUBAO_APP_ID`；流断时自动降级为文件式 ASR。
- `auto` — 有 `DOUBAO_API_KEY` 则用豆包，否则用 sherpa。

**粘贴** — 录音松开后，最终转写文本自动粘贴到当前聚焦的 macOS 窗口（`pbcopy` +
`osascript` Cmd+V）。**首次运行需要辅助功能授权**（系统设置 → 隐私与安全性 → 辅助功能
→ 启用运行 Python 的终端），否则 Cmd+V 会被系统静默忽略。可用 `CODEY_PASTE=0` 关闭粘贴；
`CODEY_PASTE_AUTO_ENTER=1` 可在粘贴后自动回车提交。

**submit / clear** — 固件（BtnB 多击）发送 `{"type":"submit"}`（回车）或
`{"type":"clear"}`（Cmd+A + 删除）；Companion 均已处理。

**chime** — `/codey/state` 中的 `chime: {agent, seq}` 在 Claude/Codex 一轮对话完成时更新
（`seq` 每次完成递增，其余时候为 `null`）。固件检测到 seq 变化时播放提示音。

**可选本地标点** — 在 `companion/models/` 下放置 sherpa `*punct*` 模型目录，可为本地
sherpa 转写结果补充标点；不放置则跳过标点步骤。

## 手表操作

- 左键：切换页面。
- 右键：开始或停止语音转写。
- 双键长按：打开或关闭设置页。
- 设置页内：左键下移，右键确认。
- 摇晃手表：切换页面。
- 空闲约 20 秒：自动降低屏幕亮度。

## Companion API

`GET /codey/state` 返回标准化手表状态：

```json
{
  "ts": 1760000000,
  "stale": false,
  "battery": { "pct": 0, "charging": false },
  "providers": [
    {
      "id": "claude",
      "name": "Claude Code",
      "session": { "used_pct": 42, "reset_epoch": 1760003600 },
      "weekly": { "used_pct": 18, "reset_epoch": 1760300000 },
      "pending_reviews": 0,
      "model": "Claude Sonnet",
      "_src": "statusline"
    }
  ],
  "lunar": {
    "date": "四月十五",
    "ganzhi": "丙午",
    "zodiac": "马",
    "jieqi": ""
  }
}
```

## 开发说明

- 主固件目标板型：`m5stack:esp32:m5stack_stopwatch`。
- 辅助脚本会传入 `--libraries libs`，因此会使用仓库内置的 WebSockets 库。
- 主仪表盘使用 PSRAM canvas 渲染，避免动画闪烁。
- StopWatch 开发流程详见 [docs/开发的基础方法.md](docs/开发的基础方法.md)。

## 下一步计划

- 将 Mac hostname、fallback IP、服务端口移入手表端设置流程。
- 用真实 Claude / Codex 待审阅任务数据替代 `pending_reviews` 占位值。
- 将语音转写接入可执行的手表命令。
- 为 Companion 增加 launchd 或 pm2 常驻运行配置。
- 增加简单模拟器或截图测试，避免仪表盘布局回归。
