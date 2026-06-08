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
- 通过 WebSocket 把麦克风音频流式送往本地/云端 ASR 引擎，并在设备端显示识别动画
  （活跃吉祥物 + 声呐环 + 流式转写）。

## 当前进展

已完成：

- 基于 Arduino CLI 的 M5Stack StopWatch 编译、烧录、串口监视工作流。
- `hello_stopwatch` 硬件和工具链冒烟测试程序。
- `stopwatch` 独立秒表程序，支持计圈和圆形进度环。
- `codey_dash` 主仪表盘固件，包含 Claude / Codex 动画页面。
- 统一的 Python Companion（单进程：状态 API + ASR WebSocket + Web 管理台），用量数据零联网。
- 通过 `companion/codey-statusline.sh` 捕获 Claude Code 真实额度数据。
- 读取 CodexBar 本地历史缓存，展示 Codex 使用量。
- Companion 侧计算农历、生肖等表盘信息。
- `companion/asr_stream.py` 流式 ASR 原型。

进行中 / 已知不足：

- `pending_reviews` 目前还是 Companion 响应里的占位字段。
- `sketches/codey_dash/codey_dash.ino` 中仍有硬编码的 Mac hostname 和 fallback IP。
- 语音输入目前主要完成转写，尚未接入具体命令执行。
- Codex 使用量依赖 CodexBar 的 macOS 缓存路径；没有该缓存时 Codex 使用量显示为 0。
- 用量数据完全离线读取（statusline 文件 + CodexBar 缓存）；仅可选的豆包 ASR 引擎会联网。

## 项目结构

```text
Codey/
├── arduino-cli.yaml             # 项目级 Arduino 板管理配置
├── asserts/
│   └── result.png               # 当前仪表盘预览图
├── companion/
│   ├── codey_companion.py       # 统一入口：HTTP 状态 API + ASR WS + Web 管理台
│   ├── asr_stream.py            # 流式 ASR WebSocket 服务（:8788）
│   ├── codey/                   # Python 包（state、quota、collect、asr、paste 等）
│   ├── web/                     # Web 管理台（设备镜像 + 识别历史）
│   ├── deploy.sh                # 一键启动脚本（预检 + start/stop/status）
│   ├── codey-statusline.sh      # Claude Code statusline 使用量捕获脚本
│   ├── models/                  # Whisper / sherpa-onnx 模型文件
│   └── data/                    # ASR 识别历史 JSONL（已 gitignore）
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
CodexBar history ───────┤
                        ├─ WebSocket（ASR :8788）─────────── M5 麦克风 PCM
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

- Python 3（仅用标准库即可提供状态 API + Web 管理台，无需 pip 依赖）
- 可选：`claude-hud`，用于保留原 Claude statusline 渲染
- 可选：CodexBar，用于读取 Codex 使用量缓存
- `companion/deploy.sh` 会先做预检再启动整套服务

语音识别实验：

- Python 3
- `numpy`、`websockets`、`sherpa-onnx`
- `companion/models/` 下的 Streaming Zipformer 模型文件
- 可选：`whisper.cpp` 工具（`whisper-server` / `whisper-cli`），用于批处理
  `POST /codey/asr` 接口

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

安装 ASR 依赖（仅语音功能需要）：

```bash
cd companion
python3 -m pip install numpy websockets sherpa-onnx
```

状态 API 与 Web 管理台无需任何第三方包。`./deploy.sh` 会在启动前替你检查这些依赖。

### ASR 模型（仅语音功能）

语音转写需要一个本地 **sherpa-onnx 流式 Zipformer** 模型。仓库已跟踪模型目录与 `tokens.txt`，
但 `.onnx` 权重文件被 gitignore——需要自行下载一次放进已有目录，最终结构如下：

```text
companion/models/sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20/
├── encoder-epoch-99-avg-1.int8.onnx
├── decoder-epoch-99-avg-1.onnx
├── joiner-epoch-99-avg-1.int8.onnx
└── tokens.txt            # 仓库已自带
```

从 sherpa-onnx 预训练模型下载名为
`sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20` 的模型（例如其
[Hugging Face 仓库](https://huggingface.co/csukuangfj/sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20)；
Hugging Face 慢可用 `https://hf-mirror.com` 镜像）。`companion/models/` 下任意匹配
`*streaming-zipformer*` 且含 `tokens.txt` 的目录都会被自动识别。

