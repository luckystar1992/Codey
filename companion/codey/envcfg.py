# companion/codey/envcfg.py
"""零依赖 .env 加载 + 语音桥配置读取。"""
import os


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


def select_engine(env=None):
    env = os.environ if env is None else env
    eng = (env.get("CODEY_ASR_ENGINE") or "auto").strip().lower()
    if eng in ("sherpa", "doubao"):
        return eng
    return "doubao" if (env.get("DOUBAO_API_KEY") or "").strip() else "sherpa"


def paste_enabled(env=None):
    env = os.environ if env is None else env
    return (env.get("CODEY_PASTE") or "1").strip() not in ("0", "false", "no")


def auto_enter(env=None):
    env = os.environ if env is None else env
    return (env.get("CODEY_PASTE_AUTO_ENTER") or "0").strip() in ("1", "true", "yes")
