# 语音输入桥 · Companion(Plan A)Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把姊妹项目 `meme` 的语音识别周边能力迁进 Codey 的 **companion(Python)** 侧:① 转写结果**自动粘贴**进当前聚焦窗口 + submit(回车)/clear(清空)控制;② **豆包云端流式 ASR** 作为可选引擎(本地 sherpa 默认,断网/无 key 回落本地);③ 标点/ITN(豆包自带,sherpa 可选本地标点);④ 任务**完成检测**(供固件 chime,Plan B 播放)。本计划只动 companion,**固件零改动即可让语音真正可用**(粘贴在 Mac 侧发生)。

**Architecture:** 新增一组 `companion/codey/` 纯模块逐个 TDD(envcfg / paste / asr_doubao / asr_punct / chime),把现有 `companion/asr_stream.py` 重构成**引擎无关的后端接口**(`SherpaBackend` / `DoubaoBackend`),协议循环在产出 final 时调用粘贴;`codey/server.py` 的刷新循环接入 chime 检测,`state.py` 在 `/codey/state` 暴露 chime 事件。零新增重依赖:豆包文件回落用 stdlib `urllib`(不引 `aiohttp`),`.env` 用 stdlib 自写加载器(不引 `python-dotenv`)。

**Tech Stack:** Python 3.11 标准库 + 既有 `websockets`/`numpy`/`sherpa-onnx`(asr_stream 已依赖);`pytest`(companion/tests,现 31 测试全绿)。macOS `pbcopy`/`osascript`。

**参考(只读,勿改):** `/Users/zyc/code/meme/mac-server/{paste.py,doubao_streaming.py,doubao_asr.py,server.py}`。
**契约不变:** firmware↔ASR 仍是 `ws://<mac>:8788` xiaozhi 风格(hello / listen start|stop / 二进制 PCM / `{"type":"stt",...}`)。本计划**新增** firmware→server 的 `{"type":"submit"}` / `{"type":"clear"}` 消息处理(固件在 Plan B 发送;companion 先就绪)。

---

## 分支 & 范围
- 分支已切:`feat/voice-input-bridge`(从 `feat/agent-session-monitor` 切出)。
- 本计划 = **Plan A(companion)**。固件侧(chime 播放、BtnB 多击发 submit/clear)= **Plan B**,待 Plan A 落地后另写。
- 引擎选择:env `CODEY_ASR_ENGINE=sherpa|doubao|auto`(默认 `auto`:有 `DOUBAO_API_KEY` 用豆包,否则 sherpa)。

## 文件结构
```
companion/
  codey/
    envcfg.py        # 新建:stdlib .env 加载 + 引擎/粘贴配置读取
    paste.py         # 新建:pbcopy + osascript 粘贴/回车/清空(移植 meme)
    asr_doubao.py    # 新建:豆包流式分帧/解析(纯) + 流式会话 + 文件回落(urllib)
    asr_punct.py     # 新建:可选 sherpa 本地标点(模型缺失则 no-op)
    chime.py         # 新建:完成检测纯函数(status 跃迁 -> chime 事件)
    server.py        # 修改:_refresh_loop 接入 chime;App.state 传 chime
    state.py         # 修改:build_state 输出 chime 字段
  asr_stream.py      # 修改:引擎无关后端 + 产出 final 粘贴 + submit/clear
  .env.example       # 新建:DOUBAO_* / CODEY_ASR_ENGINE / CODEY_PASTE_* 模板
  tests/
    test_envcfg.py test_paste.py test_asr_doubao.py test_asr_punct.py
    test_chime.py test_asr_backend.py     # 新建
```
约定:`codey/` 下纯函数不读网络/不起 osascript;副作用(subprocess/WS/HTTP)收敛在薄封装,单测用 monkeypatch 注入假实现。

---

## Task 1: envcfg — stdlib .env 加载 + 配置读取

**Files:** Create `companion/codey/envcfg.py`, `companion/tests/test_envcfg.py`

- [ ] **Step 1: 写失败测试**
```python
# companion/tests/test_envcfg.py
from codey import envcfg

def test_parse_env_text_ignores_comments_and_quotes():
    text = "\n".join([
        "# comment", "", "DOUBAO_API_KEY = abc123  ",
        'DOUBAO_APP_ID="42"', "EMPTY=", "export CODEY_ASR_ENGINE=doubao",
    ])
    d = envcfg.parse_env_text(text)
    assert d["DOUBAO_API_KEY"] == "abc123"
    assert d["DOUBAO_APP_ID"] == "42"
    assert d["EMPTY"] == ""
    assert d["CODEY_ASR_ENGINE"] == "doubao"

def test_select_engine():
    assert envcfg.select_engine({"CODEY_ASR_ENGINE": "sherpa", "DOUBAO_API_KEY": "x"}) == "sherpa"
    assert envcfg.select_engine({"CODEY_ASR_ENGINE": "doubao"}) == "doubao"
    assert envcfg.select_engine({"CODEY_ASR_ENGINE": "auto", "DOUBAO_API_KEY": "x"}) == "doubao"
    assert envcfg.select_engine({"CODEY_ASR_ENGINE": "auto"}) == "sherpa"
    assert envcfg.select_engine({}) == "sherpa"

def test_paste_flags():
    assert envcfg.paste_enabled({"CODEY_PASTE": "0"}) is False
    assert envcfg.paste_enabled({}) is True
    assert envcfg.auto_enter({"CODEY_PASTE_AUTO_ENTER": "1"}) is True
    assert envcfg.auto_enter({}) is False
```

- [ ] **Step 2: 运行,确认失败** — `cd companion && python3 -m pytest tests/test_envcfg.py -q` → 模块缺失。

- [ ] **Step 3: 实现**
```python
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
```

- [ ] **Step 4: 通过** — `python3 -m pytest tests/test_envcfg.py -q` → PASS。
- [ ] **Step 5: Commit**
```bash
git add companion/codey/envcfg.py companion/tests/test_envcfg.py
git commit -m "feat(companion): envcfg — 零依赖 .env 加载 + 引擎/粘贴配置"
```

---

## Task 2: paste — pbcopy + osascript 粘贴/回车/清空

**Files:** Create `companion/codey/paste.py`, `companion/tests/test_paste.py`

