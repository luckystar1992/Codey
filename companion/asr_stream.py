#!/usr/bin/env python3
"""
Codey streaming ASR server — true real-time partial transcripts for the StopWatch.

xiaozhi-style WebSocket voice protocol, but raw PCM (16k/mono/int16-LE) instead of Opus:
  device -> {"type":"hello",...}              server -> {"type":"hello","session_id":...}
  device -> {"type":"listen","state":"start"} (fresh recognizer stream)
  device -> <binary PCM frames>               server -> {"type":"stt","text":<growing>,"final":false}  (partials)
  device -> {"type":"listen","state":"stop"}  server -> {"type":"stt","text":<final>,"final":true}

Engine: sherpa-onnx streaming Zipformer (zh-en) — emits a continuously-growing partial
transcript every chunk + endpoint detection, so the watch shows text as you speak.

Run:  python3 asr_stream.py        (CODEY_ASR_PORT, default 8788)
"""

import asyncio
import glob
import json
import os
import sys
from collections import namedtuple

import numpy as np
import sherpa_onnx
import websockets

PORT = int(os.environ.get("CODEY_ASR_PORT", "8788"))
MODELS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "models")


def find_model_dir():
    for d in sorted(glob.glob(os.path.join(MODELS_DIR, "*streaming-zipformer*"))):
        if os.path.isdir(d) and glob.glob(os.path.join(d, "tokens.txt")):
            return d
    raise SystemExit(f"No streaming-zipformer model under {MODELS_DIR} (run the download first)")


def pick(d, *patterns):
    for pat in patterns:
        hits = sorted(glob.glob(os.path.join(d, pat)))
        if hits:
            return hits[0]
    raise SystemExit(f"missing model file {patterns} in {d}")


def build_recognizer():
    d = find_model_dir()
    # prefer int8 (faster/smaller); fall back to float32
    encoder = pick(d, "encoder-*.int8.onnx", "encoder-*.onnx")
    decoder = pick(d, "decoder-*.onnx")
    joiner = pick(d, "joiner-*.int8.onnx", "joiner-*.onnx")
    tokens = pick(d, "tokens.txt")
    print(f"[asr] model dir: {os.path.basename(d)}", flush=True)
    return sherpa_onnx.OnlineRecognizer.from_transducer(
        tokens=tokens, encoder=encoder, decoder=decoder, joiner=joiner,
        num_threads=2, sample_rate=16000, feature_dim=80,
        decoding_method="greedy_search",
        enable_endpoint_detection=True,
        rule1_min_trailing_silence=2.4,
        rule2_min_trailing_silence=1.0,
        rule3_min_utterance_length=300,
    )


_recognizer = None


def get_recognizer():
    global _recognizer
    if _recognizer is None:
        _recognizer = build_recognizer()
    return _recognizer


_punct = None


def _get_punctuator():
    global _punct
    if _punct is None:
        from codey.asr_punct import Punctuator
        _punct = Punctuator()
    return _punct


# 注入式副作用封装(默认绑定真实 paste;测试传假实现)
Paster = namedtuple("Paster", "paste enter clear enabled auto_enter")


def default_paster():
    from codey import paste as _p
    # enabled/auto_enter=None -> handle() 在用时实时读 config(改粘贴配置不必重连 WS / 重启服务或设备)
    return Paster(paste=_p.paste_to_active_window, enter=_p.press_enter, clear=_p.clear_input,
                  enabled=None, auto_enter=None)


class SherpaBackend:
    """本地 sherpa 流式后端:行为与原 handle 等价(每块 partial + endpoint 段 final)。"""
    def __init__(self, rec):
        self.rec = rec
        self.stream = rec.create_stream()
        self.committed = ""
        self.punct = None

    async def start(self):
        self.stream = self.rec.create_stream()
        self.committed = ""

    def _decode(self):
        while self.rec.is_ready(self.stream):
            self.rec.decode_stream(self.stream)
        return self.rec.get_result(self.stream).strip()

    async def accept(self, pcm):
        samples = np.frombuffer(bytes(pcm), dtype=np.int16).astype(np.float32) / 32768.0
        self.stream.accept_waveform(16000, samples)
        partial = self._decode()
        full = (self.committed + partial).strip()
        out = [{"text": full, "final": False}]
        if self.rec.is_endpoint(self.stream):
            if partial:
                self.committed = full
                out.append({"text": self.committed, "final": True})
            self.rec.reset(self.stream)
        return out

    async def stop(self):
        self.stream.accept_waveform(16000, np.zeros(int(16000 * 0.4), dtype=np.float32))
        self.stream.input_finished()
        partial = self._decode()
        text = (self.committed + partial).strip()
        if self.punct:
            text = self.punct.add(text)
        return [{"text": text, "final": True}]

    async def close(self):
        pass        # sherpa 流随 GC 释放,无需显式关闭(接口对称)


# 启动期健康探测可强制引擎(None=按配置;"sherpa"/"doubao"=强制本运行)。
_engine_override = None


def set_engine_override(engine):
    """启动期豆包不可达/超时 → set_engine_override('sherpa') 让本运行全部走本地。"""
    global _engine_override
    _engine_override = engine


def effective_engine():
    """实际生效引擎:override 优先,否则按配置(envcfg.select_engine)。"""
    from codey import envcfg
    return _engine_override or envcfg.select_engine()


def make_backend():
    """按生效引擎建后端:doubao -> 豆包流式;否则本地 sherpa。"""
    if effective_engine() == "doubao":
        from codey.asr_doubao import DoubaoBackend
        return DoubaoBackend()
    b = SherpaBackend(get_recognizer())
    b.punct = _get_punctuator()
    return b


# 设备「切到此会话」/语音流式同步:codey_companion 注入 session_id -> agent PID / status 的解析器。
_resolve_pid = None
_resolve_status = None
def set_pid_resolver(fn):
    global _resolve_pid
    _resolve_pid = fn
def set_status_resolver(fn):
    global _resolve_status
    _resolve_status = fn


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


async def main():
    from codey import envcfg
    envcfg.load_dotenv()
    print(f"Codey streaming ASR -> ws://0.0.0.0:{PORT}  (engine={envcfg.select_engine()})", flush=True)
    async with websockets.serve(lambda ws: handle(ws), "0.0.0.0", PORT, max_size=None):
        await asyncio.Future()


def run_server():
    """供 codey_companion 在后台线程里启动 ASR WS 服务(各自 asyncio loop)。"""
    asyncio.run(main())


if __name__ == "__main__":
    asyncio.run(main())