可选项：

- **云端引擎（豆包）** —— 无需下载模型，只在 `.env` 里填凭据（见下文）。
- **本地标点** —— 在 `companion/models/` 下放置 sherpa `*punct*` 模型目录，可为本地 sherpa
  结果补标点（不放则跳过）。
- **Whisper 批处理接口** —— 用于可选的 `POST /codey/asr`：`brew install whisper-cpp` 并把
  `ggml-small.bin` 放到 `companion/models/`。

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
./deploy.sh            # 预检 + 前台启动；加 `start --bg` 可后台运行
# 或直接：python3 codey_companion.py
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

推荐用 `deploy.sh` 启动——它会先做预检（Python + ASR 依赖 + sherpa 模型）、从示例自动生成
`.env`，端口被占用时拒绝启动：

```bash
cd companion
./deploy.sh             # 预检 + 前台启动（Ctrl-C 退出）
./deploy.sh start --bg  # 后台启动（日志 data/companion.log，PID data/companion.pid）
./deploy.sh status      # 查看 :8787 / :8788 状态
./deploy.sh restart     # 后台重启
./deploy.sh stop        # 停止
```

可用 `CODEY_PORT`、`CODEY_ASR_PORT`、`PYTHON` 覆盖端口/解释器。也可直接运行入口：
`python3 codey_companion.py`。

单一进程同时提供：
- HTTP 状态 API（`:8787`，对应 `GET /codey/state`）
- ASR WebSocket（`:8788`，接收 16 kHz 单声道 PCM）
- Web 管理台（`http://<mac>:8787/`，设备镜像 + 识别历史）

Web 管理台是多标签页面（**欢迎 · 教学 · 配置 · 设备镜像 · 识别历史**）：
- **欢迎**：项目简介 + 引导去 GitHub 给仓库点 Star。
- **教学**：首次配置 5 步走（设备 + macOS）。
- **配置**（`/codey/config`）：运行期改设置即时生效——ASR 引擎、粘贴/自动回车、豆包凭据、
  用量刷新间隔，以及**设备显示哪些列/字段**（列表列 状态/模型/Ctx/Tokens/内存/轮次 + 启用哪一端的页面）。
  存到 `companion/data/config.json`（已 gitignore），分层覆盖 **config.json > .env > 默认**；
  手表从 `/codey/state.display` 读取显示项配置。
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

