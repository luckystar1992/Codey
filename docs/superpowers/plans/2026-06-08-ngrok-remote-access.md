# Codey ngrok 远程访问 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让部署在公司局域网的 Codey companion 通过 ngrok 把「状态 API + 语音 ASR」安全暴露到公网,手表只要联网(不限同一 WiFi)即可访问。

**Architecture:**
- ngrok 用**免费静态域名**把 `:8787`(状态 + Web 管理台)暴露为稳定的 `https://<domain>.ngrok-free.app`;第二条 HTTP 隧道暴露 `:8788`(ASR WS,WS 走 HTTP 隧道)。
- 免费档只送 1 个静态域名,故 ASR 隧道地址可能随机:companion 查 ngrok 本地 API(`http://127.0.0.1:4040/api/tunnels`)拿到 ASR 公网 URL,塞进 `/codey/state` 的新字段 `asr_url` 下发。**设备只认一个稳定状态域名**,ASR 地址从 state 读 —— 免费档也稳。
- 鉴权:两条隧道都开 ngrok basic-auth;设备 HTTP/WS 请求带 `Authorization: Basic ...` + `ngrok-skip-browser-warning`。
- 设备走 HTTPS/WSS(TLS):`HTTPClient` over `WiFiClientSecure`(`setInsecure()`)+ `WebSocketsClient::beginSSL`。远程主机/鉴权从配网门户输入并持久化。

**Tech Stack:** ngrok(免费档 + 静态域名)、Python 标准库(companion,`urllib` 查 ngrok API)、Arduino/ESP32-S3(`WiFiClientSecure` / `HTTPClient` / vendored `WebSocketsClient`)。

---

## File Structure

| File | 责任 |
|---|---|
| `companion/ngrok.yml.example` | ngrok 双隧道配置模板(静态域名 + basic-auth);复制为 `ngrok.yml` 填值 |
| `companion/codey/ngrok_api.py` | **新增** 纯函数:解析 ngrok `:4040/api/tunnels` JSON → `{state_url, asr_url}`;注入式 fetcher 便于测试 |
| `companion/codey/state.py` | 在 state dict 注入 `asr_url`(来自 ngrok_api,失败则 `""`) |
| `companion/codey/server.py` | 启动时缓存 ngrok 公网地址(后台刷新),`/codey/state` 带上 `asr_url` |
| `companion/.env.example` | 增 `NGROK_AUTHTOKEN` / `NGROK_DOMAIN` / `NGROK_BASIC_AUTH` |
| `companion/tunnel.sh` | **新增** 起 ngrok(读 ngrok.yml)+ 提示;与 `deploy.sh` 解耦 |
| `companion/.gitignore`(或根) | 忽略 `companion/ngrok.yml` |
| `sketches/codey_dash/codey_dash.ino` | HTTPS fetchState + WSS ASR + 门户远程字段 + 持久化 + LAN/远程模式 |
| `readme.md` / `readme_zh.md` | 「远程访问(ngrok)」章节 |

测试:`companion/tests/test_ngrok_api.py`(纯函数);固件纯逻辑若有则进 `codey_ui.h` 主机测试。

---

### Task 1: ngrok 配置模板 + .env

**Files:**
- Create: `companion/ngrok.yml.example`
- Modify: `companion/.env.example`
- Modify: `.gitignore`(根)

- [ ] **Step 1: 写 ngrok.yml.example**

```yaml
# companion/ngrok.yml.example — 复制为 ngrok.yml 填值;启动:ngrok start --all --config ngrok.yml
version: "3"
agent:
  authtoken: ${NGROK_AUTHTOKEN}        # ngrok dashboard 获取
tunnels:
  codey-state:                          # 稳定:用免费静态域名
    proto: http
    addr: 8787
    domain: ${NGROK_DOMAIN}            # 形如 your-name.ngrok-free.app(dashboard 申请的免费静态域名)
    basic_auth:
      - ${NGROK_BASIC_AUTH}            # 形如 codey:somelongsecret
  codey-asr:                            # ASR WS;免费档地址随机,由 companion 经 :4040 API 下发
    proto: http
    addr: 8788
    basic_auth:
      - ${NGROK_BASIC_AUTH}
```

