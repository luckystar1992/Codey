# Codey Web 管理台:配置 / 欢迎 / 教学 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`).

**Goal:** 把 companion 的 Web 管理台从单页扩成多标签(欢迎 / 教学 / 配置 / 设备镜像 / 识别历史),新增一个**可持久化的运行期配置**(引擎、粘贴、刷新、豆包凭据、以及**设备页面显示哪些列/字段**),并提供项目介绍 + GitHub 点赞引导 + 首次配置小教学。

**Architecture:**
- 配置存 `companion/data/config.json`(gitignore),分层覆盖:**config.json > .env(os.environ) > 内置默认**。新增 `codey/config.py` 作唯一读写入口(带锁)。
- `envcfg` 的 `select_engine/paste_enabled/auto_enter` 改为走 `config`;`server._refresh_loop` 的间隔改读 `config["refresh_ms"]`;`build_state` 增 `display` 字段下发给设备。
- HTTP 增 `GET/POST /codey/config`(读当前值+schema / 改并落盘;密钥遮罩;校验)。
- `web/admin.html` 改为多标签单页:欢迎 / 教学 / 配置(绑定 /codey/config)/ 设备镜像(原 iframe)/ 识别历史(原轮询)。
- 固件 `codey_dash.ino` 读 `state.display`,**动态**渲染列表列(Status/Model/Ctx/Tokens/Memory/Turn)与详情字段(需刷机,USB 恢复后)。

**Tech Stack:** Python 标准库(config + 路由)、原生 HTML/CSS/JS(无框架,沿用 admin.html 风格)、Arduino/M5GFX(固件)。

---

## 配置 Schema(契约,所有任务以此为准)

`codey/config.py` 的 `DEFAULTS`(键 → 默认值);UI 与 state 都按这套键:

```python
DEFAULTS = {
    "asr_engine": "auto",            # auto | sherpa | doubao
    "paste": True,                   # 转写后粘贴到聚焦窗口
    "paste_auto_enter": False,       # 粘贴后回车
    "doubao_api_key": "",            # 密钥(UI 遮罩;空=回退 .env)
    "doubao_app_id": "",
    "refresh_ms": 2000,              # usage 后台刷新间隔(ms),范围 [500, 60000]
    "display": {                     # 设备页面显示项(下发到 /codey/state.display)
        "columns": {"status": True, "model": True, "ctx": True,
                     "tokens": True, "memory": True, "turn": True},
        "providers": {"claude": True, "codex": True},   # 启用哪一端的页面
    },
}
SECRET_KEYS = ("doubao_api_key",)    # GET 时遮罩为 "" + has_<key>:true
RESTART_KEYS = ("ports",)            # 只读展示(端口改需重启)
```

分层取值:`get(key)` = config.json 有则用;否则映射到对应 env(`asr_engine`→`CODEY_ASR_ENGINE`、`paste`→`CODEY_PASTE`、`paste_auto_enter`→`CODEY_PASTE_AUTO_ENTER`、`doubao_*`→`DOUBAO_*`)有则用;否则 `DEFAULTS`。`refresh_ms`/`display` 无 env 映射,直接 config 或默认。

---

### Task 1: `codey/config.py` —— 配置存储(分层 + 校验 + 原子写)

**Files:** Create `companion/codey/config.py`; Test `companion/tests/test_config.py`

- [ ] **Step 1: 写失败测试**(默认值、env 回退、config 覆盖、bool/int 强转、display 合并、原子保存往返)

```python
import json, os
from codey import config as cfg

def test_defaults_when_no_file_no_env(tmp_path, monkeypatch):
    monkeypatch.setattr(cfg, "CONFIG_PATH", str(tmp_path / "config.json"))
    for k in ("CODEY_ASR_ENGINE", "CODEY_PASTE", "DOUBAO_API_KEY"):
        monkeypatch.delenv(k, raising=False)
    assert cfg.get("asr_engine") == "auto"
    assert cfg.get("paste") is True
    assert cfg.get("refresh_ms") == 2000

def test_env_fallback(tmp_path, monkeypatch):
    monkeypatch.setattr(cfg, "CONFIG_PATH", str(tmp_path / "config.json"))
    monkeypatch.setenv("CODEY_ASR_ENGINE", "sherpa")
    monkeypatch.setenv("CODEY_PASTE", "0")
    assert cfg.get("asr_engine") == "sherpa"
    assert cfg.get("paste") is False

def test_file_overrides_env(tmp_path, monkeypatch):
    p = tmp_path / "config.json"; p.write_text(json.dumps({"asr_engine": "doubao"}))
    monkeypatch.setattr(cfg, "CONFIG_PATH", str(p))
    monkeypatch.setenv("CODEY_ASR_ENGINE", "sherpa")
    assert cfg.get("asr_engine") == "doubao"

def test_save_validates_and_roundtrips(tmp_path, monkeypatch):
    p = tmp_path / "config.json"; monkeypatch.setattr(cfg, "CONFIG_PATH", str(p))
    cfg.save({"asr_engine": "doubao", "refresh_ms": 99999, "paste": "yes",
              "display": {"columns": {"memory": False}}})
    assert cfg.get("asr_engine") == "doubao"
    assert cfg.get("refresh_ms") == 60000          # clamp 上界
    assert cfg.get("paste") is True                # "yes" -> True
    d = cfg.get("display")
    assert d["columns"]["memory"] is False and d["columns"]["status"] is True   # 深合并保留其余列

def test_save_rejects_bad_engine(tmp_path, monkeypatch):
    monkeypatch.setattr(cfg, "CONFIG_PATH", str(tmp_path / "config.json"))
    cfg.save({"asr_engine": "nope"})
    assert cfg.get("asr_engine") == "auto"          # 非法枚举被丢弃/回默认
```

