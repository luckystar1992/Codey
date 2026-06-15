# USB 有线兜底传输 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 给 Codey 设备加一套 USB-CDC 有线兜底链路 —— USB 连上且 companion 应答时优先走 USB(state/语音/控制全量),不依赖网络;USB 断开无缝回落 WiFi;无线路径功能零影响。

**Architecture:** 设备与 companion 在单路 HWCDC 上用「`C0 DE│type│len│payload│crc16`」分帧通信,非帧字节当日志透传。companion 端抽出传输无关的 `VoiceSession`(WS/USB 共用 `Channel`),USB 用 pyserial 后台线程接入;设备端在 netTask 里用 `g_usbActive` gate 切换 USB/WiFi 分支。

**Tech Stack:** Python 3(pyserial、asyncio、pytest)、Arduino/ESP32-S3(HWCDC、FreeRTOS、ArduinoJson)。

设计依据:`docs/superpowers/specs/2026-06-16-usb-wired-fallback-design.md`

---

## 文件结构

**新增(companion):**
- `companion/codey/usb_frames.py` — 帧编解码纯函数 + 增量 `FrameDecoder`(仅分帧,无 IO)
- `companion/codey/voice_session.py` — 传输无关 `VoiceSession` + `Channel` 协议(从 asr_stream 抽取的 ASR 会话逻辑)
- `companion/codey/usb_link.py` — pyserial 读写线程、握手、STATE 推送、把 PCM/控制经 `UsbChannel` 喂 `VoiceSession`
- `companion/tests/test_usb_frames.py` / `test_voice_session.py` / `test_usb_link.py`

**修改(companion):**
- `companion/asr_stream.py` — `handle()` 变薄:`WsChannel(ws)` + `VoiceSession`(签名/公共面不变)
- `companion/codey_companion.py` — 起 usb_link daemon 线程

**新增(firmware):**
- `sketches/codey_dash/codey_usb.h` — C 帧编解码 + `usbLinkPoll()` + `g_usbActive`

**修改(firmware):**
- `sketches/codey_dash/codey_dash.ino` — 全局 `g_usbActive`、include、tx-timeout、开机握手窗口、抽 `voiceApplyStt()`
- `sketches/codey_dash/codey_net.h` — netTask gate + 抽 `applyStateDoc()`

---

## Task 1: 帧编解码 `usb_frames.py`(纯函数,TDD)

**Files:**
- Create: `companion/codey/usb_frames.py`
- Test: `companion/tests/test_usb_frames.py`

- [ ] **Step 1: 写失败测试**

`companion/tests/test_usb_frames.py`:
```python
from codey import usb_frames as uf


def test_crc16_known_vector():
    # CRC-16/CCITT-FALSE("123456789") == 0x29B1
    assert uf.crc16(b"123456789") == 0x29B1


def test_encode_roundtrip_single_frame():
    raw = uf.encode(uf.STT, b'{"text":"hi"}')
    dec = uf.FrameDecoder()
    frames, logs = dec.feed(raw)
    assert logs == b""
    assert frames == [(uf.STT, b'{"text":"hi"}')]


def test_empty_payload():
    raw = uf.encode(uf.STATE_REQ)
    frames, logs = uf.FrameDecoder().feed(raw)
    assert frames == [(uf.STATE_REQ, b"")]
    assert logs == b""


def test_log_bytes_before_and_after_frame_are_passthrough():
    raw = b"[ws] connected\n" + uf.encode(uf.HELLO, b"v1") + b"[boot] ok\n"
    frames, logs = uf.FrameDecoder().feed(raw)
    assert frames == [(uf.HELLO, b"v1")]
    assert logs == b"[ws] connected\n[boot] ok\n"


def test_frame_split_across_two_feeds():
    raw = uf.encode(uf.PCM, b"\x01\x02\x03\x04")
    dec = uf.FrameDecoder()
    f1, l1 = dec.feed(raw[:4])
    f2, l2 = dec.feed(raw[4:])
    assert f1 == [] and f2 == [(uf.PCM, b"\x01\x02\x03\x04")]
    assert l1 == b"" and l2 == b""


def test_bad_crc_is_dropped_and_resyncs_to_next_frame():
    bad = bytearray(uf.encode(uf.STT, b"xx"))
    bad[-1] ^= 0xFF                                  # 破坏 CRC
    good = uf.encode(uf.STT, b"ok")
    frames, logs = uf.FrameDecoder().feed(bytes(bad) + good)
    assert (uf.STT, b"ok") in frames                 # 坏帧后能重同步到好帧
    assert (uf.STT, b"xx") not in frames


def test_trailing_lone_magic_first_byte_is_held_not_emitted_as_log():
    dec = uf.FrameDecoder()
    frames, logs = dec.feed(b"hello\xc0")            # 0xC0 可能是下一个魔数起点
    assert frames == []
    assert logs == b"hello"                          # 0xC0 暂留,不当日志吐出
    frames, logs = dec.feed(b"\xde" + uf.encode(uf.HELLO, b"")[2:])
    assert frames == [(uf.HELLO, b"")]
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cd companion && python3 -m pytest tests/test_usb_frames.py -v`
Expected: FAIL（`ModuleNotFoundError` / `AttributeError: module 'codey.usb_frames'`）

- [ ] **Step 3: 实现 `usb_frames.py`**

