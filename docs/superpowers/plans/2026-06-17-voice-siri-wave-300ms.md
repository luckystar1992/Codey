# 语音 Siri 波带 + 300ms 流式切分 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把右键语音的可视化换成 Siri 流体波带,并把送 ASR 的音频按 300ms 切分(WiFi/USB 共用,在 netTask 累积),转写按 ~300ms 节奏流式更新。

**Architecture:** 设备主 loop 与 `g_voiceSB` 不动;netTask 把取到的 32ms PCM 片段累积进 `g_sendBuf[9600]`,满 300ms 发一帧(WS `sendBIN`/USB `U_PCM`),停录 flush 尾巴。动效 `drawVoiceViz` 重写为多条流动正弦叠加的对称波带。companion `usb_frames.MAX_PAYLOAD` 抬到 16384 以收 9600B 的 PCM 帧。

**Tech Stack:** ESP32-S3 Arduino C++(M5GFX canvas、FreeRTOS StreamBuffer)、Python(pytest)。

依据:`docs/superpowers/specs/2026-06-17-voice-siri-wave-300ms-design.md`

---

## Task 1: companion 解码上限抬到 16384(收 300ms PCM 帧,TDD)

**Files:**
- Modify: `companion/codey/usb_frames.py`（`MAX_PAYLOAD`)
- Test: `companion/tests/test_usb_frames.py`（加大帧往返用例）

- [ ] **Step 1: 写失败测试** — 追加到 `companion/tests/test_usb_frames.py`:
```python
def test_large_pcm_frame_roundtrips_9600B():
    # 300ms @16k/mono/int16 = 9600B;须能编/解码往返(USB 上行 PCM 帧)
    pcm = bytes(9600)
    raw = uf.encode(uf.PCM, pcm)
    frames, logs = uf.FrameDecoder().feed(raw)
    assert frames == [(uf.PCM, pcm)] and logs == b""
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cd companion && python3 -m pytest tests/test_usb_frames.py::test_large_pcm_frame_roundtrips_9600B -v`
Expected: FAIL（9600 > 当前 MAX_PAYLOAD=2048 → 解码器把它当损坏头丢弃,frames 不含该帧）

- [ ] **Step 3: 抬高 `MAX_PAYLOAD`** — `companion/codey/usb_frames.py`,把
```python
MAX_PAYLOAD = 2048
```
改为
```python
MAX_PAYLOAD = 16384   # 须 ≥ 300ms PCM 帧(9600B)且 = 固件 USB_MAX_PAYLOAD;损坏头最多缓冲 16KB 再重同步
```

- [ ] **Step 4: 跑全量 usb_frames 测试确认通过**

Run: `cd companion && python3 -m pytest tests/test_usb_frames.py -v`
Expected: PASS（含新用例;`test_oversized_len_header_resyncs*` 仍过——其 len=0xFFFF=65535 > 16384,仍触发丢弃重同步）

- [ ] **Step 5: 跑全量套件确认零回归**

Run: `cd companion && python3 -m pytest tests/ -q`
Expected: PASS（全绿）

- [ ] **Step 6: 提交**
```bash
cd /Users/zyc/code/Codey
git add companion/codey/usb_frames.py companion/tests/test_usb_frames.py
git commit -m "feat(companion): usb_frames MAX_PAYLOAD 抬到 16384(收 300ms PCM 帧)"
```

---

## Task 2: netTask 300ms 累积发送(WiFi/USB 共用,build-verify)

**Files:**
- Modify: `sketches/codey_dash/codey_net.h`（netTask 前加累积器 + helper;netTask 两分支改用累积发送 + 停录 flush）

READ `netTask()` 当前实现(约 160-205 行)再改。两分支的 PCM 发送当前为:
- USB:`uint8_t buf[1024]; size_t n; while ((n = xStreamBufferReceive(g_voiceSB, buf, sizeof(buf), 0)) > 0) usbSendFrame(U_PCM, buf, (uint16_t)n);`
- WiFi:`while (g_wsConn && (n = xStreamBufferReceive(g_voiceSB, buf, sizeof(buf), 0)) > 0) g_ws.sendBIN(buf, n);`

- [ ] **Step 1: 在 `codey_net.h` 的 `netTask` 之前加累积器 + helper**