Run: `cd companion && python3 -m pytest tests/test_config.py -q` → FAIL.

- [ ] **Step 2: 实现 `config.py`**(纯标准库;`threading.Lock`;`load()` 每次读文件或带 mtime 缓存;`get`/`all`/`save`;`save` 做枚举/bool/int-clamp/深合并 display 校验;原子写 tmp+rename;不可变:整体替换内存缓存,不就地改)。`all()` 返回合并后全量(给 GET 用,未遮罩)。

- [ ] **Step 3: 测试转绿** → `python3 -m pytest tests/test_config.py -q` PASS。

- [ ] **Step 4: Commit** `feat(companion): config.py 分层配置(config.json>env>默认)+ 校验/原子写`

---

### Task 2: 接线 —— engine/paste/refresh 走 config + state.display 下发

**Files:** Modify `companion/codey/envcfg.py`, `companion/codey/server.py`, `companion/codey/state.py`; Tests 改 `test_envcfg.py`/`test_state.py`

- [ ] **Step 1: envcfg 走 config**(`select_engine/paste_enabled/auto_enter` 改为读 `config.get(...)`;保留 `env=` 参数签名兼容老测试——传入 env 时仍按 env 算,不传时走 config。写测试覆盖 config 覆盖 env 的情形)。
- [ ] **Step 2: refresh 读 config**:`server._refresh_loop` 的 `time.sleep(REFRESH_MS/1000)` 改 `time.sleep(max(0.5, config.get("refresh_ms")/1000))`;删模块常量或保留为默认来源。
- [ ] **Step 3: build_state 增 display**:`build_state(..., display=None)` 在返回 dict 加 `"display": display or {}`;`App.state()` 传 `display=config.get("display")`。加断言 `/codey/state` 含 `display.columns`。
- [ ] **Step 4: 全测试绿** `python3 -m pytest -q`。
- [ ] **Step 5: Commit** `feat(companion): 引擎/粘贴/刷新走 config;/codey/state 下发 display`

---

### Task 3: `GET/POST /codey/config` 路由

**Files:** Modify `companion/codey/server.py`; Test `companion/tests/test_server_routes.py`

- [ ] **Step 1: 失败测试**:`GET /codey/config` 返回 `{values, schema}`,`values.doubao_api_key == ""` 且 `values.has_doubao_api_key` 反映是否已设(遮罩);`POST /codey/config` body `{"asr_engine":"sherpa"}` → 200 + 落盘后 `config.get` 生效;非法值被校验丢弃。
- [ ] **Step 2: 实现**:`do_GET` 加 `/codey/config` → `{"values": masked(config.all()), "schema": SCHEMA}`(SCHEMA 给 UI:每项 type/options/label/restart 标记);`do_POST` 加 `/codey/config` → 读 body JSON、`config.save(body)`、回 `{"ok":true,"values":masked(all())}`;body 超限/坏 JSON → 400。遮罩:`SECRET_KEYS` 的值替换为 `""` 并加 `has_<k>` 布尔。
- [ ] **Step 3: 测试绿**。
- [ ] **Step 4: Commit** `feat(companion): GET/POST /codey/config(读写配置, 密钥遮罩, 校验)`

---

### Task 4: Web 多标签骨架 + 配置 tab

**Files:** Modify `companion/web/admin.html`

