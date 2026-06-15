# USB 有线兜底传输 — 设计

> 状态:已批准设计,待 writing-plans 拆实施计划
> 分支:`feat/usb-wired-fallback`(基于 `feat/ngrok-remote-access`)
> 日期:2026-06-16

## 0. 背景与目标

Codey 设备(M5Stack StopWatch,ESP32-S3,圆屏)当前与 Mac companion 的全部数据走 **WiFi**:

| 通道 | 现状 | 内容 |
|---|---|---|
| 状态显示 | 设备 `HTTPClient` GET → companion `:8787 /codey/state`(LAN 或 ngrok HTTPS) | 三页 UI JSON |
| 语音 | 设备 WebSocket `:8788`(`WebSocketsClient`) | PCM 上行 + 转写文本下行 |
| 控制 | 同上 WS | `listen start/stop/cancel`、`focus` |

USB 当前仅用于刷机 + `Serial.println` 调试日志。

**目标:构建一套有线兜底方案。** 当 USB 连上(且 companion 在 USB 上应答)时,**优先走 USB,不依赖网络**;USB 断开时无缝回落现有 WiFi 路径。

### 不变量(硬约束)
1. USB 链路**全量替代**网络:state 显示 / 语音(PCM↑ 转写↓)/ 控制(listen·focus·cancel)。
2. **无线路径功能零影响** —— WS `handle()` 对外可观察行为不变,由现有 pytest 守住。
3. 帧 + 调试日志**共用单路 HWCDC**(本板 `build.usb_mode=1` + `cdc_on_boot=1`,`Serial` 即原生 USB-CDC,只有一路 CDC 接口)。
4. **免配网即用**:USB 在线时设备可完全不依赖 WiFi 配置(纳入 v1)。

### 关键硬件事实
- FQBN `m5stack:esp32:m5stack_stopwatch`:`build.usb_mode=1`(HWCDC over USB-Serial-JTAG)、`build.cdc_on_boot=1`。设备 `Serial` 就是刷机/monitor 用的那路原生 USB-CDC,**仅此一路**。
- 带宽充裕:语音裸 PCM 16k/mono/int16 = 32 KB/s ≈ 256 kbps;USB FS 12 Mbps 绰绰有余。
- `Serial.begin(115200)` 的波特率对 CDC 无意义(走真实 USB 速率)。

## 1. 线协议(frame over CDC)

字节流需分帧,且要与 ASCII 日志共存。

```
C0 DE │ type(1) │ len(2, LE) │ payload(len 字节) │ crc16(2, LE)
                                                    └ CRC-16/CCITT over (type + len + payload)
```

- companion 解析器:扫魔数 `C0 DE` → 读 5 字节头 → 读 `len` 字节 payload → 读 2 字节 CRC 校验。
- **非帧字节(无魔数 / CRC 失败)累积成设备日志文本打印** → 满足「帧+日志共存」。
- 误同步分析:调试日志是 ASCII(`< 0x80`),绝不含 `0xC0`/`0xDE`,故魔数在日志流中不会出现 → 误同步概率极低;万一偏差,靠 CRC 失败丢帧重同步。

### 帧类型(payload 直接复用现有 JSON / PCM,**设备端零新增解析逻辑**)
| 方向 | type | 名称 | payload |
|---|---|---|---|
| 设备→ | `0x01` | HELLO | proto 版本 + 设备标识(握手探针) |
| 设备→ | `0x10` | STATE_REQ | 空(连接后立即拉一帧) |
| 设备→ | `0x20` | LISTEN | `{state, session, seq}` JSON(与现 WS 同) |
| 设备→ | `0x21` | PCM | 裸 PCM(16k/mono/int16-LE) |
| 设备→ | `0x30` | FOCUS | session id |
| →设备 | `0x81` | HELLO_ACK | proto 版本 + audio_params |
| →设备 | `0x90` | STATE | `/codey/state` JSON(与现 HTTP body 逐字节同) |
| →设备 | `0xA0` | STT | `{text, final, seq}` JSON(与现 WS 同) |

### 链路检测:用握手,不用 CDC-DTR
刷机 / `monitor.sh` 也会拉 DTR / 打开端口 → DTR 会误判「companion 在听」。**只有 companion 真正回 `HELLO_ACK` 才算链路在线**,健壮。

## 2. 设备侧(firmware,新文件 `sketches/codey_dash/codey_usb.h`)

netTask(core0,链路唯一决策者)中加 `usbLinkPoll()`:
- 每 ~500ms 发 `HELLO`;收到 `HELLO_ACK` → `g_usbActive = true`;连续 N 秒无任何帧 → `g_usbActive = false`。
- 非阻塞帧读取状态机:drain `Serial.available()`,增量喂状态机(等魔数 → 读头 → 读体 → 校验)。
- `Serial.setTxTimeoutMs(0)`:companion 没在读时写**不阻塞** netTask(关键 —— 否则 HELLO 探针会拖死网络任务)。

链路活跃分支(**仅 `if (g_usbActive)` gate,不改原有 WiFi 分支逻辑**):
- **state**:不调 HTTP `fetchState()`,改在收到 `STATE` 帧时喂**同一个** parseState(复用 `codey_net.h` 的解析,零改动)。
- **语音**:main loop 已把 PCM 推进 `g_voiceSB` StreamBuffer(本就传输无关)→ netTask 端把「读 SB → `g_ws.sendBIN`」改为「读 SB → 发 PCM 帧」;`listen start/stop/cancel`、`focus` 发对应帧;收 `STT` 帧写 `g_transcript`(与现 `wsEvent` 的 stt 分支写法一致)。
- 跳过 WiFi IO(可完全不连网)。