放在 `static void usbOnFrame(...)` 定义之后、`netTask` 之前:
```cpp
// ---- 300ms 音频切分:netTask 把 32ms 片段累积成 300ms 整块再发(WS/USB 共用)----
static const size_t CHUNK_BYTES = 9600;        // 300ms @ 16k/mono/int16 = 4800 样本 ×2
static uint8_t g_sendBuf[CHUNK_BYTES];         // 静态(不压 netTask 栈)
static size_t  g_sendLen = 0;

typedef void (*PcmSink)(const uint8_t*, size_t);
static void sinkUsb(const uint8_t* p, size_t n) { usbSendFrame(U_PCM, p, (uint16_t)n); }
static void sinkWs (const uint8_t* p, size_t n) { g_ws.sendBIN((uint8_t*)p, n); }

static void pcmAccum(const uint8_t* seg, size_t segN, PcmSink sink) {  // 累积满 300ms 即整块发
  size_t off = 0;
  while (off < segN) {
    size_t take = CHUNK_BYTES - g_sendLen; if (take > segN - off) take = segN - off;
    memcpy(g_sendBuf + g_sendLen, seg + off, take); g_sendLen += take; off += take;
    if (g_sendLen == CHUNK_BYTES) { sink(g_sendBuf, CHUNK_BYTES); g_sendLen = 0; }
  }
}
static void pcmFlush(PcmSink sink) { if (g_sendLen) { sink(g_sendBuf, g_sendLen); g_sendLen = 0; } }  // 停录发尾巴
```

- [ ] **Step 2: 改 netTask USB 分支为「先排空累积 → 再处理控制(stop 前 flush)」**

把 USB 分支(`if (g_usbActive) { ... }` 内)改为(保持 focus/其它不变,仅 PCM 与 listen 控制顺序调整):
```cpp
    if (g_usbActive) {
      uint8_t buf[1024]; size_t n;                                   // 先把已录 PCM 累积发出(满 300ms 即发)
      while ((n = xStreamBufferReceive(g_voiceSB, buf, sizeof(buf), 0)) > 0) pcmAccum(buf, n, sinkUsb);
      if (g_netListenReq == 1)      { g_netListenReq = 0; g_sendLen = 0; String j = listenStartJson(); usbSendFrame(U_LISTEN, (const uint8_t*)j.c_str(), (uint16_t)j.length()); }
      else if (g_netListenReq == 2) { g_netListenReq = 0; pcmFlush(sinkUsb); const char* s = "{\"state\":\"stop\"}";   usbSendFrame(U_LISTEN, (const uint8_t*)s, (uint16_t)strlen(s)); }
      else if (g_netListenReq == 3) { g_netListenReq = 0; g_sendLen = 0; const char* s = "{\"state\":\"cancel\"}"; usbSendFrame(U_LISTEN, (const uint8_t*)s, (uint16_t)strlen(s)); }
      if (g_netFocusReq) { g_netFocusReq = false; usbSendFrame(U_FOCUS, (const uint8_t*)g_focusSid, (uint16_t)strlen(g_focusSid)); }
    } else if (g_wifi) {
```
说明:listen `start`/`cancel` 复位 `g_sendLen=0`(丢上一会话残留);`stop` 前 `pcmFlush` 把不足 300ms 的尾巴发出(不丢尾音);PCM drain 移到分支最前,保证停录前把所有已录块发完。

- [ ] **Step 3: 改 netTask WiFi 分支同样累积 + 停录 flush**

把 WiFi 分支(`else if (g_wifi) { ... }` 内)的 listen/PCM 部分改为:
```cpp
      if (!started || g_netReconnect) { g_netReconnect = false; resolveMac(); wsConnect(); started = true; lastFetch = 0; }
      g_ws.loop();
      uint8_t buf[1024]; size_t n;                                   // 先累积发 PCM(满 300ms 即发)
      while (g_wsConn && (n = xStreamBufferReceive(g_voiceSB, buf, sizeof(buf), 0)) > 0) pcmAccum(buf, n, sinkWs);
      if (g_netListenReq == 1)      { g_netListenReq = 0; g_sendLen = 0; wsListen(true); }
      else if (g_netListenReq == 2) { g_netListenReq = 0; pcmFlush(sinkWs); wsListen(false); }
      else if (g_netListenReq == 3) { g_netListenReq = 0; g_sendLen = 0; wsListenCancel(); }
      if (g_netFocusReq)            { g_netFocusReq = false; wsFocus(g_focusSid); }
      uint32_t now = millis();
      if (!g_voice && (lastFetch == 0 || now - lastFetch > 30000)) { lastFetch = now; fetchState(); maybeRepointWs(); }
```
说明:与 USB 分支同构;只把「块大小 32ms→300ms」,WS 协议/其它行为不变。`g_sendLen` 跨分支共用一个累积器(同一时刻只有一条路径活跃,切换时下一次 start 会复位)。

- [ ] **Step 4: build 验证**

Run: `./scripts/build.sh sketches/codey_dash 2>&1 | tail -3`
Expected: 编译通过;`Flash usage: NN% (budget 90%)`,NN ≤ 90。

- [ ] **Step 5: 提交**
```bash
cd /Users/zyc/code/Codey
git add sketches/codey_dash/codey_net.h
git commit -m "feat(firmware): netTask 把语音按 300ms 累积成整块发(WS/USB 共用 + 停录 flush)"
```

---

## Task 3: Siri 流体波带动效(`drawVoiceViz` 重写,build-verify + 真机)

**Files:**
- Modify: `sketches/codey_dash/codey_dash.ino`（`drawVoiceViz` 函数体整体替换;调用点 `drawVoiceOverlay` 不变)