- [ ] **Step 2: .env.example 增配置**(追加到文件末尾)

```bash
# --- 远程访问(ngrok)——仅 tunnel.sh / 远程模式需要 ---
NGROK_AUTHTOKEN=
NGROK_DOMAIN=
NGROK_BASIC_AUTH=codey:change-me-to-a-long-secret
```

- [ ] **Step 3: gitignore ngrok.yml**(根 `.gitignore` 追加一行)

```
companion/ngrok.yml
```

- [ ] **Step 4: Commit**

```bash
git add companion/ngrok.yml.example companion/.env.example .gitignore
git commit -m "feat(companion): ngrok 双隧道配置模板 + .env 远程项"
```

---

### Task 2: ngrok 本地 API 解析(纯函数 + 测试)

**Files:**
- Create: `companion/codey/ngrok_api.py`
- Test: `companion/tests/test_ngrok_api.py`

- [ ] **Step 1: 先写失败测试**

```python
# companion/tests/test_ngrok_api.py
from codey.ngrok_api import parse_tunnels, public_urls

SAMPLE = {
    "tunnels": [
        {"name": "codey-state", "public_url": "https://your-name.ngrok-free.app", "config": {"addr": "http://localhost:8787"}},
        {"name": "codey-asr",   "public_url": "https://ab12cd.ngrok-free.app",     "config": {"addr": "http://localhost:8788"}},
    ]
}

def test_parse_maps_by_local_port():
    m = parse_tunnels(SAMPLE)
    assert m[8787] == "https://your-name.ngrok-free.app"
    assert m[8788] == "https://ab12cd.ngrok-free.app"

def test_public_urls_picks_state_and_asr():
    urls = public_urls(SAMPLE, state_port=8787, asr_port=8788)
    assert urls == {"state_url": "https://your-name.ngrok-free.app",
                    "asr_url": "wss://ab12cd.ngrok-free.app"}   # asr 转成 wss

def test_public_urls_tolerates_missing():
    assert public_urls({"tunnels": []}, 8787, 8788) == {"state_url": "", "asr_url": ""}
```

Run: `cd companion && python3 -m pytest tests/test_ngrok_api.py -q` → FAIL(模块不存在)。

- [ ] **Step 2: 实现 ngrok_api.py**

```python
# companion/codey/ngrok_api.py
"""解析 ngrok 本地 web API(http://127.0.0.1:4040/api/tunnels)拿到公网地址。
免费档 ASR 隧道地址随机,故由 companion 下发给设备(见 state.asr_url)。"""
import json
import urllib.request

NGROK_API = "http://127.0.0.1:4040/api/tunnels"


def _addr_port(cfg):
    addr = (cfg or {}).get("addr", "")          # "http://localhost:8788" / "localhost:8788"
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
```

- [ ] **Step 3: 测试转绿**

Run: `cd companion && python3 -m pytest tests/test_ngrok_api.py -q` → PASS。

- [ ] **Step 4: Commit**

```bash
git add companion/codey/ngrok_api.py companion/tests/test_ngrok_api.py
git commit -m "feat(companion): 解析 ngrok :4040 API 取公网地址(纯函数+测试)"
```

---

### Task 3: 把 asr_url 注入 /codey/state

**Files:**
- Modify: `companion/codey/server.py`(后台刷新 ngrok 地址 + 注入 state)
- Modify: `companion/codey/state.py`(state dict 增 `asr_url` 透传)
- Test: `companion/tests/test_server_routes.py`(增断言)

**说明:** server.py 用后台线程每 ~15s 调 `ngrok_api.fetch()`+`public_urls()` 缓存结果(env 决定端口),`/codey/state` 响应时合入 `asr_url`。不可变更新缓存(整体替换 dict,不就地改)。

