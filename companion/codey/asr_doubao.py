# companion/codey/asr_doubao.py
"""豆包 seed-asr 流式(sauc 2.0)客户端 + 文件回落。
二进制协议参考 meme/doubao_streaming.py(源自 xiaozhi-esp32-server)。"""
import asyncio
import gzip
import json
import os
import uuid

import websockets


def build_header(message_type, flags=0):
    return bytearray([(0x1 << 4) | 0x1, (message_type << 4) | flags, (0x1 << 4) | 0x1, 0])


def build_frame(message_type, payload_bytes, last=False):
    flags = 0x2 if last else 0x0
    body = gzip.compress(payload_bytes)
    frame = build_header(message_type, flags)
    frame.extend(len(body).to_bytes(4, "big"))
    frame.extend(body)
    return bytes(frame)


def parse_response(data):
    if len(data) < 4:
        return {"error": f"short {len(data)}b"}
    message_type = data[1] >> 4
    if message_type == 0xF:
        code = int.from_bytes(data[4:8], "big")
        msg_len = int.from_bytes(data[8:12], "big")
        try:
            payload = json.loads(data[12:12 + msg_len].decode("utf-8"))
        except Exception:
            payload = {"raw": data[12:].hex()}
        return {"error": f"server_error code={code}", "payload": payload}
    if len(data) < 12:
        return {"error": "too short"}
    length = int.from_bytes(data[8:12], "big")
    body = data[12:12 + length] if 0 < length <= len(data) - 12 else data[8:]
    try:
        return {"payload": json.loads(body.decode("utf-8", errors="replace"))}
    except Exception as e:
        return {"error": f"parse json: {e}"}


class UtteranceMerger:
    """累積 definite 段為 final_text(去重),partial 段拼在尾巴上給 self.text。"""
    def __init__(self):
        self.final_text = ""
        self.text = ""
        self._seen = set()

    def feed(self, utterances):
        for u in utterances or []:
            txt = u.get("text")
            if u.get("definite") and txt:
                key = (u.get("start_time"), u.get("end_time"), txt)
                if key not in self._seen:
                    self._seen.add(key)
                    self.final_text += txt
        partials = [u["text"] for u in (utterances or []) if not u.get("definite") and u.get("text")]
        self.text = self.final_text + "".join(partials)
        return self.text


WS_URL      = os.environ.get("DOUBAO_STREAMING_URL", "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async")
RESOURCE_ID = os.environ.get("DOUBAO_STREAMING_RESOURCE_ID", "volc.seedasr.sauc.duration")


def _cfg():
    return {"app_id": os.environ.get("DOUBAO_APP_ID", ""), "api_key": os.environ.get("DOUBAO_API_KEY", "")}


class StreamingASRSession:
    def __init__(self):
        self.ws = None
        self.req_id = str(uuid.uuid4())
        self.merger = UtteranceMerger()
        self._reader = None
        self._on_partial = None
        self._connect_task = None
        self._pending = []
        self._lock = asyncio.Lock()
        self._connect_error = None

    def set_partial_callback(self, cb):
        self._on_partial = cb

    def start_background(self):
        self._connect_task = asyncio.create_task(self._connect_and_drain())

    async def _connect_and_drain(self):
        try:
            await self._connect()
        except Exception as e:
            self._connect_error = e
            return
        async with self._lock:
            for pcm in (self._pending or []):
                try:
                    await self.ws.send(build_frame(0x2, pcm, last=False))
                except Exception:
                    break
            self._pending = None

    async def _connect(self):
        c = _cfg()
        headers = {
            "x-api-key": c["api_key"], "X-Api-Access-Key": c["api_key"],
            "X-Api-App-Key": c["app_id"], "X-Api-Resource-Id": RESOURCE_ID,
            "X-Api-Connect-Id": str(uuid.uuid4()),
        }
        self.ws = await websockets.connect(WS_URL, additional_headers=headers, max_size=None,
                                           ping_interval=None, ping_timeout=None,
                                           close_timeout=10, open_timeout=20)
        config = {
            "app": {"appid": c["app_id"], "token": c["api_key"]},
            "user": {"uid": "codey"},
            "request": {"reqid": self.req_id,
                        "workflow": "audio_in,resample,partition,vad,fe,decode,itn,nlu_punctuate",
                        "show_utterances": True, "result_type": "single",
                        "sequence": 1, "end_window_size": 200},
            "audio": {"format": "pcm", "codec": "pcm", "rate": 16000, "bits": 16,
                      "channel": 1, "sample_rate": 16000},
        }
        await self.ws.send(build_frame(0x1, json.dumps(config).encode("utf-8")))
        init = await asyncio.wait_for(self.ws.recv(), timeout=10)
        parsed = parse_response(init)
        if "error" in parsed:
            raise RuntimeError(f"doubao init failed: {parsed}")
        self._reader = asyncio.create_task(self._read_loop())

    async def send_audio(self, pcm):
        async with self._lock:
            if self._pending is not None:
                self._pending.append(pcm); return
            if self.ws:
                try:
                    await self.ws.send(build_frame(0x2, pcm, last=False))
                except Exception:
                    pass

    async def finalize(self, timeout_s=8.0):
        if self._connect_task and not self._connect_task.done():
            try:
                await asyncio.wait_for(asyncio.shield(self._connect_task), timeout=3.0)
            except Exception:
                pass
        if self._connect_error or not self.ws:
            await self.close(); return ""
        async with self._lock:
            for pcm in (self._pending or []):
                try: await self.ws.send(build_frame(0x2, pcm, last=False))
                except Exception: pass
            self._pending = None
            try: await self.ws.send(build_frame(0x2, b"", last=True))
            except Exception: pass
        try:
            if self._reader:
                await asyncio.wait_for(self._reader, timeout=timeout_s)
        except Exception:
            pass
        await self.close()
        return self.merger.final_text or self.merger.text

    async def close(self):
        if self._reader and not self._reader.done():
            self._reader.cancel()
            try: await self._reader
            except Exception: pass
        if self.ws:
            try: await self.ws.close()
            except Exception: pass
            self.ws = None

    async def _read_loop(self):
        try:
            while self.ws:
                try:
                    data = await self.ws.recv()
                except websockets.ConnectionClosed:
                    return
                parsed = parse_response(data)
                if "error" in parsed:
                    code = (parsed.get("payload") or {}).get("code")
                    if code == 1013:
                        continue
                    continue
                p = parsed.get("payload") or {}
                if "result" in p:
                    self.merger.feed(p["result"].get("utterances", []))
                    if self._on_partial:
                        try: self._on_partial(self.merger.text)
                        except Exception: pass
        except asyncio.CancelledError:
            pass
        except Exception:
            pass


class DoubaoBackend:
    """适配 asr_stream 后端接口。accept 返回当前累积 partial;stop 走 finalize(+文件回落见后续任务)。"""
    def __init__(self):
        self.sess = StreamingASRSession()
        self._pcm = bytearray()       # 留存原始 PCM 供文件回落(后续任务)

    async def start(self):
        self.sess.start_background()

    async def accept(self, pcm):
        self._pcm += bytes(pcm)
        await self.sess.send_audio(bytes(pcm))
        return [{"text": self.sess.merger.text, "final": False}]

    async def stop(self):
        text = await self.sess.finalize()
        return [{"text": (text or "").strip(), "final": True}]