- [ ] **Step 1: 整体替换 `drawVoiceViz`**

把 `sketches/codey_dash/codey_dash.ino` 里现有 `static void drawVoiceViz(...) { ... }`（注释 `// ---------- voice visualizer ----------` 那段,到函数右括号)整体替换为:
```cpp
// ---------- voice visualizer:Siri 流体波带 ----------
// 屏幕中部一条流动波带:3 条不同频率/相位/速度/色深的正弦叠加 + 两端包络收窄 + 上下镜像。
// 振幅由平滑电平 level 驱动。vphase: 1 听写(provider 色) / 2 识别中(琥珀,缓) / 3 结果(近平线)。
static void drawVoiceViz(int cx, int cy, float level, float t, uint32_t color, int vphase) {
  level = level < 0 ? 0 : (level > 1 ? 1 : level);
  const bool finalizing = (vphase == 2), result = (vphase == 3);
  uint32_t base = finalizing ? 0xFFB454 : color;                  // 识别中转琥珀
  const int   N = 96;                                             // 采样点
  const float W = 360.0f, MAXAMP = 46.0f;                         // 波带宽 / 最大振幅
  float drive = result ? 0.10f : (finalizing ? 0.34f : (0.22f + 0.78f * level));
  float spin  = (finalizing ? 1.1f : 1.9f) * t;                  // 流动速度

  const float freq[3]   = { 1.3f, 2.1f, 3.4f };                  // 三条波:频率/速度/振幅权/色深
  const float spd[3]    = { 1.0f, -1.5f, 0.7f };
  const float ampk[3]   = { 1.0f, 0.62f, 0.40f };
  const float shadek[3] = { 0.22f, -0.04f, -0.28f };

  for (int w = 0; w < 3; w++) {
    uint16_t cc = c565(shade(base, shadek[w]));
    int px0 = 0, py0u = 0, py0d = 0;
    for (int i = 0; i <= N; i++) {
      float fx  = (float)i / N;                                   // 0..1
      float env = sinf(fx * PI);                                  // 端点收窄到 0
      float amp = MAXAMP * drive * ampk[w] * env;
      float yv  = amp * sinf(fx * 6.2832f * freq[w] + spin * spd[w]);
      int x  = cx - (int)(W / 2) + (int)(fx * W);
      int yu = cy + (int)yv;                                      // 上波
      int yd = cy - (int)yv;                                      // 下波(镜像)→ 对称团块
      if (i > 0) { cv.drawLine(px0, py0u, x, yu, cc); cv.drawLine(px0, py0d, x, yd, cc); }
      px0 = x; py0u = yu; py0d = yd;
    }
  }
  // 中心电平光点(呼吸)
  int r = (int)(5 + drive * 7);
  cv.fillSmoothCircle(cx, cy, r, c565(shade(base, result ? 0.10f : 0.30f)));
}
```

- [ ] **Step 2: build 验证**

Run: `./scripts/build.sh sketches/codey_dash 2>&1 | tail -3`
Expected: 编译通过;`Flash usage: NN% (budget 90%)`,NN ≤ 90。

- [ ] **Step 3: 提交**
```bash
cd /Users/zyc/code/Codey
git add sketches/codey_dash/codey_dash.ino
git commit -m "feat(firmware): 语音动效改 Siri 流体波带(替代径向声条)"
```

---

## Task 4: 真机联调(manual)

- [ ] **Step 1: 刷机** — `./scripts/flash.sh sketches/codey_dash $(arduino-cli board list | awk '/usbmodem/{print $1; exit}')`
- [ ] **Step 2: 起 companion**(本分支)`cd companion && python3 codey_companion.py`
- [ ] **Step 3: 核对**
  - [ ] 动效:右键开录 → Siri 流体波带流动,振幅随说话音量起伏;识别中转琥珀变缓;结果近平线。观感满意(不满意 → 调 `N/W/MAXAMP/freq/spd/ampk/shadek/drive/spin`)。
  - [ ] 流式:转写按 ~300ms 节奏更新(不再每 32ms 刷);停录定稿不丢尾音。
  - [ ] 两路:WiFi(拔 USB)与 USB 都对。

---

## Self-Review(已执行)

**Spec coverage:** §2 动效→Task 3;§3 300ms 切分(netTask 累积/flush)→Task 2;§3 companion MAX_PAYLOAD→Task 1;§5 测试→Task 1 pytest + Task 4 真机。✅
**Placeholder scan:** 无 TBD/TODO;动效参数给了具体初值(真机微调是预期,非占位)。
**Type consistency:** `CHUNK_BYTES/g_sendBuf/g_sendLen/PcmSink/sinkUsb/sinkWs/pcmAccum/pcmFlush` 在 Task 2 定义并一致使用;`MAX_PAYLOAD=16384` 与固件 `USB_MAX_PAYLOAD=16384` 对齐;`drawVoiceViz` 签名不变(调用点 `drawVoiceOverlay` 无需改)。
