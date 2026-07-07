"""持久化运行期配置:分层取值(config.json > .env > 内置默认)+ 校验 + 原子写。

唯一读写入口。零依赖、线程安全(模块级 Lock)、严格不可变(深拷贝默认,整体替换,
绝不就地改 DEFAULTS)。坏输入永不抛错——丢弃非法枚举 / 强转 bool / clamp int / 深合并 display。
"""
import copy
import json
import math
import os
import threading

# 默认 companion/data/config.json(data/ 已 gitignore)。模块级,便于测试 monkeypatch。
CONFIG_PATH = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "data", "config.json"
)

DEFAULTS = {
    "asr_engine": "auto",            # auto | sherpa | doubao | tencent
    "paste": True,
    "paste_auto_enter": False,
    "doubao_api_key": "",
    "doubao_app_id": "",
    "tencent_appid": "",             # 腾讯云实时 ASR:AppId/SecretId/SecretKey + 引擎(默认 16k_zh)
    "tencent_secret_id": "",
    "tencent_secret_key": "",
    "tencent_engine": "16k_zh",
    "refresh_ms": 2000,              # usage 后台刷新间隔(ms),clamp [500, 60000]
    "display": {
        "columns": {"status": True, "model": True, "ctx": True,
                    "tokens": True, "memory": True, "turn": True,
                    "summary": True, "branch": True},
        "providers": {"claude": True, "codex": True},
    },
    "kindle": {
        "refresh_s": 30,             # int, clamp [5, 3600]
        "font_scale": 1.5,           # float, clamp [1.0, 3.0]
        "line_height": 1.45,         # float, clamp [1.0, 2.2]
        "font_family": "serif",      # serif | sans | mono
        "theme": "light",            # light(黑字白底) | dark(白字黑底)
        "bold_emphasis": True,
        "sizes": {"title": 21, "provider": 20, "quota": 19,
                  "session1": 19, "session2": 17},   # 各区块基准 px,渲染再 × font_scale
    },
}

SECRET_KEYS = ("doubao_api_key", "tencent_secret_id", "tencent_secret_key")

# key -> 对应 env 变量名(无映射的 refresh_ms / display 直接 config-or-default)
_ENV_MAP = {
    "asr_engine": "CODEY_ASR_ENGINE",
    "paste": "CODEY_PASTE",
    "paste_auto_enter": "CODEY_PASTE_AUTO_ENTER",
    "doubao_api_key": "DOUBAO_API_KEY",
    "doubao_app_id": "DOUBAO_APP_ID",
    "tencent_appid": "TENCENT_APPID",
    "tencent_secret_id": "TENCENT_SECRET_ID",
    "tencent_secret_key": "TENCENT_SECRET_KEY",
    "tencent_engine": "TENCENT_ENGINE",
}

_ENGINES = ("auto", "sherpa", "doubao", "tencent")
_REFRESH_MIN, _REFRESH_MAX = 500, 60000
_KINDLE_MIN, _KINDLE_MAX = 5, 3600

# int 型配置键 -> clamp 区间(get/_validate 共用;新增 int 键只需在此登记)。
# 注意:每个键必须同时在 DEFAULTS 登记,否则 get/_validate 索引 DEFAULTS[key] 会 KeyError。
_INT_CLAMPS = {
    "refresh_ms": (_REFRESH_MIN, _REFRESH_MAX),
}

_FONT_SCALE_MIN, _FONT_SCALE_MAX = 1.0, 3.0
_LINE_HEIGHT_MIN, _LINE_HEIGHT_MAX = 1.0, 2.2
_KSIZE_MIN, _KSIZE_MAX = 12, 48
_KINDLE_FONTS = ("serif", "sans", "mono")
_KINDLE_THEMES = ("light", "dark")
_TRUTHY = ("1", "true", "yes")

_LOCK = threading.Lock()


def _coerce_bool(v):
    """字符串/任意值 -> bool。'yes'/'1'/'true'(大小写不敏感)或真 True -> True,否则 False。"""
    if isinstance(v, bool):
        return v
    if isinstance(v, str):
        return v.strip().lower() in _TRUTHY
    return bool(v)


