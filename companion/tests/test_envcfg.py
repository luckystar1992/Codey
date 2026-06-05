from codey import envcfg

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
    assert envcfg.select_engine({"CODEY_ASR_ENGINE": "doubao"}) == "doubao"
    assert envcfg.select_engine({"CODEY_ASR_ENGINE": "auto", "DOUBAO_API_KEY": "x"}) == "doubao"
    assert envcfg.select_engine({"CODEY_ASR_ENGINE": "auto"}) == "sherpa"
    assert envcfg.select_engine({}) == "sherpa"

def test_paste_flags():
    assert envcfg.paste_enabled({"CODEY_PASTE": "0"}) is False
    assert envcfg.paste_enabled({}) is True
    assert envcfg.auto_enter({"CODEY_PASTE_AUTO_ENTER": "1"}) is True
    assert envcfg.auto_enter({}) is False
