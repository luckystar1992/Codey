# Companion 合一 + Web 管理平台 + ASR 历史(Plan C)Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** 把 companion(HTTP 状态 :8787)与 ASR(WebSocket :8788)**合并成一个进程**(一条命令启动,保留两端口 → 固件零改动);新增一个 **Web 管理平台**(:8787 提供):① 设备镜像(复用 `sim/codey-sim.html` 实时渲染 `/codey/state`);② ASR 识别历史。ASR 识别结果**追加到单个滚动 JSONL** 文件(带时间码),Web 与 API 读取展示。

**Architecture:** 入口 `codey_companion.py` 继续跑同步 `ThreadingHTTPServer`(:8787),并在**后台线程**里用各自的 asyncio loop 跑现有 `asr_stream` 的 WS 服务(:8788)。HTTP 路由新增 `/`(管理页)、`/sim`(设备镜像)、`/codey/history`(ASR 历史 JSON)。ASR 在产出 final 时除粘贴外,追加一行到 `companion/data/asr_history.jsonl`。Web 管理页纯只读、纯静态 + 轮询。零新依赖。

**Tech Stack:** Python 3.11 标准库(`http.server`/`threading`)+ 既有 `websockets`/`numpy`/`sherpa-onnx`;`pytest`(companion/tests)。前端:原生 HTML/JS(无框架)。

**契约不变:** 固件仍连 `:8787`(状态/`POST /codey/asr`)与 `ws://:8788`(ASR 流);本计划只在 companion 侧合并进程 + 加 Web/历史,不改这两个契约。

---

## 文件结构
```
companion/
  codey_companion.py        # 改:同进程再起 ASR WS 后台线程
  codey/
    asr_history.py          # 新:JSONL append + recent()(纯 IO,可测)
    server.py               # 改:路由 / · /sim · /codey/history;静态文件服务
  asr_stream.py             # 改:产出 final 时写历史;暴露 run_server() 供线程调用
  web/
    admin.html              # 新:管理页(设备镜像 iframe + ASR 历史面板,轮询)
  data/asr_history.jsonl    # 运行期生成(gitignore)
  tests/test_asr_history.py # 新
sim/codey-sim.html          # 改:同源(:8787)时自动轮询 /codey/state(实时镜像)
```

---

## Task 1: asr_history — JSONL 追加 + 读取

**Files:** Create `companion/codey/asr_history.py`, `companion/tests/test_asr_history.py`

- [ ] **Step 1: 写失败测试**
```python
# companion/tests/test_asr_history.py
import json, os, unittest, tempfile
from codey import asr_history


class TestAsrHistory(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.NamedTemporaryFile(suffix=".jsonl", delete=False)
        self.tmp.close()
        os.environ["CODEY_ASR_HISTORY"] = self.tmp.name

    def tearDown(self):
        os.environ.pop("CODEY_ASR_HISTORY", None)
        os.unlink(self.tmp.name)

    def test_append_then_recent_with_timecode(self):
        e = asr_history.append("你好世界", engine="sherpa", pasted=True, at_ms=1780000000000)
        self.assertEqual(e["text"], "你好世界")
        self.assertEqual(e["ts"], 1780000000000)
        self.assertIn("time", e)                 # ISO 时间码
        self.assertTrue(e["pasted"])
        rows = asr_history.recent(10)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["text"], "你好世界")

    def test_empty_text_skipped(self):
        self.assertIsNone(asr_history.append("   ", engine="sherpa"))
        self.assertEqual(asr_history.recent(10), [])

    def test_recent_returns_last_n_in_order(self):
        for i in range(5):
            asr_history.append(f"t{i}", at_ms=1780000000000 + i)
        rows = asr_history.recent(3)
        self.assertEqual([r["text"] for r in rows], ["t2", "t3", "t4"])
```

- [ ] **Step 2: 运行,确认失败** — `cd companion && python3 -m pytest tests/test_asr_history.py -q`