- [ ] **Step 1: 写失败测试**(state 含 `asr_url` 键)

```python
def test_state_has_asr_url_key():
    # App() 默认无 ngrok 时 asr_url 应为空字符串(键存在)
    from codey.server import App
    app = App()
    body = app.state_json()              # 已有的 state 组装入口(按现有命名调整)
    import json; d = json.loads(body)
    assert "asr_url" in d
```

Run → FAIL。

- [ ] **Step 2: state.py 透传 asr_url**

在组装 state 的 dict 里加 `"asr_url": asr_url`(函数签名增 `asr_url=""` 参数,默认空,保持纯函数 + 不可变)。

- [ ] **Step 3: server.py 后台刷新 + 合入**

```python
# server.py(App 内,示意)
from codey import ngrok_api
import os, threading, time

STATE_PORT = int(os.environ.get("CODEY_PORT") or 8787)
ASR_PORT   = int(os.environ.get("CODEY_ASR_PORT") or 8788)

# App.__init__:self._ngrok = {"state_url": "", "asr_url": ""}
def _ngrok_poll(self):                    # 后台线程
    while True:
        urls = ngrok_api.public_urls(ngrok_api.fetch(), STATE_PORT, ASR_PORT)
        self._ngrok = urls               # 不可变整体替换
        time.sleep(15)
# start_background():threading.Thread(target=self._ngrok_poll, daemon=True).start()
# state 组装:传入 asr_url=self._ngrok.get("asr_url", "")
```

- [ ] **Step 4: 全测试转绿**

Run: `cd companion && python3 -m pytest -q` → all PASS。

- [ ] **Step 5: Commit**

```bash
git add companion/codey/server.py companion/codey/state.py companion/tests/test_server_routes.py
git commit -m "feat(companion): /codey/state 下发 ngrok asr_url(后台刷新)"
```

---

### Task 4: tunnel.sh 启动 ngrok

**Files:**
- Create: `companion/tunnel.sh`

- [ ] **Step 1: 写 tunnel.sh**(预检 ngrok / ngrok.yml,起 `ngrok start --all`)

```bash
#!/usr/bin/env bash
# companion/tunnel.sh — 起 ngrok 双隧道(state :8787 + asr :8788)。先 ./deploy.sh start --bg 再跑本脚本。
set -euo pipefail
cd "$(dirname "$0")"
command -v ngrok >/dev/null 2>&1 || { echo "未装 ngrok:brew install ngrok/ngrok/ngrok" >&2; exit 1; }
[ -f ngrok.yml ] || { echo "缺 ngrok.yml(从 ngrok.yml.example 复制并填值)" >&2; exit 1; }
[ -f .env ] && set -a && . ./.env && set +a   # 注入 NGROK_* 供 ${} 展开
echo "[tunnel] 起 ngrok;本地面板 http://127.0.0.1:4040"
exec ngrok start --all --config ngrok.yml
```

- [ ] **Step 2: chmod + 语法 + 冒烟(无 token 时应给出清晰报错)**

```bash
chmod +x companion/tunnel.sh && bash -n companion/tunnel.sh && echo OK
```

- [ ] **Step 3: Commit**

```bash
git add companion/tunnel.sh
git commit -m "feat(companion): tunnel.sh 一键起 ngrok 双隧道"
```

---

### Task 5: README 远程访问章节(中英)

**Files:**
- Modify: `readme.md`, `readme_zh.md`

- [ ] **Step 1: 新增「Remote access (ngrok) / 远程访问(ngrok)」小节**,内容:
  - `brew install ngrok`,`ngrok config add-authtoken`,dashboard 申请免费静态域名。
  - 复制 `ngrok.yml.example`→`ngrok.yml`,填 `.env` 的 `NGROK_*`。
  - 先 `./deploy.sh start --bg` 再 `./tunnel.sh`。
  - 设备配网门户填:状态域名 `your-name.ngrok-free.app` + basic-auth `codey:secret`;ASR 地址自动从 state 下发。
  - 安全提示:basic-auth 必填;不要把 ngrok.yml/.env 提交。
