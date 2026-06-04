"""会话状态判定。优先级:done > executing > thinking > waiting。纯函数。"""


def derive_status(has_active_descendant=False, pending_tool=False,
                  model_generating=False, done=False):
    if done:
        return "done"
    if has_active_descendant or pending_tool:
        return "executing"
    if model_generating:
        return "thinking"
    return "waiting"