语音输入桥与设备端识别动画移植自
[meme（作者 EthanM2025）](https://github.com/EthanM2025/meme)——详见 [致谢](#致谢)。

**配置** —— 复制示例 env 文件（启动时自动加载）并按需编辑：

```bash
cp companion/.env.example companion/.env
```

| 变量 | 默认 | 作用 |
|---|---|---|
| `CODEY_ASR_ENGINE` | `auto` | `sherpa`（本地离线）、`doubao`（云端）或 `auto` |
| `CODEY_PASTE` | `1` | 把最终转写粘贴到当前聚焦的 macOS 窗口 |
| `CODEY_PASTE_AUTO_ENTER` | `0` | 粘贴后按回车（自动提交） |
| `DOUBAO_API_KEY` / `DOUBAO_APP_ID` | — | 仅 `doubao` 引擎需要 |

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

**设备端动画** — 录音过程中，手表显示对应端的吉祥物进入随麦克风电平呼吸的活跃态，外围环绕
声呐「聆听」环，上方流式显示转写文本，下方为 `● LISTENING…` / `RECOGNIZING…` 状态行。

**可选本地标点** — 在 `companion/models/` 下放置 sherpa `*punct*` 模型目录，可为本地
sherpa 转写结果补充标点；不放置则跳过标点步骤。

## 远程访问（ngrok）

默认情况下手表与 Companion 必须在同一 Wi-Fi。若想跨网络访问 Companion——例如 Mac 放在公司、
手表连家里的 Wi-Fi——可用 **ngrok** 把服务暴露到公网。它会带 basic-auth 把
`/codey/state`（模型/用量信息）和 ASR WebSocket 一起暴露出去。

**配置**

1. 安装并完成 ngrok 鉴权：

   ```bash
   brew install ngrok/ngrok/ngrok
   ngrok config add-authtoken <token>   # 或在 .env 里设 NGROK_AUTHTOKEN
   ```

2. 在 ngrok 控制台（Domains）申请一个免费 **静态域名**，得到稳定的
   `your-name.ngrok-free.app`，手表可固定写死它。
3. 复制隧道配置，并在 `.env` 里填好凭据：

   ```bash
   cp companion/ngrok.yml.example companion/ngrok.yml
   ```

   | 变量 | 作用 |
   |---|---|
   | `NGROK_AUTHTOKEN` | ngrok agent 的 authtoken |
   | `NGROK_DOMAIN` | 静态域名（如 `your-name.ngrok-free.app`） |
   | `NGROK_BASIC_AUTH` | basic-auth 凭据，如 `codey:<long-secret>` |

4. 先起服务，再起隧道：

   ```bash
   cd companion
   ./deploy.sh start --bg   # Companion 监听 :8787 / :8788
   ./tunnel.sh              # ngrok 双隧道（state + asr）
   ```

5. 在手表的 Wi-Fi 配网页里，把 **Remote host** 设为你的静态域名，**Auth** 设为
   `codey:<long-secret>`。ASR 地址会通过 `/codey/state` 的 `asr_url` 自动下发，
   因此只需配置状态域名这一个值。

**工作原理** — 状态 API 跑在稳定的静态域名上；免费档下 ASR 隧道地址是随机的，因此由 Companion
从 ngrok 本地 API（`:4040`）读取后写入 `/codey/state.asr_url`，设备只需这一个稳定域名。设备走
HTTPS/WSS（TLS），并携带 `Authorization`（basic-auth）请求头。

> 若覆盖了 `CODEY_PORT` / `CODEY_ASR_PORT`，需同步修改 `ngrok.yml` 里对应的 `addr:`——Companion
> 按本地端口匹配隧道,端口对不上会让 `asr_url` 为空。

**验证** — 任意网络下执行：

```bash
curl -u codey:<secret> -H "ngrok-skip-browser-warning: true" https://your-name.ngrok-free.app/codey/state
```

**安全提示** — basic-auth 是**必需**的：任何拿到 URL 和凭据的人都能读取你的用量/会话数据，
并向 ASR socket 推流音频。切勿提交 `ngrok.yml` 或 `.env`（两者均已 gitignore）。设备会跳过 TLS
证书校验（`setInsecure`），所以 basic-auth 密钥是保护端点的唯一屏障——请设得足够长且随机。

> 配网页里的 **Remote host** / **Auth** 字段将在固件中加入（属后续任务）；上面的手表步骤描述的是
> 目标交互方式。

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

## 致谢

Codey 的语音相关能力建立在开源项目
[**meme**（作者 EthanM2025）](https://github.com/EthanM2025/meme)（MIT 许可）之上——这是一个
面向同款 M5Stack StopWatch 的姊妹项目。从 meme 借鉴/移植而来：

- **语音输入桥** —— 流式 ASR 到剪贴板的完整链路：最终转写自动粘贴到当前聚焦的 macOS 窗口、
  豆包（火山引擎）流式 ASR（带标点/ITN）、xiaozhi 风格 WebSocket 协议、submit/clear 控制，
  以及完成提示音。
- **设备端识别动画与交互** —— 圆屏上「说话 → 实时转写 + 吉祥物动画」的体验。Codey 在自己的
  M5GFX 渲染栈上复刻了 meme 的思路：对应端的吉祥物进入随麦克风电平呼吸的活跃态，外围环绕
  声呐「聆听」环，上方流式显示转写文本。

感谢 EthanM2025 将 meme 开源。原始实现见
[meme 仓库](https://github.com/EthanM2025/meme)。

## 下一步计划

- 将 Mac hostname、fallback IP、服务端口移入手表端设置流程。
- 用真实 Claude / Codex 待审阅任务数据替代 `pending_reviews` 占位值。
- 将语音转写接入可执行的手表命令。
- 为 Companion 增加 launchd 常驻运行配置（`deploy.sh` 已能 start/stop）。
- 增加简单模拟器或截图测试，避免仪表盘布局回归。