- [ ] **Step 2: Commit**

```bash
git add readme.md readme_zh.md
git commit -m "docs: README 增 ngrok 远程访问部署(中英)"
```

---

### Task 6: 固件 — HTTPS 取 state + 远程模式

**Files:**
- Modify: `sketches/codey_dash/codey_dash.ino`

**说明:** 新增「远程模式」:门户输入 `remoteHost`(状态域名)+ `remoteAuth`(`user:pass`),持久化到 prefs。非空则 `g_companionUrl = "https://" + remoteHost + "/codey/state"`,用 `WiFiClientSecure`(`setInsecure()`)+ `HTTPClient`,带 `Authorization: Basic <b64>` 和 `ngrok-skip-browser-warning: true`。为空则保持现有 LAN 逻辑(mDNS/fallback IP)。

- [ ] **Step 1: 取 state 改 HTTPS + 头部**

```cpp
// fetchState():远程模式走 TLS;b64 / 头部
static String g_remoteHost, g_remoteAuthB64;     // 从 prefs 读;空=LAN 模式
// ...
HTTPClient http;
if (g_remoteHost.length()) {
  static WiFiClientSecure tls; tls.setInsecure();               // ngrok 证书不校验(够用)
  if (!http.begin(tls, g_companionUrl)) return;                 // https://<host>/codey/state
  if (g_remoteAuthB64.length()) http.addHeader("Authorization", "Basic " + g_remoteAuthB64);
  http.addHeader("ngrok-skip-browser-warning", "true");
} else {
  if (!http.begin(g_companionUrl)) return;                      // 原 LAN 明文
}
int code = http.GET();
// ... 原解析;新增读 doc["asr_url"] -> g_asrUrl
```

- [ ] **Step 2: 解析 asr_url**(state JSON 增字段)

```cpp
{ const char* a = doc["asr_url"] | ""; strncpy(g_asrUrl, a, sizeof(g_asrUrl)-1); g_asrUrl[sizeof(g_asrUrl)-1]=0; }
```

- [ ] **Step 3: 编译**

Run: `./scripts/build.sh sketches/codey_dash` → 编译通过(记录 flash %)。

- [ ] **Step 4: Commit**

```bash
git add sketches/codey_dash/codey_dash.ino
git commit -m "feat(firmware): 远程模式 HTTPS 取 state(TLS + basic-auth + skip-warning)"
```

---

### Task 7: 固件 — WSS 连 ASR(地址来自 state)

**Files:**
- Modify: `sketches/codey_dash/codey_dash.ino`

**说明:** 远程模式下,ASR 不再用 `g_macIp:8788`,而是解析 state 下发的 `g_asrUrl`(`wss://host[/path]`),用 `g_ws.beginSSL(host, 443, "/")` + `setExtraHeaders`(`Authorization` + `ngrok-skip-browser-warning`)。`g_asrUrl` 变化时重连。LAN 模式保持 `g_ws.begin(g_macIp, 8788, "/")`。

- [ ] **Step 1: 解析 wss URL → host(去掉 `wss://` 与可选 path)**

```cpp
// 纯逻辑:parseWssHost("wss://ab12.ngrok-free.app/x") -> "ab12.ngrok-free.app"(放 codey_ui.h,加主机测试)
```

- [ ] **Step 2: 远程模式 beginSSL + 头部**

```cpp
if (g_remoteHost.length() && g_asrUrl[0]) {
  String host = parseWssHost(g_asrUrl);
  if (g_remoteAuthB64.length())
    g_ws.setExtraHeaders(("Authorization: Basic " + g_remoteAuthB64 + "\r\nngrok-skip-browser-warning: true").c_str());
  g_ws.beginSSL(host.c_str(), 443, "/");
} else {
  g_ws.begin(g_macIp.c_str(), ASR_PORT, "/");
}
```