移植 `meme/mac-server/paste.py`,但把 subprocess 调用收敛成可注入的薄层,便于单测断言"剪贴板写了什么 / osascript 收到什么"。

- [ ] **Step 1: 写失败测试**
```python
# companion/tests/test_paste.py
from codey import paste

def test_paste_sets_clipboard_then_cmd_v(monkeypatch):
    calls = []
    monkeypatch.setattr(paste, "_pbcopy", lambda text: calls.append(("pbcopy", text)))
    monkeypatch.setattr(paste, "_osascript", lambda script: calls.append(("osa", script)))
    paste.paste_to_active_window("你好 world")
    assert calls[0] == ("pbcopy", "你好 world")
    assert calls[1][0] == "osa" and "keystroke \"v\"" in calls[1][1]

def test_paste_empty_is_noop(monkeypatch):
    calls = []
    monkeypatch.setattr(paste, "_pbcopy", lambda t: calls.append(t))
    monkeypatch.setattr(paste, "_osascript", lambda s: calls.append(s))
    paste.paste_to_active_window("")
    assert calls == []

def test_press_enter_and_clear(monkeypatch):
    scripts = []
    monkeypatch.setattr(paste, "_osascript", lambda s: scripts.append(s))
    paste.press_enter(); paste.clear_input()
    assert "keystroke return" in scripts[0]
    assert "keystroke \"a\" using command down" in scripts[1] and "key code 51" in scripts[1]
```

- [ ] **Step 2: 运行,确认失败** — 模块缺失。

- [ ] **Step 3: 实现**
```python
# companion/codey/paste.py
"""把转写文本打进 macOS 当前聚焦窗口(pbcopy + osascript Cmd+V)。
首次运行需在 系统设置→隐私与安全性→辅助功能 给运行 Python 的终端授权。"""
import subprocess


def _pbcopy(text):
    p = subprocess.Popen(["pbcopy"], stdin=subprocess.PIPE)
    p.communicate(text.encode("utf-8"))


def _osascript(script):
    subprocess.run(["osascript", "-e", script], check=False)


def paste_to_active_window(text):
    if not text:
        return
    _pbcopy(text)
    _osascript('tell application "System Events" to keystroke "v" using command down')


def press_enter():
    _osascript('tell application "System Events" to keystroke return')


def clear_input():
    _osascript('tell application "System Events"\n'
               '  keystroke "a" using command down\n'
               '  key code 51\n'
               'end tell')


def get_active_app():
    try:
        out = subprocess.check_output(
            ["osascript", "-e",
             'tell application "System Events" to name of first application process whose frontmost is true'],
            stderr=subprocess.DEVNULL, timeout=2)
        return out.decode("utf-8").strip()
    except Exception:
        return "?"
```

- [ ] **Step 4: 通过** — `python3 -m pytest tests/test_paste.py -q`。
- [ ] **Step 5: Commit**
```bash
git add companion/codey/paste.py companion/tests/test_paste.py
git commit -m "feat(companion): paste — 粘贴/回车/清空(pbcopy+osascript,可注入)"
```

---

## Task 3: asr_stream 引擎无关后端 + 产出 final 粘贴 + submit/clear

**Files:** Modify `companion/asr_stream.py`; Create `companion/tests/test_asr_backend.py`

把现有 `handle()` 重构成**事件式后端接口**(先只实现 `SherpaBackend`,行为与现状一致),协议循环引擎无关;`listen:stop` 的 final 文本触发粘贴;新增 `{"type":"submit"}`/`{"type":"clear"}`。`handle()` 接受注入参数(make_backend / paster),便于单测。

后端接口:`start()`;`accept(pcm)->list[dict{text,final}]`;`stop()->list[dict{text,final}]`。

- [ ] **Step 1: 写失败测试(假后端 + 假 ws + 假 paster,验证协议/粘贴/submit/clear)**
```python
# companion/tests/test_asr_backend.py
import asyncio, json
import importlib

asr = importlib.import_module("asr_stream")   # companion/ 在 sys.path(pytest rootdir)

class FakeBackend:
    def __init__(self): self.started = False; self.fed = []
    async def start(self): self.started = True
    async def accept(self, pcm): self.fed.append(pcm); return [{"text": "partial", "final": False}]
    async def stop(self): return [{"text": "你好世界", "final": True}]

class FakeWS:
    def __init__(self, incoming): self._in = list(incoming); self.sent = []
    def __aiter__(self): self._it = iter(self._in); return self
    async def __anext__(self):
        try: return next(self._it)
        except StopIteration: raise StopAsyncIteration
    async def send(self, m): self.sent.append(m)

def run(coro): return asyncio.get_event_loop().run_until_complete(coro)

def test_final_triggers_paste_and_protocol():
    paster = {"paste": [], "enter": 0, "clear": 0}
    fake = FakeBackend()
    ws = FakeWS([
        json.dumps({"type": "hello"}),
        json.dumps({"type": "listen", "state": "start"}),
        b"\x00\x01" * 8,
        json.dumps({"type": "listen", "state": "stop"}),
    ])
    run(asr.handle(ws,
                   make_backend=lambda: fake,
                   paster=asr.Paster(
                       paste=lambda t: paster["paste"].append(t),
                       enter=lambda: paster.__setitem__("enter", paster["enter"] + 1),
                       clear=lambda: paster.__setitem__("clear", paster["clear"] + 1),
                       enabled=True, auto_enter=False)))
    assert fake.started and fake.fed                      # 收到 PCM
    # 发回了 hello + 至少一个 partial stt + final stt
    types = [json.loads(m)["type"] for m in ws.sent if isinstance(m, str)]
    assert "hello" in types and "stt" in types
    assert paster["paste"] == ["你好世界"]               # final 粘贴一次

def test_submit_and_clear_messages():
    paster = {"paste": [], "enter": 0, "clear": 0}
    ws = FakeWS([json.dumps({"type": "submit"}), json.dumps({"type": "clear"})])
    run(asr.handle(ws, make_backend=lambda: FakeBackend(),
                   paster=asr.Paster(
                       paste=lambda t: paster["paste"].append(t),
                       enter=lambda: paster.__setitem__("enter", paster["enter"] + 1),
                       clear=lambda: paster.__setitem__("clear", paster["clear"] + 1),
                       enabled=True, auto_enter=False)))
    assert paster["enter"] == 1 and paster["clear"] == 1

def test_auto_enter_after_paste():
    paster = {"paste": [], "enter": 0}
    ws = FakeWS([json.dumps({"type": "listen", "state": "stop"})])
    run(asr.handle(ws, make_backend=lambda: FakeBackend(),
                   paster=asr.Paster(
                       paste=lambda t: paster["paste"].append(t),
                       enter=lambda: paster.__setitem__("enter", paster["enter"] + 1),
                       clear=lambda: None, enabled=True, auto_enter=True)))
    assert paster["paste"] == ["你好世界"] and paster["enter"] == 1
```
> 注:`FakeWS` 未发 `listen:start`,`stop` 时 backend 仍可 `stop()`;实现需容忍"未 start 直接 stop"(创建一个 backend 兜底)。

