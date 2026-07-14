"""USB-CDC 有线兜底链路:pyserial 后台线程,与 HTTP/WS 并存。
握手(HELLO→ACK)→ 定时推 STATE → PCM/控制喂共享 VoiceSession。非帧字节当设备日志打印。
另外承载「路径 B:USB 直连配置」——server.py 的 HTTP handler 线程通过 send_config_request()
往设备发 CFG_GET/CFG_SET,阻塞等设备回 CFG_STATE/CFG_ACK(见开发文档「显示驱动与配网技术选型」§5.2)。"""
import asyncio
import glob
import json
import queue
import threading
import time

from codey import usb_frames as uf

BAUD = 115200          # HWCDC 忽略波特率,占位
STATE_PUSH_SEC = 1.0   # state 推送周期
CFG_TIMEOUT_SEC = 8.0  # 覆盖固件端 WiFi.begin 尝试上限(8s)+ 帧往返余量,server.py 侧再加点余量


class ConfigBridge:
    """HTTP 线程 ↔ USB 读线程 的一发一收桥。_lock 把并发请求串行化,避免响应张冠李戴。"""
    def __init__(self):
        self._lock = threading.Lock()
        self._q = queue.Queue(maxsize=1)

    def request(self, ser, io_lock, ftype, payload, timeout):
        with self._lock:
            while not self._q.empty():
                self._q.get_nowait()          # 清掉可能残留的陈旧响应
            with io_lock:
                ser.write(uf.encode(ftype, payload))
            try:
                return self._q.get(timeout=timeout)
            except queue.Empty:
                return None

    def deliver(self, ftype, payload):
        try:
            self._q.put_nowait((ftype, payload))
        except queue.Full:
            pass


# 当前在线的 USB 链路(串口/写锁/配置桥);_session_loop 建链时填、断链时清空。
_link = {"ser": None, "lock": None, "bridge": None}


def send_config_request(ftype, payload, timeout=CFG_TIMEOUT_SEC):
    """线程安全:server.py 的 HTTP handler 调用。USB 链路不在线/无响应 → 返回 None(调用方回 503)。"""
    ser, lock, bridge = _link["ser"], _link["lock"], _link["bridge"]
    if not ser or not bridge:
        return None
    return bridge.request(ser, lock, ftype, payload, timeout)


class UsbChannel:
    """VoiceSession 出站适配:把 stt/hello/focus_ack 编成帧写串口。"""
    def __init__(self, writer, lock=None):
        self.writer = writer
        self._lock = lock or threading.Lock()

    def _write(self, raw):
        with self._lock:
            self.writer.write(raw)

    def _send(self, ftype, obj):
        self._write(uf.encode(ftype, json.dumps(obj, ensure_ascii=False).encode("utf-8")))

    async def send_text(self, text, final, seq):
        self._send(uf.STT, {"type": "stt", "text": text, "final": final, "seq": seq})

    async def send_hello(self):
        self._write(uf.encode(uf.HELLO_ACK, json.dumps({
            "type": "hello", "transport": "usb", "session_id": "codey",
            "audio_params": {"format": "pcm", "sample_rate": 16000, "channels": 1},
        }).encode("utf-8")))

    async def send_focus_ack(self, ok, reason):
        pass        # USB 端设备不消费 focus_ack;留空(接口对称)


async def handle_frame(ftype, payload, channel, session, on_hello=None, bridge=None):
    """单帧路由(可单测):HELLO→ACK+标在线;PCM→on_pcm;LISTEN/FOCUS→on_control;
    CFG_STATE/CFG_ACK→交给 ConfigBridge,唤醒等待中的 HTTP 请求(路径 B)。"""
    if ftype == uf.HELLO:
        await channel.send_hello()          # 完整握手描述(含 audio_params),经锁写出
        if on_hello:
            on_hello()
    elif ftype == uf.STATE_REQ:
        if on_hello:                                  # 复用:请求即视作在线,触发一次推送
            on_hello()
    elif ftype == uf.PCM:
        await session.on_pcm(payload)
    elif ftype == uf.LISTEN:
        try:
            d = json.loads(payload or b"{}")
        except (json.JSONDecodeError, ValueError):
            print("[usb] bad LISTEN payload, skipping", flush=True)
            return
        d["type"] = "listen"
        await session.on_control(d)
    elif ftype == uf.FOCUS:
        await session.on_control({"type": "focus", "session": payload.decode("utf-8", "ignore")})
    elif ftype in (uf.CFG_STATE, uf.CFG_ACK):
        if bridge:
            bridge.deliver(ftype, payload)


