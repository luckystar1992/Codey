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


def make_backend():
    """按 env 选引擎。doubao 在后续任务接入;此处先只有 sherpa。"""
    from codey import envcfg
    if envcfg.select_engine() == "doubao":
        from codey.asr_doubao import DoubaoBackend          # 后续任务提供
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


async def handle(ws, make_backend=make_backend, paster=None):
    if paster is None:
        paster = default_paster()
    from codey import envcfg as _ec
    def paste_on():  return _ec.paste_enabled() if paster.enabled is None else paster.enabled
    def enter_on():  return _ec.auto_enter() if paster.auto_enter is None else paster.auto_enter
    backend = None
    last_sent = None
    cur_seq = 0          # 回显 listen:start 带来的会话序号,设备据此丢弃陈旧会话的迟到结果
    from codey import focus as _focus     # 终端 pane 流式同步
    loop = asyncio.get_event_loop()
    target_pane = None   # 本轮语音目标 Kaku pane(详情页发起时解析);None=回退到「停录粘贴到前台」
    synced = ""          # 已同步进该 pane 输入框的文本(下次 diff 增量更新)

    async def sync_to_pane(text):         # 流式 partial diff 同步进目标 pane 输入:退格删改动尾 + 补新尾
        nonlocal synced
        if target_pane is None:
            return
        text = text or ""
        i = 0
        while i < len(synced) and i < len(text) and synced[i] == text[i]:
            i += 1
        payload = "\x7f" * (len(synced) - i) + text[i:]
        ok = True
        if payload:
            ok = await loop.run_in_executor(None, _focus.send_text_to_pane, target_pane, payload)
        if ok:                            # 发送失败则不推进 baseline,下个 partial 重算全量增量(防永久错位)
            synced = text

    async def send(text, final):
        nonlocal last_sent
        text = (text or "").strip()
        if final or text != last_sent:
            await ws.send(json.dumps({"type": "stt", "text": text, "final": final, "seq": cur_seq},
                                     ensure_ascii=False))
            last_sent = text
            await sync_to_pane(text)      # 同步进对应会话的 Mac 终端输入

    async def emit(events):
        final_text = None
        for ev in events:
            await send(ev["text"], ev["final"])
            if ev["final"] and ev["text"]:
                final_text = ev["text"]
        return final_text

    async def close_backend(b):
        if b is None:
            return
        close = getattr(b, "close", None)
        if close:
            try:
                await close()
            except Exception:
                pass

    try:
        async for msg in ws:
            if isinstance(msg, (bytes, bytearray)):
                if backend is None:
                    backend = make_backend(); await backend.start()
                try:
                    await emit(await backend.accept(msg))
                except Exception as e:           # 单帧解码出错不应拖垮整条连接
                    print(f"[asr] accept error: {e}", flush=True)
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
                    await close_backend(backend)         # 关掉上一个(防 doubao ws/reader 泄漏)
                    backend = make_backend(); await backend.start()
                    last_sent = None
                    synced = ""
                    sid = data.get("session") or ""
                    target_pane = None
                    if sid and _resolve_pid:             # 详情页发起 → 解析目标会话的 Kaku pane(流式同步进它)
                        status = _resolve_status(sid) if _resolve_status else "waiting"
                        if status == "waiting":          # 仅 agent 在等输入(空闲于提示符)才注入,避免打断生成/污染 TUI
                            pid = _resolve_pid(sid)
                            target_pane = await loop.run_in_executor(None, _focus.pane_for_pid, pid)
                            print(f"[voice] target session={sid} pid={pid} pane={target_pane}", flush=True)
                        else:                            # 非空闲 → 不流式注入,回退到停录粘贴到前台
                            print(f"[voice] session={sid} status={status} 非空闲 → 不流式同步", flush=True)
                    try:                                 # 本轮会话序号,后续 stt 回显;公网客户端可能发非法值
                        cur_seq = int(data.get("seq") or 0)
                    except (TypeError, ValueError):
                        cur_seq = 0
                elif t == "listen" and data.get("state") == "stop":
                    if backend is None:
                        backend = make_backend(); await backend.start()
                    try:
                        final_text = await emit(await backend.stop())
                    except Exception as e:
                        print(f"[asr] stop error: {e}", flush=True); final_text = None
                    await close_backend(backend)
                    backend = None
                    if target_pane is not None:          # 已流式同步进目标 pane → 不再粘贴到前台
                        await sync_to_pane(final_text or "")   # 定稿/空/异常都对账:覆盖 partial 或退格清残留
                        pasted = False
                    else:
                        pasted = final_text and paste_on()
                        if pasted:
                            try:                         # 粘贴失败(如未授辅助功能)不应断开连接
                                paster.paste(final_text)
                                if enter_on():
                                    paster.enter()
                            except Exception as e:
                                print(f"[asr] paste error: {e}", flush=True)
                    if final_text:
                        from codey import asr_history
                        asr_history.append(final_text, engine=_ec.select_engine(), pasted=bool(pasted))
                    target_pane = None; synced = ""      # 重置:防 stop 后迟到缓冲 PCM 再 diff 污染已定稿输入
                elif t == "listen" and data.get("state") == "cancel":   # 设备 BtnA 取消 → 清掉已同步进输入框的文本
                    if target_pane is not None and synced:
                        await loop.run_in_executor(None, _focus.send_text_to_pane,
                                                   target_pane, "\x7f" * len(synced))
                    await close_backend(backend); backend = None
                    target_pane = None; synced = ""
                elif t == "submit":
                    if paste_on():
                        try: paster.enter()
                        except Exception: pass
                elif t == "clear":
                    if paste_on():
                        try: paster.clear()
                        except Exception: pass
                elif t == "focus":                       # 设备详情页点屏「切到此会话」→ 切 macOS 终端 tab
                    sid = data.get("session") or ""
                    pid = _resolve_pid(sid) if _resolve_pid else 0
                    from codey import focus as _focus
                    ok, why = await asyncio.get_event_loop().run_in_executor(   # osascript 阻塞 → 丢线程池,不卡 loop
                        None, _focus.focus_pid, pid)
                    print(f"[focus] session={sid} pid={pid} -> {ok} ({why})", flush=True)
                    try:
                        await ws.send(json.dumps({"type": "focus_ack", "ok": bool(ok), "reason": why}))
                    except Exception:
                        pass
    except websockets.ConnectionClosed:
        pass
    finally:
        await close_backend(backend)                     # 中途断连(无 listen:stop)也释放在用后端


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
