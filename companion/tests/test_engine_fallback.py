"""启动期豆包健康探测 → 引擎回落 的接线测试(asr_stream override + codey_companion glue)。"""
import importlib

asr = importlib.import_module("asr_stream")        # companion/ 已在 sys.path(conftest)
cc = importlib.import_module("codey_companion")
from codey import asr_doubao


def test_engine_override_takes_precedence():
    try:
        asr.set_engine_override("sherpa")
        assert asr.effective_engine() == "sherpa"
        asr.set_engine_override("doubao")
        assert asr.effective_engine() == "doubao"
    finally:
        asr.set_engine_override(None)
    from codey import envcfg
    assert asr.effective_engine() == envcfg.select_engine()      # 复位后按配置


def test_fallback_on_high_latency(monkeypatch):
    try:
        asr.set_engine_override(None)
        monkeypatch.setattr(asr, "effective_engine", lambda: "doubao")        # 当作选了豆包
        monkeypatch.setattr(asr_doubao, "health_check", lambda *a, **k: (True, 350.0, ""))
        cc.maybe_fallback_engine()
        assert asr._engine_override == "sherpa"                  # 延时高 → 回落
    finally:
        asr.set_engine_override(None)


def test_fallback_when_unreachable(monkeypatch):
    try:
        asr.set_engine_override(None)
        monkeypatch.setattr(asr, "effective_engine", lambda: "doubao")
        monkeypatch.setattr(asr_doubao, "health_check", lambda *a, **k: (False, None, "ConnectionError: x"))
        cc.maybe_fallback_engine()
        assert asr._engine_override == "sherpa"                  # 不可达 → 回落
    finally:
        asr.set_engine_override(None)


def test_keep_doubao_when_fast(monkeypatch):
    try:
        asr.set_engine_override(None)
        monkeypatch.setattr(asr, "effective_engine", lambda: "doubao")
        monkeypatch.setattr(asr_doubao, "health_check", lambda *a, **k: (True, 45.0, ""))
        cc.maybe_fallback_engine()
        assert asr._engine_override is None                      # 快 → 不回落
    finally:
        asr.set_engine_override(None)


def test_skip_probe_when_already_local(monkeypatch):
    """本就选本地 sherpa 时不探测(health_check 不应被调用)。"""
    called = {"n": 0}
    def spy(*a, **k):
        called["n"] += 1
        return (False, None, "should-not-be-called")
    try:
        asr.set_engine_override(None)
        monkeypatch.setattr(asr, "effective_engine", lambda: "sherpa")
        monkeypatch.setattr(asr_doubao, "health_check", spy)
        cc.maybe_fallback_engine()
        assert called["n"] == 0 and asr._engine_override is None
    finally:
        asr.set_engine_override(None)
