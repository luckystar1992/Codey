# companion/codey/envcfg.py
"""零依赖 .env 加载 + 语音桥配置读取。

select_engine/paste_enabled/auto_enter 保留 env= 参数:显式传入 env 时按 env 算
(老测试/老调用不变);env 为 None 时改走 config.get(...)(config.json > .env > 默认)。
"""
import os

from . import config


def parse_env_text(text):
    out = {}
    for raw in (text or "").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("export "):
            line = line[len("export "):]
        if "=" not in line:
            continue
        k, v = line.split("=", 1)
        k = k.strip()
        v = v.strip()
        if len(v) >= 2 and v[0] == v[-1] and v[0] in "\"'":
            v = v[1:-1]
        if k:
            out[k] = v
    return out


def load_dotenv(path=None):
    """把 companion/.env(若存在)加载进 os.environ(已存在的不覆盖)。返回加载的 dict。"""
    if path is None:
        path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), ".env")
    try:
        with open(path, "r", encoding="utf-8") as f:
            data = parse_env_text(f.read())
    except OSError:
        return {}
    for k, v in data.items():
        os.environ.setdefault(k, v)
    return data


def _tencent_keys(get):
    return bool((get("tencent_appid") or "").strip()
                and (get("tencent_secret_id") or "").strip()
                and (get("tencent_secret_key") or "").strip())


def select_engine(env=None):
    # env=None:走 config(asr_engine + 各家 key);显式 env:按 env 算(兼容老调用)。
    # auto 优先级:tencent(三件套齐) > doubao(key) > sherpa(本地)。
    if env is None:
        eng = (config.get("asr_engine") or "auto").strip().lower()
        if eng in ("sherpa", "doubao", "tencent"):
            return eng
        if _tencent_keys(config.get):
            return "tencent"
        return "doubao" if (config.get("doubao_api_key") or "").strip() else "sherpa"
    eng = (env.get("CODEY_ASR_ENGINE") or "auto").strip().lower()
    if eng in ("sherpa", "doubao", "tencent"):
        return eng
    if env.get("TENCENT_APPID", "").strip() and env.get("TENCENT_SECRET_ID", "").strip() and env.get("TENCENT_SECRET_KEY", "").strip():
        return "tencent"
    return "doubao" if (env.get("DOUBAO_API_KEY") or "").strip() else "sherpa"


def paste_enabled(env=None):
    if env is None:
        return bool(config.get("paste"))
    return (env.get("CODEY_PASTE") or "1").strip() not in ("0", "false", "no")


def auto_enter(env=None):
    if env is None:
        return bool(config.get("paste_auto_enter"))
    return (env.get("CODEY_PASTE_AUTO_ENTER") or "0").strip() in ("1", "true", "yes")