- [ ] **Step 2: 运行,确认失败** — `cd companion && python3 -m pytest tests/test_asr_backend.py -q`。需要 `conftest.py` 把 companion/ 加进 sys.path(若 import asr_stream 失败,见 Step 3 附注)。

- [ ] **Step 3: 重构 asr_stream.py**

把 `asr_stream.py` 的 `handle` 段及之后替换为下面结构(保留顶部 import / `build_recognizer` / `recognizer` 不变;新增 `numpy as np` 已在)。新增 `Paster`、`SherpaBackend`、引擎无关 `handle`、`make_backend()` 工厂、`main()`:

```python
from collections import namedtuple

# 注入式副作用封装(默认绑定真实 paste;测试传假实现)
Paster = namedtuple("Paster", "paste enter clear enabled auto_enter")


def default_paster():
    from codey import paste as _p, envcfg as _c
    return Paster(paste=_p.paste_to_active_window, enter=_p.press_enter, clear=_p.clear_input,
                  enabled=_c.paste_enabled(), auto_enter=_c.auto_enter())


class SherpaBackend:
    """本地 sherpa 流式后端:行为与原 handle 等价(每块 partial + endpoint 段 final)。"""
    def __init__(self, rec):
        self.rec = rec
        self.stream = rec.create_stream()
        self.committed = ""

    async def start(self):
        self.stream = self.rec.create_stream()
        self.committed = ""

    def _decode(self):
        while self.rec.is_ready(self.stream):
            self.rec.decode_stream(self.stream)
        return self.rec.get_result(self.stream).strip()

    async def accept(self, pcm):
        samples = np.frombuffer(bytes(pcm), dtype=np.int16).astype(np.float32) / 32768.0
        self.stream.accept_waveform(16000, samples)
        partial = self._decode()
        full = (self.committed + partial).strip()
        out = [{"text": full, "final": False}]
        if self.rec.is_endpoint(self.stream):
            if partial:
                self.committed = full
                out.append({"text": self.committed, "final": True})
            self.rec.reset(self.stream)
        return out

    async def stop(self):
        self.stream.accept_waveform(16000, np.zeros(int(16000 * 0.4), dtype=np.float32))
        self.stream.input_finished()
        partial = self._decode()
        return [{"text": (self.committed + partial).strip(), "final": True}]


def make_backend():
    """按 env 选引擎。doubao 在 Task 5 接入;此处先只有 sherpa。"""
    from codey import envcfg
    if envcfg.select_engine() == "doubao":
        from codey.asr_doubao import DoubaoBackend          # Task 5
        return DoubaoBackend()
    return SherpaBackend(recognizer)


async def handle(ws, make_backend=make_backend, paster=None):
    if paster is None:
        paster = default_paster()
    backend = None
    last_sent = None

    async def send(text, final):
        nonlocal last_sent
        text = (text or "").strip()
        if final or text != last_sent:
            await ws.send(json.dumps({"type": "stt", "text": text, "final": final}, ensure_ascii=False))
            last_sent = text

    async def emit(events):
        """把后端事件发给手表;返回最后一条 final 文本(若有)。"""
        final_text = None
        for ev in events:
            await send(ev["text"], ev["final"])
            if ev["final"] and ev["text"]:
                final_text = ev["text"]
        return final_text

    try:
        async for msg in ws:
            if isinstance(msg, (bytes, bytearray)):
                if backend is None:
                    backend = make_backend(); await backend.start()
                await emit(await backend.accept(msg))
            else:
                try:
                    data = json.loads(msg)
                except Exception:
                    continue
                t = data.get("type")
                if t == "hello":
                    await ws.send(json.dumps({
                        "type": "hello", "transport": "websocket", "session_id": "codey",
                        "audio_params": {"format": "pcm", "sample_rate": 16000, "channels": 1},
                    }))
                elif t == "listen" and data.get("state") == "start":
                    backend = make_backend(); await backend.start()
                    last_sent = None
                elif t == "listen" and data.get("state") == "stop":
                    if backend is None:
                        backend = make_backend(); await backend.start()
                    final_text = await emit(await backend.stop())
                    backend = None
                    if final_text and paster.enabled:
                        paster.paste(final_text)
                        if paster.auto_enter:
                            paster.enter()
                elif t == "submit":
                    if paster.enabled:
                        paster.enter()
                elif t == "clear":
                    if paster.enabled:
                        paster.clear()
    except websockets.ConnectionClosed:
        pass


async def main():
    from codey import envcfg
    envcfg.load_dotenv()
    print(f"Codey streaming ASR -> ws://0.0.0.0:{PORT}  (engine={envcfg.select_engine()})", flush=True)
    async with websockets.serve(lambda ws: handle(ws), "0.0.0.0", PORT, max_size=None):
        await asyncio.Future()
```
> 删除原 `handle(ws)`(旧 sherpa 内联实现)与原 `main()`。`recognizer = build_recognizer()` 在 doubao-only 场景会白白加载本地模型 —— 优化留到 Task 5(改成惰性)。本任务保持 `recognizer` 模块级构建不变。

附:新增 `companion/tests/conftest.py`(若尚无)让 `import asr_stream` 生效:
```python
# companion/tests/conftest.py
import os, sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))  # companion/
```

- [ ] **Step 4: 通过 + 回归** — `cd companion && python3 -m pytest -q`(新后端测试 PASS,且原 31 测试不回归)。
- [ ] **Step 5: Commit**
```bash
git add companion/asr_stream.py companion/tests/test_asr_backend.py companion/tests/conftest.py
git commit -m "feat(companion): asr_stream 引擎无关后端 + final 粘贴 + submit/clear"
```

