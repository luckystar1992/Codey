import json, os
from codey import config as cfg


def test_defaults_when_no_file_no_env(tmp_path, monkeypatch):
    monkeypatch.setattr(cfg, "CONFIG_PATH", str(tmp_path / "config.json"))
    for k in ("CODEY_ASR_ENGINE", "CODEY_PASTE", "DOUBAO_API_KEY"):
        monkeypatch.delenv(k, raising=False)
    assert cfg.get("asr_engine") == "auto"
    assert cfg.get("paste") is True
    assert cfg.get("refresh_ms") == 2000


def test_env_fallback(tmp_path, monkeypatch):
    monkeypatch.setattr(cfg, "CONFIG_PATH", str(tmp_path / "config.json"))
    monkeypatch.setenv("CODEY_ASR_ENGINE", "sherpa")
    monkeypatch.setenv("CODEY_PASTE", "0")
    assert cfg.get("asr_engine") == "sherpa"
    assert cfg.get("paste") is False


def test_file_overrides_env(tmp_path, monkeypatch):
    p = tmp_path / "config.json"; p.write_text(json.dumps({"asr_engine": "doubao"}))
    monkeypatch.setattr(cfg, "CONFIG_PATH", str(p))
    monkeypatch.setenv("CODEY_ASR_ENGINE", "sherpa")
    assert cfg.get("asr_engine") == "doubao"


def test_save_validates_and_roundtrips(tmp_path, monkeypatch):
    p = tmp_path / "config.json"; monkeypatch.setattr(cfg, "CONFIG_PATH", str(p))
    cfg.save({"asr_engine": "doubao", "refresh_ms": 99999, "paste": "yes",
              "display": {"columns": {"memory": False}}})
    assert cfg.get("asr_engine") == "doubao"
    assert cfg.get("refresh_ms") == 60000          # clamp 上界
    assert cfg.get("paste") is True                # "yes" -> True
    d = cfg.get("display")
    assert d["columns"]["memory"] is False and d["columns"]["status"] is True   # 深合并保留其余列


def test_save_rejects_bad_engine(tmp_path, monkeypatch):
    monkeypatch.setattr(cfg, "CONFIG_PATH", str(tmp_path / "config.json"))
    cfg.save({"asr_engine": "nope"})
    assert cfg.get("asr_engine") == "auto"          # 非法枚举被丢弃/回默认
