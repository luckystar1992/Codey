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
                        ├─ companion/server.js ── HTTP /codey/state ── M5 StopWatch
ccusage fallback ───────┤
                        │
CodexBar history ───────┘

M5 麦克风 ── WebSocket PCM ── companion/asr_stream.py ── 实时转写文本
```

手表端不直接调用 Claude Code 或 Codex。它只连接运行在 Mac 上的 Companion 进程，由
Companion 把不同来源的额度数据标准化为统一 JSON。

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

启动 Companion：

```bash
cd companion
npm start
```

在 Mac 上检查 API：

```bash
curl http://127.0.0.1:8787/codey/state
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

## 可选：流式 ASR

在你偏好的 Python 环境中安装依赖：

```bash
pip install numpy websockets sherpa-onnx
```

启动流式 ASR 服务：

```bash
cd companion
python3 asr_stream.py
```

固件会连接 `ws://<mac-ip>:8788/`，并从 StopWatch 麦克风发送 16 kHz 单声道 PCM 音频。

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