---

## Task 4: asr_doubao 分帧/解析(纯函数)

**Files:** Create `companion/codey/asr_doubao.py`(先只放纯函数), `companion/tests/test_asr_doubao.py`

移植 `meme/doubao_streaming.py` 的二进制协议为纯函数,先单测分帧/解析,不碰网络。

- [ ] **Step 1: 写失败测试**
```python
# companion/tests/test_asr_doubao.py
import gzip, json
from codey import asr_doubao as d

def test_build_frame_roundtrip_header_and_gzip():
    frame = d.build_frame(0x1, b'{"a":1}', last=False)
    assert frame[0] == (0x1 << 4) | 0x1                  # version|header_size
    assert frame[1] >> 4 == 0x1 and frame[1] & 0x0F == 0  # msg_type=1, flags=0
    body_len = int.from_bytes(frame[4:8], "big")
    body = frame[8:8 + body_len]
    assert gzip.decompress(body) == b'{"a":1}'

def test_last_audio_frame_sets_flag():
    frame = d.build_frame(0x2, b"", last=True)
    assert frame[1] & 0x0F == 0x2                          # last flag

def test_parse_normal_response_json():
    payload = json.dumps({"result": {"utterances": [{"text": "你好", "definite": True,
              "start_time": 0, "end_time": 10}]}}).encode()
    seq = (0).to_bytes(4, "big"); ln = len(payload).to_bytes(4, "big")
    data = bytes([0x10, 0x10, 0x11, 0x00]) + seq + ln + payload
    out = d.parse_response(data)
    assert "payload" in out and out["payload"]["result"]["utterances"][0]["text"] == "你好"

def test_parse_error_response():
    msg = json.dumps({"code": 1013}).encode()
    data = bytes([0x10, 0xF0, 0x11, 0x00]) + (1013).to_bytes(4, "big") + len(msg).to_bytes(4, "big") + msg
    out = d.parse_response(data)
    assert "error" in out and out["payload"]["code"] == 1013

def test_merge_utterances_dedups_finals():
    st = d.UtteranceMerger()
    st.feed([{"text": "你好", "definite": True, "start_time": 0, "end_time": 1}])
    st.feed([{"text": "你好", "definite": True, "start_time": 0, "end_time": 1}])  # dup
    st.feed([{"text": "世界", "definite": True, "start_time": 1, "end_time": 2}])
    st.feed([{"text": "ing", "definite": False}])
    assert st.final_text == "你好世界"
    assert st.text == "你好世界ing"
```

- [ ] **Step 2: 运行,确认失败** — 模块缺失。

- [ ] **Step 3: 实现(纯函数部分)**
```python
# companion/codey/asr_doubao.py
"""豆包 seed-asr 流式(sauc 2.0)客户端 + 文件回落。
二进制协议参考 meme/doubao_streaming.py(源自 xiaozhi-esp32-server)。"""
import gzip
import json


def build_header(message_type, flags=0):
    return bytearray([(0x1 << 4) | 0x1, (message_type << 4) | flags, (0x1 << 4) | 0x1, 0])


def build_frame(message_type, payload_bytes, last=False):
    flags = 0x2 if last else 0x0
    body = gzip.compress(payload_bytes)
    frame = build_header(message_type, flags)
    frame.extend(len(body).to_bytes(4, "big"))
    frame.extend(body)
    return bytes(frame)


def parse_response(data):
    if len(data) < 4:
        return {"error": f"short {len(data)}b"}
    message_type = data[1] >> 4
    if message_type == 0xF:
        code = int.from_bytes(data[4:8], "big")
        msg_len = int.from_bytes(data[8:12], "big")
        try:
            payload = json.loads(data[12:12 + msg_len].decode("utf-8"))
        except Exception:
            payload = {"raw": data[12:].hex()}
        return {"error": f"server_error code={code}", "payload": payload}
    if len(data) < 12:
        return {"error": "too short"}
    length = int.from_bytes(data[8:12], "big")
    body = data[12:12 + length] if 0 < length <= len(data) - 12 else data[8:]
    try:
        return {"payload": json.loads(body.decode("utf-8", errors="replace"))}
    except Exception as e:
        return {"error": f"parse json: {e}"}


class UtteranceMerger:
    """累积 definite 段为 final_text(去重),partial 段拼在尾巴上给 self.text。"""
    def __init__(self):
        self.final_text = ""
        self.text = ""
        self._seen = set()

    def feed(self, utterances):
        for u in utterances or []:
            txt = u.get("text")
            if u.get("definite") and txt:
                key = (u.get("start_time"), u.get("end_time"), txt)
                if key not in self._seen:
                    self._seen.add(key)
                    self.final_text += txt
        partials = [u["text"] for u in (utterances or []) if not u.get("definite") and u.get("text")]
        self.text = self.final_text + "".join(partials)
        return self.text
```

- [ ] **Step 4: 通过** — `python3 -m pytest tests/test_asr_doubao.py -q`。
- [ ] **Step 5: Commit**
```bash
git add companion/codey/asr_doubao.py companion/tests/test_asr_doubao.py
git commit -m "feat(companion): asr_doubao — 流式分帧/解析 + utterance 合并(纯函数)"
```

---

## Task 5: 豆包流式会话 + DoubaoBackend 接入引擎选择

**Files:** Modify `companion/codey/asr_doubao.py`(加 `StreamingASRSession` + `DoubaoBackend`);Modify `companion/asr_stream.py`(惰性构建 sherpa recognizer)

WS 会话移植自 meme(fire-and-forget 握手 + PCM 预缓冲 + reader 累积 final)。`DoubaoBackend` 适配 Task 3 的 `start/accept/stop` 接口。无 key/网络时不在单测覆盖(集成手测)。

