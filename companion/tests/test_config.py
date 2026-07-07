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


def test_default_columns_include_summary_branch(tmp_path, monkeypatch):
    monkeypatch.setattr(cfg, "CONFIG_PATH", str(tmp_path / "config.json"))
    cols = cfg.get("display")["columns"]
    assert cols.get("summary") is True, "summary missing from default columns"
    assert cols.get("branch") is True, "branch missing from default columns"


def test_kindle_group_defaults(tmp_path, monkeypatch):
    monkeypatch.setattr(cfg, "CONFIG_PATH", str(tmp_path / "config.json"))
    k = cfg.get("kindle")
    assert k["refresh_s"] == 30 and k["font_scale"] == 1.5 and k["line_height"] == 1.45
    assert k["font_family"] == "serif" and k["theme"] == "light" and k["bold_emphasis"] is True
    assert k["sizes"]["title"] == 21 and k["sizes"]["session2"] == 17
    assert "kindle_refresh_s" not in cfg.all()          # 顶层键已并入组


def test_kindle_group_clamps_and_coerces(tmp_path, monkeypatch):
    monkeypatch.setattr(cfg, "CONFIG_PATH", str(tmp_path / "config.json"))
    cfg.save({"kindle": {"font_scale": 9, "line_height": 0.1, "refresh_s": 1,
                         "font_family": "bogus", "theme": "dark", "bold_emphasis": "no",
                         "sizes": {"title": 999, "quota": 5}}})
    k = cfg.get("kindle")
    assert k["font_scale"] == 3.0 and k["line_height"] == 1.0    # float clamp 上/下界
    assert k["refresh_s"] == 5                                    # int clamp 下界
    assert k["font_family"] == "serif"                           # 非法枚举回默认
    assert k["theme"] == "dark"                                  # 合法枚举保留
    assert k["bold_emphasis"] is False                          # "no" -> False
    assert k["sizes"]["title"] == 48 and k["sizes"]["quota"] == 12   # size clamp
    assert k["sizes"]["provider"] == 20                          # 未改分区保留默认(深合并)


def test_kindle_group_bad_types_fall_back(tmp_path, monkeypatch):
    p = tmp_path / "config.json"
    p.write_text(json.dumps({"kindle": {"font_scale": "abc", "sizes": "nope"}}))
    monkeypatch.setattr(cfg, "CONFIG_PATH", str(p))
    k = cfg.get("kindle")
    assert k["font_scale"] == 1.5 and k["sizes"]["title"] == 21   # 坏值不抛错,回默认


def test_kindle_group_infinity_falls_back_not_raises(tmp_path, monkeypatch):
    # JSON 里的 1e999 会被 json.loads 解析成 inf;int(inf) 抛 OverflowError,不得外泄
    monkeypatch.setattr(cfg, "CONFIG_PATH", str(tmp_path / "config.json"))
    cfg.save({"kindle": {"refresh_s": 1e999, "sizes": {"title": 1e999}}})   # 不得抛 OverflowError
    k = cfg.get("kindle")
    assert k["refresh_s"] == 30 and k["sizes"]["title"] == 21     # inf 视坏值 → 回默认
    p = tmp_path / "config.json"
    p.write_text('{"kindle": {"refresh_s": 1e999}}')             # 盘上坏值读取也不抛错
    assert cfg.get("kindle")["refresh_s"] == 30