`companion/codey/usb_frames.py`:
```python
"""USB-CDC 帧编解码:C0 DE | type(1) | len(2 LE) | payload | crc16(2 LE)。
非帧字节当设备日志透传。纯函数 + 增量解码器,无 IO,便于单测。"""

MAGIC = b"\xc0\xde"

# frame types(与固件 codey_usb.h 的枚举一一对应)
HELLO = 0x01
STATE_REQ = 0x10
LISTEN = 0x20
PCM = 0x21
FOCUS = 0x30
HELLO_ACK = 0x81
STATE = 0x90
STT = 0xA0


def crc16(data: bytes) -> int:
    """CRC-16/CCITT-FALSE: poly=0x1021, init=0xFFFF, no reflect, no xorout。"""
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if (crc & 0x8000) else (crc << 1) & 0xFFFF
    return crc


def encode(ftype: int, payload: bytes = b"") -> bytes:
    n = len(payload)
    if n > 0xFFFF:
        raise ValueError("payload too large")
    body = bytes([ftype, n & 0xFF, (n >> 8) & 0xFF]) + payload   # crc 覆盖 type+len+payload
    c = crc16(body)
    return MAGIC + body + bytes([c & 0xFF, (c >> 8) & 0xFF])


class FrameDecoder:
    """增量喂字节,产出 (frames, logs)。frames=[(type, payload)];logs=非帧字节。"""

    def __init__(self):
        self._buf = bytearray()

    def feed(self, data: bytes):
        self._buf.extend(data)
        frames = []
        logs = bytearray()
        while True:
            i = self._buf.find(MAGIC)
            if i < 0:                                  # 无完整魔数
                keep = 1 if self._buf[-1:] == MAGIC[:1] else 0   # 末尾孤立 0xC0 可能是魔数起点,暂留
                if len(self._buf) > keep:
                    logs.extend(self._buf[: len(self._buf) - keep])
                    del self._buf[: len(self._buf) - keep]
                break
            if i > 0:                                  # 魔数前的字节是日志
                logs.extend(self._buf[:i])
                del self._buf[:i]
            if len(self._buf) < 5:                     # 魔数(2)+type(1)+len(2) 还不够
                break
            n = self._buf[3] | (self._buf[4] << 8)
            total = 5 + n + 2
            if len(self._buf) < total:                 # 半帧,等更多字节
                break
            body = bytes(self._buf[2 : 5 + n])
            crc_rx = self._buf[5 + n] | (self._buf[6 + n] << 8)
            if crc16(body) == crc_rx:
                frames.append((self._buf[2], bytes(self._buf[5 : 5 + n])))
                del self._buf[:total]
            else:                                      # 坏帧:吞掉魔数首字节当日志,重同步
                logs.extend(self._buf[:1])
                del self._buf[:1]
        return frames, bytes(logs)
```

- [ ] **Step 4: 跑测试确认通过**

Run: `cd companion && python3 -m pytest tests/test_usb_frames.py -v`
Expected: PASS（7 passed）

- [ ] **Step 5: 提交**

```bash
git add companion/codey/usb_frames.py companion/tests/test_usb_frames.py
git commit -m "feat(companion): USB-CDC 帧编解码 usb_frames(魔数+CRC16+增量解码)"
```

---

## Task 2: 抽出传输无关 `VoiceSession` + asr_stream 改薄(TDD)

把 `asr_stream.handle()` 的会话逻辑搬进 `VoiceSession`,出站走 `Channel`。WS 路径用 `WsChannel` 包装,对外行为逐字节不变。

**Files:**
- Create: `companion/codey/voice_session.py`
- Modify: `companion/asr_stream.py`（`handle()` 函数体,当前 163-311 行)
- Test: `companion/tests/test_voice_session.py`

- [ ] **Step 1: 写失败测试**（用 fake channel/backend/focus,锁住关键行为)