- [ ] **Step 1: 实现 StreamingASRSession + DoubaoBackend(追加到 asr_doubao.py)**
```python
import asyncio
import os
import uuid
import websockets

WS_URL      = os.environ.get("DOUBAO_STREAMING_URL", "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async")
RESOURCE_ID = os.environ.get("DOUBAO_STREAMING_RESOURCE_ID", "volc.seedasr.sauc.duration")


def _cfg():
    return {"app_id": os.environ.get("DOUBAO_APP_ID", ""), "api_key": os.environ.get("DOUBAO_API_KEY", "")}


class StreamingASRSession:
    def __init__(self):
        self.ws = None
        self.req_id = str(uuid.uuid4())
        self.merger = UtteranceMerger()
        self._reader = None
        self._on_partial = None
        self._connect_task = None
        self._pending = []
        self._lock = asyncio.Lock()
        self._connect_error = None

    def set_partial_callback(self, cb):
        self._on_partial = cb

    def start_background(self):
        self._connect_task = asyncio.create_task(self._connect_and_drain())

    async def _connect_and_drain(self):
        try:
            await self._connect()
        except Exception as e:
            self._connect_error = e
            return
        async with self._lock:
            for pcm in (self._pending or []):
                try:
                    await self.ws.send(build_frame(0x2, pcm, last=False))
                except Exception:
                    break
            self._pending = None

    async def _connect(self):
        c = _cfg()
        headers = {
            "x-api-key": c["api_key"], "X-Api-Access-Key": c["api_key"],
            "X-Api-App-Key": c["app_id"], "X-Api-Resource-Id": RESOURCE_ID,
            "X-Api-Connect-Id": str(uuid.uuid4()),
        }
        self.ws = await websockets.connect(WS_URL, additional_headers=headers, max_size=None,
                                           ping_interval=None, ping_timeout=None,
                                           close_timeout=10, open_timeout=20)
        config = {
            "app": {"appid": c["app_id"], "token": c["api_key"]},
            "user": {"uid": "codey"},
            "request": {"reqid": self.req_id,
                        "workflow": "audio_in,resample,partition,vad,fe,decode,itn,nlu_punctuate",
                        "show_utterances": True, "result_type": "single",
                        "sequence": 1, "end_window_size": 200},
            "audio": {"format": "pcm", "codec": "pcm", "rate": 16000, "bits": 16,
                      "channel": 1, "sample_rate": 16000},
        }
        await self.ws.send(build_frame(0x1, json.dumps(config).encode("utf-8")))
        init = await asyncio.wait_for(self.ws.recv(), timeout=10)
        parsed = parse_response(init)
        if "error" in parsed:
            raise RuntimeError(f"doubao init failed: {parsed}")
        self._reader = asyncio.create_task(self._read_loop())

    async def send_audio(self, pcm):
        async with self._lock:
            if self._pending is not None:
                self._pending.append(pcm); return
            if self.ws:
                try:
                    await self.ws.send(build_frame(0x2, pcm, last=False))
                except Exception:
                    pass

    async def finalize(self, timeout_s=8.0):
        if self._connect_task and not self._connect_task.done():
            try:
                await asyncio.wait_for(asyncio.shield(self._connect_task), timeout=3.0)
            except Exception:
                pass
        if self._connect_error or not self.ws:
            await self.close(); return ""
        async with self._lock:
            for pcm in (self._pending or []):
                try: await self.ws.send(build_frame(0x2, pcm, last=False))
                except Exception: pass
            self._pending = None
            try: await self.ws.send(build_frame(0x2, b"", last=True))
            except Exception: pass
        try:
            if self._reader:
                await asyncio.wait_for(self._reader, timeout=timeout_s)
        except Exception:
            pass
        await self.close()
        return self.merger.final_text or self.merger.text

    async def close(self):
        if self._reader and not self._reader.done():
            self._reader.cancel()
            try: await self._reader
            except Exception: pass
        if self.ws:
            try: await self.ws.close()
            except Exception: pass
            self.ws = None

    async def _read_loop(self):
        try:
            while self.ws:
                try:
                    data = await self.ws.recv()
                except websockets.ConnectionClosed:
                    return
                parsed = parse_response(data)
                if "error" in parsed:
                    code = (parsed.get("payload") or {}).get("code")
                    if code == 1013:
                        continue
                    continue
                p = parsed.get("payload") or {}
                if "result" in p:
                    self.merger.feed(p["result"].get("utterances", []))
                    if self._on_partial:
                        try: self._on_partial(self.merger.text)
                        except Exception: pass
        except asyncio.CancelledError:
            pass
        except Exception:
            pass


class DoubaoBackend:
    """适配 asr_stream 后端接口。accept 返回当前累积 partial;stop 走 finalize(+文件回落见 Task 6)。"""
    def __init__(self):
        self.sess = StreamingASRSession()
        self._pcm = bytearray()       # 留存原始 PCM 供文件回落(Task 6)

    async def start(self):
        self.sess.start_background()

    async def accept(self, pcm):
        self._pcm += bytes(pcm)
        await self.sess.send_audio(bytes(pcm))
        return [{"text": self.sess.merger.text, "final": False}]

    async def stop(self):
        text = await self.sess.finalize()
        return [{"text": (text or "").strip(), "final": True}]
```

- [ ] **Step 2: asr_stream 惰性 recognizer**(避免 doubao-only 时白加载 173MB 模型)

把 `recognizer = build_recognizer()`(模块级)改为惰性:
```python
_recognizer = None
def get_recognizer():
    global _recognizer
    if _recognizer is None:
        _recognizer = build_recognizer()
    return _recognizer
```
并把 `make_backend()` 里 `SherpaBackend(recognizer)` 改成 `SherpaBackend(get_recognizer())`。

- [ ] **Step 3: 验证**
- `cd companion && python3 -m pytest -q`(全绿;doubao 网络路径不在单测)。
- 语法/导入自检:`python3 -c "import sys; sys.path.insert(0,'.'); import asr_stream; from codey.asr_doubao import DoubaoBackend; print('ok')"`(在 `companion/` 下;需已装 `websockets`)。Expected: `ok`。

- [ ] **Step 4: Commit**
```bash
git add companion/codey/asr_doubao.py companion/asr_stream.py
git commit -m "feat(companion): 豆包流式会话 + DoubaoBackend + sherpa 惰性加载"
```

---

## Task 6: 豆包文件回落(stdlib urllib)+ 多层回退

**Files:** Modify `companion/codey/asr_doubao.py`(加 `transcribe_wav_file` + `pcm_to_wav_bytes`);Modify `DoubaoBackend.stop`(流式空→文件回落);Create `companion/tests/test_asr_fallback.py`

移植 meme 文件 ASR,但用 **stdlib `urllib`+`asyncio.to_thread`** 替代 `aiohttp`(不引新依赖)。流式 finalize 返回空且有缓存 PCM 时,落回文件识别。

