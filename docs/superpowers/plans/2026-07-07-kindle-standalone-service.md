# 独立 Kindle 服务 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 新增独立部署的轻量 Kindle 服务(`codey_kindle.py` + `deploy_kindle.sh`),复用完整 companion 相同的数据源(`App`/`collect`),但只跑「采集 + HTTP」,不拉 whisper/ASR/USB/ngrok,纯标准库零第三方依赖。

**Architecture:** `App.start_background()` 抽出 `start_collectors()`(只启采集线程)与 `_collect_once()`(单次采集,可无线程测试)。新入口 `codey_kindle.py` 只 `App() → start_collectors() → ThreadingHTTPServer(make_handler(app))`,不 import `asr_stream`(避免拉入 numpy/sherpa/websockets)。`lan_ip()` 抽到共享 `codey/netinfo.py`。独立 `deploy_kindle.sh` 提供 start/stop/restart/status。

**Tech Stack:** Python 3 标准库;pytest;bash。

**Spec:** `docs/superpowers/specs/2026-07-07-kindle-standalone-service-design.md`

## Global Constraints

- 独立入口 `codey_kindle.py` **绝不 import `asr_stream`**(否则拉入 numpy/sherpa/websockets,破坏零依赖)。
- 端口默认 **8787**(`CODEY_PORT` 可改);独立服务与完整 companion **二选一**,不同端口并行。
- `server.py` 重构行为**逐位等价**:`start_background` 仍 whisper + collectors + ngrok,由现有 158+ 测试守住。
- `deploy_kindle.sh`:独立 `data/kindle.pid` / `data/kindle.log`;只管 HTTP 端口;preflight 只查 python3,不要求 sherpa 模型/numpy/websockets。
- 测试从 `companion/` 跑:`cd /Users/zyc/code/Codey/companion && python3 -m pytest tests/... -v`。
- 中文注释,贴合各文件现有风格;不可变、小函数。

---

### Task 1: 抽 `codey/netinfo.py`(消除 lan_ip 重复)

**Files:**
- Create: `companion/codey/netinfo.py`
- Modify: `companion/codey_companion.py:21-29`(删除本地 `lan_ip`,改 import)
- Test: `companion/tests/test_netinfo.py`(新建)

**Interfaces:**
- Consumes: 无
- Produces: `netinfo.lan_ip() -> str`(本机 LAN IP,失败回 `"127.0.0.1"`)。Task 3 的 `codey_kindle.py` 依赖它。

- [ ] **Step 1: 写失败测试**

新建 `companion/tests/test_netinfo.py`:

```python
"""netinfo.lan_ip 单测:monkeypatch socket 覆盖回环-only 与有 LAN IP 两种情形。"""
from codey import netinfo


def test_lan_ip_returns_lan_when_present(monkeypatch):
    monkeypatch.setattr(netinfo.socket, "gethostname", lambda: "host")
    monkeypatch.setattr(netinfo.socket, "gethostbyname_ex",
                        lambda h: ("host", [], ["127.0.0.1", "192.168.1.42"]))
    assert netinfo.lan_ip() == "192.168.1.42"


def test_lan_ip_loopback_only_falls_back(monkeypatch):
    monkeypatch.setattr(netinfo.socket, "gethostname", lambda: "host")
    monkeypatch.setattr(netinfo.socket, "gethostbyname_ex",
                        lambda h: ("host", [], ["127.0.0.1"]))
    assert netinfo.lan_ip() == "127.0.0.1"


def test_lan_ip_swallows_errors(monkeypatch):
    def boom(_):
        raise OSError("no dns")
    monkeypatch.setattr(netinfo.socket, "gethostname", lambda: "host")
    monkeypatch.setattr(netinfo.socket, "gethostbyname_ex", boom)
    assert netinfo.lan_ip() == "127.0.0.1"
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cd /Users/zyc/code/Codey/companion && python3 -m pytest tests/test_netinfo.py -v`
Expected: ERROR `ModuleNotFoundError: No module named 'codey.netinfo'`