`companion/tests/test_voice_session.py`:
```python
import asyncio
import pytest
from codey.voice_session import VoiceSession


class FakeChannel:
    def __init__(self):
        self.texts = []      # (text, final, seq)
        self.hellos = 0
        self.focus_acks = []

    async def send_text(self, text, final, seq):
        self.texts.append((text, final, seq))

    async def send_hello(self):
        self.hellos += 1

    async def send_focus_ack(self, ok, reason):
        self.focus_acks.append((ok, reason))


class FakeBackend:
    """按预置脚本回放 partial/final;accept 返回事件列表。"""
    def __init__(self, script=None):
        self.script = list(script or [])
        self.started = 0
        self.closed = 0

    async def start(self):
        self.started += 1

    async def accept(self, pcm):
        return self.script.pop(0) if self.script else [{"text": "", "final": False}]

    async def stop(self):
        return [{"text": "最终文本", "final": True}]

    async def close(self):
        self.closed += 1


class FakeFocus:
    def __init__(self):
        self.sent = []        # (pane, text)
        self.pane = 7

    def pane_for_pid(self, pid):
        return self.pane

    def send_text_to_pane(self, pane, text):
        self.sent.append((pane, text))
        return True

    def focus_pid(self, pid):
        return True, "ok"


def make_session(channel, backend, focus, status="waiting"):
    loop = asyncio.get_event_loop()
    return VoiceSession(
        channel=channel,
        make_backend=lambda: backend,
        paster=None,
        loop=loop,
        resolve_pid=lambda sid: 1234,
        resolve_status=lambda sid: status,
        focus=focus,
    )


@pytest.mark.asyncio
async def test_hello_replies_via_channel():
    ch = FakeChannel()
    s = make_session(ch, FakeBackend(), FakeFocus())
    await s.on_control({"type": "hello"})
    assert ch.hellos == 1


@pytest.mark.asyncio
async def test_waiting_session_streams_partials_to_pane():
    ch, be, fx = FakeChannel(), FakeBackend([[{"text": "你好", "final": False}]]), FakeFocus()
    s = make_session(ch, be, fx, status="waiting")
    await s.on_control({"type": "listen", "state": "start", "session": "sid-1", "seq": 3})
    await s.on_pcm(b"\x00\x00")
    assert ch.texts[-1] == ("你好", False, 3)         # stt 带本轮 seq
    assert fx.sent == [(7, "你好")]                    # 流式同步进 pane


@pytest.mark.asyncio
async def test_non_waiting_session_does_not_inject_pane():
    ch, be, fx = FakeChannel(), FakeBackend([[{"text": "x", "final": False}]]), FakeFocus()
    s = make_session(ch, be, fx, status="running")    # 非空闲 → 不注入
    await s.on_control({"type": "listen", "state": "start", "session": "sid-1", "seq": 1})
    await s.on_pcm(b"\x00\x00")
    assert fx.sent == []


@pytest.mark.asyncio
async def test_partial_diff_uses_backspaces_for_changed_tail():
    ch, fx = FakeChannel(), FakeFocus()
    be = FakeBackend([[{"text": "你好", "final": False}], [{"text": "你们", "final": False}]])
    s = make_session(ch, be, fx, status="waiting")
    await s.on_control({"type": "listen", "state": "start", "session": "sid-1", "seq": 0})
    await s.on_pcm(b"\x00\x00")
    await s.on_pcm(b"\x00\x00")
    assert fx.sent[1] == (7, "\x7f们")                 # 公共前缀"你"保留,删"好"补"们"


@pytest.mark.asyncio
async def test_cancel_backspaces_synced_text():
    ch, fx = FakeChannel(), FakeFocus()
    be = FakeBackend([[{"text": "三个字", "final": False}]])
    s = make_session(ch, be, fx, status="waiting")
    await s.on_control({"type": "listen", "state": "start", "session": "sid-1", "seq": 0})
    await s.on_pcm(b"\x00\x00")
    await s.on_control({"type": "listen", "state": "cancel"})
    assert fx.sent[-1] == (7, "\x7f\x7f\x7f")          # 退 3 个码点清残留
```

`companion/tests/conftest.py` 若无 `asyncio_mode`,在 `test_voice_session.py` 顶部加 `pytestmark = pytest.mark.asyncio` 不可行(逐函数已标);确认 `pytest-asyncio` 可用:`cd companion && python3 -c "import pytest_asyncio"`,缺则 `pip install pytest-asyncio` 并在 `companion/pytest.ini`/`conftest.py` 设 `asyncio_mode = auto`。

- [ ] **Step 2: 跑测试确认失败**

Run: `cd companion && python3 -m pytest tests/test_voice_session.py -v`
Expected: FAIL（`ModuleNotFoundError: codey.voice_session`）

- [ ] **Step 3: 实现 `voice_session.py`**（搬运 `handle()` 内逻辑,出站换 channel)

