from codey import envcfg
from codey import config as cfg

def test_parse_env_text_ignores_comments_and_quotes():
    text = "\n".join([
        "# comment", "", "DOUBAO_API_KEY = abc123  ",
        'DOUBAO_APP_ID="42"', "EMPTY=", "export CODEY_ASR_ENGINE=doubao",
    ])
    d = envcfg.parse_env_text(text)
    assert d["DOUBAO_API_KEY"] == "abc123"
    assert d["DOUBAO_APP_ID"] == "42"
    assert d["EMPTY"] == ""
    assert d["CODEY_ASR_ENGINE"] == "doubao"

def test_select_engine():
    assert envcfg.select_engine({"CODEY_ASR_ENGINE": "sherpa", "DOUBAO_API_KEY": "x"}) == "sherpa"
    # 显式 doubao 但无 doubao 配置 → 不用 doubao,回落本地(只用配置存在的服务)
    assert envcfg.select_engine({"CODEY_ASR_ENGINE": "doubao"}) == "sherpa"
    # 显式 doubao 且配置在 → doubao
    assert envcfg.select_engine({"CODEY_ASR_ENGINE": "doubao", "DOUBAO_API_KEY": "x"}) == "doubao"
    assert envcfg.select_engine({"CODEY_ASR_ENGINE": "auto", "DOUBAO_API_KEY": "x"}) == "doubao"
    assert envcfg.select_engine({"CODEY_ASR_ENGINE": "auto"}) == "sherpa"
    assert envcfg.select_engine({}) == "sherpa"

def test_paste_flags():
    assert envcfg.paste_enabled({"CODEY_PASTE": "0"}) is False
    assert envcfg.paste_enabled({}) is True
    assert envcfg.auto_enter({"CODEY_PASTE_AUTO_ENTER": "1"}) is True
    assert envcfg.auto_enter({}) is False


def test_engine_from_config_when_env_none(tmp_path, monkeypatch):
    # config 的 asr_engine 应压过 env;doubao 须配置在才生效,故同时给 key
    p = tmp_path / "config.json"; p.write_text('{"asr_engine": "doubao", "doubao_api_key": "x"}')
    monkeypatch.setattr(cfg, "CONFIG_PATH", str(p))
    monkeypatch.setenv("CODEY_ASR_ENGINE", "sherpa")
    assert envcfg.select_engine() == "doubao"


def test_paste_from_config_when_env_none(tmp_path, monkeypatch):
    p = tmp_path / "config.json"; p.write_text('{"paste": false, "paste_auto_enter": true}')
    monkeypatch.setattr(cfg, "CONFIG_PATH", str(p))
    monkeypatch.delenv("CODEY_PASTE", raising=False)
    monkeypatch.delenv("CODEY_PASTE_AUTO_ENTER", raising=False)
    assert envcfg.paste_enabled() is False
    assert envcfg.auto_enter() is True