- [ ] **Step 3: 实现**
```python
# companion/codey/asr_history.py
"""ASR 识别历史:单个滚动 JSONL(append),每行带时间码 ts(ms)+ time(ISO)。"""
import json
import os
import time
from datetime import datetime, timezone

_DEFAULT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "data", "asr_history.jsonl")


def _path():
    return os.environ.get("CODEY_ASR_HISTORY") or _DEFAULT


def append(text, engine="", pasted=False, app="", at_ms=None):
    text = (text or "").strip()
    if not text:
        return None
    at = int(at_ms) if at_ms is not None else int(time.time() * 1000)
    iso = datetime.fromtimestamp(at / 1000, tz=timezone.utc).astimezone().isoformat(timespec="seconds")
    entry = {"ts": at, "time": iso, "text": text, "engine": engine, "pasted": bool(pasted), "app": app}
    p = _path()
    try:
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "a", encoding="utf-8") as f:
            f.write(json.dumps(entry, ensure_ascii=False) + "\n")
    except OSError as e:
        print("asr_history append failed:", e)
    return entry


def recent(n=100):
    try:
        with open(_path(), encoding="utf-8") as f:
            lines = f.readlines()
    except OSError:
        return []
    out = []
    for ln in lines[-int(n):]:
        ln = ln.strip()
        if not ln:
            continue
        try:
            out.append(json.loads(ln))
        except Exception:
            pass
    return out
```

- [ ] **Step 4: 通过** — `python3 -m pytest tests/test_asr_history.py -q`
- [ ] **Step 5: Commit**
```bash
git add companion/codey/asr_history.py companion/tests/test_asr_history.py
git commit -m "feat(companion): asr_history — 滚动 JSONL(带时间码)append/recent"
```

---

## Task 2: asr_stream — 产出 final 写历史 + 暴露 run_server()

**Files:** Modify `companion/asr_stream.py`

- [ ] **Step 1: final 时写历史**

在 `handle()` 的 `listen:stop` 分支里,产出 `final_text` 后(粘贴块之后)追加历史。找到:
```python
                    final_text = await emit(await backend.stop())
                    backend = None
                    if final_text and paster.enabled:
                        try:
                            paster.paste(final_text)
                            if paster.auto_enter:
                                paster.enter()
                        except Exception as e:
                            print(f"[asr] paste error: {e}", flush=True)
```
在其后追加:
```python
                    if final_text:
                        from codey import asr_history, envcfg as _ec
                        asr_history.append(final_text, engine=_ec.select_engine(), pasted=paster.enabled)
```

- [ ] **Step 2: 暴露线程入口**

在 `main()` 之后、`if __name__ == "__main__":` 之前加:
```python
def run_server():
    """供 codey_companion 在后台线程里启动 ASR WS 服务(各自 asyncio loop)。"""
    asyncio.run(main())
```
保留底部 `if __name__ == "__main__": asyncio.run(main())` 不变(仍可独立运行)。

- [ ] **Step 3: 验证**
- 导入自检(companion 下):`python3 -c "import sys; sys.path.insert(0,'.'); import asr_stream; print(hasattr(asr_stream,'run_server'))"` → `True`。
- 全量回归:`cd companion && python3 -m pytest -q`(不回归;ASR 活路径不在单测)。

- [ ] **Step 4: Commit**
```bash
git add companion/asr_stream.py
git commit -m "feat(companion): ASR 产出 final 写历史 + run_server() 线程入口"
```

---

## Task 3: codey_companion — 同进程起 ASR WS 线程

**Files:** Modify `companion/codey_companion.py`

- [ ] **Step 1: 起后台 ASR 线程**

顶部 import 加 `import threading` 和 `import asr_stream`(与 codey_companion 同目录,top-level)。在 `main()` 里 `app.start_background()` 之后、起 HTTP 之前加:
```python
    threading.Thread(target=asr_stream.run_server, daemon=True).start()   # ASR WS :8788(同进程)
    print(f"Codey ASR    -> ws://{lan_ip()}:8788  (engine={__import__('codey.envcfg', fromlist=['x']).select_engine()})")
```
> 简化:用 `from codey import envcfg` 顶部导入后 `envcfg.select_engine()` 打印更清晰。可改成顶部 `from codey import envcfg` 再 `print(... envcfg.select_engine())`。