`companion/codey/voice_session.py`:
```python
"""传输无关的语音会话核:从 asr_stream.handle 抽出,WS/USB 共用。
出站经 Channel(send_text/send_hello/send_focus_ack),入站 on_pcm/on_control。"""
import json


class VoiceSession:
    def __init__(self, channel, make_backend, paster, loop,
                 resolve_pid=None, resolve_status=None, focus=None):
        self.ch = channel
        self.make_backend = make_backend
        self.paster = paster
        self.loop = loop
        self._resolve_pid = resolve_pid
        self._resolve_status = resolve_status
        if focus is None:
            from codey import focus as _focus
            focus = _focus
        self.focus = focus
        self.backend = None
        self.last_sent = None
        self.cur_seq = 0
        self.target_pane = None
        self.synced = ""

    def _paste_on(self):
        from codey import envcfg as _ec
        return _ec.paste_enabled() if self.paster is None or self.paster.enabled is None else self.paster.enabled

    def _enter_on(self):
        from codey import envcfg as _ec
        return _ec.auto_enter() if self.paster is None or self.paster.auto_enter is None else self.paster.auto_enter

    async def _sync_to_pane(self, text):
        if self.target_pane is None:
            return
        text = text or ""
        i = 0
        while i < len(self.synced) and i < len(text) and self.synced[i] == text[i]:
            i += 1
        payload = "\x7f" * (len(self.synced) - i) + text[i:]
        ok = True
        if payload:
            ok = await self.loop.run_in_executor(None, self.focus.send_text_to_pane, self.target_pane, payload)
        if ok:
            self.synced = text

    async def _send(self, text, final):
        text = (text or "").strip()
        if final or text != self.last_sent:
            await self.ch.send_text(text, final, self.cur_seq)
            self.last_sent = text
            await self._sync_to_pane(text)

    async def _emit(self, events):
        final_text = None
        for ev in events:
            await self._send(ev["text"], ev["final"])
            if ev["final"] and ev["text"]:
                final_text = ev["text"]
        return final_text

    async def _close_backend(self):
        if self.backend is None:
            return
        close = getattr(self.backend, "close", None)
        if close:
            try:
                await close()
            except Exception:
                pass

    async def on_pcm(self, pcm):
        if self.backend is None:
            self.backend = self.make_backend()
            await self.backend.start()
        try:
            await self._emit(await self.backend.accept(pcm))
        except Exception as e:
            print(f"[asr] accept error: {e}", flush=True)

    async def on_control(self, data):
        t = data.get("type")
        if t == "hello":
            await self.ch.send_hello()
        elif t == "listen" and data.get("state") == "start":
            await self._close_backend()
            self.backend = self.make_backend()
            await self.backend.start()
            self.last_sent = None
            self.synced = ""
            sid = data.get("session") or ""
            self.target_pane = None
            if sid and self._resolve_pid:
                status = self._resolve_status(sid) if self._resolve_status else "waiting"
                if status == "waiting":
                    pid = self._resolve_pid(sid)
                    self.target_pane = await self.loop.run_in_executor(None, self.focus.pane_for_pid, pid)
                    print(f"[voice] target session={sid} pid={pid} pane={self.target_pane}", flush=True)
                else:
                    print(f"[voice] session={sid} status={status} 非空闲 → 不流式同步", flush=True)
            try:
                self.cur_seq = int(data.get("seq") or 0)
            except (TypeError, ValueError):
                self.cur_seq = 0
        elif t == "listen" and data.get("state") == "stop":
            if self.backend is None:
                self.backend = self.make_backend()
                await self.backend.start()
            try:
                final_text = await self._emit(await self.backend.stop())
            except Exception as e:
                print(f"[asr] stop error: {e}", flush=True)
                final_text = None
            await self._close_backend()
            self.backend = None
            if self.target_pane is not None:
                await self._sync_to_pane(final_text or "")
                pasted = False
            else:
                pasted = final_text and self._paste_on()
                if pasted:
                    try:
                        self.paster.paste(final_text)
                        if self._enter_on():
                            self.paster.enter()
                    except Exception as e:
                        print(f"[asr] paste error: {e}", flush=True)
            if final_text:
                from codey import asr_history, envcfg as _ec
                asr_history.append(final_text, engine=_ec.select_engine(), pasted=bool(pasted))
            self.target_pane = None
            self.synced = ""
        elif t == "listen" and data.get("state") == "cancel":
            if self.target_pane is not None and self.synced:
                await self.loop.run_in_executor(None, self.focus.send_text_to_pane,
                                                self.target_pane, "\x7f" * len(self.synced))
            await self._close_backend()
            self.backend = None
            self.target_pane = None
            self.synced = ""
        elif t == "submit":
            if self._paste_on() and self.paster:
                try:
                    self.paster.enter()
                except Exception:
                    pass
        elif t == "clear":
            if self._paste_on() and self.paster:
                try:
                    self.paster.clear()
                except Exception:
                    pass
        elif t == "focus":
            sid = data.get("session") or ""
            pid = self._resolve_pid(sid) if self._resolve_pid else 0
            ok, why = await self.loop.run_in_executor(None, self.focus.focus_pid, pid)
            print(f"[focus] session={sid} pid={pid} -> {ok} ({why})", flush=True)
            await self.ch.send_focus_ack(ok, why)

    async def close(self):
        await self._close_backend()
```

- [ ] **Step 4: 跑 voice_session 测试确认通过**

Run: `cd companion && python3 -m pytest tests/test_voice_session.py -v`
Expected: PASS（6 passed）

- [ ] **Step 5: asr_stream.handle 改薄(WsChannel + VoiceSession)**

把 `companion/asr_stream.py` 当前 `handle()`(163-311 行)整体替换为:
```python
class WsChannel:
    def __init__(self, ws):
        self.ws = ws

    async def send_text(self, text, final, seq):
        await self.ws.send(json.dumps({"type": "stt", "text": text, "final": final, "seq": seq},
                                      ensure_ascii=False))

    async def send_hello(self):
        await self.ws.send(json.dumps({
            "type": "hello", "transport": "websocket", "session_id": "codey",
            "audio_params": {"format": "pcm", "sample_rate": 16000, "channels": 1},
        }))

    async def send_focus_ack(self, ok, reason):
        try:
            await self.ws.send(json.dumps({"type": "focus_ack", "ok": bool(ok), "reason": reason}))
        except Exception:
            pass


async def handle(ws, make_backend=make_backend, paster=None):
    if paster is None:
        paster = default_paster()
    loop = asyncio.get_event_loop()
    from codey.voice_session import VoiceSession
    session = VoiceSession(WsChannel(ws), make_backend, paster, loop,
                           resolve_pid=_resolve_pid, resolve_status=_resolve_status)
    try:
        async for msg in ws:
            if isinstance(msg, (bytes, bytearray)):
                await session.on_pcm(msg)
            else:
                try:
                    data = json.loads(msg)
                except Exception:
                    continue
                await session.on_control(data)
    except websockets.ConnectionClosed:
        pass
    finally:
        await session.close()
```

- [ ] **Step 6: 跑全量 companion 测试,确认 WS 路径零回归**

