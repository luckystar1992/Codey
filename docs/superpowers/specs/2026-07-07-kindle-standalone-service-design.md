# 独立 Kindle 服务 — 设计

> 状态:已批准设计,待 writing-plans 拆实施计划
> 分支:`feat/kindle-monitor-page`(接在 Kindle 监视页 + 预览 tab 之后)
> 日期:2026-07-07

## 0. 背景与目标

`/kindle` 监视页现挂在完整 companion 主服务(`codey_companion.py`,端口 8787)里。该主服务
`main()` 会拉起一大堆后台:whisper 子进程、ASR WebSocket(:8788)、USB link、ngrok 轮询。

用户只想在 Kindle 上看监视页时,不必启动这一整套。目标:**提供一个独立部署的轻量 Kindle 服务,
用与完整 companion 完全相同的数据源(会话/用量采集),但只跑「采集 + HTTP」两件事。**

关键洞察(已核实):`App()` 与 `collect` 只依赖标准库;重依赖(numpy / sherpa_onnx / websockets)
全部在 `asr_stream` 里。独立入口不 import `asr_stream`,即可做到**纯标准库、零第三方依赖、
无需 sherpa 模型**就能运行。

## 1. 方案选择

- **A(选定):独立入口 `codey_kindle.py` + 独立启动器 `deploy_kindle.sh`。** 复用现有 `App` /
  `make_handler`(同数据源、同路由),只在启动时不拉 whisper/ASR/USB/ngrok。部署与完整 companion
  完全隔离(独立脚本 / PID / log)。
- B(否):在 `deploy.sh` 加 `kindle` 子命令。会把两套服务的 PID/log/preflight 逻辑塞进一个脚本,耦合更重。
- C(否):独立服务专属精简路由(只 `/kindle` + `/codey/state`)。复用全 handler 零成本且 `/admin` 也能用,精简反而多写代码。

## 2. 组件与数据流

### 2.1 `companion/codey/server.py`(小重构,行为不变)
- 抽 `start_collectors() -> Thread`:只启动会话采集后台线程(填 `session_cache`/`tok_rate`),返回该线程。
- 抽 `_collect_once()`:采集循环的**单次循环体**(collect + 加锁更新 cache/tok_rate/chime)。
- `_refresh_loop` 变为 `while True: try: self._collect_once() except ...: print(); time.sleep(...)`。
- `start_background` 改为调用 `self.start_collectors()`(其余 whisper/ngrok 不变)。行为逐位等价,由现有测试守住。

### 2.2 `companion/codey/netinfo.py`(新小工具)
- `lan_ip() -> str`:本机 LAN IP(仅主机名解析,失败回 `"127.0.0.1"`)。从 `codey_companion.py` 抽出,消除重复。
- `codey_companion.py` 改为 `from codey.netinfo import lan_ip`,删除本地同名函数。

### 2.3 `companion/codey_kindle.py`(新入口)
```python
app = App()
app.start_collectors()                                  # 只采集,不起 whisper/ASR/USB/ngrok
httpd = ThreadingHTTPServer(("0.0.0.0", PORT), make_handler(app))
httpd.serve_forever()
```
- **不 import `asr_stream`**(避免拉入 numpy/sherpa/websockets)。
- 端口默认 `8787`(`CODEY_PORT` 可改)。
- 复用全 handler:`/kindle`、`/admin`(含 Kindle 预览 tab)、`/codey/state`、`/codey/config` 均可用;
  `/codey/asr` 因 whisper 未启动而失效(Kindle 模式不需要)。

### 2.4 `companion/deploy_kindle.sh`(新启动器)
- 子命令 `start [--bg]` / `stop` / `restart` / `status`,体验对齐 `deploy.sh`。
- 只管 HTTP 端口(无 ASR 端口);独立 `data/kindle.pid` / `data/kindle.log`。
- 轻量 preflight:只查 `python3` 存在,**不**要求 sherpa 模型 / numpy / websockets。
- `ensure_port_free`:端口被占则拒绝并提示(勿与完整 companion 同端口共存,或用 `CODEY_PORT` 换端口)。

### 数据流
`_collect_once` 周期 `collect.collect_sessions()` → `session_cache`;`/kindle`、`/codey/state` 读它。
零联网(ngrok 未启,`state.asr_url` 恒 `""`)。**与完整 companion 同一份采集逻辑,数据完全一致。**

## 3. 错误处理

- 采集单次失败:`_refresh_loop` 的 try/except 吞掉,不影响服务(行为不变)。
- 端口占用 / 缺 python3:`deploy_kindle.sh` 明确报错退出。
- `codey_kindle.py` `serve_forever` 被 Ctrl-C 中断 → `finally` 关闭 httpd(whisper 从未启动,无需 stop)。

## 4. 测试

- `companion/tests/test_netinfo.py`:`lan_ip()` 在仅回环时回 `127.0.0.1`;有 LAN IP 时返回它(monkeypatch `socket.gethostbyname_ex`)。
- `companion/tests/test_server_routes.py` 追加:
  - `_collect_once()` 在 monkeypatch `collect.collect_sessions` 后确定性更新 `session_cache` 与 `tok_rate`(无线程)。
  - `start_collectors()` 返回存活的 daemon 线程。
- `companion/tests/test_kindle_standalone.py`:子进程 `import codey_kindle` 后,断言
  `numpy` / `sherpa_onnx` / `websockets` / `asr_stream` **均未加载**——锁住「轻量零依赖」核心不变量。
- `deploy_kindle.sh` 与真机由用户手动冒烟:前台起 → 浏览器/Kindle 开 `/kindle` → `status` / `stop`。

## 5. 不做的事(YAGNI)

- 不改完整 companion 的运行行为(仅无副作用重构)。
- 不做独立服务专属精简路由。
- 不支持独立服务与完整 companion 同端口并行(明确二选一)。
- README 补一句独立启动用法(编辑现有文件,不新建文档)。