- [ ] **Step 3: 实现**

新建 `companion/codey/netinfo.py`:

```python
"""网络信息小工具:本机 LAN IP(仅主机名解析,不对外发包)。"""
import socket


def lan_ip():
    """本机 LAN IP;仅解析主机名,失败回 "127.0.0.1"。"""
    try:
        for ip in socket.gethostbyname_ex(socket.gethostname())[2]:
            if not ip.startswith("127."):
                return ip
    except Exception:
        pass
    return "127.0.0.1"
```

改 `companion/codey_companion.py`:把 imports 里的 `import socket` 保留(其余用),并把本地 `lan_ip` 定义删除、改为从 netinfo 引入。

找到:

```python
import asr_stream
from codey.server import App, make_handler

PORT = int(os.environ.get("CODEY_PORT") or 8787)


def lan_ip():
    """本机 LAN IP(仅做主机名解析,不对外发包)。"""
    try:
        for ip in socket.gethostbyname_ex(socket.gethostname())[2]:
            if not ip.startswith("127."):
                return ip
    except Exception:
        pass
    return "127.0.0.1"
```

替换为:

```python
import asr_stream
from codey.netinfo import lan_ip
from codey.server import App, make_handler

PORT = int(os.environ.get("CODEY_PORT") or 8787)
```

(`import socket` 若在 `codey_companion.py` 中已不再被其它代码使用,一并删除该行;若仍被使用则保留。当前该文件除 lan_ip 外未用 socket,故删除 `import socket`。)

- [ ] **Step 4: 跑测试确认通过 + 入口仍可导入**

Run: `cd /Users/zyc/code/Codey/companion && python3 -m pytest tests/test_netinfo.py -v && python3 -c "import ast,sys; ast.parse(open('codey_companion.py').read()); print('codey_companion.py 语法 OK')"`
Expected: 测试全 PASS;打印「语法 OK」

- [ ] **Step 5: Commit**

```bash
cd /Users/zyc/code/Codey && git add companion/codey/netinfo.py companion/codey_companion.py companion/tests/test_netinfo.py && git commit -m "refactor(companion): 抽 codey/netinfo.lan_ip 共享,消除 codey_companion 内重复"
```

---

### Task 2: `server.py` 抽 `start_collectors()` 与 `_collect_once()`

**Files:**
- Modify: `companion/codey/server.py:138-157`(`start_background` / `_refresh_loop`)
- Test: `companion/tests/test_server_routes.py`(`TestServerRoutes` 类末尾追加)

**Interfaces:**
- Consumes: 现有 `collect.collect_sessions()`、`collect.tokens_per_min(prev, cur)`、`App.session_cache`/`tok_rate`/`chime`/`lock`
- Produces:
  - `App._collect_once() -> None`:执行一次采集并加锁更新 `session_cache`/`tok_rate`/`chime`。
  - `App.start_collectors() -> threading.Thread`:启动跑 `_refresh_loop` 的 daemon 线程并返回它。
  - `App.start_background()` 行为不变。Task 3 的 `codey_kindle.py` 依赖 `start_collectors`。

- [ ] **Step 1: 写失败测试**

在 `companion/tests/test_server_routes.py` 的 `TestServerRoutes` 类末尾追加:

```python
    def test_collect_once_updates_cache(self):
        from codey import collect
        app = server.App()
        fake = {"claude": [{"tokens_total": 1000}], "codex": []}
        orig = collect.collect_sessions
        collect.collect_sessions = lambda: fake
        try:
            app._collect_once()
        finally:
            collect.collect_sessions = orig
        self.assertEqual(app.session_cache, fake)
        self.assertEqual(app.tok_rate["claude"]["val"], 0)          # 首次:prev=None → 0
        self.assertIsNotNone(app.tok_rate["claude"]["prev"])         # prev 已记录
        self.assertEqual(app.tok_rate["claude"]["prev"]["tokens"], 1000)

    def test_start_collectors_returns_live_daemon_thread(self):
        from codey import collect
        app = server.App()
        orig = collect.collect_sessions
        collect.collect_sessions = lambda: {"claude": [], "codex": []}   # 让后台线程做轻活
        try:
            t = app.start_collectors()
            self.assertTrue(t.is_alive())
            self.assertTrue(t.daemon)
        finally:
            collect.collect_sessions = orig
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cd /Users/zyc/code/Codey/companion && python3 -m pytest tests/test_server_routes.py -v -k "collect_once or start_collectors"`
Expected: FAIL(`App` 无 `_collect_once` / `start_collectors` 属性 → AttributeError)