Run: `cd companion && python3 -m pytest tests/ -q`
Expected: PASS（全绿;尤其 `test_asr_backend.py` / `test_asr_fallback.py` / `test_asr_doubao.py` 不退）

- [ ] **Step 7: 提交**

```bash
git add companion/codey/voice_session.py companion/asr_stream.py companion/tests/test_voice_session.py
git commit -m "refactor(companion): 抽出传输无关 VoiceSession,asr_stream.handle 改薄(WS 行为不变)"
```

---

## Task 3: `usb_link.py` + companion 接线(TDD)

**Files:**
- Create: `companion/codey/usb_link.py`
- Modify: `companion/codey_companion.py`（main(),36-41 行附近)
- Test: `companion/tests/test_usb_link.py`

- [ ] **Step 1: 写失败测试**（用 fake serial + fake app,验握手/STATE/PCM 路由)

`companion/tests/test_usb_link.py`:
```python
import asyncio
import pytest
from codey import usb_frames as uf
from codey.usb_link import UsbChannel, handle_frame


class FakeSerialWriter:
    def __init__(self):
        self.written = bytearray()

    def write(self, data):
        self.written.extend(data)


class FakeSession:
    def __init__(self):
        self.pcm = []
        self.controls = []

    async def on_pcm(self, b):
        self.pcm.append(b)

    async def on_control(self, d):
        self.controls.append(d)


@pytest.mark.asyncio
async def test_hello_frame_triggers_ack_and_marks_online():
    w = FakeSerialWriter()
    ch = UsbChannel(w)
    sess = FakeSession()
    state = {"online": False}
    await handle_frame(uf.HELLO, b"v1", ch, sess, on_hello=lambda: state.__setitem__("online", True))
    assert state["online"] is True
    frames, _ = uf.FrameDecoder().feed(bytes(w.written))
    assert frames and frames[0][0] == uf.HELLO_ACK


@pytest.mark.asyncio
async def test_pcm_frame_routes_to_session():
    sess = FakeSession()
    await handle_frame(uf.PCM, b"\x01\x02", UsbChannel(FakeSerialWriter()), sess)
    assert sess.pcm == [b"\x01\x02"]


@pytest.mark.asyncio
async def test_listen_frame_routes_as_control_json():
    sess = FakeSession()
    payload = b'{"state":"start","session":"sid-1","seq":2}'
    await handle_frame(uf.LISTEN, payload, UsbChannel(FakeSerialWriter()), sess)
    assert sess.controls == [{"type": "listen", "state": "start", "session": "sid-1", "seq": 2}]


@pytest.mark.asyncio
async def test_focus_frame_routes_as_control():
    sess = FakeSession()
    await handle_frame(uf.FOCUS, b"sid-9", UsbChannel(FakeSerialWriter()), sess)
    assert sess.controls == [{"type": "focus", "session": "sid-9"}]


@pytest.mark.asyncio
async def test_usbchannel_send_text_emits_stt_frame():
    w = FakeSerialWriter()
    await UsbChannel(w).send_text("你好", False, 4)
    frames, _ = uf.FrameDecoder().feed(bytes(w.written))
    assert frames[0][0] == uf.STT
    import json
    assert json.loads(frames[0][1]) == {"type": "stt", "text": "你好", "final": False, "seq": 4}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cd companion && python3 -m pytest tests/test_usb_link.py -v`
Expected: FAIL（`ModuleNotFoundError: codey.usb_link`）

- [ ] **Step 3: 实现 `usb_link.py`**

`companion/codey/usb_link.py`:
```python
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
```

- [ ] **Step 4: 跑 usb_link 测试确认通过**

Run: `cd companion && python3 -m pytest tests/test_usb_link.py -v`
Expected: PASS（5 passed）

- [ ] **Step 5: 接线 codey_companion.py**

`companion/codey_companion.py` 在 `main()` 内,起 ASR WS 线程那段(36-39 行)之后加:
```python
    import asr_stream as _asr
    from codey import usb_link
    threading.Thread(target=usb_link.run, args=(app, _asr.make_backend), daemon=True).start()
    print(f"Codey USB link  -> /dev/cu.usbmodem* (有线兜底,优先)")
```

- [ ] **Step 6: 冒烟:companion 能起、无串口时不崩**

Run: `cd companion && timeout 3 python3 codey_companion.py 2>&1 | head -20 || true`
Expected: 打印三行服务地址(含 `USB link`),无串口时 usb 线程静默重试,进程不退。

- [ ] **Step 7: 提交**

```bash
git add companion/codey/usb_link.py companion/codey_companion.py companion/tests/test_usb_link.py
git commit -m "feat(companion): USB 有线兜底链路 usb_link(握手/STATE 推送/语音核接入)"
```

---

## Task 4: 固件帧编解码 `codey_usb.h`(build-verify)

**Files:**
- Create: `sketches/codey_dash/codey_usb.h`
- Modify: `sketches/codey_dash/codey_dash.ino`（全局区 ~106 行后加 `g_usbActive`;setup 区 include + 初始化)

- [ ] **Step 1: 写 `codey_usb.h`**

