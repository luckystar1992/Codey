"""上下文窗口判定:模型名含 [1m] 或观测 token > 200k -> 1M,否则 200k。"""


def context_window_for(model, max_observed_tokens):
    m = str(model if model is not None else "").lower()
    try:
        mx = float(max_observed_tokens)
    except (TypeError, ValueError):
        mx = 0.0
    if "[1m]" in m or mx > 200000:
        return 1000000
    return 200000
