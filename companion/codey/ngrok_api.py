# companion/codey/ngrok_api.py
"""解析 ngrok 本地 web API(http://127.0.0.1:4040/api/tunnels)拿到公网地址。
免费档 ASR 隧道地址随机,故由 companion 下发给设备(见 state.asr_url)。"""
import json
import urllib.request

NGROK_API = "http://127.0.0.1:4040/api/tunnels"


def _addr_port(cfg):
    addr = (cfg or {}).get("addr", "")
    try:
        return int(addr.rsplit(":", 1)[1])
    except (IndexError, ValueError):
        return None


def parse_tunnels(doc):
    """{local_port: public_url}(取每个本地端口最后一条隧道)。"""
    out = {}
    for t in (doc or {}).get("tunnels", []):
        p = _addr_port(t.get("config"))
        if p is not None and t.get("public_url"):
            out[p] = t["public_url"]
    return out


def public_urls(doc, state_port, asr_port):
    """状态保持 https;ASR 转成 wss(设备 WebSocketsClient.beginSSL)。"""
    m = parse_tunnels(doc)
    asr = m.get(asr_port, "")
    return {
        "state_url": m.get(state_port, ""),
        "asr_url": asr.replace("https://", "wss://").replace("http://", "ws://") if asr else "",
    }


def fetch(opener=None, timeout=2):
    """实网抓取;失败返回 {}。opener 可注入(测试/绕代理)。"""
    try:
        req = urllib.request.Request(NGROK_API)
        op = opener or urllib.request.build_opener(urllib.request.ProxyHandler({}))
        with op.open(req, timeout=timeout) as r:
            return json.loads(r.read().decode("utf-8"))
    except Exception:
        return {}
