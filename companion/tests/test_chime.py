from codey import chime

def test_detect_completion_executing_to_waiting():
    prev = {"claude": [{"id": "a", "status": "executing"}], "codex": []}
    cur  = {"claude": [{"id": "a", "status": "waiting"}],   "codex": []}
    st = chime.ChimeState()
    st.update(prev_cache=prev, cur_cache=cur)
    assert st.event["agent"] == "claude" and st.event["seq"] == 1

def test_no_event_when_no_transition():
    cache = {"claude": [{"id": "a", "status": "executing"}], "codex": []}
    st = chime.ChimeState()
    st.update(prev_cache=cache, cur_cache=cache)
    assert st.event is None and st.seq == 0

def test_seq_increments_per_completion():
    st = chime.ChimeState()
    st.update(prev_cache={"claude": [{"id": "a", "status": "thinking"}], "codex": []},
              cur_cache={"claude": [{"id": "a", "status": "done"}], "codex": []})
    st.update(prev_cache={"codex": [{"id": "b", "status": "executing"}], "claude": []},
              cur_cache={"codex": [{"id": "b", "status": "waiting"}], "claude": []})
    assert st.seq == 2 and st.event["agent"] == "codex"

def test_new_or_vanished_session_no_false_chime():
    st = chime.ChimeState()
    st.update(prev_cache={"claude": [], "codex": []},
              cur_cache={"claude": [{"id": "a", "status": "waiting"}], "codex": []})  # 新出现的 waiting
    assert st.event is None and st.seq == 0

def test_last_none_before_any_completion():
    st = chime.ChimeState()
    assert st.last is None

def test_last_persists_across_idle_ticks():
    # 一次完成后,后续无新完成的 tick:event 清空,last 仍保留(慢轮询设备据此读到 seq=1)
    st = chime.ChimeState()
    st.update(prev_cache={"claude": [{"id": "a", "status": "executing"}], "codex": []},
              cur_cache={"claude": [{"id": "a", "status": "waiting"}], "codex": []})
    assert st.last == {"agent": "claude", "seq": 1}
    st.update(prev_cache={"claude": [{"id": "a", "status": "waiting"}], "codex": []},
              cur_cache={"claude": [{"id": "a", "status": "waiting"}], "codex": []})
    assert st.event is None and st.last == {"agent": "claude", "seq": 1}

def test_last_tracks_latest_completion():
    st = chime.ChimeState()
    st.update(prev_cache={"claude": [{"id": "a", "status": "thinking"}], "codex": []},
              cur_cache={"claude": [{"id": "a", "status": "done"}], "codex": []})
    st.update(prev_cache={"codex": [{"id": "b", "status": "executing"}], "claude": []},
              cur_cache={"codex": [{"id": "b", "status": "waiting"}], "claude": []})
    assert st.last == {"agent": "codex", "seq": 2}
