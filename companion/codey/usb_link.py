"""USB-CDC 有线兜底链路:pyserial 后台线程,与 HTTP/WS 并存。
握手(HELLO→ACK)→ 定时推 STATE → PCM/控制喂共享 VoiceSession。非帧字节当设备日志打印。"""
import asyncio
import glob
import json
import threading
import time

from codey import usb_frames as uf

BAUD = 115200          # HWCDC 忽略波特率,占位
STATE_PUSH_SEC = 1.0   # state 推送周期


class UsbChannel:
    """VoiceSession 出站适配:把 stt/hello/focus_ack 编成帧写串口。"""
    def __init__(self, writer):
        self.writer = writer

    def _send(self, ftype, obj):
        self.writer.write(uf.encode(ftype, json.dumps(obj, ensure_ascii=False).encode("utf-8")))

    async def send_text(self, text, final, seq):
        self._send(uf.STT, {"type": "stt", "text": text, "final": final, "seq": seq})

    async def send_hello(self):
        self.writer.write(uf.encode(uf.HELLO_ACK, json.dumps({
            "type": "hello", "transport": "usb", "session_id": "codey",
            "audio_params": {"format": "pcm", "sample_rate": 16000, "channels": 1},
        }).encode("utf-8")))

    async def send_focus_ack(self, ok, reason):
        pass        # USB 端设备不消费 focus_ack;留空(接口对称)


async def handle_frame(ftype, payload, channel, session, on_hello=None):
    """单帧路由(可单测):HELLO→ACK+标在线;PCM→on_pcm;LISTEN/FOCUS→on_control。"""
    if ftype == uf.HELLO:
        channel.writer.write(uf.encode(uf.HELLO_ACK, b'{"type":"hello","transport":"usb"}'))
        if on_hello:
            on_hello()
    elif ftype == uf.STATE_REQ:
        if on_hello:                                  # 复用:请求即视作在线,触发一次推送
            on_hello()
    elif ftype == uf.PCM:
        await session.on_pcm(payload)
    elif ftype == uf.LISTEN:
        d = json.loads(payload or b"{}")
        d["type"] = "listen"
        await session.on_control(d)
    elif ftype == uf.FOCUS:
        await session.on_control({"type": "focus", "session": payload.decode("utf-8", "ignore")})


def find_port():
    hits = sorted(glob.glob("/dev/cu.usbmodem*"))
    return hits[0] if hits else None


def run(app, make_backend):
    """daemon 线程入口:维护串口 + 自有 asyncio loop。app 提供 state()/pid/status 解析。"""
    import serial                                     # pyserial;缺失则该兜底链路不可用
    from codey.voice_session import VoiceSession

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
    channel = UsbChannel(ser)
    online = {"v": False, "last_push": 0.0}
    session = VoiceSession(channel, make_backend, None, loop,
                           resolve_pid=app.pid_for_session, resolve_status=app.status_for_session)
    dec = uf.FrameDecoder()

    def mark_online():
        online["v"] = True
        _push_state(ser, app)
        online["last_push"] = time.time()

    try:
        while True:
            data = ser.read(4096)
            if data:
                frames, logs = dec.feed(data)
                if logs:
                    print("[dev] " + logs.decode("utf-8", "replace"), end="", flush=True)
                for ftype, payload in frames:
                    fut = asyncio.run_coroutine_threadsafe(
                        handle_frame(ftype, payload, channel, session, on_hello=mark_online), loop)
                    fut.result()
            now = time.time()
            if online["v"] and now - online["last_push"] >= STATE_PUSH_SEC:
                _push_state(ser, app)
                online["last_push"] = now
    except Exception as e:
        print(f"[usb] link lost: {e}", flush=True)
        try:
            ser.close()
        except Exception:
            pass


def _push_state(ser, app):
    try:
        ser.write(uf.encode(uf.STATE, json.dumps(app.state()).encode("utf-8")))
    except Exception as e:
        print(f"[usb] state push failed: {e}", flush=True)
