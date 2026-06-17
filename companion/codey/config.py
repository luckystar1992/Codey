"""持久化运行期配置:分层取值(config.json > .env > 内置默认)+ 校验 + 原子写。

唯一读写入口。零依赖、线程安全(模块级 Lock)、严格不可变(深拷贝默认,整体替换,
绝不就地改 DEFAULTS)。坏输入永不抛错——丢弃非法枚举 / 强转 bool / clamp int / 深合并 display。
"""
import copy
import json
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
    except (TypeError, ValueError):
        return default


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


def get(key):
    """合并取值:config.json > env > DEFAULTS。display 深合并;refresh_ms clamp。"""
    file_cfg = _read_file()

    if key == "display":
        return _deep_merge_display(file_cfg.get("display"))

    if key == "refresh_ms":
        if "refresh_ms" in file_cfg:
            return _clamp(_coerce_int(file_cfg["refresh_ms"], DEFAULTS["refresh_ms"]),
                          _REFRESH_MIN, _REFRESH_MAX)
        return DEFAULTS["refresh_ms"]

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

    if "refresh_ms" in partial:
        out["refresh_ms"] = _clamp(
            _coerce_int(partial["refresh_ms"], DEFAULTS["refresh_ms"]),
            _REFRESH_MIN, _REFRESH_MAX,
        )

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
            else:
                merged[k] = v
        _atomic_write(merged)
        return merged