def _emit_dev_logs(log_buf, new_bytes):
    """累积非帧字节,按行打印「干净」设备日志:滤掉损坏帧漏出的二进制碎片,只留可见 ASCII 行。
    未满行(无 \\n)留在 buf;纯二进制无换行时丢旧防无界增长。"""
    log_buf.extend(new_bytes)
    while b"\n" in log_buf:
        i = log_buf.index(b"\n")
        line = bytes(log_buf[:i])
        del log_buf[:i + 1]
        clean = "".join(chr(b) for b in line if 32 <= b < 127 or b == 9)   # 可见 ASCII + tab
        if clean.strip():
            print("[dev] " + clean, flush=True)
    if len(log_buf) > 2048:                                                # 纯二进制无换行 → 丢旧
        del log_buf[:-256]


def find_port():
    hits = sorted(glob.glob("/dev/cu.usbmodem*"))
    return hits[0] if hits else None


def run(app, make_backend):
    """daemon 线程入口:维护串口 + 自有 asyncio loop。app 提供 state()/pid/status 解析。"""
    import serial                                     # pyserial;缺失则该兜底链路不可用

    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)

    def serve():
        while True:
            port = find_port()
            if not port:
                time.sleep(1.0)
                continue
            try:
                ser = serial.Serial(port, BAUD, timeout=0.05)
            except Exception as e:
                print(f"[usb] open {port} failed: {e}", flush=True)
                time.sleep(1.0)
                continue
            print(f"[usb] link on {port}", flush=True)
            _session_loop(ser, app, make_backend, loop)

    threading.Thread(target=loop.run_forever, daemon=True).start()
    serve()


def _session_loop(ser, app, make_backend, loop):
    from codey.voice_session import VoiceSession   # 在本函数作用域导入(run() 的局部 import 在此不可见)
    lock = threading.Lock()
    channel = UsbChannel(ser, lock)
    online = {"v": False, "last_push": 0.0}
    session = VoiceSession(channel, make_backend, None, loop,
                           resolve_pid=app.pid_for_session, resolve_status=app.status_for_session)
    dec = uf.FrameDecoder()
    log_buf = bytearray()

    bridge = ConfigBridge()
    _link["ser"], _link["lock"], _link["bridge"] = ser, lock, bridge   # 路径 B:本链路上线,可转发配置请求

    def mark_online():
        online["v"] = True
        _push_state(ser, app, lock)
        online["last_push"] = time.time()

    try:
        while True:
            data = ser.read(4096)
            if data:
                frames, logs = dec.feed(data)
                if logs:
                    _emit_dev_logs(log_buf, logs)
                for ftype, payload in frames:
                    # 顺序处理(.result() 阻塞读线程直到该帧处理完)— 刻意与 WS 路径一致,
                    # 避免 PCM 与 listen:stop 并发复用同一 sherpa backend。代价:ASR 慢时读暂停,
                    # 极端下串口 RX 溢出 → CRC/半帧 → 解码器重同步(降级不损坏)。v1 可接受。
                    fut = asyncio.run_coroutine_threadsafe(
                        handle_frame(ftype, payload, channel, session, on_hello=mark_online, bridge=bridge), loop)
                    fut.result(timeout=15)        # 单帧处理卡死(ASR/executor 挂)→ 超时抛错 → 关口重连,不永久卡死读线程
            now = time.time()
            if online["v"] and now - online["last_push"] >= STATE_PUSH_SEC:
                _push_state(ser, app, lock)
                online["last_push"] = now
    except Exception as e:
        print(f"[usb] link lost: {e}", flush=True)
        try:
            ser.close()
        except Exception:
            pass
    finally:
        if _link["ser"] is ser:                      # 本链路下线;若已被新链路替换则不误清
            _link["ser"], _link["lock"], _link["bridge"] = None, None, None


def _push_state(ser, app, lock):
    try:
        payload = json.dumps(app.state()).encode("utf-8")
        if len(payload) > uf.STATE_MAX:           # 超固件 RX 缓冲 → 设备会丢弃此帧(会话过多);打日志,不静默
            print(f"[usb] STATE payload {len(payload)}B > {uf.STATE_MAX}B,设备将丢弃此帧(活动会话过多)", flush=True)
        raw = uf.encode(uf.STATE, payload)
        with lock:
            ser.write(raw)
    except Exception as e:
        print(f"[usb] state push failed: {e}", flush=True)
