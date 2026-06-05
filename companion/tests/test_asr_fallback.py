import struct, asyncio
from codey import asr_doubao as d

def test_pcm_to_wav_header():
    pcm = b"\x01\x00" * 16000        # 1s @16k mono int16
    wav = d.pcm_to_wav_bytes(pcm, rate=16000)
    assert wav[:4] == b"RIFF" and wav[8:12] == b"WAVE"
    assert struct.unpack("<I", wav[40:44])[0] == len(pcm)   # data chunk size

def test_doubao_backend_falls_back_when_stream_empty(monkeypatch):
    b = d.DoubaoBackend()
    b._pcm = bytearray(b"\x00\x00" * 16000)     # 1s captured
    async def fake_finalize(*a, **k): return ""           # 流式返回空
    monkeypatch.setattr(b.sess, "finalize", fake_finalize)
    async def fake_file(wav_bytes, **k): return {"text": "回落结果"}
    monkeypatch.setattr(d, "transcribe_wav_bytes", fake_file)
    out = asyncio.get_event_loop().run_until_complete(b.stop())
    assert out == [{"text": "回落结果", "final": True}]

def test_no_fallback_when_stream_has_text(monkeypatch):
    b = d.DoubaoBackend(); b._pcm = bytearray(b"\x00\x00" * 16000)
    async def fake_finalize(*a, **k): return "流式结果"
    monkeypatch.setattr(b.sess, "finalize", fake_finalize)
    called = {"n": 0}
    async def fake_file(*a, **k): called["n"] += 1; return {"text": "x"}
    monkeypatch.setattr(d, "transcribe_wav_bytes", fake_file)
    out = asyncio.get_event_loop().run_until_complete(b.stop())
    assert out == [{"text": "流式结果", "final": True}] and called["n"] == 0
