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
    loop = asyncio.get_running_loop()
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


@pytest.mark.asyncio
async def test_stop_reconciles_final_text_to_pane():
    """stop 后最终文本应覆盖到 pane(替换 partial 尾)。"""
    ch, fx = FakeChannel(), FakeFocus()
    be = FakeBackend([[{"text": "部分", "final": False}]])
    s = make_session(ch, be, fx, status="waiting")
    await s.on_control({"type": "listen", "state": "start", "session": "sid-1", "seq": 0})
    await s.on_pcm(b"\x00\x00")          # synced = "部分"
    await s.on_control({"type": "listen", "state": "stop"})
    # FakeBackend.stop() returns "最终文本"; should diff from "部分"
    # 公共前缀 "" → delete 2 chars, append 最终文本
    assert fx.sent[-1] == (7, "\x7f\x7f最终文本")