- [ ] **Step 2: 验证两端口同进程监听**

Run(后台起服务再探端口):
```bash
cd companion && (python3 codey_companion.py >/tmp/codey_uni.log 2>&1 & echo $! >/tmp/codey_uni.pid); sleep 4
lsof -nP -iTCP:8787 -sTCP:LISTEN | tail -1
lsof -nP -iTCP:8788 -sTCP:LISTEN | tail -1
cat /tmp/codey_uni.log
kill $(cat /tmp/codey_uni.pid) 2>/dev/null
```
Expected: 8787 与 8788 都 LISTEN,且属于**同一个 python 进程 PID**;日志打印两行 `Codey companion -> ...:8787` 与 `Codey ASR -> ws://...:8788`。

- [ ] **Step 3: Commit**
```bash
git add companion/codey_companion.py
git commit -m "feat(companion): 合一 —— 同进程同时起 HTTP(:8787) 与 ASR WS(:8788)"
```

---

## Task 4: server.py — 路由 / · /sim · /codey/history + 静态服务

**Files:** Modify `companion/codey/server.py`; Create `companion/tests/test_server_routes.py`

- [ ] **Step 1: 写失败测试(history 路由解析 + 静态读取纯逻辑)**

把可测的部分抽成纯函数:历史条数解析 + 静态文件读取。在测试里直接调:
```python
# companion/tests/test_server_routes.py
import os, tempfile, unittest
from codey import server, asr_history


class TestServerRoutes(unittest.TestCase):
    def test_parse_history_n(self):
        self.assertEqual(server.parse_history_n("/codey/history"), 100)
        self.assertEqual(server.parse_history_n("/codey/history?n=20"), 20)
        self.assertEqual(server.parse_history_n("/codey/history?n=abc"), 100)
        self.assertEqual(server.parse_history_n("/codey/history?n=99999"), server.HISTORY_MAX)

    def test_static_bytes_and_ctype(self):
        d = tempfile.mkdtemp()
        with open(os.path.join(d, "a.html"), "w") as f:
            f.write("<h1>hi</h1>")
        body, ctype = server.read_static(os.path.join(d, "a.html"))
        self.assertEqual(body, b"<h1>hi</h1>")
        self.assertEqual(ctype, "text/html; charset=utf-8")
        self.assertEqual(server.read_static(os.path.join(d, "missing.html")), (None, None))
```

- [ ] **Step 2: 运行,确认失败**。

- [ ] **Step 3: 实现**

在 `server.py` 顶部加 import + 常量 + 纯助手:
```python
import os
from urllib.parse import urlparse, parse_qs
from . import asr_history

HISTORY_MAX = 500
WEB_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "web")   # companion/web
SIM_PATH = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "sim", "codey-sim.html")  # repo/sim


def parse_history_n(path):
    try:
        q = parse_qs(urlparse(path).query)
        n = int(q.get("n", ["100"])[0])
    except (ValueError, TypeError):
        return 100
    return max(1, min(HISTORY_MAX, n))


def read_static(path):
    try:
        with open(path, "rb") as f:
            body = f.read()
    except OSError:
        return None, None
    ext = os.path.splitext(path)[1].lower()
    ctype = {".html": "text/html; charset=utf-8", ".js": "text/javascript",
             ".css": "text/css", ".json": "application/json"}.get(ext, "application/octet-stream")
    return body, ctype
```
在 `Handler.do_GET` 里把现有逻辑扩成:
```python
        def do_GET(self):
            path = urlparse(self.path).path
            if path in ("/", "/admin", "/admin.html"):
                body, ctype = read_static(os.path.join(WEB_DIR, "admin.html"))
                self._send(200, body, ctype) if body else self._send(404, b"admin.html missing", "text/plain")
            elif path == "/sim":
                body, ctype = read_static(SIM_PATH)
                self._send(200, body, ctype) if body else self._send(404, b"sim missing", "text/plain")
            elif path.startswith("/codey/state"):
                self._send(200, json.dumps(app.state()).encode())
            elif path.startswith("/codey/history"):
                self._send(200, json.dumps({"entries": asr_history.recent(parse_history_n(self.path))},
                                           ensure_ascii=False).encode())
            else:
                self._send(404, b"not found", "text/plain")
```

