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


def _has_tencent(get):
    return bool((get("tencent_appid") or "").strip()
                and (get("tencent_secret_id") or "").strip()
                and (get("tencent_secret_key") or "").strip())


def _has_doubao(get):
    return bool((get("doubao_api_key") or "").strip())


def _resolve_engine(explicit, has_tencent, has_doubao):
    """只用「配置存在」的引擎。显式 sherpa 永远本地;显式 tencent/doubao 须其配置在才生效,
    否则与 auto 一样按存在情况优先 tencent > doubao > sherpa(本地)。"""
    if explicit == "sherpa":
        return "sherpa"
    if explicit == "tencent" and has_tencent:
        return "tencent"
    if explicit == "doubao" and has_doubao:
        return "doubao"
    if has_tencent:
        return "tencent"
    if has_doubao:
        return "doubao"
    return "sherpa"


# 显式 env dict 路径:config 键 → env 变量名
_ENGINE_ENV = {
    "tencent_appid": "TENCENT_APPID", "tencent_secret_id": "TENCENT_SECRET_ID",
    "tencent_secret_key": "TENCENT_SECRET_KEY", "doubao_api_key": "DOUBAO_API_KEY",
}


def select_engine(env=None):
    """选生效 ASR 引擎:只用配置齐全的云服务,优先 tencent > doubao,均无则本地 sherpa。
    显式 CODEY_ASR_ENGINE/asr_engine 仅当该服务配置存在时才生效(否则按存在情况自动选)。
    env=None 走 config(config.json > env > 默认);显式 env dict 兼容老调用。"""
    if env is None:
        get = config.get
        explicit = (config.get("asr_engine") or "auto").strip().lower()
    else:
        def get(k):
            return env.get(_ENGINE_ENV.get(k, k), "")
        explicit = (env.get("CODEY_ASR_ENGINE") or "auto").strip().lower()
    return _resolve_engine(explicit, _has_tencent(get), _has_doubao(get))


def paste_enabled(env=None):
    if env is None:
        return bool(config.get("paste"))
    return (env.get("CODEY_PASTE") or "1").strip() not in ("0", "false", "no")


def auto_enter(env=None):
    if env is None:
        return bool(config.get("paste_auto_enter"))
    return (env.get("CODEY_PASTE_AUTO_ENTER") or "0").strip() in ("1", "true", "yes")