- [ ] **Step 3: 实现**

`companion/codey/server.py`,找到:

```python
    def start_background(self):
        self.whisper.start()
        threading.Thread(target=self._refresh_loop, daemon=True).start()
        threading.Thread(target=self._ngrok_loop, daemon=True).start()

    def _refresh_loop(self):
        while True:
            try:
                cache = collect.collect_sessions()
                with self.lock:
                    self.chime.update(prev_cache=self.session_cache, cur_cache=cache)
                    self.session_cache = cache
                    for pid in ("claude", "codex"):
                        total = sum(s.get("tokens_total", 0) for s in cache.get(pid, []))
                        cur = {"tokens": total, "at": time.time() * 1000}
                        prev = self.tok_rate[pid]["prev"]
                        self.tok_rate[pid] = {"prev": cur, "val": collect.tokens_per_min(prev, cur)}
            except Exception as e:                                 # 单次失败不影响服务
                print("collect_sessions failed:", e)
            time.sleep(max(0.5, config.get("refresh_ms") / 1000))  # 实时可配,下界 0.5s
```

替换为:

```python
    def start_background(self):
        self.whisper.start()
        self.start_collectors()
        threading.Thread(target=self._ngrok_loop, daemon=True).start()

    def start_collectors(self):
        """只起会话采集后台线程(填 session_cache/tok_rate);/kindle 与 /codey/state 依赖它。返回该线程。"""
        t = threading.Thread(target=self._refresh_loop, daemon=True)
        t.start()
        return t

    def _collect_once(self):
        """采集一次并加锁更新 session_cache / tok_rate / chime。"""
        cache = collect.collect_sessions()
        with self.lock:
            self.chime.update(prev_cache=self.session_cache, cur_cache=cache)
            self.session_cache = cache
            for pid in ("claude", "codex"):
                total = sum(s.get("tokens_total", 0) for s in cache.get(pid, []))
                cur = {"tokens": total, "at": time.time() * 1000}
                prev = self.tok_rate[pid]["prev"]
                self.tok_rate[pid] = {"prev": cur, "val": collect.tokens_per_min(prev, cur)}

    def _refresh_loop(self):
        while True:
            try:
                self._collect_once()
            except Exception as e:                                 # 单次失败不影响服务
                print("collect_sessions failed:", e)
            time.sleep(max(0.5, config.get("refresh_ms") / 1000))  # 实时可配,下界 0.5s
```

- [ ] **Step 4: 跑测试确认通过 + 全量回归**

Run: `cd /Users/zyc/code/Codey/companion && python3 -m pytest tests/ -q`
Expected: 全部 PASS(新增 2 个;既有全绿证明重构无回归)

- [ ] **Step 5: Commit**

```bash
cd /Users/zyc/code/Codey && git add companion/codey/server.py companion/tests/test_server_routes.py && git commit -m "refactor(companion): App 抽 start_collectors/_collect_once(采集可独立启动+无线程测试),start_background 行为不变"
```

---

### Task 3: 新入口 `codey_kindle.py` + 轻量零依赖守卫

**Files:**
- Create: `companion/codey_kindle.py`
- Test: `companion/tests/test_kindle_standalone.py`(新建)

**Interfaces:**
- Consumes: `App`/`make_handler`(`codey.server`)、`netinfo.lan_ip`(Task 1)、`App.start_collectors`(Task 2)
- Produces: `codey_kindle.PORT`、`codey_kindle.main()`;可执行入口