- [ ] **Step 4: 通过 + 回归** — `cd companion && python3 -m pytest -q`(新 + 原全绿)。
- [ ] **Step 5: Commit**
```bash
git add companion/codey/server.py companion/tests/test_server_routes.py
git commit -m "feat(companion): HTTP 增 / · /sim · /codey/history 路由 + 静态服务"
```

---

## Task 5: web/admin.html(管理页)+ sim 实时轮询

**Files:** Create `companion/web/admin.html`; Modify `sim/codey-sim.html`

- [ ] **Step 1: 管理页**

Create `companion/web/admin.html`(深色,左设备镜像 iframe + 右 ASR 历史轮询;移动端纵向堆叠):
```html
<!DOCTYPE html>
<html lang="zh"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Codey Companion · 管理台</title>
<style>
  :root{--bg:#0c0d10;--panel:#15171b;--ink:#e6e8ec;--dim:#8b9097;--green:#3ccb7f;--line:#262a31;}
  *{box-sizing:border-box} html,body{margin:0;height:100%;background:var(--bg);color:var(--ink);
    font-family:ui-monospace,SFMono-Regular,Menlo,monospace}
  .top{padding:12px 18px;border-bottom:1px solid var(--line);display:flex;gap:12px;align-items:center}
  .top b{font-size:15px} .top .dot{width:8px;height:8px;border-radius:50%;background:var(--green)}
  .wrap{display:flex;gap:18px;padding:18px;flex-wrap:wrap}
  .mirror{flex:0 0 auto} .mirror iframe{width:480px;height:520px;border:0;border-radius:14px;background:#000}
  .hist{flex:1 1 360px;min-width:320px;background:var(--panel);border:1px solid var(--line);border-radius:14px;
    padding:14px;max-height:560px;overflow:auto}
  .hist h2{margin:0 0 10px;font-size:13px;color:var(--dim);letter-spacing:1px}
  .row{padding:8px 6px;border-bottom:1px solid var(--line)}
  .row .t{font-size:11px;color:var(--dim)} .row .x{font-size:14px;margin-top:3px;white-space:pre-wrap;word-break:break-word}
  .row .e{font-size:10px;color:#6f757d;margin-left:8px}
  .empty{color:#6f757d;font-size:13px;padding:10px}
</style></head><body>
  <div class="top"><span class="dot" id="dot"></span><b>Codey Companion 管理台</b>
    <span class="dim" id="meta" style="color:var(--dim);font-size:12px"></span></div>
  <div class="wrap">
    <div class="mirror"><iframe src="/sim?live=1" title="device"></iframe></div>
    <div class="hist"><h2>ASR 识别历史</h2><div id="list"><div class="empty">加载中…</div></div></div>
  </div>
<script>
async function poll(){
  try{
    const r=await fetch('/codey/history?n=100'); const j=await r.json();
    const list=document.getElementById('list'); const es=(j.entries||[]).slice().reverse();
    document.getElementById('dot').style.background = '#3ccb7f';
    document.getElementById('meta').textContent = es.length+' 条 · 自动刷新';
    list.innerHTML = es.length ? es.map(e=>`<div class="row">
      <div class="t">${(e.time||'').replace('T',' ')}<span class="e">${e.engine||''}${e.pasted?' · pasted':''}</span></div>
      <div class="x">${(e.text||'').replace(/[<>&]/g,c=>({'<':'&lt;','>':'&gt;','&':'&amp;'}[c]))}</div></div>`).join('')
      : '<div class="empty">暂无识别记录</div>';
  }catch(err){ document.getElementById('dot').style.background='#ff5d5d';
    document.getElementById('meta').textContent='连接失败:'+err.message; }
}
poll(); setInterval(poll, 2000);
</script></body></html>
```

- [ ] **Step 2: sim 实时轮询**