def _coerce_int(v, default):
    try:
        return int(v)
    except (TypeError, ValueError, OverflowError):   # inf(如 JSON 1e999)-> OverflowError
        return default


def _coerce_float(v, default):
    try:
        f = float(v)
    except (TypeError, ValueError):
        return default
    return f if math.isfinite(f) else default


def _clamp(n, lo, hi):
    return max(lo, min(hi, n))


def _env_bool(name, fallback):
    """env 真值:paste 走 'not in {0,false,no}',其余走 truthy。"""
    raw = os.environ.get(name)
    if raw is None:
        return fallback
    s = raw.strip().lower()
    if name == "CODEY_PASTE":
        return s not in ("0", "false", "no")
    return s in _TRUTHY


def _read_file():
    """读 CONFIG_PATH 的 JSON dict;不存在/坏文件 -> {}。永不抛错。"""
    try:
        with open(CONFIG_PATH, "r", encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, ValueError):
        return {}
    return data if isinstance(data, dict) else {}


def _deep_merge_display(file_display):
    """以 DEFAULTS['display'] 深拷贝为底,叠加 file 中已知的 columns/providers 布尔键。"""
    merged = copy.deepcopy(DEFAULTS["display"])
    if not isinstance(file_display, dict):
        return merged
    for group in ("columns", "providers"):
        sub = file_display.get(group)
        if not isinstance(sub, dict):
            continue
        for k in merged[group]:                      # 仅保留已知键
            if k in sub:
                merged[group][k] = _coerce_bool(sub[k])
    return merged


def _clean_kindle(kp):
    """从 partial kindle dict 提取合法字段成干净 dict(数值 clamp / 枚举校验 / bool 强转 /
    sizes 逐键 clamp)。坏值/非法枚举丢弃。get 与 _validate 共用。"""
    out = {}
    if not isinstance(kp, dict):
        return out
    d = DEFAULTS["kindle"]
    if "refresh_s" in kp:
        out["refresh_s"] = _clamp(_coerce_int(kp["refresh_s"], d["refresh_s"]), _KINDLE_MIN, _KINDLE_MAX)
    if "font_scale" in kp:
        out["font_scale"] = _clamp(_coerce_float(kp["font_scale"], d["font_scale"]), _FONT_SCALE_MIN, _FONT_SCALE_MAX)
    if "line_height" in kp:
        out["line_height"] = _clamp(_coerce_float(kp["line_height"], d["line_height"]), _LINE_HEIGHT_MIN, _LINE_HEIGHT_MAX)
    if "font_family" in kp:
        s = str(kp["font_family"]).strip().lower()
        if s in _KINDLE_FONTS:
            out["font_family"] = s
    if "theme" in kp:
        s = str(kp["theme"]).strip().lower()
        if s in _KINDLE_THEMES:
            out["theme"] = s
    if "bold_emphasis" in kp:
        out["bold_emphasis"] = _coerce_bool(kp["bold_emphasis"])
    if isinstance(kp.get("sizes"), dict):
        szout = {}
        for key in d["sizes"]:
            if key in kp["sizes"]:
                szout[key] = _clamp(_coerce_int(kp["sizes"][key], d["sizes"][key]), _KSIZE_MIN, _KSIZE_MAX)
        if szout:
            out["sizes"] = szout
    return out


def _merge_kindle(file_kindle):
    """以 DEFAULTS['kindle'] 深拷贝为底,叠加 file 中合法字段(sizes 深合并保留其余分区)。"""
    merged = copy.deepcopy(DEFAULTS["kindle"])
    for k, v in _clean_kindle(file_kindle).items():
        if k == "sizes":
            merged["sizes"].update(v)
        else:
            merged[k] = v
    return merged