- [ ] **Step 1: 写失败测试**

新建 `companion/tests/test_kindle_standalone.py`:

```python
"""独立 Kindle 入口守卫:import codey_kindle 不得拉入重依赖(numpy/sherpa/websockets/asr_stream)。"""
import subprocess
import sys
import os

COMPANION_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def test_import_stays_lightweight():
    # 子进程干净 import,检查重依赖均未被加载(锁「零第三方依赖」不变量)
    code = (
        "import codey_kindle, sys; "
        "heavy=[m for m in ('numpy','sherpa_onnx','websockets','asr_stream') if m in sys.modules]; "
        "print(','.join(heavy)); "
        "sys.exit(1 if heavy else 0)"
    )
    r = subprocess.run([sys.executable, "-c", code], cwd=COMPANION_DIR,
                       capture_output=True, text=True)
    assert r.returncode == 0, f"重依赖被加载: {r.stdout.strip()} / stderr={r.stderr.strip()}"


def test_exposes_port_and_main():
    code = ("import codey_kindle; "
            "assert isinstance(codey_kindle.PORT, int); "
            "assert callable(codey_kindle.main)")
    r = subprocess.run([sys.executable, "-c", code], cwd=COMPANION_DIR,
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cd /Users/zyc/code/Codey/companion && python3 -m pytest tests/test_kindle_standalone.py -v`
Expected: FAIL(`import codey_kindle` → ModuleNotFoundError,子进程非零退出)

- [ ] **Step 3: 实现**

新建 `companion/codey_kindle.py`:

```python
#!/usr/bin/env python3
"""Codey Kindle 服务 —— 只跑「会话采集 + HTTP」的轻量入口,供 Kindle/浏览器看监视页。

与完整 companion 用同一份数据源(App/collect),但不启动 whisper / ASR WebSocket / USB link /
ngrok,纯 Python 标准库、零第三方依赖、无需 sherpa 模型。适合只想看 /kindle 监视页的场景。

  GET /kindle        e-ink 监视页(自带 meta refresh 自动刷新)
  GET /codey/state   归一化用量+会话 JSON(/kindle 内部数据源)
  GET /admin         Web 管理台(语音相关功能在本模式下不可用)

运行:  python3 codey_kindle.py   (或 ./deploy_kindle.sh)
"""
import os
from http.server import ThreadingHTTPServer

from codey.netinfo import lan_ip
from codey.server import App, make_handler

PORT = int(os.environ.get("CODEY_PORT") or 8787)


def main():
    app = App()
    app.start_collectors()                  # 只采集会话,不起 whisper/ASR/USB/ngrok
    httpd = ThreadingHTTPServer(("0.0.0.0", PORT), make_handler(app))
    ip = lan_ip()
    print(f"Codey Kindle    -> http://{ip}:{PORT}/kindle  (port {PORT})")
    print(f"                   http://{ip}:{PORT}/codey/state | http://{ip}:{PORT}/admin")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        httpd.server_close()


if __name__ == "__main__":
    main()
```

- [ ] **Step 4: 跑测试确认通过**

Run: `cd /Users/zyc/code/Codey/companion && python3 -m pytest tests/test_kindle_standalone.py -v`
Expected: 全部 PASS(证明 import 轻量、暴露 PORT/main)

- [ ] **Step 5: 冒烟(手动)**

```bash
cd /Users/zyc/code/Codey/companion && CODEY_PORT=8799 python3 codey_kindle.py &
sleep 2
curl -s http://127.0.0.1:8799/kindle | head -5
curl -s http://127.0.0.1:8799/codey/state | head -c 200
kill %1
```
Expected: `/kindle` 返回含 `CODEY MONITOR` 的 HTML;`/codey/state` 返回 JSON(providers 数组)。

- [ ] **Step 6: Commit**

```bash
cd /Users/zyc/code/Codey && git add companion/codey_kindle.py companion/tests/test_kindle_standalone.py && git commit -m "feat(companion): 独立 Kindle 入口 codey_kindle.py(App+采集+HTTP,不拉 whisper/ASR/USB/ngrok,零第三方依赖)"
```