`sketches/codey_dash/codey_usb.h`:
```cpp
// sketches/codey_dash/codey_usb.h — USB-CDC 有线兜底:帧 C0 DE|type|len(2 LE)|payload|crc16(2 LE)。
// 与日志共用单路 HWCDC;发送加锁减少与 println 交错,接收用增量状态机。companion 端 codey/usb_frames.py 同构。
#pragma once

static const uint8_t USB_M0 = 0xC0, USB_M1 = 0xDE;
enum { U_HELLO = 0x01, U_STATE_REQ = 0x10, U_LISTEN = 0x20, U_PCM = 0x21, U_FOCUS = 0x30,
       U_HELLO_ACK = 0x81, U_STATE = 0x90, U_STT = 0xA0 };

static SemaphoreHandle_t g_usbTxMtx = nullptr;   // setup 里 create;序列化整帧写出

static uint16_t usbCrc16Upd(uint16_t c, const uint8_t* d, size_t n) {
  for (size_t i = 0; i < n; i++) { c ^= (uint16_t)d[i] << 8;
    for (int k = 0; k < 8; k++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1); }
  return c;
}

static void usbSendFrame(uint8_t type, const uint8_t* p, uint16_t n) {
  uint8_t head[5] = { USB_M0, USB_M1, type, (uint8_t)(n & 0xFF), (uint8_t)(n >> 8) };
  uint16_t c = usbCrc16Upd(0xFFFF, head + 2, 3);        // crc over type+len
  c = usbCrc16Upd(c, p, n);                             //          +payload
  uint8_t tail[2] = { (uint8_t)(c & 0xFF), (uint8_t)(c >> 8) };
  if (g_usbTxMtx) xSemaphoreTake(g_usbTxMtx, portMAX_DELAY);
  Serial.write(head, 5); if (n) Serial.write(p, n); Serial.write(tail, 2);
  if (g_usbTxMtx) xSemaphoreGive(g_usbTxMtx);
}

// 增量接收状态机:每收到一整帧调 usbOnFrame()(在 codey_net.h 里定义,处理 HELLO_ACK/STATE/STT)。
static void usbOnFrame(uint8_t type, const uint8_t* payload, uint16_t len);   // fwd decl

static uint8_t  g_rxStage = 0;            // 0=找C0 1=见C0等DE 2=收头 3=收体+crc
static uint8_t  g_rxHdr[3];               // type, lenLo, lenHi
static uint8_t  g_rxHdrGot = 0;
static uint16_t g_rxLen = 0, g_rxGot = 0;
static uint8_t  g_rxBuf[2200];            // payload 上限(STATE json ~1-2KB)
static uint8_t  g_rxCrc[2]; static uint8_t g_rxCrcGot = 0;

static void usbRxByte(uint8_t b) {
  switch (g_rxStage) {
    case 0: if (b == USB_M0) g_rxStage = 1; break;
    case 1: g_rxStage = (b == USB_M1) ? 2 : (b == USB_M0 ? 1 : 0); g_rxHdrGot = 0; break;
    case 2:
      g_rxHdr[g_rxHdrGot++] = b;
      if (g_rxHdrGot == 3) {
        g_rxLen = g_rxHdr[1] | (g_rxHdr[2] << 8);
        if (g_rxLen > sizeof(g_rxBuf)) { g_rxStage = 0; break; }   // 超界丢弃,重同步
        g_rxGot = 0; g_rxCrcGot = 0; g_rxStage = 3;
      }
      break;
    case 3:
      if (g_rxGot < g_rxLen) { g_rxBuf[g_rxGot++] = b; break; }
      g_rxCrc[g_rxCrcGot++] = b;
      if (g_rxCrcGot == 2) {
        uint16_t want = g_rxCrc[0] | (g_rxCrc[1] << 8);
        uint16_t got = usbCrc16Upd(0xFFFF, g_rxHdr, 3);
        got = usbCrc16Upd(got, g_rxBuf, g_rxLen);
        if (got == want) usbOnFrame(g_rxHdr[0], g_rxBuf, g_rxLen);
        g_rxStage = 0;
      }
      break;
  }
}

static void usbRxPump() {                  // 非阻塞 drain
  int avail = Serial.available();
  while (avail-- > 0) { int c = Serial.read(); if (c < 0) break; usbRxByte((uint8_t)c); }
}
```

- [ ] **Step 2: ino 加全局 + 初始化**

`sketches/codey_dash/codey_dash.ino`:全局区(约 106 行 `g_netReconnect` 附近)加:
```cpp
static volatile bool g_usbActive = false;     // USB 有线链路在线(netTask 写/读;在=优先 USB,不走网络)
static volatile uint32_t g_usbLastRx = 0;     // 最近一帧时间(ms);超时判离线
```
setup() 内 `Serial.begin(115200);` 之后加:
```cpp
  Serial.setTxTimeoutMs(0);                     // companion 没在读时写不阻塞 netTask(关键)
  g_usbTxMtx = xSemaphoreCreateMutex();
```
并在文件包含区(约 325 行 `#include "codey_portal.h"` 前)加 `#include "codey_usb.h"`(注意:需在 `g_usbActive` 等全局之后;若顺序冲突,把 include 放到全局声明之后、netTask include 之前)。

- [ ] **Step 3: build 验证(编译 + flash 门禁)**

Run: `./scripts/build.sh sketches/codey_dash 2>&1 | tail -5`
Expected: 编译通过;`Flash usage: NN% (budget 90%)` 且 NN ≤ 90。

