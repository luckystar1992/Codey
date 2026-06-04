"""小工具:百分比夹取、码点截断、项目名、ISO 时间戳解析。纯函数。"""
import math
from datetime import datetime


def clamp_pct(x):
    """取整(四舍五入,半值向上,贴合 JS Math.round)并夹到 0..100。"""
    try:
        v = math.floor(float(x) + 0.5)
    except (TypeError, ValueError):
        v = 0
    return max(0, min(100, v))


def truncate(s, max_len):
    """按码点截断,超出加 '…'(总长含省略号 <= max_len)。"""
    s = str(s if s is not None else "")
    if len(s) <= max_len:
        return s
    return s[: max(0, max_len - 1)] + "…"


def project_name(cwd):
    """取 cwd 末段作项目名。"""
    parts = [p for p in str(cwd if cwd is not None else "").split("/") if p]
    return parts[-1] if parts else ""


def parse_ts_ms(ts):
    """ISO-8601 字符串 -> epoch 毫秒;失败返回 0。"""
    if not ts:
        return 0
    try:
        dt = datetime.fromisoformat(str(ts).replace("Z", "+00:00"))
        return int(dt.timestamp() * 1000)
    except (ValueError, TypeError):
        return 0
