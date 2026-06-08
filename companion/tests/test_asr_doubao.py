import gzip, json
from codey import asr_doubao as d

def test_build_frame_roundtrip_header_and_gzip():
    frame = d.build_frame(0x1, b'{"a":1}', last=False)
    assert frame[0] == (0x1 << 4) | 0x1                  # version|header_size
    assert frame[1] >> 4 == 0x1 and frame[1] & 0x0F == 0  # msg_type=1, flags=0
    body_len = int.from_bytes(frame[4:8], "big")
    body = frame[8:8 + body_len]
    assert gzip.decompress(body) == b'{"a":1}'

def test_last_audio_frame_sets_flag():
    frame = d.build_frame(0x2, b"", last=True)
    assert frame[1] & 0x0F == 0x2                          # last flag

def test_parse_normal_response_json():
    payload = json.dumps({"result": {"utterances": [{"text": "你好", "definite": True,
              "start_time": 0, "end_time": 10}]}}).encode()
    seq = (0).to_bytes(4, "big"); ln = len(payload).to_bytes(4, "big")
    data = bytes([0x10, 0x10, 0x11, 0x00]) + seq + ln + payload
    out = d.parse_response(data)
    assert "payload" in out and out["payload"]["result"]["utterances"][0]["text"] == "你好"

def test_parse_error_response():
    msg = json.dumps({"code": 1013}).encode()
    data = bytes([0x10, 0xF0, 0x11, 0x00]) + (1013).to_bytes(4, "big") + len(msg).to_bytes(4, "big") + msg
    out = d.parse_response(data)
    assert "error" in out and out["payload"]["code"] == 1013

def test_merge_utterances_dedups_finals():
    st = d.UtteranceMerger()
    st.feed([{"text": "你好", "definite": True, "start_time": 0, "end_time": 1}])
    st.feed([{"text": "你好", "definite": True, "start_time": 0, "end_time": 1}])  # dup
    st.feed([{"text": "世界", "definite": True, "start_time": 1, "end_time": 2}])
    st.feed([{"text": "ing", "definite": False}])
    assert st.final_text == "你好世界"
    assert st.text == "你好世界ing"


def test_cfg_reads_from_config_then_env(tmp_path, monkeypatch):
    # 配置台(config.json)优先;无 config.json 时回退 .env(os.environ)
    from codey import config as cfg
    monkeypatch.setattr(cfg, "CONFIG_PATH", str(tmp_path / "config.json"))
    monkeypatch.setenv("DOUBAO_API_KEY", "env-key")
    monkeypatch.setenv("DOUBAO_APP_ID", "env-app")
    assert d._cfg() == {"app_id": "env-app", "api_key": "env-key"}     # 回退 env
    cfg.save({"doubao_api_key": "cfg-key", "doubao_app_id": "cfg-app"})
    assert d._cfg() == {"app_id": "cfg-app", "api_key": "cfg-key"}     # 配置台覆盖 env