- [ ] **Step 3: 编译 + 主机测试(parseWssHost)**

Run: `bash sketches/codey_dash/test/run_tests.sh` + `./scripts/build.sh sketches/codey_dash` → PASS / 编译通过。

- [ ] **Step 4: Commit**

```bash
git add sketches/codey_dash/codey_dash.ino sketches/codey_dash/codey_ui.h sketches/codey_dash/test/codey_ui_test.cpp
git commit -m "feat(firmware): 远程模式 WSS 连 ASR(地址来自 state.asr_url)"
```

---

### Task 8: 固件 — 配网门户增「远程主机 + 鉴权」字段

**Files:**
- Modify: `sketches/codey_dash/codey_dash.ino`(`portalHtml()` + `portalHandleConnect()` + prefs 读写)

**说明:** 门户增两个输入:Remote host(`your-name.ngrok-free.app`,空=LAN)与 Auth(`user:pass`)。保存到 `g_prefs`(key `rhost`/`rauth`);`rauth` 存原文,启动时 base64 → `g_remoteAuthB64`。提供「清空=回 LAN 模式」。

- [ ] **Step 1: portalHtml 增字段**(两个 `<input>`)。
- [ ] **Step 2: portalHandleConnect 读取并 `g_prefs.putString("rhost"/"rauth", ...)`。**
- [ ] **Step 3: 启动读 prefs → `g_remoteHost`/`g_remoteAuthB64`(base64 编码 user:pass)。** 加一个小 base64 编码器(或用 `mbedtls_base64_encode`)。
- [ ] **Step 4: 编译。**

Run: `./scripts/build.sh sketches/codey_dash` → 编译通过。

- [ ] **Step 5: Commit**

```bash
git add sketches/codey_dash/codey_dash.ino
git commit -m "feat(firmware): 配网门户增远程主机+鉴权字段(持久化, 空=LAN)"
```

---

### Task 9: 端到端验证(部分需真机/USB)

- [ ] **Step 1: companion 侧实测**:`./deploy.sh start --bg` + 假 ngrok(或真 ngrok)→ `curl -s http://127.0.0.1:8787/codey/state | grep asr_url`;起 `./tunnel.sh` 后 `curl :4040/api/tunnels` 与 state 的 `asr_url` 对上。
- [ ] **Step 2: 公网实测**:外网 `curl -u codey:secret https://<domain>.ngrok-free.app/codey/state`(带 `ngrok-skip-browser-warning`)→ 200 + 正确 JSON;不带鉴权 → 401。
- [ ] **Step 3: 固件**(USB 接上后):`./scripts/flash.sh sketches/codey_dash`;门户填远程域名+鉴权;断开公司 WiFi 用手机热点,确认手表仍能拉 state + 语音 ASR 走 wss。
- [ ] **Step 4: 安全自查**:`git status` 确认 `ngrok.yml`/`.env` 未被提交;basic-auth 已开。

---

## Self-Review

- **覆盖**:暴露状态+ASR(Task 3/6/7)、HTTP 隧道+静态域名(Task 1)、鉴权(Task 1 basic_auth + Task 6/7 header)——三项决策全覆盖。
- **2 端点/1 静态域名矛盾**:由 `asr_url` 经 state 下发解决(Task 2/3),设备只认稳定状态域名。
- **不可变**:ngrok 缓存整体替换(Task 3);state 透传新参数默认空。
- **安全**:basic-auth 必填;`ngrok.yml`/`.env` gitignore;TLS `setInsecure()` 已标注权衡(ngrok 证书,LAN 外可接受;后续可换 CA 固定)。
- **风险**:① ngrok 免费档并发隧道数限制——若不允许 2 条同时在线,回退「单端口统一」(把 ASR WS 经 `websockets` 的 `process_request` 并入 :8787,1 条隧道)。② ESP32 wss 推 PCM 的内存/延迟——先验证再调缓冲。③ 固件现无 USB,Task 6–8 可编译,刷机/真机验证顺延(Task 9 Step 3)。