- [ ] **Step 1: 写失败测试(纯:WAV 封装 + 回退选择逻辑)**
```python
# companion/tests/test_asr_fallback.py
import struct, asyncio
from codey import asr_doubao as d

def test_pcm_to_wav_header():
    pcm = b"\x01\x00" * 16000        # 1s @16k mono int16
    wav = d.pcm_to_wav_bytes(pcm, rate=16000)
    assert wav[:4] == b"RIFF" and wav[8:12] == b"WAVE"
    # data chunk size == len(pcm)
    assert struct.unpack("<I", wav[40:44])[0] == len(pcm)

def test_doubao_backend_falls_back_when_stream_empty(monkeypatch):
    b = d.DoubaoBackend()
    b._pcm = bytearray(b"\x00\x00" * 16000)     # 1s captured
    async def fake_finalize(*a, **k): return ""           # 流式返回空
    monkeypatch.setattr(b.sess, "finalize", fake_finalize)
    async def fake_file(wav_bytes, **k): return {"text": "回落结果"}
    monkeypatch.setattr(d, "transcribe_wav_bytes", fake_file)
    out = asyncio.get_event_loop().run_until_complete(b.stop())
    assert out == [{"text": "回落结果", "final": True}]

def test_no_fallback_when_stream_has_text(monkeypatch):
    b = d.DoubaoBackend(); b._pcm = bytearray(b"\x00\x00" * 16000)
    async def fake_finalize(*a, **k): return "流式结果"
    monkeypatch.setattr(b.sess, "finalize", fake_finalize)
    called = {"n": 0}
    async def fake_file(*a, **k): called["n"] += 1; return {"text": "x"}
    monkeypatch.setattr(d, "transcribe_wav_bytes", fake_file)
    out = asyncio.get_event_loop().run_until_complete(b.stop())
    assert out == [{"text": "流式结果", "final": True}] and called["n"] == 0
```

- [ ] **Step 2: 运行,确认失败**。

- [ ] **Step 3: 实现**(追加到 asr_doubao.py;并改 `DoubaoBackend.stop`)
```python
import base64
import struct
import time
import urllib.request

SUBMIT_URL = "https://openspeech.bytedance.com/api/v3/auc/bigmodel/submit"
QUERY_URL  = "https://openspeech.bytedance.com/api/v3/auc/bigmodel/query"
FILE_RESOURCE_ID = os.environ.get("DOUBAO_RESOURCE_ID", "volc.seedasr.auc")


def pcm_to_wav_bytes(pcm, rate=16000, bits=16, channels=1):
    byte_rate = rate * channels * bits // 8
    block_align = channels * bits // 8
    data_len = len(pcm)
    return (b"RIFF" + struct.pack("<I", 36 + data_len) + b"WAVE"
            + b"fmt " + struct.pack("<IHHIIHH", 16, 1, channels, rate, byte_rate, block_align, bits)
            + b"data" + struct.pack("<I", data_len) + pcm)


def _post_json(url, headers, body):
    req = urllib.request.Request(url, data=json.dumps(body).encode("utf-8"),
                                 headers={**headers, "Content-Type": "application/json"}, method="POST")
    with urllib.request.urlopen(req, timeout=20) as r:
        return dict(r.headers), r.read()


async def transcribe_wav_bytes(wav_bytes, timeout_s=15.0):
    c = _cfg()
    if not c["api_key"]:
        return {"error": "DOUBAO_API_KEY empty"}
    import uuid as _uuid
    req_id = str(_uuid.uuid4())
    headers = {"x-api-key": c["api_key"], "X-Api-Resource-Id": FILE_RESOURCE_ID,
               "X-Api-Request-Id": req_id, "X-Api-Sequence": "-1"}
    body = {"user": {"uid": "codey"},
            "audio": {"data": base64.b64encode(wav_bytes).decode("ascii"),
                      "format": "wav", "rate": 16000, "bits": 16, "channel": 1},
            "request": {"model_name": "bigmodel", "enable_itn": True, "enable_punc": True}}

    def _submit():
        h, _ = _post_json(SUBMIT_URL, headers, body)
        return h.get("X-Api-Status-Code", "")
    status = await asyncio.to_thread(_submit)
    if status != "20000000":
        return {"error": f"submit failed: {status}"}

    deadline = time.time() + timeout_s
    delay = 0.1
    while time.time() < deadline:
        await asyncio.sleep(delay)

        def _query():
            h, raw = _post_json(QUERY_URL, headers, {})
            return h.get("X-Api-Status-Code", ""), raw
        st, raw = await asyncio.to_thread(_query)
        if st == "20000000":
            data = json.loads(raw.decode("utf-8"))
            return {"text": data.get("result", {}).get("text", "")}
        if st[:1] in ("4", "5"):
            return {"error": f"query failed: {st}"}
        delay = min(delay * 1.6, 0.5)
    return {"error": f"timeout {timeout_s}s"}
```
改 `DoubaoBackend.stop`:
```python
    async def stop(self):
        text = (await self.sess.finalize() or "").strip()
        if not text and len(self._pcm) > 16000:          # 流式空 + 有 >0.5s 音频 → 文件回落
            res = await transcribe_wav_bytes(pcm_to_wav_bytes(bytes(self._pcm)))
            text = (res.get("text") or "").strip()
        return [{"text": text, "final": True}]
```

- [ ] **Step 4: 通过** — `python3 -m pytest tests/test_asr_fallback.py -q`。
- [ ] **Step 5: Commit**
```bash
git add companion/codey/asr_doubao.py companion/tests/test_asr_fallback.py
git commit -m "feat(companion): 豆包文件回落(urllib)+ 流式空多层回退"
```

---

## Task 7: 可选 sherpa 本地标点(模型缺失 no-op)

**Files:** Create `companion/codey/asr_punct.py`, `companion/tests/test_asr_punct.py`;Modify `SherpaBackend.stop`(给 final 文本加标点)

豆包路径自带标点;本地 sherpa 默认裸输出。提供一个**可选**本地标点后处理:`models/` 下有 `*punct*` 模型则用 `sherpa_onnx.OfflinePunctuation` 加标点,**没有就原样返回**(零设置成本)。

