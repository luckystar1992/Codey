# companion/codey/asr_tencent.py
"""腾讯云实时 ASR 后端(适配 asr_stream 的 start/accept/stop/close 接口)。

桥接腾讯云语音 SDK(回调式,websocket-client 后台线程)→ Codey 的「accept 返回 growing partial、
stop 返回 final」语义。SDK vendored 在 companion/vendor/tencent_speech(扁平 import 需入 sys.path)。
依赖:websocket-client。key 走 config/env(tencent_appid/secret_id/secret_key/engine)。"""
import asyncio
import os
import socket
import sys
import time

# 腾讯 ASR endpoint:健康探测(TCP 连接 RTT 近似网络延时)。超阈值 → 回落本地 sherpa。
TENCENT_HOST = "asr.cloud.tencent.com"
TENCENT_MAX_LATENCY_MS = float(os.environ.get("TENCENT_MAX_LATENCY_MS", "200"))

_VENDOR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                       "vendor", "tencent_speech")


def _ensure_sdk_path():
    if _VENDOR not in sys.path:
        sys.path.insert(0, _VENDOR)


def _cfg():
    from codey import config            # 分层:config.json > env > 默认
    return {
        "appid": config.get("tencent_appid"),
        "secret_id": config.get("tencent_secret_id"),
        "secret_key": config.get("tencent_secret_key"),
        "engine": (config.get("tencent_engine") or "16k_zh"),
    }


def health_check(timeout=1.0, attempts=2):
    """探腾讯 ASR endpoint 可达性 + 网络延时(TCP 连接 RTT,无需鉴权)。取最小 RTT 削抖。
    返回 (ok: bool, latency_ms: float|None, reason: str)。"""
    best = None
    reason = ""
    for _ in range(max(1, attempts)):
        t0 = time.perf_counter()
        try:
            with socket.create_connection((TENCENT_HOST, 443), timeout=timeout):
                dt = (time.perf_counter() - t0) * 1000.0
                best = dt if best is None else min(best, dt)
        except Exception as e:
            reason = f"{type(e).__name__}: {e}"
    if best is None:
        return (False, None, reason or "unreachable")
    return (True, best, "")


class _TencentListener:
    """SDK 回调累积(纯类、duck-type,可单测)。slice_type 1=partial / 2=句末。
    text = 已结束句拼接 + 当前句 partial(growing,语义同 SherpaBackend)。"""

    def __init__(self):
        self.committed = ""        # 已 on_sentence_end 的句子拼接
        self.text = ""             # 对外 growing text:committed + 当前句 partial
        self.error = None

    @staticmethod
    def _seg(response):
        return (response.get("result") or {}).get("voice_text_str", "") or ""

    def on_recognition_start(self, response):
        pass

    def on_sentence_begin(self, response):
        pass

    def on_recognition_result_change(self, response):   # slice_type 1:当前句 partial
        self.text = self.committed + self._seg(response)

    def on_sentence_end(self, response):                # slice_type 2:句末定稿 → 并入 committed
        self.committed += self._seg(response)
        self.text = self.committed

    def on_recognition_complete(self, response):
        pass

    def on_fail(self, response):
        self.error = (response or {}).get("message") or "tencent asr fail"
        print(f"[asr] tencent fail: {self.error}", flush=True)


class TencentBackend:
    """腾讯实时 ASR 后端。SDK 的 write/stop 会阻塞(等连接/等终态)→ 全部丢线程池跑,不卡 asyncio loop。"""

    def __init__(self):
        self._rec = None
        self._lis = None

    def _build(self):
        _ensure_sdk_path()
        from common import credential
        from asr import speech_recognizer
        c = _cfg()
        cred = credential.Credential(c["secret_id"], c["secret_key"])
        lis = _TencentListener()
        rec = speech_recognizer.SpeechRecognizer(c["appid"], cred, c["engine"], lis)
        rec.set_voice_format(1)        # 1 = PCM
        rec.set_need_vad(1)            # 服务端 VAD 断句
        rec.set_filter_modal(1)        # 过滤语气词
        rec.set_filter_dirty(1)        # 过滤脏词
        rec.set_filter_punc(0)         # 保留标点(0=不过滤)
        rec.set_convert_num_mode(1)    # 智能转数字
        rec.set_word_info(0)           # 不要词级时间戳
        return rec, lis

    async def start(self):
        loop = asyncio.get_event_loop()
        self._rec, self._lis = self._build()
        await loop.run_in_executor(None, self._rec.start)

    async def accept(self, pcm):
        loop = asyncio.get_event_loop()
        if self._rec is not None:
            await loop.run_in_executor(None, self._rec.write, bytes(pcm))
        return [{"text": (self._lis.text if self._lis else ""), "final": False}]

    async def stop(self):
        loop = asyncio.get_event_loop()
        if self._rec is not None:
            await loop.run_in_executor(None, self._rec.stop)       # 阻塞至终态(wst.join)
        text = (self._lis.text if self._lis else "") or ""
        return [{"text": text, "final": True}]

    async def close(self):
        loop = asyncio.get_event_loop()
        rec, self._rec = self._rec, None
        if rec is not None:
            try:
                await loop.run_in_executor(None, rec.stop)
            except Exception:
                pass
