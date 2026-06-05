from codey import asr_punct

def test_punctuator_noop_when_no_model(monkeypatch, tmp_path):
    monkeypatch.setattr(asr_punct, "MODELS_DIR", str(tmp_path))   # 空目录
    p = asr_punct.Punctuator()
    assert p.available is False
    assert p.add("你好世界这是一句话") == "你好世界这是一句话"   # 原样

def test_punctuator_uses_engine_when_present(monkeypatch, tmp_path):
    monkeypatch.setattr(asr_punct, "MODELS_DIR", str(tmp_path))
    fake = lambda text: text + "。"
    p = asr_punct.Punctuator(engine=type("E", (), {"add_punctuation": staticmethod(fake)})())
    assert p.available is True
    assert p.add("你好") == "你好。"