- [ ] **Step 4: 提交**

```bash
git add sketches/codey_dash/codey_usb.h sketches/codey_dash/codey_dash.ino
git commit -m "feat(firmware): USB-CDC 帧编解码 codey_usb.h(收发状态机+CRC16+tx-timeout=0)"
```

---

## Task 5: 固件 netTask gate + STATE/STT 复用 + 免配网启动(build-verify)

**Files:**
- Modify: `sketches/codey_dash/codey_dash.ino`（抽 `voiceApplyStt()`;开机握手窗口)
- Modify: `sketches/codey_dash/codey_net.h`（抽 `applyStateDoc()`;netTask gate;`usbOnFrame()` 定义)

- [ ] **Step 1: 抽 `voiceApplyStt()` 供两路复用**

`sketches/codey_dash/codey_dash.ino`:把 `wsEvent()` 里 stt 分支(约 180-188 行)抽成函数,放在 `wsEvent` 前:
```cpp
// stt 落地(WS 与 USB 共用):seq 过滤陈旧会话 + 写 g_transcript + final 置位。
static void voiceApplyStt(const char* text, bool final, int seq, bool hasSeq) {
  if (hasSeq && (uint16_t)seq != g_voiceSeq) return;          // 丢弃陈旧会话的迟到 stt
  strncpy(g_transcript, text ? text : "", sizeof(g_transcript) - 1);
  g_transcript[sizeof(g_transcript) - 1] = '\0';
  if (final) g_sttFinal = true;
}
```
并把 `wsEvent` 的 stt 分支改为调用它:
```cpp
    if (strcmp(doc["type"] | "", "stt") == 0) {
      voiceApplyStt(doc["text"] | "", doc["final"] | false, doc["seq"] | 0, doc["seq"].is<int>());
    }
```

- [ ] **Step 2: 抽 `applyStateDoc()` 供两路复用**

`sketches/codey_dash/codey_net.h`:把 `fetchState()`(约 45-127 行)里「`deserializeJson` 成功之后填充全局」的那段(约 62-126 行)抽成:
```cpp
static void applyStateDoc(JsonDocument& doc) {
  // ...（原 fetchState 中解析 doc → PROV/g_model/display 等的整段逻辑原样搬入)...
}
```
`fetchState()` 改为:解析成功后调 `applyStateDoc(doc);`(HTTP 路径行为不变)。

- [ ] **Step 3: 定义 `usbOnFrame()`(在 codey_net.h,wsConnect 等之后)**

```cpp
// USB 入站帧落地:更新在线时戳;HELLO_ACK→标在线;STATE→复用 applyStateDoc;STT→复用 voiceApplyStt。
static void usbOnFrame(uint8_t type, const uint8_t* payload, uint16_t len) {
  g_usbLastRx = millis();
  if (type == U_HELLO_ACK) { g_usbActive = true; g_companionOk = true; }
  else if (type == U_STATE) {
    JsonDocument doc;
    if (!deserializeJson(doc, payload, len)) { applyStateDoc(doc); g_haveData = true; }
  } else if (type == U_STT) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, len)) return;
    voiceApplyStt(doc["text"] | "", doc["final"] | false, doc["seq"] | 0, doc["seq"].is<int>());
  }
}
```

- [ ] **Step 4: netTask gate(USB 优先,断则回 WiFi)**

`sketches/codey_dash/codey_net.h` 的 `netTask()` 循环体改为:先 `usbRxPump()`,据 `g_usbActive` 分流。把现有循环体(约 144-157 行)改为:
```cpp
    usbRxPump();
    if (g_usbActive && millis() - g_usbLastRx > 4000) g_usbActive = false;   // 超时回落 WiFi
    if (!g_usbActive) usbSendFrame(U_HELLO, (const uint8_t*)"v1", 2);        // 探针:未在线时持续找 companion

    if (g_usbActive) {
      // —— USB 分支:不走网络 ——
      if (g_netListenReq == 1)      { g_netListenReq = 0; usbSendFrame(U_LISTEN, (const uint8_t*)listenStartJson().c_str(), listenStartJson().length()); }
      else if (g_netListenReq == 2) { g_netListenReq = 0; usbSendFrame(U_LISTEN, (const uint8_t*)"{\"state\":\"stop\"}", 16); }
      else if (g_netListenReq == 3) { g_netListenReq = 0; usbSendFrame(U_LISTEN, (const uint8_t*)"{\"state\":\"cancel\"}", 18); }
      if (g_netFocusReq)            { g_netFocusReq = false; usbSendFrame(U_FOCUS, (const uint8_t*)g_focusSid, strlen(g_focusSid)); }
      uint8_t buf[1024]; size_t n;
      while ((n = xStreamBufferReceive(g_voiceSB, buf, sizeof(buf), 0)) > 0) usbSendFrame(U_PCM, buf, n);
    } else if (g_wifi) {
      // —— 现有 WiFi 分支:原样 ——
      if (!started || g_netReconnect) { g_netReconnect = false; resolveMac(); wsConnect(); started = true; lastFetch = 0; }
      g_ws.loop();
      if (g_netListenReq == 1)      { g_netListenReq = 0; wsListen(true); }
      else if (g_netListenReq == 2) { g_netListenReq = 0; wsListen(false); }
      else if (g_netListenReq == 3) { g_netListenReq = 0; wsListenCancel(); }
      if (g_netFocusReq)            { g_netFocusReq = false; wsFocus(g_focusSid); }
      uint8_t buf[1024]; size_t n;
      while (g_wsConn && (n = xStreamBufferReceive(g_voiceSB, buf, sizeof(buf), 0)) > 0) g_ws.sendBIN(buf, n);
      uint32_t now = millis();
      if (!g_voice && (lastFetch == 0 || now - lastFetch > 30000)) { lastFetch = now; fetchState(); maybeRepointWs(); }
    } else { started = false; }
```
说明:`listenStartJson()` 是把 `wsListen(true)` 里构造的 `{"type":"listen","state":"start","session":...,"seq":...}` 去掉 `"type"` 字段的等价 JSON(companion 端 `handle_frame` 会补 `type`)。若 `wsListen` 已有现成字符串构造,复用之;否则新增一个返回 `String` 的小 helper,放在 `codey_net.h` 顶部。FOCUS/PCM 同理。`g_netListenReq`/`g_voiceSB` 等已是传输无关的跨核信号,主 loop 无需改动。

