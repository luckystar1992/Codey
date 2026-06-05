# companion/codey/chime.py
"""任务完成检测:会话 status 由活跃(executing/thinking)跃迁到 idle(waiting/done)记一次完成。"""

_ACTIVE = {"executing", "thinking"}
_IDLE = {"waiting", "done"}


def _by_id(sessions):
    return {s.get("id"): s.get("status") for s in (sessions or []) if s.get("id")}


def detect(prev_cache, cur_cache):
    """返回本 tick 完成的 agent 列表(按 claude/codex 顺序)。"""
    done = []
    for agent in ("claude", "codex"):
        prev = _by_id((prev_cache or {}).get(agent))
        cur = _by_id((cur_cache or {}).get(agent))
        for sid, status in cur.items():
            if sid in prev and prev[sid] in _ACTIVE and status in _IDLE:
                done.append(agent)
                break
    return done


class ChimeState:
    def __init__(self):
        self.seq = 0
        self.event = None        # {"agent":..,"seq":..} or None(本 tick 无新完成)

    def update(self, prev_cache, cur_cache):
        agents = detect(prev_cache, cur_cache)
        self.event = None
        for agent in agents:
            self.seq += 1
            self.event = {"agent": agent, "seq": self.seq}     # 多个同 tick 取最后一个;seq 已各自 +1
        return self.event
