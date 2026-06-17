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


def test_select_engine_env_tencent_precedence():
    from codey import envcfg
    # tencent 三件套齐 → 优先于 doubao
    env = {"TENCENT_APPID": "a", "TENCENT_SECRET_ID": "b", "TENCENT_SECRET_KEY": "c", "DOUBAO_API_KEY": "d"}
    assert envcfg.select_engine(env) == "tencent"
    # 只有 doubao key → doubao
    assert envcfg.select_engine({"DOUBAO_API_KEY": "d"}) == "doubao"
    # 显式 doubao 但无 doubao 配置、却有 tencent 配置 → 不用 doubao,回落到配置在的 tencent
    assert envcfg.select_engine({"CODEY_ASR_ENGINE": "doubao", "TENCENT_APPID": "a",
                                 "TENCENT_SECRET_ID": "b", "TENCENT_SECRET_KEY": "c"}) == "tencent"
    # 显式 doubao 且 doubao 配置在 → doubao(即便 tencent 也在)
    assert envcfg.select_engine({"CODEY_ASR_ENGINE": "doubao", "DOUBAO_API_KEY": "d",
                                 "TENCENT_APPID": "a", "TENCENT_SECRET_ID": "b", "TENCENT_SECRET_KEY": "c"}) == "doubao"
    # 显式 sherpa → 永远本地(即便云配置都在)
    assert envcfg.select_engine({"CODEY_ASR_ENGINE": "sherpa", "TENCENT_APPID": "a",
                                 "TENCENT_SECRET_ID": "b", "TENCENT_SECRET_KEY": "c"}) == "sherpa"
    # 啥都没有 → sherpa;tencent 三件套缺一不算
    assert envcfg.select_engine({}) == "sherpa"
    assert envcfg.select_engine({"TENCENT_APPID": "a", "TENCENT_SECRET_ID": "b"}) == "sherpa"


def test_select_engine_config_tencent(monkeypatch, tmp_path):
    from codey import config, envcfg
    monkeypatch.setattr(config, "CONFIG_PATH", str(tmp_path / "config.json"))
    config.save({"tencent_appid": "a", "tencent_secret_id": "b", "tencent_secret_key": "c"})
    assert envcfg.select_engine() == "tencent"          # 配置三件套 → tencent
    config.save({"asr_engine": "sherpa"})
    assert envcfg.select_engine() == "sherpa"           # 显式 sherpa 覆盖


def test_fallback_dispatches_to_tencent(monkeypatch):
    from codey import asr_tencent
    try:
        asr.set_engine_override(None)
        monkeypatch.setattr(asr, "effective_engine", lambda: "tencent")
        monkeypatch.setattr(asr_tencent, "health_check", lambda *a, **k: (True, 350.0, ""))
        cc.maybe_fallback_engine()
        assert asr._engine_override == "sherpa"          # 腾讯延时高 → 回落 sherpa
    finally:
        asr.set_engine_override(None)


def test_fallback_keeps_tencent_when_fast(monkeypatch):
    from codey import asr_tencent
    try:
        asr.set_engine_override(None)
        monkeypatch.setattr(asr, "effective_engine", lambda: "tencent")
        monkeypatch.setattr(asr_tencent, "health_check", lambda *a, **k: (True, 60.0, ""))
        cc.maybe_fallback_engine()
        assert asr._engine_override is None              # 快 → 不回落
    finally:
        asr.set_engine_override(None)