- [ ] **Step 1:** 顶部加标签导航 `欢迎 | 教学 | 配置 | 设备镜像 | 识别历史`(纯 JS 切换 section,无框架,沿用现有暗色 CSS 变量)。设备镜像=原 `/sim?live=1` iframe;识别历史=原轮询逻辑(移进对应 section,切到该 tab 才轮询)。
- [ ] **Step 2: 配置 tab:** 打开时 `GET /codey/config`,按 `schema` 渲染表单(select=引擎、checkbox=paste/auto_enter/各显示列/各端、number=refresh_ms、password=豆包密钥占位「已设置/未设置」、端口只读+重启提示);「保存」`POST /codey/config` 改动项,显示成功/校验错误;`display.columns`/`display.providers` 用勾选组。
- [ ] **Step 3:** 本地起服务核对:`./deploy.sh start --bg`,浏览器开 `/`,切各 tab,改一项保存后刷新仍在(落盘),`curl /codey/config` 对上。
- [ ] **Step 4: Commit** `feat(web): 管理台多标签 + 配置 tab(绑定 /codey/config)`

---

### Task 5: 欢迎 tab(项目介绍 + GitHub 点赞引导)

**Files:** Modify `companion/web/admin.html`

- [ ] **Step 1:** 欢迎 section:一句话项目简介 + 三四个亮点(Claude/Codex 用量监视、设备端语音输入、Web 管理台、ngrok 远程)+ 醒目「⭐ 在 GitHub 上 Star」按钮链到 `https://github.com/luckystar1992/Codey`(新窗口)+ 一行致谢 meme 链接。文案中文,排版与暗色风格一致。
- [ ] **Step 2: Commit** `feat(web): 欢迎页(项目介绍 + GitHub Star 引导)`

---

### Task 6: 教学 tab(首次配置小教程)

**Files:** Modify `companion/web/admin.html`

- [ ] **Step 1:** 教学 section:分步骤(带序号卡片):① Mac 起服务(`cd companion && ./deploy.sh start --bg`)② 授权辅助功能(系统设置→隐私与安全性→辅助功能,否则粘贴失效)③ 下载 sherpa 模型(指向 README ASR 模型一节)④ 手表配网(`Codey-Setup` 热点→`192.168.4.1`→选同一局域网)⑤(可选)远程 ngrok 一句话+指向 README。每步简短可照做;关键命令用 `<code>`。
- [ ] **Step 2: Commit** `docs(web): 教学页(设备 + macOS 首次配置分步引导)`

---

### Task 7: 固件 —— 按 state.display 动态渲染列表列 / 详情字段

**Files:** Modify `sketches/codey_dash/codey_dash.ino`(纯列选择逻辑可抽到 `codey_ui.h` + host 测试)

- [ ] **Step 1:** `fetchState()` 解析 `doc["display"]`:读 `columns.{status,model,ctx,tokens,memory,turn}` 与 `providers.{claude,codex}` 到全局(缺省全 true,向后兼容旧 state)。
- [ ] **Step 2:** `renderListPage` 改为**遍历启用的列**动态算列宽/列头/单元格(把「启用列集合 → 列布局」纯逻辑抽到 `codey_ui.h`,host 测试:给定勾选返回列数/顺序)。详情页按 columns 显隐字段。providers 关掉的一端跳过其页面/列表。
- [ ] **Step 3:** `./scripts/build.sh sketches/codey_dash` 编译通过(记录 flash %);`bash sketches/codey_dash/test/run_tests.sh` ALL PASS。
- [ ] **Step 4: Commit** `feat(firmware): 列表列/详情字段按 state.display 动态渲染`

---

### Task 8: README + 验证

**Files:** Modify `readme.md`/`readme_zh.md`;端到端

- [ ] **Step 1:** README 在「Companion 服务/Web 管理台」一节补:多标签(欢迎/教学/配置)、`/codey/config`、config.json 落盘位置、设备显示项可配。
- [ ] **Step 2:** 端到端:起服务→ `curl /codey/config` →改 display.columns.memory=false→ `curl /codey/state` 看 `display` 下发→(固件刷机后)设备表格少 Memory 列。`pytest -q` 全绿、host ALL PASS。
- [ ] **Step 3: Commit** `docs: README 增 Web 管理台配置/欢迎/教学`

---

## Self-Review

- **覆盖:** 配置 tab(Task 1-4,含 refresh 与所有可参数化项)、欢迎(5)、教学(6)、设备显示项可配(2 下发 + 7 固件渲染)——全覆盖用户三点。
- **分层/不可变:** config.json>env>默认;内存缓存整体替换;原子写。
- **安全:** 密钥遮罩(GET 不回明文);config.json 在 data/(gitignore);POST 体积/JSON 校验;config 路由在 LAN 或 ngrok basic-auth 之后。
- **向后兼容:** display 缺省全开(旧固件/旧 state 不受影响);envcfg 保留 env= 签名;无 config.json 时纯走 .env(老部署不变)。
- **风险:** ① 固件动态列宽布局是 Task 7 难点(列少时居中/对齐重算)——抽纯逻辑 + host 测试降风险;② 设备 USB 现断,Task 7 仅编译,刷机/真机验证顺延;③ 引擎/粘贴改动在「下一次 WS 连接/下一句」生效(default_paster/make_backend 时机),非即时,UI 提示一下。
