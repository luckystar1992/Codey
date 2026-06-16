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