---

### Task 4: 独立启动器 `deploy_kindle.sh` + README 说明

**Files:**
- Create: `companion/deploy_kindle.sh`
- Modify: `readme.md` 与 `readme_zh.md`(补一句独立启动用法)

**Interfaces:**
- Consumes: `codey_kindle.py`(Task 3)
- Produces: `./deploy_kindle.sh start|--bg|stop|restart|status`;无下游任务

- [ ] **Step 1: 实现启动器**

新建 `companion/deploy_kindle.sh`:

```bash
#!/usr/bin/env bash
# Codey Kindle 独立服务启动器 —— 只跑会话采集 + HTTP(:8787,含 /kindle 监视页)。
# 与完整 companion 用同一数据源,但不启动 ASR/whisper/USB/ngrok,纯标准库、零第三方依赖、无需 sherpa 模型。
# 用法:
#   ./deploy_kindle.sh [start]       前台启动(Ctrl-C 退出)
#   ./deploy_kindle.sh start --bg    后台启动(nohup;日志 data/kindle.log,PID data/kindle.pid)
#   ./deploy_kindle.sh stop          停止
#   ./deploy_kindle.sh restart       重启(后台)
#   ./deploy_kindle.sh status        查看状态
# 可选环境变量:CODEY_PORT(默认 8787) PYTHON(默认 python3)
# 注意:勿与完整 companion 同端口并行;二选一,或用 CODEY_PORT 换端口。
set -euo pipefail
cd "$(dirname "$0")"

PY="${PYTHON:-python3}"
HTTP_PORT="${CODEY_PORT:-8787}"
ENTRY="codey_kindle.py"
DATA_DIR="data"
PID_FILE="$DATA_DIR/kindle.pid"
LOG_FILE="$DATA_DIR/kindle.log"

log()  { printf '\033[36m[kindle]\033[0m %s\n' "$*"; }
die()  { printf '\033[31m[kindle] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }
pids_on_port() { lsof -nP -iTCP:"$1" -sTCP:LISTEN -t 2>/dev/null || true; }

preflight() {
  command -v "$PY" >/dev/null 2>&1 || die "找不到 Python:$PY(设环境变量 PYTHON 指定解释器)"
}

ensure_port_free() {
  local busy; busy="$(pids_on_port "$HTTP_PORT")"
  [ -z "$busy" ] || die "端口 $HTTP_PORT 已被占用(PID:$busy)。勿与完整 companion 同端口共存;先 stop,或用 CODEY_PORT 换端口。"
}

cmd_start() {
  preflight
  ensure_port_free
  if [ "${1:-}" = "--bg" ]; then
    mkdir -p "$DATA_DIR"
    nohup "$PY" "$ENTRY" >>"$LOG_FILE" 2>&1 &
    echo "$!" > "$PID_FILE"
    sleep 1
    kill -0 "$(cat "$PID_FILE")" 2>/dev/null || die "启动失败,见日志:$LOG_FILE"
    log "后台启动 PID=$(cat "$PID_FILE") | 日志 $LOG_FILE"
    log "Kindle 打开 http://localhost:$HTTP_PORT/kindle"
  else
    log "前台启动(Ctrl-C 退出)…  Kindle 打开 http://localhost:$HTTP_PORT/kindle"
    exec "$PY" "$ENTRY"
  fi
}

cmd_stop() {
  local stopped=0 pid busy
  if [ -f "$PID_FILE" ]; then
    pid="$(cat "$PID_FILE")"
    if kill -0 "$pid" 2>/dev/null; then kill "$pid" 2>/dev/null && { stopped=1; log "已停止 PID=$pid"; }; fi
    rm -f "$PID_FILE"
  fi
  busy="$(pids_on_port "$HTTP_PORT")"
  [ -n "$busy" ] && kill $busy 2>/dev/null && { stopped=1; log "已停止端口 $HTTP_PORT 上的进程:$busy"; }
  [ "$stopped" = 1 ] || log "没有正在运行的服务"
  return 0
}

cmd_status() {
  local busy; busy="$(pids_on_port "$HTTP_PORT")"
  if [ -n "$busy" ]; then log "端口 $HTTP_PORT:运行中(PID $busy)"; else log "端口 $HTTP_PORT:未运行"; fi
}

case "${1:-start}" in
  start)   shift || true; cmd_start "${1:-}" ;;
  --bg)    cmd_start --bg ;;
  stop)    cmd_stop ;;
  restart) cmd_stop; sleep 1; cmd_start --bg ;;
  status)  cmd_status ;;
  *)       die "未知命令:$1(可用:start | stop | restart | status)" ;;
esac
```

