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
    from codey import paste as _p, envcfg as _c
    return Paster(paste=_p.paste_to_active_window, enter=_p.press_enter, clear=_p.clear_input,
                  enabled=_c.paste_enabled(), auto_enter=_c.auto_enter())


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


def make_backend():
    """按 env 选引擎。doubao 在后续任务接入;此处先只有 sherpa。"""
    from codey import envcfg
    if envcfg.select_engine() == "doubao":
        from codey.asr_doubao import DoubaoBackend          # 后续任务提供
        return DoubaoBackend()
    b = SherpaBackend(get_recognizer())
    b.punct = _get_punctuator()
    return b


async def handle(ws, make_backend=make_backend, paster=None):
    if paster is None:
        paster = default_paster()
    backend = None
    last_sent = None

    async def send(text, final):
        nonlocal last_sent
        text = (text or "").strip()
        if final or text != last_sent:
            await ws.send(json.dumps({"type": "stt", "text": text, "final": final}, ensure_ascii=False))
            last_sent = text

    async def emit(events):
        final_text = None
        for ev in events:
            await send(ev["text"], ev["final"])
            if ev["final"] and ev["text"]:
                final_text = ev["text"]
        return final_text

    try:
        async for msg in ws:
            if isinstance(msg, (bytes, bytearray)):
                if backend is None:
                    backend = make_backend(); await backend.start()
                await emit(await backend.accept(msg))
            else:
                try:
                    data = json.loads(msg)
                except Exception:
                    continue
                t = data.get("type")
                if t == "hello":
                    await ws.send(json.dumps({
                        "type": "hello", "transport": "websocket", "session_id": "codey",
                        "audio_params": {"format": "pcm", "sample_rate": 16000, "channels": 1},
                    }))
                elif t == "listen" and data.get("state") == "start":
                    backend = make_backend(); await backend.start()
                    last_sent = None
                elif t == "listen" and data.get("state") == "stop":
                    if backend is None:
                        backend = make_backend(); await backend.start()
                    final_text = await emit(await backend.stop())
                    backend = None
                    if final_text and paster.enabled:
                        paster.paste(final_text)
                        if paster.auto_enter:
                            paster.enter()
                elif t == "submit":
                    if paster.enabled:
                        paster.enter()
                elif t == "clear":
                    if paster.enabled:
                        paster.clear()
    except websockets.ConnectionClosed:
        pass


async def main():
    from codey import envcfg
    envcfg.load_dotenv()
    print(f"Codey streaming ASR -> ws://0.0.0.0:{PORT}  (engine={envcfg.select_engine()})", flush=True)
    async with websockets.serve(lambda ws: handle(ws), "0.0.0.0", PORT, max_size=None):
        await asyncio.Future()


if __name__ == "__main__":
    asyncio.run(main())