def get(key):
    """合并取值:config.json > env > DEFAULTS。display / kindle 深合并;refresh_ms clamp。"""
    file_cfg = _read_file()

    if key == "kindle":
        return _merge_kindle(file_cfg.get("kindle"))

    if key == "display":
        return _deep_merge_display(file_cfg.get("display"))

    if key in _INT_CLAMPS:
        lo, hi = _INT_CLAMPS[key]
        if key in file_cfg:
            return _clamp(_coerce_int(file_cfg[key], DEFAULTS[key]), lo, hi)
        return DEFAULTS[key]

    if key not in DEFAULTS:
        return None

    if key in file_cfg:
        val = file_cfg[key]
        if key in ("paste", "paste_auto_enter"):
            return _coerce_bool(val)
        if key == "asr_engine":
            s = str(val).strip().lower()
            return s if s in _ENGINES else DEFAULTS["asr_engine"]
        return val

    # env 回退
    env_name = _ENV_MAP.get(key)
    if env_name:
        if key in ("paste", "paste_auto_enter"):
            return _env_bool(env_name, DEFAULTS[key])
        if key == "asr_engine":
            raw = os.environ.get(env_name)
            if raw is not None:
                s = raw.strip().lower()
                return s if s in _ENGINES else DEFAULTS["asr_engine"]
        else:
            raw = os.environ.get(env_name)
            if raw is not None:
                return raw

    return copy.deepcopy(DEFAULTS[key])


def all():
    """全量合并 dict(未遮罩;调用方负责遮罩密钥)。"""
    return {k: get(k) for k in DEFAULTS}


def _validate(partial):
    """从 partial 提取合法字段成一个干净 dict;非法值丢弃/强转,永不抛错。"""
    out = {}
    if not isinstance(partial, dict):
        return out

    if "asr_engine" in partial:
        s = str(partial["asr_engine"]).strip().lower()
        if s in _ENGINES:
            out["asr_engine"] = s                    # 非法枚举:丢弃(回默认)

    for k in ("paste", "paste_auto_enter"):
        if k in partial:
            out[k] = _coerce_bool(partial[k])

    for k in ("doubao_api_key", "doubao_app_id",
              "tencent_appid", "tencent_secret_id", "tencent_secret_key", "tencent_engine"):
        if k in partial:
            out[k] = str(partial[k])

    for k, (lo, hi) in _INT_CLAMPS.items():
        if k in partial:
            out[k] = _clamp(_coerce_int(partial[k], DEFAULTS[k]), lo, hi)

    if "display" in partial and isinstance(partial["display"], dict):
        disp = {}
        for group in ("columns", "providers"):
            sub = partial["display"].get(group)
            if isinstance(sub, dict):
                known = DEFAULTS["display"][group]
                cleaned = {k: _coerce_bool(sub[k]) for k in known if k in sub}
                if cleaned:
                    disp[group] = cleaned
        if disp:
            out["display"] = disp

    if "kindle" in partial:
        kclean = _clean_kindle(partial["kindle"])
        if kclean:
            out["kindle"] = kclean

    return out


def _atomic_write(data):
    """tmp 文件 + os.replace 原子落盘。"""
    os.makedirs(os.path.dirname(CONFIG_PATH), exist_ok=True)
    tmp = CONFIG_PATH + ".tmp"
    try:
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(data, f, ensure_ascii=False, indent=2)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, CONFIG_PATH)
    except OSError:
        try:
            os.remove(tmp)
        except OSError:
            pass
        raise


def save(partial):
    """校验 partial 后深合并进盘上 config,原子写。返回合并后的盘上 dict。"""
    clean = _validate(partial)
    with _LOCK:
        current = _read_file()
        merged = copy.deepcopy(current) if isinstance(current, dict) else {}
        for k, v in clean.items():
            if k == "display":
                base = merged.get("display")
                base = copy.deepcopy(base) if isinstance(base, dict) else {}
                for group, sub in v.items():
                    grp = base.get(group)
                    grp = copy.deepcopy(grp) if isinstance(grp, dict) else {}
                    grp.update(sub)
                    base[group] = grp
                merged["display"] = base
            elif k == "kindle":
                base = merged.get("kindle")
                base = copy.deepcopy(base) if isinstance(base, dict) else {}
                for kk, vv in v.items():
                    if kk == "sizes":
                        sz = base.get("sizes")
                        sz = copy.deepcopy(sz) if isinstance(sz, dict) else {}
                        sz.update(vv)
                        base["sizes"] = sz
                    else:
                        base[kk] = vv
                merged["kindle"] = base
            else:
                merged[k] = v
        _atomic_write(merged)
        return merged