- [ ] **Step 5: 免配网启动(开机握手窗口)**

`sketches/codey_dash/codey_dash.ino` setup() 里,进 `wifiConfigPortal()`(约 486 行)之前加一个短窗口:
```cpp
  for (int i = 0; i < 30 && !g_usbActive; i++) { usbRxPump(); usbSendFrame(U_HELLO, (const uint8_t*)"v1", 2); delay(50); }
  if (!g_usbActive) { /* 现有配网门户逻辑 */ }
```
即:握手成功(`g_usbActive`)则跳过配网门户,纯 USB 启动;否则维持现有行为。

- [ ] **Step 6: build 验证**

Run: `./scripts/build.sh sketches/codey_dash 2>&1 | tail -5`
Expected: 编译通过;`Flash usage: NN% (budget 90%)`,NN ≤ 90。

- [ ] **Step 7: 提交**

```bash
git add sketches/codey_dash/codey_dash.ino sketches/codey_dash/codey_net.h
git commit -m "feat(firmware): netTask USB 优先 gate + STATE/STT 复用 + 免配网启动"
```

---

## Task 6: 真机联调验证(manual)

**Files:** 无(验证)

- [ ] **Step 1: 刷机**

Run: `./scripts/flash.sh sketches/codey_dash $(arduino-cli board list | awk '/usbmodem/{print $1; exit}')`
Expected: `Hash of data verified` + `Hard resetting`。

- [ ] **Step 2: 起 companion(占用串口前先停 monitor)**

Run: `cd companion && python3 codey_companion.py`
Expected: 打印 `USB link -> /dev/cu.usbmodem*`;设备握手后日志出现 `[usb] link on ...`、`[dev] ...` 透传设备日志。

- [ ] **Step 3: 逐项核对**

- [ ] state:**拔掉 WiFi / 不配网**,设备三页 UI 仍由 USB 推的 STATE 正常刷新。
- [ ] 语音:右键开录 → 就地声波环动 → 转写经 USB STT 实时上屏 → 停录定稿;`waiting` 会话流式注入对应 Kaku pane。
- [ ] focus:详情页点屏 → Mac 终端切到该会话 tab。
- [ ] 切换:拔 USB → 4s 内 `g_usbActive` 翻假 → 自动回落 WiFi(若已配网)继续工作;重插 → 重新握手优先 USB。
- [ ] 日志:companion 控制台能看到 `[dev]` 透传的设备日志,帧不被日志破坏(无持续 CRC 报错)。

- [ ] **Step 4: 记录结果**

把真机结果(通过/问题)更新进内存 `ngrok-remote-access-branch` 或新建 `usb-wired-fallback-branch` 记忆。

---

## Self-Review(已执行)

**Spec coverage:**
- §1 线协议 → Task 1(codec)+ Task 4(固件 codec),帧类型一一对应 ✅
- §2 设备侧 gate/tx-timeout/免配网 → Task 4 + Task 5 ✅
- §3 companion usb_frames/usb_link/接线 → Task 1 + Task 3 ✅
- §4 共享语音核 → Task 2 ✅
- §5 错误处理 → 散布:CRC 重同步(T1/T4)、tx-timeout=0(T4)、超时回落(T5)、端口重试(T3)✅
- §6 测试 → T1/T2/T3 pytest + T6 真机 ✅

**Placeholder scan:** 无 TBD/TODO;固件「原样搬入」段以精确行号锚点给出(applyStateDoc 抽取),executing agent 读源即可,非占位。

**Type consistency:** 帧常量名(`HELLO/STATE_REQ/LISTEN/PCM/FOCUS/HELLO_ACK/STATE/STT`)Python(`uf.*`)与固件(`U_*`)对齐;`VoiceSession(channel, make_backend, paster, loop, resolve_pid, resolve_status, focus)` 构造签名在 T2 定义、T2/T3 使用一致;`Channel` 三方法(`send_text/send_hello/send_focus_ack`)在 WsChannel/UsbChannel/FakeChannel 一致;`usbOnFrame/usbRxPump/usbSendFrame/g_usbActive/voiceApplyStt/applyStateDoc` 跨 T4/T5 一致。
