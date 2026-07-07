"""Kindle 监视页:把 /codey/state 渲染成零 JS 纯 HTML。纯函数。

面向早期 Kindle 实验性浏览器(老 WebKit):零 JS、<meta refresh> 整页刷新、
白底黑字高对比(e-ink)、单列大字号。只读 state,缺键容错,永不抛错。
"""
import html
import math
import time

from .util import clamp_pct

# 与 config._KINDLE_MIN/MAX 及 DEFAULTS["kindle_refresh_s"] 一致;此处独立再夹一次,
# 保证本纯函数即便被传入未经 config 夹取的值也永不越界(有意的双重 clamp)。
_KINDLE_MIN, _KINDLE_MAX, _KINDLE_DEFAULT = 5, 3600, 30
_STATUS_ICON = {"executing": "●", "thinking": "◐", "waiting": "○", "done": "✓"}

_CSS = (
    "body{background:#fff;color:#000;margin:0;padding:10px 14px;"
    "font-family:Georgia,serif;font-size:19px;line-height:1.45}"
    ".hdr{border-bottom:3px solid #000;padding-bottom:6px}"
    ".hdr b{font-size:21px;letter-spacing:1px}"
    ".hdr .t{float:right;font-size:17px}"
    ".warn{border:2px solid #000;padding:4px 8px;margin:8px 0;font-weight:bold}"
    ".prov{border-bottom:3px solid #000;padding:10px 0}"
    ".prov h2{font-size:20px;margin:0 0 6px}"
    ".qrow{margin:4px 0}.qrow .lbl{display:inline-block;width:2.2em}"
    ".bar{display:inline-block;width:52%;height:13px;border:2px solid #000;"
    "vertical-align:middle}.bar i{display:block;height:100%;background:#000}"
    ".pct{font-weight:bold}"
    ".agg{font-size:17px;margin:4px 0 8px}"
    ".sess{border-top:1px solid #000;padding:7px 0}"
    ".sess .l1{font-weight:bold}"
    ".sess .l2,.sess .l3{font-size:17px;margin-left:1.4em}"
    ".none{font-size:17px;padding:6px 0}"
)


def _esc(v):
    return html.escape(str(v if v is not None else ""), quote=True)


def _fmt_kilo(n):
    """1234 -> '1.2k';999 -> '999';坏值/非有限值(nan/inf) -> '0'。"""
    try:
        f = float(n)
        if not math.isfinite(f):        # nan/inf 不可 int(),按坏值处理
            return "0"
        if f >= 1000:
            return "{:.1f}k".format(f / 1000.0)
        return str(int(f))
    except (TypeError, ValueError):
        return "0"


def _bar_html(pct):
    p = clamp_pct(pct)
    return ('<span class="bar"><i style="width:{p}%"></i></span> '
            '<span class="pct">{p}%</span>'.format(p=p))


def _quota_html(label, quota):
    quota = quota if isinstance(quota, dict) else {}
    return ('<div class="qrow"><span class="lbl">{}</span>{}</div>'
            .format(_esc(label), _bar_html(quota.get("used_pct", 0))))


def _session_html(s):
    s = s if isinstance(s, dict) else {}
    icon = _STATUS_ICON.get(s.get("status"), "○")
    git = s.get("git") if isinstance(s.get("git"), dict) else {}
    l2 = " · ".join(x for x in (
        _esc(s.get("model")),
        "ctx {}%".format(clamp_pct(s.get("context_pct"))),
        _esc(git.get("branch")),
    ) if x)
    parts = ['<div class="sess"><div class="l1">{} {}  {}</div>'.format(
        icon, _esc(s.get("status") or "-"), _esc(s.get("name") or "-"))]
    parts.append('<div class="l2">{}</div>'.format(l2 or "-"))
    summary = _esc(s.get("summary"))
    if summary:
        parts.append('<div class="l3">{}</div>'.format(summary))
    parts.append("</div>")
    return "".join(parts)


def _provider_html(p):
    p = p if isinstance(p, dict) else {}
    agg = p.get("agg") if isinstance(p.get("agg"), dict) else {}
    parts = ['<div class="prov"><h2>{}{}</h2>'.format(
        _esc(p.get("name") or p.get("id") or "?"),
        " —— 已限流" if p.get("limited") else "")]
    parts.append(_quota_html("5h", p.get("session")))
    parts.append(_quota_html("周", p.get("weekly")))
    try:
        active = int(p.get("active_count") or 0)
    except (TypeError, ValueError):
        active = 0
    parts.append('<div class="agg">{} active · {} tok/min</div>'.format(
        active, _fmt_kilo(agg.get("tokens_per_min", 0))))
    sessions = p.get("sessions") if isinstance(p.get("sessions"), list) else []
    if sessions:
        parts.extend(_session_html(s) for s in sessions)
    else:
        parts.append('<div class="none">(无活跃会话)</div>')
    parts.append("</div>")
    return "".join(parts)


def render(state, refresh_s, now=None):
    """state + 刷新秒数 -> 完整 HTML 文档字符串。缺键/坏值容错,永不抛错。

    now:epoch 秒,None 取当前时间(测试传定值保证确定性)。
    """
    state = state if isinstance(state, dict) else {}
    try:
        r = int(refresh_s)
    except (TypeError, ValueError, OverflowError):   # inf -> OverflowError
        r = _KINDLE_DEFAULT
    r = max(_KINDLE_MIN, min(_KINDLE_MAX, r))
    hhmm = time.strftime("%H:%M", time.localtime(now if now is not None else time.time()))
    providers = state.get("providers") if isinstance(state.get("providers"), list) else []
    body = ['<div class="hdr"><span class="t">{} ({}s)</span><b>CODEY MONITOR</b></div>'
            .format(hhmm, r)]
    if state.get("stale"):
        body.append('<div class="warn">⚠ 数据可能过期</div>')
    body.extend(_provider_html(p) for p in providers)
    if not providers:
        body.append('<div class="none">(无数据)</div>')
    return (
        "<!DOCTYPE html>\n"
        '<html lang="zh"><head><meta charset="utf-8">\n'
        '<meta http-equiv="refresh" content="{r}">\n'
        '<meta name="viewport" content="width=device-width,initial-scale=1">\n'
        "<title>Codey Monitor</title>\n"
        "<style>{css}</style></head>\n"
        "<body>{body}</body></html>"
    ).format(r=r, css=_CSS, body="".join(body))