- [ ] **Step 1: 写失败测试(只测 no-op 与接口,不依赖真模型)**
```python
# companion/tests/test_asr_punct.py
from codey import asr_punct

def test_punctuator_noop_when_no_model(monkeypatch, tmp_path):
    monkeypatch.setattr(asr_punct, "MODELS_DIR", str(tmp_path))   # 空目录
    p = asr_punct.Punctuator()
    assert p.available is False
    assert p.add("你好世界这是一句话") == "你好世界这是一句话"   # 原样

def test_punctuator_uses_engine_when_present(monkeypatch, tmp_path):
    monkeypatch.setattr(asr_punct, "MODELS_DIR", str(tmp_path))
    fake = lambda text: text + "。"
    p = asr_punct.Punctuator(engine=type("E", (), {"add_punctuation": staticmethod(fake)})())
    assert p.available is True
    assert p.add("你好") == "你好。"
```

- [ ] **Step 2: 运行,确认失败**。

- [ ] **Step 3: 实现**
```python
# companion/codey/asr_punct.py
"""可选:本地 sherpa-onnx 标点(models/ 下有 *punct* 模型才启用,否则 no-op)。"""
import glob
import os

MODELS_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "models")


def _find_punct_model():
    for d in sorted(glob.glob(os.path.join(MODELS_DIR, "*punct*"))):
        hits = glob.glob(os.path.join(d, "*.onnx"))
        if hits:
            return hits[0]
    return None


def _load_engine():
    model = _find_punct_model()
    if not model:
        return None
    try:
        import sherpa_onnx
        cfg = sherpa_onnx.OfflinePunctuationConfig(
            model=sherpa_onnx.OfflinePunctuationModelConfig(ct_transformer=model))
        return sherpa_onnx.OfflinePunctuation(cfg)
    except Exception:
        return None


class Punctuator:
    def __init__(self, engine="auto"):
        self._engine = _load_engine() if engine == "auto" else engine

    @property
    def available(self):
        return self._engine is not None

    def add(self, text):
        text = (text or "").strip()
        if not text or not self._engine:
            return text
        try:
            return self._engine.add_punctuation(text)
        except Exception:
            return text
```
改 `SherpaBackend`:`__init__` 里 `self.punct = None`;`make_backend()` 选 sherpa 时 `b = SherpaBackend(get_recognizer()); b.punct = _get_punctuator(); return b`,并加模块级惰性单例:
```python
_punct = None
def _get_punctuator():
    global _punct
    if _punct is None:
        from codey.asr_punct import Punctuator
        _punct = Punctuator()
    return _punct
```
`SherpaBackend.stop` 返回前:`text = self.punct.add(text) if self.punct else text`。

- [ ] **Step 4: 通过** — `python3 -m pytest tests/test_asr_punct.py -q` + 全量回归。
- [ ] **Step 5: Commit**
```bash
git add companion/codey/asr_punct.py companion/tests/test_asr_punct.py companion/asr_stream.py
git commit -m "feat(companion): 可选 sherpa 本地标点(缺模型 no-op)"
```

---

## Task 8: chime — 完成检测 + /codey/state 暴露

**Files:** Create `companion/codey/chime.py`, `companion/tests/test_chime.py`;Modify `companion/codey/state.py`(build_state 输出 chime);Modify `companion/codey/server.py`(_refresh_loop 维护 chime 状态)

完成检测:某会话 status 由 `executing|thinking` 跃迁到 `waiting|done` = 一次"任务完成",产出 `{"agent":"claude"|"codex","seq":N}`,固件按 `seq` 变化播放一次(Plan B)。

- [ ] **Step 1: 写失败测试**
```python
# companion/tests/test_chime.py
from codey import chime

def test_detect_completion_executing_to_waiting():
    prev = {"claude": [{"id": "a", "status": "executing"}], "codex": []}
    cur  = {"claude": [{"id": "a", "status": "waiting"}],   "codex": []}
    st = chime.ChimeState()
    st.update(prev_cache=prev, cur_cache=cur)
    assert st.event["agent"] == "claude" and st.event["seq"] == 1

def test_no_event_when_no_transition():
    cache = {"claude": [{"id": "a", "status": "executing"}], "codex": []}
    st = chime.ChimeState()
    st.update(prev_cache=cache, cur_cache=cache)
    assert st.event is None and st.seq == 0

def test_seq_increments_per_completion():
    st = chime.ChimeState()
    st.update(prev_cache={"claude": [{"id": "a", "status": "thinking"}], "codex": []},
              cur_cache={"claude": [{"id": "a", "status": "done"}], "codex": []})
    st.update(prev_cache={"codex": [{"id": "b", "status": "executing"}], "claude": []},
              cur_cache={"codex": [{"id": "b", "status": "waiting"}], "claude": []})
    assert st.seq == 2 and st.event["agent"] == "codex"

def test_new_or_vanished_session_no_false_chime():
    st = chime.ChimeState()
    st.update(prev_cache={"claude": [], "codex": []},
              cur_cache={"claude": [{"id": "a", "status": "waiting"}], "codex": []})  # 新出现的 waiting
    assert st.event is None and st.seq == 0
```

- [ ] **Step 2: 运行,确认失败**。

- [ ] **Step 3: 实现**
```python
# companion/codey/chime.py
"""任务完成检测:会话 status 由活跃(executing/thinking)跃迁到 idle(waiting/done)记一次完成。"""

_ACTIVE = {"executing", "thinking"}
_IDLE = {"waiting", "done"}


def _by_id(sessions):
    return {s.get("id"): s.get("status") for s in (sessions or []) if s.get("id")}


def detect(prev_cache, cur_cache):
    """返回本 tick 完成的 agent 列表(按 claude/codex 顺序)。"""
    done = []
    for agent in ("claude", "codex"):
        prev = _by_id((prev_cache or {}).get(agent))
        cur = _by_id((cur_cache or {}).get(agent))
        for sid, status in cur.items():
            if sid in prev and prev[sid] in _ACTIVE and status in _IDLE:
                done.append(agent)
                break
    return done


class ChimeState:
    def __init__(self):
        self.seq = 0
        self.event = None        # {"agent":..,"seq":..} or None(本 tick 无新完成)

    def update(self, prev_cache, cur_cache):
        agents = detect(prev_cache, cur_cache)
        self.event = None
        for agent in agents:
            self.seq += 1
            self.event = {"agent": agent, "seq": self.seq}     # 多个同 tick 取最后一个;seq 已各自 +1
        return self.event
```

