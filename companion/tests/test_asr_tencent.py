"""腾讯 ASR 后端:listener 文本累积 + health_check(不触网、不需 SDK 真连)。"""
from codey import asr_tencent as t


def test_listener_partial_then_final_accumulates():
    lis = t._TencentListener()
    lis.on_recognition_result_change({"result": {"voice_text_str": "你好"}})
    assert lis.text == "你好"
    lis.on_recognition_result_change({"result": {"voice_text_str": "你好世界"}})   # 当前句 partial 增长
    assert lis.text == "你好世界"
    lis.on_sentence_end({"result": {"voice_text_str": "你好世界。"}})              # 句末并入 committed
    assert lis.committed == "你好世界。" and lis.text == "你好世界。"


def test_listener_multi_sentence_concatenates():
    lis = t._TencentListener()
    lis.on_sentence_end({"result": {"voice_text_str": "第一句。"}})
    lis.on_recognition_result_change({"result": {"voice_text_str": "第二"}})
    assert lis.text == "第一句。第二"                                            # committed + 当前句 partial
    lis.on_sentence_end({"result": {"voice_text_str": "第二句。"}})
    assert lis.text == "第一句。第二句。"


def test_listener_on_fail_records_error():
    lis = t._TencentListener()
    lis.on_fail({"message": "auth failed"})
    assert lis.error == "auth failed"


def test_listener_missing_result_is_empty():
    lis = t._TencentListener()
    lis.on_recognition_result_change({})        # 无 result 字段 → 不崩,空串
    assert lis.text == ""


def test_health_check_unreachable(monkeypatch):
    import socket
    def boom(*a, **k):
        raise OSError("nope")
    monkeypatch.setattr(socket, "create_connection", boom)
    ok, lat, why = t.health_check(timeout=0.01, attempts=1)
    assert ok is False and lat is None and "nope" in why


def test_health_check_reachable_returns_latency(monkeypatch):
    import socket
    class _Conn:
        def __enter__(self): return self
        def __exit__(self, *a): return False
    monkeypatch.setattr(socket, "create_connection", lambda *a, **k: _Conn())
    ok, lat, why = t.health_check(timeout=1.0, attempts=2)
    assert ok is True and lat is not None and lat >= 0 and why == ""
