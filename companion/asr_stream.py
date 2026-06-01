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


recognizer = build_recognizer()


async def handle(ws):
    stream = recognizer.create_stream()
    committed = ""        # text from finalized (endpointed) segments this session
    last_sent = None

    async def send(text, final):
        nonlocal last_sent
        text = text.strip()
        if final or text != last_sent:
            await ws.send(json.dumps({"type": "stt", "text": text, "final": final}, ensure_ascii=False))
            last_sent = text

    def decode():
        while recognizer.is_ready(stream):
            recognizer.decode_stream(stream)
        return recognizer.get_result(stream).strip()

    print("[asr] client connected", flush=True)
    try:
        async for msg in ws:
            if isinstance(msg, (bytes, bytearray)):
                samples = np.frombuffer(bytes(msg), dtype=np.int16).astype(np.float32) / 32768.0
                stream.accept_waveform(16000, samples)
                partial = decode()
                full = (committed + partial).strip()
                await send(full, False)
                if recognizer.is_endpoint(stream):
                    if partial:
                        committed = (committed + partial).strip()
                        await send(committed, True)
                    recognizer.reset(stream)
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
                    stream = recognizer.create_stream(); committed = ""; last_sent = None
                    print("[asr] listen start", flush=True)
                elif t == "listen" and data.get("state") == "stop":
                    stream.accept_waveform(16000, np.zeros(int(16000 * 0.4), dtype=np.float32))  # flush tail
                    stream.input_finished()
                    partial = decode()
                    final = (committed + partial).strip()
                    await send(final, True)
                    print(f"[asr] final: {final!r}", flush=True)
    except websockets.ConnectionClosed:
        pass
    finally:
        print("[asr] client closed", flush=True)


async def main():
    print(f"Codey streaming ASR -> ws://0.0.0.0:{PORT}  (sherpa-onnx)", flush=True)
    async with websockets.serve(handle, "0.0.0.0", PORT, max_size=None):
        await asyncio.Future()


if __name__ == "__main__":
    asyncio.run(main())
