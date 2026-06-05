import asyncio, json
import importlib

asr = importlib.import_module("asr_stream")   # companion/ in sys.path via conftest

class FakeBackend:
    def __init__(self): self.started = False; self.fed = []
    async def start(self): self.started = True
    async def accept(self, pcm): self.fed.append(pcm); return [{"text": "partial", "final": False}]
    async def stop(self): return [{"text": "你好世界", "final": True}]

class FakeWS:
    def __init__(self, incoming): self._in = list(incoming); self.sent = []
    def __aiter__(self): self._it = iter(self._in); return self
    async def __anext__(self):
        try: return next(self._it)
        except StopIteration: raise StopAsyncIteration
    async def send(self, m): self.sent.append(m)

def run(coro): return asyncio.get_event_loop().run_until_complete(coro)

def test_final_triggers_paste_and_protocol():
    paster = {"paste": [], "enter": 0, "clear": 0}
    fake = FakeBackend()
    ws = FakeWS([
        json.dumps({"type": "hello"}),
        json.dumps({"type": "listen", "state": "start"}),
        b"\x00\x01" * 8,
        json.dumps({"type": "listen", "state": "stop"}),
    ])
    run(asr.handle(ws,
                   make_backend=lambda: fake,
                   paster=asr.Paster(
                       paste=lambda t: paster["paste"].append(t),
                       enter=lambda: paster.__setitem__("enter", paster["enter"] + 1),
                       clear=lambda: paster.__setitem__("clear", paster["clear"] + 1),
                       enabled=True, auto_enter=False)))
    assert fake.started and fake.fed
    types = [json.loads(m)["type"] for m in ws.sent if isinstance(m, str)]
    assert "hello" in types and "stt" in types
    assert paster["paste"] == ["你好世界"]

def test_submit_and_clear_messages():
    paster = {"paste": [], "enter": 0, "clear": 0}
    ws = FakeWS([json.dumps({"type": "submit"}), json.dumps({"type": "clear"})])
    run(asr.handle(ws, make_backend=lambda: FakeBackend(),
                   paster=asr.Paster(
                       paste=lambda t: paster["paste"].append(t),
                       enter=lambda: paster.__setitem__("enter", paster["enter"] + 1),
                       clear=lambda: paster.__setitem__("clear", paster["clear"] + 1),
                       enabled=True, auto_enter=False)))
    assert paster["enter"] == 1 and paster["clear"] == 1

def test_auto_enter_after_paste():
    paster = {"paste": [], "enter": 0}
    ws = FakeWS([json.dumps({"type": "listen", "state": "stop"})])
    run(asr.handle(ws, make_backend=lambda: FakeBackend(),
                   paster=asr.Paster(
                       paste=lambda t: paster["paste"].append(t),
                       enter=lambda: paster.__setitem__("enter", paster["enter"] + 1),
                       clear=lambda: None, enabled=True, auto_enter=True)))
    assert paster["paste"] == ["你好世界"] and paster["enter"] == 1


def _noop_paster():
    return asr.Paster(paste=lambda t: None, enter=lambda: None, clear=lambda: None,
                      enabled=True, auto_enter=False)


class ClosableBackend(FakeBackend):
    def __init__(self, tag, closed):
        super().__init__(); self.tag = tag; self._closed = closed
    async def close(self): self._closed.append(self.tag)


def test_in_flight_backend_closed_on_abrupt_disconnect():
    # listen:start then PCM, stream ends WITHOUT listen:stop -> finally must close the backend
    closed = []
    ws = FakeWS([json.dumps({"type": "listen", "state": "start"}), b"\x00\x01" * 8])
    run(asr.handle(ws, make_backend=lambda: ClosableBackend("a", closed), paster=_noop_paster()))
    assert closed == ["a"]


def test_old_backend_closed_when_new_listen_start():
    closed = []
    tags = iter(["a", "b"])
    ws = FakeWS([json.dumps({"type": "listen", "state": "start"}),
                 json.dumps({"type": "listen", "state": "start"})])
    run(asr.handle(ws, make_backend=lambda: ClosableBackend(next(tags), closed),
                   paster=_noop_paster()))
    # 2nd start closes 'a'; finally closes 'b'
    assert closed == ["a", "b"]


def test_accept_error_does_not_kill_connection():
    # a backend whose accept raises must not abort the loop; submit afterwards still works
    paster = {"enter": 0}
    class BadAccept(FakeBackend):
        async def accept(self, pcm): raise RuntimeError("decode boom")
    ws = FakeWS([json.dumps({"type": "listen", "state": "start"}), b"\x00\x01" * 8,
                 json.dumps({"type": "submit"})])
    run(asr.handle(ws, make_backend=lambda: BadAccept(),
                   paster=asr.Paster(paste=lambda t: None,
                                     enter=lambda: paster.__setitem__("enter", paster["enter"] + 1),
                                     clear=lambda: None, enabled=True, auto_enter=False)))
    assert paster["enter"] == 1     # 连接存活,后续 submit 仍被处理
