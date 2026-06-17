# 语音:Siri 流体波带动效 + 300ms 流式切分 — 设计

> 状态:已批准设计,待 writing-plans
> 分支:`feat/usb-wired-fallback`(承接当前工作)
> 日期:2026-06-17

## 0. 目标
1. 替换右键语音的可视化动效(当前 `drawVoiceViz` 的呼吸光球+径向声条,用户觉得丑)为 **Siri 风格流体波带**。
2. 音频按 **300ms 切分**送到 Mac 识别接口,每块识别返回 → 设备转写按 ~300ms 节奏流式更新。
3. 两条传输路径(WiFi WS / USB)行为一致;切分在设备端做,天然两路共用。

## 1. 现状(已确认)
- 录音:`STREAM_CHUNK = 512` 样本(~32ms),双缓冲 `g_seg[2][512]`,主 loop 消费满段 → 算电平 `g_micLevel` → `xStreamBufferSend(g_voiceSB, …)` → netTask 发(WS `sendBIN` / USB `U_PCM` 帧)。
- 语音**已经是流式**(sherpa 每 accept 出 growing partial);本次是把节奏改成 300ms,不是修“不流式”。
- 动效入口:`drawVoiceOverlay()` 调 `drawVoiceViz(CX, ry, level, t, color, g_vphase)`;转写在顶部(y≈100),状态行底部(y≈412)。
- `g_vphase`:1 听写 / 2 识别中 / 3 结果。

## 2. A — Siri 流体波带(`drawVoiceViz` 重写)
- 屏幕中部一条**水平流动波带**(沿用现 `ry`):2–3 条不同频率/相位/速度的平滑正弦叠加 → 有机流动。
- 振幅 = 平滑后的电平(`g_micLevel`,帧间 lerp 削抖)× 两端**包络**(向左右收窄到 0 → 圆屏内居中团块,不触边)。
- **上下镜像**(波 + 其镜像,或两波间填充)做 Siri 对称团块感;抗锯齿平滑线。
- phase 配色:① 听写=provider 色、随电平灵动;② 识别中=琥珀、转速更慢、振幅压低;③ 结果=平静收束近平线。
- 顶部转写 / 底部状态行布局不变。具体参数(波数/频率/速度/包络/线宽/配色)真机微调。

## 3. B — 300ms 流式切分(在 netTask 累积,主 loop 与 g_voiceSB 不动)
设计取舍:**累积放在消费端 netTask**,而不是生产端主 loop。原因:`g_voiceSB` 是字节**流缓冲**(无消息边界),netTask 现在按 ≤1024B 取出逐帧发;若在主 loop 凑 300ms 再塞流缓冲,取出端仍会被拆成小帧。把累积放进 netTask,主 loop 与 g_voiceSB 全不动,传输关注点内聚在 netTask。

- **主 loop 完全不变**:仍录 512 样本小段、算 `g_micLevel`(每 ~32ms,动画丝滑)、`xStreamBufferSend` 推 32ms 片段。
- **netTask 新增累积**:静态缓冲 `g_sendBuf[CHUNK_BYTES]`(`CHUNK_BYTES = 9600` = 4800 样本×2 = 300ms);静态长度 `g_sendLen`。每轮从 `g_voiceSB` 取 32ms 片段拷进 `g_sendBuf`;满 9600 → 整块发(WS 一次 `sendBIN` / USB 一个 `U_PCM` 帧),`g_sendLen=0`。WS 与 USB 两分支同样累积。
- **停录 flush**:收到 `g_netListenReq==2`(stop)时,先把 `g_sendBuf` 里不足 300ms 的尾巴整块发出(避免丢尾音),`g_sendLen=0`,再发 stop 控制帧。`listen:start`(req==1)时 `g_sendLen=0` 复位。
- **companion**:`codey/usb_frames.py` 的 `MAX_PAYLOAD` 2048 → **16384**(让 9600B 的 300ms `U_PCM` 帧通过 USB 解码;WS 无大小限制)。其余 companion 不改 —— `backend.accept(pcm)` 收到 300ms 块即出一个 partial。
- 内存:`g_sendBuf` 静态 9600B(netTask 不放栈,避免 16K 栈压力);`g_voiceSB` 不动(仍载 32ms 片段,8192B 足够)。
- 取舍:动画电平 32ms 采样(丝滑),送 ASR/出转写节奏 300ms(用户要的效果);首个 partial 约 300ms 后出现。

## 4. 不变量 / 风险
- WS 路径行为只改“块大小”(32ms→300ms),协议不变;sherpa/doubao accept 任意块长均可。
- USB 路径:300ms PCM 帧 9600B < 固件 `USB_MAX_PAYLOAD`(16384)TX 上限,可发;companion 解码上限同步抬到 16384。
- 累积在 netTask、主 loop 与 `g_voiceSB` 不动 → 改面最小,丢音风险低;只需保证停录 flush 尾巴。
- 动效是视觉活,真机迭代到满意。

## 5. 测试
- companion:`usb_frames` 现有测试在 `MAX_PAYLOAD` 调整后仍全绿(`test_oversized_len` 用 0xFFFF 仍 > 16384 → 仍重同步);可加一例:9600B payload 编/解码往返通过。
- 固件:`build.sh` flash 门禁 + 真机:① 波带观感;② 转写按 ~300ms 更新;③ 停录尾音不丢;④ WiFi 与 USB 都对。

## 6. 文件
- 固件 `sketches/codey_dash/codey_dash.ino`:`drawVoiceViz` 重写为 Siri 波带。
- 固件 `sketches/codey_dash/codey_net.h`:netTask 两分支加 `g_sendBuf`/`g_sendLen` 300ms 累积 + 停录 flush。
- companion `companion/codey/usb_frames.py`:`MAX_PAYLOAD` → 16384;`companion/tests/test_usb_frames.py`:加 9600B 大帧往返用例。