- [ ] **Step 2: 赋可执行位 + 语法校验**

Run:
```bash
cd /Users/zyc/code/Codey/companion && chmod +x deploy_kindle.sh && bash -n deploy_kindle.sh && echo "bash 语法 OK"
```
Expected: 打印「bash 语法 OK」(无语法错误)

- [ ] **Step 3: 冒烟(手动,前后台 + status/stop)**

```bash
cd /Users/zyc/code/Codey/companion && CODEY_PORT=8799 ./deploy_kindle.sh start --bg
CODEY_PORT=8799 ./deploy_kindle.sh status
curl -s http://127.0.0.1:8799/kindle | grep -o "CODEY MONITOR" | head -1
CODEY_PORT=8799 ./deploy_kindle.sh stop
CODEY_PORT=8799 ./deploy_kindle.sh status
```
Expected:依次显示「后台启动 PID=…」→「端口 8799:运行中」→ `CODEY MONITOR` → 「已停止…」→「端口 8799:未运行」。

- [ ] **Step 4: README 补一句**

在 `readme_zh.md` 里找到介绍 `deploy.sh` / 启动服务的段落,追加一行(若无明确锚点则加在启动说明附近):

```markdown
- **只看 Kindle 监视页(轻量)**:`cd companion && ./deploy_kindle.sh`(或 `--bg` 后台)。只跑采集 + HTTP、
  不启动语音/ASR/USB/ngrok,无需 sherpa 模型;Kindle 连同一局域网后打开 `http://<Mac-IP>:8787/kindle`。
```

在 `readme.md` 对应位置追加英文一行:

```markdown
- **Kindle monitor only (lightweight)**: `cd companion && ./deploy_kindle.sh` (or `--bg`). Runs just the
  collector + HTTP server — no voice/ASR/USB/ngrok, no sherpa model needed. Open `http://<Mac-IP>:8787/kindle` on the Kindle.
```

- [ ] **Step 5: Commit**

```bash
cd /Users/zyc/code/Codey && git add companion/deploy_kindle.sh readme.md readme_zh.md && git commit -m "feat(companion): 独立启动器 deploy_kindle.sh(start/stop/restart/status,独立 PID/log,轻量 preflight)+ README 说明"
```

---

## Self-Review 记录

- **Spec 覆盖**:§2.1 server 重构 → Task 2;§2.2 netinfo → Task 1;§2.3 codey_kindle.py → Task 3;§2.4 deploy_kindle.sh → Task 4;§3 错误处理(_refresh_loop try 不变、端口/python3 报错、finally 关闭)→ Task 2/3/4;§4 测试(netinfo/_collect_once/start_collectors/轻量守卫)逐条对应 Task 1/2/3;§5 README 说明 → Task 4 Step 4。无缺口。
- **占位符**:无 TBD/TODO;每处改动/新建给了完整代码。
- **一致性**:`start_collectors()` 在 Task 2 定义(返回 Thread)、Task 3 消费一致;`_collect_once()` Task 2 定义、Task 2 测试消费一致;`netinfo.lan_ip` Task 1 定义、Task 3 消费一致;`CODEY_PORT`/端口 8787 各处一致;冒烟统一用 8799 避免撞占默认端口。