链路不活跃 → 现有 WiFi/WS/HTTP 路径**原样运行**。

日志:保留 `Serial.println`;帧写入用一个轻量 `portMUX`/mutex 包一下,降低与 println 交错的概率(交错了 companion 端 CRC 也能重同步)。

### 免配网启动(v1)
开机流程当前在无 WiFi 凭据时会阻塞进 `wifiConfigPortal()`。改为:**开机先给 USB 握手一个短窗口(如 1.5s),若 `HELLO_ACK` 到达则跳过配网门户**,设备零 WiFi 配置即用纯 USB。若窗口内无 ACK 则维持现有配网行为。

## 3. companion 侧

### 新模块 `companion/codey/usb_frames.py`(纯函数,易测)
- `encode(type, payload) -> bytes`、`crc16(data) -> int`、`FrameDecoder`(增量喂字节,产出 `(frames, log_bytes)`,处理半帧 / 重同步)。

### 新模块 `companion/codey/usb_link.py`(daemon 线程 + 自有 asyncio loop)
- pyserial 自动找 `/dev/cu.usbmodem*`;打开失败 / 掉线 → 重试循环(热插拔)。
- 读线程把字节经 `FrameDecoder` 解析:帧 → 投递事件队列;log_bytes → 打印为设备日志。
- 收 `HELLO` → 回 `HELLO_ACK` + 标记在线 → 定时推 `STATE`(复用 `app.state()`,server.py:169)+ 响应 `STATE_REQ`。
- 收 `PCM`/`LISTEN`/`FOCUS` → 喂**共享语音核**(§4)。

### 接线 `companion/codey_companion.py`
再起一个 daemon 线程启动 `usb_link.run()`,与现有 HTTP `:8787` + ASR WS `:8788` 并存;复用 `app.state` / `app.pid_for_session` / `app.status_for_session`。

## 4. 共享语音核(传输无关,satisfies 不变量 #2)

### 新模块 `companion/codey/voice_session.py`
把 `asr_stream.handle()` 的会话逻辑抽成传输无关的 `VoiceSession`:
- 持有 backend 生命周期、`target_pane`、`synced`、`cur_seq`、`last_sent`。
- partial diff → `focus.sync_to_pane`(退格删改动尾 + 补新尾,send 失败不推进 baseline)。
- status 门控(仅 `waiting` 空闲会话才注入)、stop 对账(定稿/空/异常都对账)、cancel 退格清残留。
- 入站:`on_pcm(bytes)`、`on_control(dict)`(hello / listen-start/stop/cancel / submit)。
- 出站走 `Channel`:
  ```python
  class Channel(Protocol):
      async def send_text(self, text: str, final: bool, seq: int): ...
      async def send_hello(self): ...
  ```

### 改写 `companion/asr_stream.py`
`handle(ws, ...)` 变薄:构造 `WsChannel(ws)` + `VoiceSession`,`async for msg in ws` 把 bytes→`on_pcm`、text-json→`on_control`。**WS 对外行为逐字节不变**;`handle()` 签名保持,现有 pytest 全绿。

usb_link 用 `UsbChannel(frame_writer)` + **同一** `VoiceSession`。

## 5. 错误处理
- 端口打开失败 / 设备消失 → 重试 + 日志;设备侧无 ACK → 自动回落 WiFi。
- CRC 失败 → 丢帧 + 重同步 + 错误计数。
- HWCDC `tx_timeout = 0` → 设备永不阻塞。
- 设备 USB 活跃但超时无 `STATE` → 保留上一帧 state + 角标提示;无 ACK 翻回 WiFi。
- companion 端单帧解码 / accept 异常不拖垮整条链路(沿用现 asr_stream 的容错粒度)。

## 6. 测试
- **pytest(companion)**:
  - `usb_frames`:encode/decode 往返、CRC、交错日志重同步、半帧、损坏帧丢弃。
  - `VoiceSession`:行为对账(沿用现有 WS 用例 + 新核独立用例 —— partial diff / status 门控 / stop 对账 / cancel)。
  - `usb_link`:握手(HELLO→ACK)、STATE 推送、PCM→语音核 路由(用 fake serial + fake backend)。
- **固件**:`scripts/build.sh` flash 门禁 + 真机联调验证;可选 host 侧 python 复刻帧格式做 round-trip 对拍。

## 7. 实施顺序(交付给 writing-plans 细化)
1. `usb_frames.py` + 测试(纯函数,先立地基)。
2. `voice_session.py` 抽取 + `asr_stream.py` 改薄(**WS 现有测试必须保持全绿**)。
3. `usb_link.py` + `codey_companion.py` 接线 + usb_link 测试。
4. 固件 `codey_usb.h` + netTask gate + 免配网启动 + build 门禁。
5. 真机联调(USB 在/拔双向切换、语音、state、focus)。

## 8. 已考虑但放弃的备选
- **检测用 CDC-DTR 状态**:刷机/monitor 拉 DTR 会误判,放弃,改握手。
- **第二路 CDC 分流日志/数据**:本板 `usb_mode=1`(HWCDC,单 CDC),要切 TinyUSB(`usb_mode=0`)才能多 CDC,会改动上传方式,代价大;改用「帧带魔数+CRC,非帧字节当日志」共存(用户已确认)。
- **USB 端复制 ASR 喂入逻辑(不碰 asr_stream)**:零风险但重复;用户选了抽共享核(DRY、低耦合)。