- [ ] **Step 4: 接入 state.py**(build_state 增加 chime 字段)

`build_state` 签名加可选参数,顶层 JSON 加 `chime`:
```python
def build_state(session_cache, tok_rate, chime=None):
    ...
    return {
        "ts": ...,
        "chime": chime,                 # {"agent":..,"seq":N} 或 None;固件按 seq 变化播放一次
        "providers": [...],
    }
```
(把 `chime` 放在现有顶层 dict 里,紧挨 `ts`。)

- [ ] **Step 5: 接入 server.py 的 _refresh_loop**

`App.__init__` 加 `self.chime = ChimeState()`(`from .chime import ChimeState`);`_refresh_loop` 在用新 cache 覆盖前先检测:
```python
def _refresh_loop(self):
    while True:
        try:
            cache = collect.collect_sessions()
            with self.lock:
                self.chime.update(prev_cache=self.session_cache, cur_cache=cache)  # 先比对再覆盖
                self.session_cache = cache
                # ... tok_rate 更新不变 ...
        except Exception:
            pass
        time.sleep(REFRESH_MS / 1000)
```
`App.state()`:`return build_state(cache, tok, self.chime.event)`(在 lock 内取 `self.chime.event`)。

- [ ] **Step 6: 通过 + 回归** — `cd companion && python3 -m pytest -q`(chime 测试 PASS,原测试不回归;若 state 测试断言顶层 keys,更新它接受新增 `chime`)。

- [ ] **Step 7: Commit**
```bash
git add companion/codey/chime.py companion/tests/test_chime.py companion/codey/state.py companion/codey/server.py
git commit -m "feat(companion): chime 完成检测 + /codey/state 暴露(供固件播放)"
```

---

## Task 9: .env.example + 文档 + 运行说明

**Files:** Create `companion/.env.example`;Modify `readme.md` / `readme_zh.md`(语音桥与引擎切换说明)

- [ ] **Step 1: .env.example**
```bash
# companion/.env.example — 复制为 companion/.env 并填值(asr_stream 启动时自动加载)
# ASR 引擎: sherpa(本地默认) | doubao | auto(有 DOUBAO_API_KEY 用豆包否则 sherpa)
CODEY_ASR_ENGINE=auto
# 粘贴桥: 1=转写后粘贴到当前窗口(默认), 0=只回传手表显示
CODEY_PASTE=1
# 粘贴后自动回车提交(默认 0;固件 BtnB 也能发 submit)
CODEY_PASTE_AUTO_ENTER=0
# 豆包(火山引擎)凭据 —— 仅 doubao 引擎需要。volcengine.com 开通"流式语音识别大模型2.0"
DOUBAO_API_KEY=
DOUBAO_APP_ID=
# 可选覆盖(一般不用改):
# DOUBAO_STREAMING_RESOURCE_ID=volc.seedasr.sauc.duration
# DOUBAO_RESOURCE_ID=volc.seedasr.auc
```
并确认 `companion/.gitignore`(或仓库根 `.gitignore`)忽略 `.env`(不提交密钥)。若没有则加一行 `companion/.env`。

- [ ] **Step 2: README 增补**(`readme.md` 与 `readme_zh.md` 各加一节"语音输入桥 / Voice input bridge"):
  - 启动:`cd companion && python3 asr_stream.py`(自动 `load_dotenv`);引擎/粘贴看 `.env`。
  - 首次需在 系统设置→隐私与安全性→辅助功能 给终端授权(否则 Cmd+V 无效)。
  - 行为:松手出字→自动粘贴到当前聚焦窗口;固件 BtnB 多击发 `submit`(回车)/`clear`(清空)(Plan B)。
  - chime:`/codey/state` 增 `chime:{agent,seq}` 字段,完成提示音由固件播放(Plan B)。

- [ ] **Step 3: 全量回归 + 冒烟**
- `cd companion && python3 -m pytest -q` → 全绿。
- 冒烟(本地 sherpa,无需 key):`python3 asr_stream.py` 起服务,用现有手表/或 `wscat` 连 `ws://127.0.0.1:8788` 发 hello→listen start→PCM→listen stop,确认回 `stt` 且(在 Mac 上)文本被粘贴进前台输入框。无设备时至少确认服务启动打印 `engine=sherpa` 不报错。

- [ ] **Step 4: Commit**
```bash
git add companion/.env.example readme.md readme_zh.md .gitignore
git commit -m "docs(companion): 语音输入桥 .env.example + README(引擎切换/粘贴授权/chime)"
```

---

## 完成判据(Plan A)
- `cd companion && python3 -m pytest -q` 全绿(新增 envcfg/paste/asr_doubao/asr_punct/chime/asr_backend/asr_fallback 测试 + 原 31 不回归)。
- 本地 sherpa 引擎下:松手出字 → **自动粘贴**进当前 Mac 窗口;`{"type":"submit"|"clear"}` → 回车/清空(固件 Plan B 触发,companion 已就绪)。
- 设 `DOUBAO_API_KEY` + `CODEY_ASR_ENGINE=doubao` 后走豆包流式(自带标点/ITN),流式空回落文件 ASR;断网/无 key `auto` 回落本地 sherpa。
- `/codey/state` 含 `chime:{agent,seq}`,完成时 seq 自增(固件 Plan B 播放)。
- **固件零改动**即可享受"语音→粘贴"主价值。

## 已知简化 / 留待
- **豆包活路径无单测**(需 key/网络):分帧/解析/合并/WAV 封装/回退选择已纯函数覆盖;WS 活路径靠集成手测。
- **sherpa 标点需另下模型**(`models/*punct*`);没下就 no-op,不阻塞。
- **chime 仅 companion 侧检测 + state 字段**;播放与"BtnB 多击发 submit/clear"在 **Plan B(固件)**。
- **粘贴是 Mac 全局行为**(打进任意前台窗口),依赖辅助功能授权;焦点不在输入框时会粘到别处——与 meme 同款权衡。

## 后续(Plan B,固件,另写)
- BtnB 多击区分 语音(按住)/ clear(双击);submit(回车)放另一手势;发 `{"type":"submit"|"clear"}`。
- 读 `/codey/state.chime.seq`,变化时用 M5.Speaker 播 880Hz(Claude)/660Hz(Codex);注意 Speaker 与 Mic 共用 ES8311,需在录音/播放间切换。