`sim/codey-sim.html` 末尾(`render()` 调用处附近、`</script>` 之前)加:同源或带 `?live=1` 时自动轮询 `/codey/state`。复用其已有的 live-fetch 逻辑——把 `liveBtn` 的 fetch 处理抽成函数 `liveFetch()`,然后:
```javascript
// 作为设备镜像嵌入(?live=1 或非 file:// 同源)时,自动轮询 /codey/state
if (new URLSearchParams(location.search).get('live') === '1' || location.protocol.startsWith('http')) {
  const tick = async () => { try {
    const r = await fetch('/codey/state', {cache:'no-store'}); const j = await r.json();
    if (j.providers) { STATE = j; render(); }
  } catch(e){} };
  tick(); setInterval(tick, 2000);
}
```
> 若 sim 里 live 逻辑耦合在按钮回调中,先抽一个 `liveFetch()` 函数共用;保持按钮仍可用。

- [ ] **Step 3: 验证(端到端冒烟)**

Run:
```bash
cd companion && (python3 codey_companion.py >/tmp/codey_uni.log 2>&1 & echo $! >/tmp/codey_uni.pid); sleep 4
curl -s --noproxy '*' http://127.0.0.1:8787/codey/history | python3 -m json.tool | head
curl -s --noproxy '*' -o /dev/null -w "/ -> %{http_code}\n"     http://127.0.0.1:8787/
curl -s --noproxy '*' -o /dev/null -w "/sim -> %{http_code}\n"  http://127.0.0.1:8787/sim
kill $(cat /tmp/codey_uni.pid) 2>/dev/null
```
Expected: `/codey/history` 返回 `{"entries": [...]}`(可能空);`/ -> 200`、`/sim -> 200`。浏览器开 `http://<mac>:8787/` 应看到设备镜像(随真实会话刷新)+ ASR 历史面板。

- [ ] **Step 4: Commit**
```bash
git add companion/web/admin.html sim/codey-sim.html
git commit -m "feat(companion): Web 管理台(设备镜像 + ASR 历史)+ sim 实时轮询"
```

---

## Task 6: .gitignore + README + 全链路冒烟

**Files:** Modify root `.gitignore`;Modify `readme.md`/`readme_zh.md`

- [ ] **Step 1: 忽略运行期数据**

确认 `.gitignore` 含 `companion/data/`(ASR 历史 JSONL 运行期生成,不入库)。无则加一行。

- [ ] **Step 2: README**

`readme.md`/`readme_zh.md` 增补:**一条命令** `cd companion && python3 codey_companion.py` 现在同时起 状态(:8787)、ASR(:8788)、Web 管理台(http://<mac>:8787/)。管理台 = 设备镜像(`/sim`)+ ASR 历史(`/codey/history`,存 `companion/data/asr_history.jsonl`,带时间码)。原 `asr_stream.py` 仍可单独跑(开发用)。

- [ ] **Step 3: 全量回归** — `cd companion && python3 -m pytest -q` 全绿。

- [ ] **Step 4: Commit**
```bash
git add .gitignore readme.md readme_zh.md
git commit -m "docs(companion): 合一服务 + Web 管理台说明;忽略 data/"
```

---

## 完成判据
- `cd companion && python3 -m pytest -q` 全绿(新增 asr_history/server_routes + 原有不回归)。
- **一条命令** `python3 codey_companion.py` 同进程监听 :8787 与 :8788(`lsof` 同 PID)。
- 浏览器 `http://<mac>:8787/`:看到**设备镜像**(实时随 `/codey/state` 刷新)+ **ASR 识别历史**(随说话增长)。
- ASR 每次 final → 追加一行到 `companion/data/asr_history.jsonl`(含 `ts`+`time` 时间码)。
- **固件零改动**:仍连 :8787/:8788,行为不变。

## 已知简化 / 留待
- ASR 活路径(真识别→历史)需真机/录音冒烟;`asr_history` 的 append/recent 已纯函数覆盖。
- 单文件 append 无上限轮替;长期可加按大小/日期切分(YAGNI,先不做)。
- Web 管理台**只读**(无切引擎/控制);如需控制另开计划。
- sim 作镜像内嵌:依赖与 companion 同源(:8787)以避开 CORS;独立打开仍可用 LIVE 按钮。
