# 固件会话监视页(Plan 2)Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 `sketches/codey_dash/codey_dash.ino` 从「两端账号额度 + 表盘」三页改造成消费 `/codey/state` 新契约(`sessions[]`/`active_count`/`agg`)的**实时 agent 会话监视器**:仪表盘首页 + Claude/Codex 会话列表页 + 单会话详情页,并新增 `M5.Touch` 滑动滚动 / 点按进详情。

**Architecture:** 纯逻辑(UTF-8 按码点缩略、数字/时长格式化、model 短名、status 映射)抽进 `codey_ui.h`,用 `g++` 在主机上 TDD(红→绿);会话/provider 数据结构进 `session_store.h`;绘制(三页 render)、触摸手势状态机、JSON 解析改在 `.ino` 内,以 `./scripts/build.sh` 编译通过 + 真机视觉核对为验收(本项目固件历来无 `.ino` 单测,沿用此模式)。设计以 `sim/codey-sim.html` 为像素级蓝本,坐标按 380→466 缩放(系数 ≈1.226)取整,最终在真机微调。

**Tech Stack:** Arduino C++ / M5Unified(`M5.Display` / `M5Canvas` / `M5.Touch`)/ ArduinoJson;主机侧 `g++ -std=c++17`(仅 `codey_ui.h` 纯函数)。

**参考:**
- 设计 spec:`docs/superpowers/specs/2026-06-03-agent-session-monitor-design.md`(§3 页面、§4 详情、§5 交互)
- 视觉/交互蓝本:`sim/codey-sim.html`(`renderDashboard`/`renderList`/`renderDetail`/触摸手势)
- 契约真值:`companion/codey/state.py` + `companion/codey/build_session.py`(已实现并 31 测试全绿)

---

## 契约速查(固件消费的字段,勿臆测)

```jsonc
{ "ts": 1780000000, "stale": false,
  "providers": [
    { "id": "claude", "name": "Claude Code",
      "session": { "used_pct": 38, "reset_epoch": 0 },
      "weekly":  { "used_pct": 61, "reset_epoch": 0 },   // ← 边缘弧用 weekly.used_pct
      "model": "Opus 4.8",                                // provider 级最常用模型(可能为 null)
      "active_count": 3,
      "agg": { "dirty_repos": 4, "tokens_per_min": 94000 },
      "sessions": [
        { "id": "<sid>", "name": "Codey",
          "status": "executing",            // executing|thinking|waiting|done
          "model": "claude-opus-4-8",       // 原始名 → 固件 modelShort() 美化
          "context_pct": 47, "context_tokens": 94000, "context_window": 200000,
          "tokens_total": 1234567, "turn": 23,
          "git": { "branch": "main", "added": 3, "modified": 12 },
          "current_task": "Edit companion/server.js",
          "subagents": 2, "ports": [3000, 5173],
          "started_at": 1780000000, "effort": "high" } ] },
    { "id": "codex", "name": "Codex", "...": "同构(绿)" }
  ] }
}
```
- 服务端**已按** active(executing/thinking)优先、再 context_pct 降序排好 `sessions[]`;固件**不必重排单端**,仅仪表盘跨两端合并时再比较。
- 新契约**没有** `lunar` / `pending_reviews`(旧固件用过,本计划一并删除)。

---

## 文件结构

```
sketches/codey_dash/
  codey_ui.h         # 新建:纯函数(无 Arduino 依赖,主机可测)— status/截断/格式化/model 短名
  session_store.h    # 新建:Sess / Prov 数据结构 + 容量常量
  codey_dash.ino     # 改造:删表盘/旧表计;解析填充 Prov.sess[];三页 render;触摸手势
  wifi_store.h       # 不动
  test/
    codey_ui_test.cpp  # 新建:codey_ui.h 的主机单测(g++ + assert)
    run_tests.sh       # 新建:编译并运行主机单测
```

约定:`codey_ui.h` 只包含 `<string.h>/<stdio.h>/<stdint.h>`、全部为 `static inline` C 风格(`const char*` 入、`char* out` 出),不依赖 `Arduino.h`,故主机 `g++` 可直接编译测试。

---

## Task 0: 主机测试夹具(g++ + assert)

**Files:**
- Create: `sketches/codey_dash/codey_ui.h`(占位,仅保证可编译)
- Create: `sketches/codey_dash/test/codey_ui_test.cpp`
- Create: `sketches/codey_dash/test/run_tests.sh`

- [ ] **Step 1: 建占位头**

```cpp
// sketches/codey_dash/codey_ui.h
#pragma once
#include <stdint.h>
#include <string.h>
#include <stdio.h>
// (helpers added in Task 1)
```

- [ ] **Step 2: 写最小测试 + runner**

```cpp
// sketches/codey_dash/test/codey_ui_test.cpp
#include <cassert>
#include <cstdio>
#include "../codey_ui.h"

int main() {
  printf("codey_ui tests: OK (placeholder)\n");
  return 0;
}
```

```bash
# sketches/codey_dash/test/run_tests.sh
#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
g++ -std=c++17 -Wall -Wextra -o /tmp/codey_ui_test codey_ui_test.cpp
/tmp/codey_ui_test
```

- [ ] **Step 3: 运行,确认通过**

Run: `bash sketches/codey_dash/test/run_tests.sh`
Expected: 打印 `codey_ui tests: OK (placeholder)`,退出码 0。

- [ ] **Step 4: Commit**

```bash
chmod +x sketches/codey_dash/test/run_tests.sh
git add sketches/codey_dash/codey_ui.h sketches/codey_dash/test/
git commit -m "test(firmware): codey_ui 主机测试夹具(g++)"
```

---

## Task 1: codey_ui.h — 纯函数(status/截断/格式化/model 短名)

**Files:**
- Modify: `sketches/codey_dash/codey_ui.h`
- Modify: `sketches/codey_dash/test/codey_ui_test.cpp`

- [ ] **Step 1: 写失败测试**

替换 `test/codey_ui_test.cpp` 全文为:

```cpp
// sketches/codey_dash/test/codey_ui_test.cpp
#include <cassert>
#include <cstring>
#include <cstdio>
#include "../codey_ui.h"

static const char* S(char* b, size_t n, void(*f)(const char*, char*, size_t), const char* in) {
  f(in, b, n); return b;
}

int main() {
  char b[64];

  // ---- statusFromStr / statusWord / statusRank ----
  assert(statusFromStr("executing") == ST_EXECUTING);
  assert(statusFromStr("thinking")  == ST_THINKING);
  assert(statusFromStr("waiting")   == ST_WAITING);
  assert(statusFromStr("done")      == ST_DONE);
  assert(statusFromStr(nullptr)     == ST_WAITING);
  assert(!strcmp(statusWord(ST_EXECUTING), "EXECUTING"));
  assert(!strcmp(statusWord(ST_DONE), "DONE"));
  assert(statusRank(ST_EXECUTING) == 0 && statusRank(ST_THINKING) == 1 && statusRank(ST_WAITING) == 2 && statusRank(ST_DONE) == 2);

  // ---- cpLen: UTF-8 码点计数 ----
  assert(cpLen("abc") == 3);
  assert(cpLen("项目名称") == 4);
  assert(cpLen("") == 0);

  // ---- truncCp: 按码点截断,超长加 …(U+2026) ----
  truncCp("abcdef", 4, b, sizeof(b)); assert(!strcmp(b, "abc\xE2\x80\xA6"));
  truncCp("短", 4, b, sizeof(b));      assert(!strcmp(b, "短"));
  truncCp("项目名称很长", 4, b, sizeof(b)); assert(!strcmp(b, "项目名\xE2\x80\xA6"));
  truncCp("abc", 3, b, sizeof(b));     assert(!strcmp(b, "abc"));

  // ---- fmtK ----
  fmtK(950, b, sizeof(b));     assert(!strcmp(b, "950"));
  fmtK(1500, b, sizeof(b));    assert(!strcmp(b, "1.5k"));
  fmtK(94000, b, sizeof(b));   assert(!strcmp(b, "94.0k"));   // 与 sim k() 一致(<100k 保留 1 位小数)
  fmtK(1234567, b, sizeof(b)); assert(!strcmp(b, "1235k"));
  fmtK(0, b, sizeof(b));       assert(!strcmp(b, "0"));

  // ---- fmtElapsed ----
  fmtElapsed(40, b, sizeof(b));        assert(!strcmp(b, "0m"));
  fmtElapsed(40*60, b, sizeof(b));     assert(!strcmp(b, "40m"));
  fmtElapsed(3*3600+36*60, b, sizeof(b)); assert(!strcmp(b, "3h36m"));

  // ---- modelShort ----
  modelShort("claude-opus-4-8", b, sizeof(b));            assert(!strcmp(b, "Opus 4.8"));
  modelShort("claude-sonnet-4-6", b, sizeof(b));          assert(!strcmp(b, "Son 4.6"));
  modelShort("claude-haiku-4-5-20251001", b, sizeof(b));  assert(!strcmp(b, "Haiku 4.5"));
  modelShort("gpt-5.1-codex", b, sizeof(b));              assert(!strcmp(b, "GPT-5.1"));
  modelShort("gpt-5.1-codex-mini", b, sizeof(b));         assert(!strcmp(b, "GPT-5.1"));
  modelShort("", b, sizeof(b));                            assert(!strcmp(b, ""));
  modelShort("Opus 4.8", b, sizeof(b));                    assert(!strcmp(b, "Opus 4.8"));
  // 越界回归:超长数字串不得溢出内部 ver[12](v<VMAX 保护,版本截断到 11 位)
  modelShort("claude-opus-99999999999999999999", b, sizeof(b)); assert(!strcmp(b, "Opus 99999999999"));

  (void)S;
  printf("codey_ui tests: ALL PASS\n");
  return 0;
}
```

- [ ] **Step 2: 运行,确认失败**

Run: `bash sketches/codey_dash/test/run_tests.sh`
Expected: 编译失败 —`statusFromStr` 等未声明。

- [ ] **Step 3: 实现 codey_ui.h**

替换 `codey_ui.h` 全文为:

```cpp
// sketches/codey_dash/codey_ui.h — pure UI helpers (host-testable; no Arduino deps).
#pragma once
#include <stdint.h>
#include <string.h>
#include <stdio.h>

// ---- session status ----
enum SessStatus { ST_EXECUTING = 0, ST_THINKING = 1, ST_WAITING = 2, ST_DONE = 3 };

static inline SessStatus statusFromStr(const char* s) {
  if (!s) return ST_WAITING;
  if (!strcmp(s, "executing")) return ST_EXECUTING;
  if (!strcmp(s, "thinking"))  return ST_THINKING;
  if (!strcmp(s, "done"))      return ST_DONE;
  return ST_WAITING;
}
static inline const char* statusWord(SessStatus s) {
  switch (s) {
    case ST_EXECUTING: return "EXECUTING";
    case ST_THINKING:  return "THINKING";
    case ST_DONE:      return "DONE";
    default:           return "WAITING";
  }
}
// sort rank for cross-provider merge: executing < thinking < (waiting|done)
static inline int statusRank(SessStatus s) { return s == ST_EXECUTING ? 0 : s == ST_THINKING ? 1 : 2; }

// ---- UTF-8 codepoint helpers ----
static inline int utf8Len(unsigned char c) { return c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1; }

static inline int cpLen(const char* s) {
  int n = 0;
  for (int i = 0; s && s[i];) { i += utf8Len((unsigned char)s[i]); n++; }
  return n;
}

// copy <= maxCp codepoints into out; if truncated, keep maxCp-1 cps + "…"(U+2026, 3 bytes)
static inline void truncCp(const char* s, int maxCp, char* out, size_t outSz) {
  if (!out || outSz == 0) return;
  if (!s) { out[0] = 0; return; }
  if (cpLen(s) <= maxCp) { snprintf(out, outSz, "%s", s); return; }
  int keep = maxCp - 1; if (keep < 0) keep = 0;
  int cut = 0, cnt = 0;
  for (int i = 0; s[i] && cnt < keep;) { i += utf8Len((unsigned char)s[i]); cnt++; cut = i; }
  size_t k = (size_t)cut; if (k > outSz - 1) k = outSz - 1;
  memcpy(out, s, k);
  // append "…" if it fits
  const char* ell = "\xE2\x80\xA6";
  if (k + 3 < outSz) { memcpy(out + k, ell, 3); out[k + 3] = 0; }
  else out[k] = 0;
}

// ---- number / duration formatting ----
static inline void fmtK(long n, char* out, size_t outSz) {
  if (n >= 1000) {
    double kf = n / 1000.0;
    if (n >= 100000) snprintf(out, outSz, "%.0fk", kf);
    else             snprintf(out, outSz, "%.1fk", kf);
  } else {
    snprintf(out, outSz, "%ld", n);
  }
}

static inline void fmtElapsed(long secs, char* out, size_t outSz) {
  if (secs < 0) secs = 0;
  long h = secs / 3600, m = (secs % 3600) / 60;
  if (h > 0) snprintf(out, outSz, "%ldh%02ldm", h, m);
  else       snprintf(out, outSz, "%ldm", m);
}

// ---- model short name: "claude-opus-4-8"->"Opus 4.8", "gpt-5.1-codex"->"GPT-5.1" ----
static inline void modelShort(const char* full, char* out, size_t outSz) {
  if (!out || outSz == 0) return;
  if (!full || !full[0]) { out[0] = 0; return; }

  char low[64]; size_t i = 0;
  for (; full[i] && i < sizeof(low) - 1; i++) { char c = full[i]; low[i] = (c >= 'A' && c <= 'Z') ? c + 32 : c; }
  low[i] = 0;

  const char* base = nullptr; bool gpt = false;
  if (strstr(low, "opus"))        base = "Opus";
  else if (strstr(low, "sonnet")) base = "Son";
  else if (strstr(low, "haiku"))  base = "Haiku";
  else if (strstr(low, "gpt"))  { base = "GPT"; gpt = true; }

  if (!base) { snprintf(out, outSz, "%s", full); return; }   // unknown -> passthrough

  // version: first digit group, optional .second group (sep '.' or '-')
  char ver[12] = {0}; int v = 0; const int VMAX = (int)sizeof(ver) - 1;   // every write guarded by v<VMAX
  for (size_t j = 0; low[j] && v < VMAX; j++) {
    if (low[j] >= '0' && low[j] <= '9') {
      ver[v++] = low[j];                                     // major digits
      size_t kk = j + 1;
      while (low[kk] >= '0' && low[kk] <= '9' && v < VMAX) ver[v++] = low[kk++];
      if ((low[kk] == '.' || low[kk] == '-') && low[kk + 1] >= '0' && low[kk + 1] <= '9' && v < VMAX) {
        ver[v++] = '.'; kk++;
        while (low[kk] >= '0' && low[kk] <= '9' && v < VMAX) ver[v++] = low[kk++];
      }
      break;
    }
  }
  ver[v] = 0;

  if (!ver[0])      snprintf(out, outSz, "%s", base);
  else if (gpt)     snprintf(out, outSz, "%s-%s", base, ver);
  else              snprintf(out, outSz, "%s %s", base, ver);
}
```

- [ ] **Step 4: 运行,确认通过**

Run: `bash sketches/codey_dash/test/run_tests.sh`
Expected: `codey_ui tests: ALL PASS`,退出码 0。

- [ ] **Step 5: Commit**

```bash
git add sketches/codey_dash/codey_ui.h sketches/codey_dash/test/codey_ui_test.cpp
git commit -m "feat(firmware): codey_ui 纯函数(status/截断/格式化/model 短名)+ 测试"
```

---

## Task 2: session_store.h — Sess / Prov 数据结构

**Files:**
- Create: `sketches/codey_dash/session_store.h`

纯结构定义,无逻辑,故无单测;以编译通过为准(Task 3 接入时一并编译)。

- [ ] **Step 1: 写头文件**

```cpp
// sketches/codey_dash/session_store.h — fixed-capacity session / provider model.
#pragma once
#include <stdint.h>

static const int MAX_SESS  = 12;   // per provider (overflow shown as "+N more")
static const int MAX_PORTS = 6;

struct Sess {
  char    id[40];
  char    name[40];
  char    model[24];      // raw model name (display via modelShort())
  char    branch[40];
  char    task[64];       // current_task
  char    effort[10];
  uint8_t status;         // SessStatus
  int     ctxPct;
  long    ctxTok, ctxWin, tokTotal;
  int     turn;
  int     added, modified;
  int     subagents;
  int     ports[MAX_PORTS];
  int     nports;
  long    startedAt;      // epoch seconds
};

struct Prov {
  const char* name;       // static label ("Claude" / "Codex")
  uint32_t    color;
  int  sessUsed, weekUsed;        // account usage % (weekUsed feeds the edge arc)
  long sessReset, weekReset;      // reset epochs (parsed; not currently rendered)
  int  activeCount, dirtyRepos;
  long tokPerMin;
  Sess sess[MAX_SESS];
  int  nsess;
};
```

- [ ] **Step 2: 验证语法(主机快速编译)**

Run: `g++ -std=c++17 -fsyntax-only -x c++ sketches/codey_dash/session_store.h && echo SYNTAX_OK`
Expected: 打印 `SYNTAX_OK`。

- [ ] **Step 3: Commit**

```bash
git add sketches/codey_dash/session_store.h
git commit -m "feat(firmware): session_store — Sess/Prov 数据结构"
```

---

## Task 3: 拆除表盘/旧表计,迁移到 Prov,三页骨架可编译

> 把旧的「Claude/Codex 额度页 + 表盘页」相关绘制与状态删干净,改成 `PAGES={dashboard,claude,codex}` 骨架(各页先画占位文字),保证编译通过、能切页。后续 Task 4–7 再填实页面。

**Files:**
- Modify: `sketches/codey_dash/codey_dash.ino`

- [ ] **Step 1: 顶部接入新头 + 替换 Provider 定义**

在文件顶部 include 区(`#include "wifi_store.h"` 之后)加:

```cpp
#include "codey_ui.h"
#include "session_store.h"
```

删除旧的 `struct Provider {…}` 与 `static Provider PROV[2] = {…}`(约 31–40 行),替换为:

```cpp
static Prov PROV[2] = {
  { "Claude", COL_CLAUDE, 0, 0, 0, 0, 0, 0, 0, {}, 0 },
  { "Codex",  COL_CODEX,  0, 0, 0, 0, 0, 0, 0, {}, 0 },
};
```

- [ ] **Step 2: 删除随表盘移除的成员与函数**

删除以下声明/定义(均仅服务于旧 UI):
- 变量:`g_sReset[2]` / `g_wReset[2]`(被 `PROV[i].sessReset/weekReset` 取代)、`g_lunar[40]` / `g_zodiac[16]`。
- 函数整体:`drawMeter(...)`、`drawPill(...)`、`drawGaugeDots(...)`、`drawMoon(...)`、`moonFrac()`、`drawWatchFace()`。
- `moodFor(...)` 暂留(详情页 mascot 会改用 status,见 Task 7;此处先保留以免大改 mascot 调用)。

> 用编辑器搜索每个名字,确保删除其定义**且**没有遗留调用(下面 Step 3/4 会处理 `render()` 与 `fetchState()` 里的调用)。

- [ ] **Step 3: 重写 render() 为三页分发(占位)**

把整个 `render()` 函数体替换为:

```cpp
// forward decls (实现见 Task 4-7)
static void renderDashboard();
static void renderListPage(int provIdx);
static void renderDetailPage();

// detail 视图状态:active<0 表示不在详情;否则 detailProv(0/1) + detailIdx
static int detailProv = -1, detailIdx = 0;

static void render() {
  if (g_voice) { drawVoiceOverlay(); cv.pushSprite(0, 0); return; }

  if (detailProv >= 0) { renderDetailPage(); cv.pushSprite(0, 0); return; }
  if (page == 0)      renderDashboard();
  else                renderListPage(page - 1);   // page1->claude(0), page2->codex(1)
  cv.pushSprite(0, 0);
}

// 占位实现(Task 4-7 替换)
static void placeholder(const char* label) {
  cv.fillSprite(c565(0x000000));
  cv.setFont(&fonts::FreeSansBold18pt7b); cv.setTextDatum(middle_center);
  cv.setTextColor(c565(0x8a9097));
  cv.drawString(label, CX, CY);
}
static void renderDashboard()       { placeholder("DASHBOARD"); }
static void renderListPage(int i)   { placeholder(PROV[i].name); }
static void renderDetailPage()      { placeholder("DETAIL"); }
```

> 注意:`drawArc` / `drawHeader` / `drawClaude` / `drawCodex` / `drawWifiStatus` / `drawDots` / `drawAvatarOrb` / 动画状态 `updateAnim` 全部保留,Task 4–7 复用。`g_ringA/g_ringB` 缓存环也保留(Task 4 复用)。

- [ ] **Step 4: 修正 fetchState() 的旧字段引用(临时,Task 8 重写)**

`fetchState()` 内删除会引用已删成员的行,临时只解析账号额度到新结构:
- 删除 `lunar` 解析块(`JsonObject lu = doc["lunar"]; …`)与 `g_lunar/g_zodiac` 赋值。
- 把:
  ```cpp
  PROV[i].sessionUsed = pr["session"]["used_pct"] | PROV[i].sessionUsed;
  PROV[i].weeklyUsed  = pr["weekly"]["used_pct"]  | PROV[i].weeklyUsed;
  PROV[i].pending     = pr["pending_reviews"]     | PROV[i].pending;
  g_sReset[i] = pr["session"]["reset_epoch"] | 0L;
  g_wReset[i] = pr["weekly"]["reset_epoch"]  | 0L;
  ```
  替换为:
  ```cpp
  PROV[i].sessUsed  = pr["session"]["used_pct"] | 0;
  PROV[i].weekUsed  = pr["weekly"]["used_pct"]  | 0;
  PROV[i].sessReset = pr["session"]["reset_epoch"] | 0L;
  PROV[i].weekReset = pr["weekly"]["reset_epoch"]  | 0L;
  ```
- `fetchState()` 失败自愈块里把 `PROV[i].sessionUsed=0;…;PROV[i].pending=0;` 改为 `PROV[i].sessUsed=0; PROV[i].weekUsed=0; PROV[i].nsess=0;`,并删除 `g_lunar[0]=g_zodiac[0]=0;`(`g_model/g_codexModel` 保留)。
- model 解析行保留不变(写入 `g_model`/`g_codexModel`)。
- `Serial.printf("[fetch] ok …")` 里把 `PROV[0].sessionUsed` 等改为 `PROV[0].sessUsed`/`PROV[0].weekUsed` 等。

- [ ] **Step 5: 把切页逻辑改回 3 页且语义不变(已是 %3,确认)**

`loop()` 内两处 `page = (page + 1) % 3;` 保持不变(dashboard/claude/codex 正好 3 页)。注释 `// Claude / Codex / watch face` 改为 `// dashboard / claude / codex`。

- [ ] **Step 6: 编译**

Run: `./scripts/build.sh sketches/codey_dash`
Expected: 编译成功(`Sketch uses …`),无未定义符号 / 无对已删函数的残留调用。若报错,按报错删除遗留引用。

- [ ] **Step 7: Commit**

```bash
git add sketches/codey_dash/codey_dash.ino
git commit -m "refactor(firmware): 删表盘/旧表计,迁移 Prov + 三页骨架"
```

---

## Task 4: drawArcRange — 通用弧 + 仪表盘双弧

> 现有 `drawArc` 固定画整段 276° 环。仪表盘要左(Claude)右(Codex)两段。把它泛化成可指定起始角/扫角,旧整段调用用新函数包一层。

**Files:**
- Modify: `sketches/codey_dash/codey_dash.ino`

- [ ] **Step 1: 用 drawArcRange 重写 drawArc**

把现有 `drawArc(M5Canvas& dst, uint32_t color, int pct)` 替换为下面两个函数(几何/AA 算法不变,仅把 `startDeg/sweepDeg` 提成参数,且填充方向支持反向):

```cpp
// 通用 AA 弧:沿 [startDeg, startDeg+sweepDeg] 画轨道,按 pct 填充;reverse=true 时从末端起填。
static void drawArcRange(M5Canvas& dst, uint32_t color, int pct,
                         float startDeg, float sweepDeg, bool reverse) {
  const float rIn = 209.0f, rOut = 223.0f;
  const float p = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
  const float fillDeg = sweepDeg * p / 100.0f;
  const float loR = (rIn - 0.7f) * (rIn - 0.7f), hiR = (rOut + 0.7f) * (rOut + 0.7f);
  const int Rb = (int)rOut + 1;
  for (int dy = -Rb; dy <= Rb; dy++) {
    int py = CY + dy; if ((unsigned)py >= (unsigned)SIZE) continue;
    float fy = (float)dy, fyy = fy * fy;
    for (int dx = -Rb; dx <= Rb; dx++) {
      float r2 = (float)dx * dx + fyy;
      if (r2 > hiR || r2 < loR) continue;
      int px = CX + dx; if ((unsigned)px >= (unsigned)SIZE) continue;
      float rr = sqrtf(r2);
      float cov = fminf(rr - (rIn - 0.5f), (rOut + 0.5f) - rr);
      if (cov <= 0.0f) continue; if (cov > 1.0f) cov = 1.0f;
      float d = atan2f((float)dx, -fy) * 57.2957795f;          // design angle (0=top, cw)
      float dn = d - startDeg; if (dn < 0) dn += 360.0f;
      if (dn > sweepDeg) continue;                             // outside this segment
      float along = reverse ? (sweepDeg - dn) : dn;            // distance from the "fill origin"
      uint32_t rgb = (along <= fillDeg) ? color : 0x23262c;
      dst.drawPixel(px, py, c565(shade(rgb, -(1.0f - cov))));
    }
  }
  if (pct > 0) {                                               // glowing cap at the progress tip
    float tip = reverse ? (startDeg + sweepDeg - fillDeg) : (startDeg + fillDeg);
    float a = (tip - 90.0f) * DEG_TO_RAD;
    int hx = CX + 216 * cosf(a), hy = CY + 216 * sinf(a);
    dst.fillSmoothCircle(hx, hy, 9, c565(COL_WHITE));
    dst.fillSmoothCircle(hx, hy, 6, c565(color));
  }
}

// 旧整段环(底部 84° 缺口居中):列表/详情页单弧沿用。
static void drawArc(M5Canvas& dst, uint32_t color, int pct) {
  drawArcRange(dst, color, pct, -138.0f, 276.0f, false);
}
```

- [ ] **Step 2: 加双弧绘制(仪表盘用)**

在 `drawArc` 之后加:

```cpp
// 仪表盘:左半=Claude(从顶往左下填),右半=Codex(从顶往右下填),底部缺口居中。
static void drawDualArc(M5Canvas& dst, int claudePct, int codexPct) {
  drawArcRange(dst, COL_CLAUDE, claudePct, -138.0f, 138.0f, true);   // left side, fill from top
  drawArcRange(dst, COL_CODEX,  codexPct,    0.0f,  138.0f, false);  // right side, fill from top
}
```

- [ ] **Step 3: 编译**

Run: `./scripts/build.sh sketches/codey_dash`
Expected: 成功。

- [ ] **Step 4: Commit**

```bash
git add sketches/codey_dash/codey_dash.ino
git commit -m "feat(firmware): drawArcRange 通用弧 + 仪表盘双弧"
```

---

## Task 5: 仪表盘首页 renderDashboard()

> 蓝本 `sim/codey-sim.html:183 renderDashboard`。元素:双弧 → 顶部 `CLAUDE x% · CODEX y% WK` → 双吉祥物夹活跃计数 `a·b` → 跨两端 top5 会话色块(2 列)+ `+N more` → 底部 `⎇ N dirty · Xk/min` → 页点。坐标按 466 取(下列数值可在真机微调)。

**Files:**
- Modify: `sketches/codey_dash/codey_dash.ino`

- [ ] **Step 1: 加迷你吉祥物 + 跨端合并工具**

在 mascot helpers 区(`drawCodex` 之后)加:

```cpp
// 小号吉祥物:3D orb + 两只眼,用于仪表盘/列表(大号 drawClaude/drawCodex 留给详情页)
static void drawMiniMascot(int cx, int cy, int R, uint32_t color, bool isClaude, uint8_t status) {
  drawAvatarOrb(cx, cy, R, color);
  float open = (status == ST_EXECUTING) ? 1.15f : (status == ST_THINKING) ? 0.85f : 0.55f;
  if (aBlink) open = 0.12f;
  int ex = (int)(R * 0.34f), ey = cy - (int)(R * 0.05f);
  int ew = (int)(R * 0.20f), eh = (int)(R * 0.30f * open) + 2;
  uint16_t ec = isClaude ? c565(0x140a04) : c565(0x8AD8C7);
  if (isClaude) {
    cv.fillRoundRect(cx - ex - ew / 2, ey - eh / 2, ew, eh, 2, ec);
    cv.fillRoundRect(cx + ex - ew / 2, ey - eh / 2, ew, eh, 2, ec);
  } else {
    cv.fillSmoothCircle(cx - ex, ey, max(2, eh / 2), ec);
    cv.fillSmoothCircle(cx + ex, ey, max(2, eh / 2), ec);
  }
}

// 跨两端把会话引用收集进数组并按 (rank, -ctxPct) 排序;返回总数。
struct SessRef { int prov; int idx; };
static int collectSorted(SessRef* out, int cap) {
  int n = 0;
  for (int pr = 0; pr < 2; pr++)
    for (int i = 0; i < PROV[pr].nsess && n < cap; i++) out[n++] = { pr, i };
  for (int a = 0; a < n; a++)                              // 简单插入排序(n<=24)
    for (int b = a + 1; b < n; b++) {
      const Sess& A = PROV[out[a].prov].sess[out[a].idx];
      const Sess& B = PROV[out[b].prov].sess[out[b].idx];
      int ra = statusRank((SessStatus)A.status), rb = statusRank((SessStatus)B.status);
      if (rb < ra || (rb == ra && B.ctxPct > A.ctxPct)) { SessRef t = out[a]; out[a] = out[b]; out[b] = t; }
    }
  return n;
}
```

- [ ] **Step 2: 实现 renderDashboard()(替换占位)**

```cpp
static void renderDashboard() {
  // 双弧缓存在 g_ringA(仪表盘专用),仅在 pct 变化时重算
  static int rcA = -1, rcX = -1;
  if (!g_ringAok) { cv.fillSprite(c565(0x000000)); }
  else {
    if (PROV[0].weekUsed != rcA || PROV[1].weekUsed != rcX) {
      g_ringA.fillSprite(c565(0x000000));
      drawDualArc(g_ringA, PROV[0].weekUsed, PROV[1].weekUsed);
      rcA = PROV[0].weekUsed; rcX = PROV[1].weekUsed;
    }
    g_ringA.pushSprite(&cv, 0, 0);
  }

  // 顶部标题
  char hdr[40];
  snprintf(hdr, sizeof(hdr), "CLAUDE %d%%  ·  CODEX %d%% WK", PROV[0].weekUsed, PROV[1].weekUsed);
  cv.setFont(&fonts::FreeSans9pt7b); cv.setTextSize(1); cv.setTextDatum(middle_center);
  cv.setTextColor(c565(0x8a9097)); cv.drawString(hdr, CX, 52);

  // 双吉祥物 + 中央活跃计数 a·b
  drawMiniMascot(CX - 96, 104, 30, COL_CLAUDE, true,  ST_EXECUTING);
  drawMiniMascot(CX + 96, 104, 30, COL_CODEX,  false, ST_THINKING);
  cv.setFont(&fonts::FreeSans9pt7b); cv.setTextColor(c565(0x8a9097)); cv.setTextDatum(middle_center);
  cv.drawString("ACTIVE", CX, 86);
  char ac[16]; snprintf(ac, sizeof(ac), "%d", PROV[0].activeCount);
  char xc[16]; snprintf(xc, sizeof(xc), "%d", PROV[1].activeCount);
  cv.setFont(&fonts::FreeSansBold18pt7b);
  int wA = cv.textWidth(ac), wDot = cv.textWidth(" · "), wX = cv.textWidth(xc);
  int x0 = CX - (wA + wDot + wX) / 2;
  cv.setTextDatum(middle_left); cv.setTextColor(c565(0xe6e8ec)); cv.drawString(ac, x0, 116);
  cv.setTextColor(c565(COL_CLAUDE)); cv.drawString(" · ", x0 + wA, 116);
  cv.setTextColor(c565(0xe6e8ec)); cv.drawString(xc, x0 + wA + wDot, 116);

  // 跨端 top5 色块(2 列网格)
  SessRef refs[MAX_SESS * 2]; int total = collectSorted(refs, MAX_SESS * 2);
  const int N = 5, colW = 168, rowH = 34, gap = 8;
  const int gx = CX - colW - gap / 2, gy = 168;
  cv.setFont(&fonts::FreeSans9pt7b); cv.setTextDatum(middle_left);
  for (int i = 0; i < total && i < N; i++) {
    const Sess& s = PROV[refs[i].prov].sess[refs[i].idx];
    bool isC = refs[i].prov == 0;
    int cx = gx + (i % 2) * (colW + gap), cy = gy + (i / 2) * (rowH + gap);
    cv.fillRoundRect(cx, cy, colW, rowH, 8, c565(shade(isC ? COL_CLAUDE : COL_CODEX, -0.78f)));
    cv.drawRoundRect(cx, cy, colW, rowH, 8, c565(shade(isC ? COL_CLAUDE : COL_CODEX, -0.40f)));
    char nm[32]; truncCp(s.name, 7, nm, sizeof(nm));
    const char* ico = s.status == ST_EXECUTING ? ">" : s.status == ST_THINKING ? "*" : s.status == ST_DONE ? "v" : "=";
    char line[48]; snprintf(line, sizeof(line), "%s %s %d%%", ico, nm, s.ctxPct);
    cv.setTextColor(c565(isC ? COL_CLAUDE : COL_CODEX));
    cv.drawString(line, cx + 10, cy + rowH / 2);
  }
  if (total > N) {
    int cx = gx + (N % 2) * (colW + gap), cy = gy + (N / 2) * (rowH + gap);
    char more[24]; snprintf(more, sizeof(more), "+%d more", total - N);
    cv.setTextColor(c565(0x6f757d)); cv.setTextDatum(middle_center);
    cv.drawString(more, cx + colW / 2, cy + rowH / 2);
  }
  if (total == 0) {
    cv.setFont(&fonts::FreeSans9pt7b); cv.setTextColor(c565(0x6f757d)); cv.setTextDatum(middle_center);
    cv.drawString("no active sessions", CX, 200);
  }

  // 底部聚合
  char agg[48]; char km[16];
  fmtK(PROV[0].tokPerMin + PROV[1].tokPerMin, km, sizeof(km));
  snprintf(agg, sizeof(agg), "%c %d dirty · %s/min", '~', PROV[0].dirtyRepos + PROV[1].dirtyRepos, km);
  cv.setFont(&fonts::FreeMono9pt7b); cv.setTextColor(c565(0x6f757d)); cv.setTextDatum(middle_center);
  cv.drawString(agg, CX, 330);

  drawWifiStatus(406);
  drawDots(0, COL_WHITE);
}
```

> 图标用 ASCII(`> * v =`)而非 Unicode 箭头——M5GFX 默认字体不含 ▶◆⏸✓。如需符号,后续可用自绘三角/方块替换(留作微调)。

- [ ] **Step 3: 编译 + 真机视觉核对**

Run: `./scripts/build.sh sketches/codey_dash`
Expected: 成功。
Run(真机,用户已授权 autonomous flash):`./scripts/flash.sh sketches/codey_dash`
On device:首页应显示双弧 + 顶部标题 + `ACTIVE a·b` + 会话色块(无会话时显示 `no active sessions`)+ 底部聚合 + 页点。与 `sim/codey-sim.html` 仪表盘比对,记录需要微调的坐标(此步只验证元素齐全且不崩)。

- [ ] **Step 4: Commit**

```bash
git add sketches/codey_dash/codey_dash.ino
git commit -m "feat(firmware): 仪表盘首页(双弧+双吉祥物+会话色块+聚合)"
```

---

## Task 6: 会话列表页 renderListPage()

> 蓝本 `sim/codey-sim.html:221 renderList`。元素:单弧=该端 weekly% → 头部小吉祥物 + `CLAUDE · N` → 两行/会话(行1:状态图标+名称+状态词;行2:`model · ctx% · token · turn`)→ 可滚动 → 顶/底渐隐 → 页点。

**Files:**
- Modify: `sketches/codey_dash/codey_dash.ino`

- [ ] **Step 1: 加列表布局常量 + 滚动状态**

在 `static int detailProv = -1, detailIdx = 0;` 附近加:

```cpp
// 列表页布局(466 屏内)
static const int ROW_H = 47, LIST_TOP = 116, LIST_BOT = 56;
static int g_scroll[2] = { 0, 0 };                       // claude/codex 各自的滚动像素偏移

static int listViewH() { return SIZE - LIST_TOP - LIST_BOT; }
static int maxScrollFor(int provIdx) {
  int content = PROV[provIdx].nsess * ROW_H;
  int m = content - listViewH();
  return m > 0 ? m : 0;
}
```

- [ ] **Step 2: 实现 renderListPage()(替换占位)**

```cpp
static void renderListPage(int provIdx) {
  const Prov& p = PROV[provIdx];
  uint32_t color = p.color;

  // 单弧缓存在 g_ringB(仅列表用;详情页直接画 cv),pct 变化才重算
  static int rbPct = -1; static uint32_t rbCol = 0;
  if (g_ringBok) {
    if (p.weekUsed != rbPct || color != rbCol) {
      g_ringB.fillSprite(c565(0x000000));
      drawArc(g_ringB, color, p.weekUsed);
      rbPct = p.weekUsed; rbCol = color;
    }
    g_ringB.pushSprite(&cv, 0, 0);
  } else cv.fillSprite(c565(0x000000));

  // 头部:小吉祥物 + "CLAUDE · N"
  drawMiniMascot(CX - 52, 60, 22, color, provIdx == 0, ST_THINKING);
  char hd[24]; snprintf(hd, sizeof(hd), "%s · %d", provIdx == 0 ? "CLAUDE" : "CODEX", p.nsess);
  cv.setFont(&fonts::FreeSans9pt7b); cv.setTextSize(1); cv.setTextDatum(middle_left);
  cv.setTextColor(c565(0xcfd2d8)); cv.drawString(hd, CX - 20, 60);

  // 空态
  if (p.nsess == 0) {
    cv.setFont(&fonts::FreeSans9pt7b); cv.setTextColor(c565(0x6f757d)); cv.setTextDatum(middle_center);
    cv.drawString("no sessions", CX, CY);
    drawDots(provIdx + 1, color);
    return;
  }

  // 裁剪窗 + 滚动绘制
  int sc = g_scroll[provIdx];
  if (sc > maxScrollFor(provIdx)) { sc = maxScrollFor(provIdx); g_scroll[provIdx] = sc; }
  cv.setClipRect(40, LIST_TOP, SIZE - 80, listViewH());
  for (int i = 0; i < p.nsess; i++) {
    int y = LIST_TOP - sc + i * ROW_H;
    if (y + ROW_H < LIST_TOP || y > SIZE - LIST_BOT) continue;     // 屏外跳过
    const Sess& s = p.sess[i];
    SessStatus st = (SessStatus)s.status;
    const char* ico = st == ST_EXECUTING ? ">" : st == ST_THINKING ? "*" : st == ST_DONE ? "v" : "=";
    uint16_t tint = c565(st == ST_EXECUTING ? shade(color, 0.25f) : st == ST_THINKING ? 0xffd479 : 0x8b9097);

    char nm[40]; truncCp(s.name, 16, nm, sizeof(nm));
    // 行1
    cv.setFont(&fonts::FreeSansBold12pt7b); cv.setTextDatum(middle_left);
    int x = 48;
    cv.setTextColor(c565(0xe6e8ec));
    char l1[48]; snprintf(l1, sizeof(l1), "%s %s", ico, nm);
    cv.drawString(l1, x, y + 14);
    int wl1 = cv.textWidth(l1);
    cv.setFont(&fonts::FreeSans9pt7b); cv.setTextColor(tint);
    cv.drawString(statusWord(st), x + wl1 + 10, y + 14);
    // 行2
    char md[24]; modelShort(s.model, md, sizeof(md));
    char kt[16]; fmtK(s.tokTotal, kt, sizeof(kt));
    char l2[64]; snprintf(l2, sizeof(l2), "%s · %d%% · %s · t%d", md, s.ctxPct, kt, s.turn);
    cv.setFont(&fonts::FreeMono9pt7b); cv.setTextColor(c565(0x8a8d94));
    cv.drawString(l2, x, y + 34);
    // 分隔线
    cv.drawFastHLine(48, y + ROW_H - 1, SIZE - 96, c565(0x1a1c20));
  }
  cv.clearClipRect();

  // 顶/底渐隐(纯色淡出条,提示可滚动)
  if (sc > 0)                       cv.fillRect(40, LIST_TOP, SIZE - 80, 8, c565(0x000000));
  if (sc < maxScrollFor(provIdx))   cv.fillRect(40, SIZE - LIST_BOT - 8, SIZE - 80, 8, c565(0x000000));

  drawDots(provIdx + 1, color);
}
```

- [ ] **Step 3: 编译 + 真机核对**

Run: `./scripts/build.sh sketches/codey_dash`
Expected: 成功。
Run: `./scripts/flash.sh sketches/codey_dash`
On device:BtnA 短按从仪表盘切到 Claude/Codex 列表,看到每会话两行;若会话多于一屏,先确认渲染正确(滚动在 Task 8 接触摸)。无会话时显示 `no sessions`。

- [ ] **Step 4: Commit**

```bash
git add sketches/codey_dash/codey_dash.ino
git commit -m "feat(firmware): Claude/Codex 会话列表页(两行/会话 + 滚动渲染)"
```

---

## Task 7: 单会话详情页 renderDetailPage()

> 蓝本 `sim/codey-sim.html:249 renderDetail` + spec §4。元素:单弧=该会话 ctx% → 头部 `● Claude · <name>` + 第二行 `model · 时长 · turn n` → 中央大吉祥物 + 状态词 → 信息行(任务/git/ctx/subagents·ports)→ 会话位置点 `i/N`。

**Files:**
- Modify: `sketches/codey_dash/codey_dash.ino`

- [ ] **Step 1: 让大吉祥物随 status 取 mood**

`drawClaude/drawCodex` 第 4 参 `mood` 仍为字符串。加一个映射,详情页用:

在 mascot helpers 区加:

```cpp
static const char* moodForStatus(uint8_t status) {
  switch (status) {
    case ST_EXECUTING: return "alert";
    case ST_THINKING:  return "focused";
    case ST_DONE:      return "happy";
    default:           return "sleepy";   // waiting
  }
}
```

- [ ] **Step 2: 实现 renderDetailPage()(替换占位)**

```cpp
static void renderDetailPage() {
  const Prov& p = PROV[detailProv];
  if (detailIdx < 0 || detailIdx >= p.nsess) { cv.fillSprite(c565(0x000000)); return; }
  const Sess& s = p.sess[detailIdx];
  uint32_t color = p.color;
  SessStatus st = (SessStatus)s.status;

  // 弧 = ctx%(详情页每次重算到 cv,频率低可接受)
  cv.fillSprite(c565(0x000000));
  drawArc(cv, color, s.ctxPct);

  // 头部:点 + provider · name
  cv.fillCircle(CX - 70, 36, 4, c565(color));
  char title[48]; char nm[40]; truncCp(s.name, 14, nm, sizeof(nm));
  snprintf(title, sizeof(title), "%s · %s", detailProv == 0 ? "Claude" : "Codex", nm);
  cv.setFont(&fonts::FreeSansBold12pt7b); cv.setTextSize(1); cv.setTextDatum(middle_left);
  cv.setTextColor(c565(0xcfd2d8)); cv.drawString(title, CX - 58, 36);

  // 第二行:model · 时长 · turn
  long nowE = time(nullptr); bool epochOK = nowE > 1700000000L;
  long elapsed = (epochOK && s.startedAt > 0) ? (nowE - s.startedAt) : 0;
  char md[24]; modelShort(s.model, md, sizeof(md));
  char el[16]; fmtElapsed(elapsed, el, sizeof(el));
  char l2[64]; snprintf(l2, sizeof(l2), "%s · %s · turn %d", md, el, s.turn);
  cv.setFont(&fonts::FreeMono9pt7b); cv.setTextDatum(middle_center);
  cv.setTextColor(c565(0x7d828a)); cv.drawString(l2, CX, 64);

  // 中央大吉祥物 + 状态词
  float t = (millis() - bootMs) / 1000.0f;
  const char* mood = moodForStatus(s.status);
  if (detailProv == 0) drawClaude(CX, 150, color, mood, t);
  else                 drawCodex(CX, 150, color, mood, t);
  cv.setFont(&fonts::FreeSansBold12pt7b); cv.setTextDatum(middle_center);
  uint16_t sw = c565(st == ST_EXECUTING ? shade(color, 0.25f) : st == ST_THINKING ? 0xffd479 : 0x8b9097);
  cv.setTextColor(sw); cv.drawString(statusWord(st), CX, 224);

  // 信息行(左对齐,逐行)
  const int ix = 66, iw = SIZE - 132; int y = 256; const int dy = 26;
  cv.setFont(&fonts::FreeMono9pt7b); cv.setTextDatum(middle_left);
  cv.setClipRect(ix, y - 14, iw, dy * 4 + 8);
  char buf[96];
  // ① 当前任务
  if (s.task[0]) { cv.setTextColor(c565(0xe6e8ec)); snprintf(buf, sizeof(buf), "* %s", s.task); }
  else           { cv.setTextColor(c565(0x6f757d)); snprintf(buf, sizeof(buf), "* idle"); }
  cv.drawString(buf, ix, y); y += dy;
  // ② git
  cv.setTextColor(c565(0xc3c7cd));
  snprintf(buf, sizeof(buf), "git %s +%d ~%d", s.branch[0] ? s.branch : "-", s.added, s.modified);
  cv.drawString(buf, ix, y); y += dy;
  // ③ ctx / tokens
  char kc[16], kw[16], kt[16];
  fmtK(s.ctxTok, kc, sizeof(kc)); fmtK(s.ctxWin, kw, sizeof(kw)); fmtK(s.tokTotal, kt, sizeof(kt));
  snprintf(buf, sizeof(buf), "ctx %s/%s · %s tok", kc, kw, kt);
  cv.drawString(buf, ix, y); y += dy;
  // ④ subagents · ports
  int n = snprintf(buf, sizeof(buf), "%d subagents", s.subagents);
  for (int i = 0; i < s.nports && n < (int)sizeof(buf) - 8; i++)
    n += snprintf(buf + n, sizeof(buf) - n, "%s:%d", i == 0 ? " · " : " ", s.ports[i]);
  cv.drawString(buf, ix, y);
  cv.clearClipRect();

  // 位置:session i/N
  char pos[24]; snprintf(pos, sizeof(pos), "session %d/%d", detailIdx + 1, p.nsess);
  cv.setFont(&fonts::FreeSans9pt7b); cv.setTextDatum(middle_center);
  cv.setTextColor(c565(0x5a5d64)); cv.drawString(pos, CX, 438);
}
```

- [ ] **Step 3: 编译**

Run: `./scripts/build.sh sketches/codey_dash`
Expected: 成功。(详情页此刻还进不去 —— 入口在 Task 8;可在 Step 4 临时验证。)

- [ ] **Step 4: 临时验证渲染(可选)**

把 `render()` 顶部临时改为 `detailProv = (PROV[0].nsess>0?0:(PROV[1].nsess>0?1:-1)); detailIdx = 0;` 编译刷机看一眼详情页布局,核对后**还原**该行。或直接等 Task 8 接好入口再核对。

- [ ] **Step 5: Commit**

```bash
git add sketches/codey_dash/codey_dash.ino
git commit -m "feat(firmware): 单会话详情页(大吉祥物+任务/git/ctx/subagents)"
```

---

## Task 8: 触摸手势 + 详情进出 + 按键兜底

> 蓝本 `sim/codey-sim.html:337` 触摸段 + §5 交互表。能力:列表页竖拖滚动;横滑切页(详情里切会话);单击进详情(列表点行=该会话/点空白=置顶;仪表盘=全局最忙);双击退出详情;BtnA 短按=切页(详情里翻下一会话);BtnA 长按=列表进置顶详情 / 详情返回列表。摇晃切页保留。

**Files:**
- Modify: `sketches/codey_dash/codey_dash.ino`

- [ ] **Step 1: 加触摸/手势状态 + 阈值**

在 `g_scroll[2]` 附近加:

```cpp
// 触摸手势(466 屏内阈值)
static const int  TAP_MOVE = 12, SWIPE_MIN = 56; static const uint32_t DBL_MS = 300;
static bool     g_tDown = false; static int g_tx0 = 0, g_ty0 = 0; static int g_tStartScroll = 0;
static char     g_tAxis = 0;                 // 0 未定 / 'x' / 'y'
static int      g_tProv = -1;                // 竖拖作用的列表 provIdx(-1=非列表页)
static uint32_t g_lastTapMs = 0;             // 双击判定
static uint32_t g_pendTapMs = 0; static int g_pendTapRow = -2;   // 待派发的单击(row:-1空白,-2无)
```

- [ ] **Step 2: 加手势辅助函数**

在 `loop()` 之前加:

```cpp
static int curListProv() { return (detailProv < 0 && page >= 1 && page <= 2) ? page - 1 : -1; }

// 列表页:由屏幕 y 命中会话行号;-1 = 空白
static int rowHitAt(int provIdx, int ty) {
  if (ty < LIST_TOP || ty > SIZE - LIST_BOT) return -1;
  int idx = (g_scroll[provIdx] + (ty - LIST_TOP)) / ROW_H;
  return (idx >= 0 && idx < PROV[provIdx].nsess) ? idx : -1;
}

// 全局最忙(仪表盘单击进详情)
static bool globalTop(int& prov, int& idx) {
  SessRef refs[MAX_SESS * 2]; int n = collectSorted(refs, MAX_SESS * 2);
  if (n == 0) return false;
  prov = refs[0].prov; idx = refs[0].idx; return true;
}

static void enterDetail(int prov, int idx) { detailProv = prov; detailIdx = idx; }
static void exitDetail() { detailProv = -1; }

// 横滑:详情切会话,否则切页。dir:+1 下一 / -1 上一
static void swipePage(int dir) {
  if (detailProv >= 0) {
    int n = PROV[detailProv].nsess; if (n > 0) detailIdx = (detailIdx + dir + n) % n;
  } else {
    page = (page + dir + 3) % 3;
  }
}

// 单击派发:列表点行=该会话,点空白=置顶;仪表盘=全局最忙;详情不响应单击
static void doTap(int row) {
  if (detailProv >= 0) return;
  if (page == 0) { int pr, ix; if (globalTop(pr, ix)) enterDetail(pr, ix); }
  else {
    int pi = page - 1;
    if (PROV[pi].nsess == 0) return;
    enterDetail(pi, row >= 0 ? row : 0);
  }
}

// 长按 BtnA:列表→置顶详情;详情→返回列表
static void btnALong() {
  if (detailProv >= 0) { exitDetail(); return; }
  int pi = curListProv();
  if (pi >= 0 && PROV[pi].nsess > 0) enterDetail(pi, 0);
}
// 短按 BtnA:详情翻下一会话;否则切页
static void btnAShort() {
  if (detailProv >= 0) { int n = PROV[detailProv].nsess; if (n > 0) detailIdx = (detailIdx + 1) % n; }
  else page = (page + 1) % 3;
}
```

- [ ] **Step 3: 在 loop() 接入触摸采样(语音/设置态除外)**

在 `loop()` 内、`M5.update();` 之后、`uint32_t now = millis();` 之后,加触摸处理块。它必须在 `g_inSettings`/`g_voice` 之外才生效:

```cpp
  // ---- 触摸手势(仅非设置/非语音态)----
  if (!g_inSettings && !g_voice) {
    auto td = M5.Touch.getDetail();
    if (td.wasPressed()) {
      g_tDown = true; g_tx0 = td.x; g_ty0 = td.y; g_tAxis = 0;
      g_tProv = curListProv(); g_tStartScroll = (g_tProv >= 0) ? g_scroll[g_tProv] : 0;
    } else if (g_tDown && td.isPressed()) {
      int dx = td.x - g_tx0, dy = td.y - g_ty0;
      if (!g_tAxis && (abs(dx) > TAP_MOVE || abs(dy) > TAP_MOVE)) g_tAxis = (abs(dx) > abs(dy)) ? 'x' : 'y';
      if (g_tAxis == 'y' && g_tProv >= 0) {                     // 竖拖滚动列表
        int ns = g_tStartScroll - dy;
        int mx = maxScrollFor(g_tProv); ns = ns < 0 ? 0 : (ns > mx ? mx : ns);
        g_scroll[g_tProv] = ns;
      }
      lastActiveMs = now;
    } else if (g_tDown && td.wasReleased()) {
      int dx = td.x - g_tx0, dy = td.y - g_ty0;
      if (g_tAxis == 'x' && abs(dx) >= SWIPE_MIN) swipePage(dx < 0 ? 1 : -1);   // 横滑
      else if (g_tAxis == 0 && abs(dx) < TAP_MOVE && abs(dy) < TAP_MOVE) {       // 点击 -> 单/双击判定
        if (now - g_lastTapMs < DBL_MS) { g_lastTapMs = 0; g_pendTapRow = -2;    // 双击 -> 退出详情
          if (detailProv >= 0) exitDetail(); }
        else { g_lastTapMs = now;                                                // 记一次单击,延迟派发
          g_pendTapMs = now; g_pendTapRow = (g_tProv >= 0) ? rowHitAt(g_tProv, g_ty0) : -1; }
      }
      g_tDown = false; g_tAxis = 0; g_tProv = -1; lastActiveMs = now;
    }
    // 单击延迟派发(等过双击窗口确认不是双击)
    if (g_pendTapRow != -2 && now - g_pendTapMs >= DBL_MS) { doTap(g_pendTapRow); g_pendTapRow = -2; }
  }
```

> `M5.Touch.getDetail()` 在 `M5.begin()` 后即可用(StopWatch C152 带电容触摸)。若 `M5.Touch.isEnabled()` 为假(应不会),触摸块自然不产生事件,按键仍可用。

- [ ] **Step 4: 把 BtnA 短按/长按接到新逻辑**

在 `loop()` 里找到 `else if (!g_voice && M5.BtnA.wasPressed() && !M5.BtnB.isPressed())` 分支(原 `page=(page+1)%3`)。改为**短按用 `btnAShort()`,并新增长按检测**。替换该 `else if` 分支为:

```cpp
    } else if (!g_voice && !M5.BtnB.isPressed()) {              // 左键:短按切页/翻会话,长按进/出详情
      static uint32_t aDownAt = 0; static bool aLong = false;
      if (M5.BtnA.wasPressed()) { aDownAt = now; aLong = false; }
      if (M5.BtnA.isPressed() && !aLong && aDownAt && now - aDownAt > 550) { aLong = true; btnALong(); }
      if (M5.BtnA.wasReleased()) { if (!aLong) btnAShort(); aDownAt = 0; }
    }
```

> 这样短按(<550ms 释放)调用 `btnAShort()`,长按(按住 >550ms)触发一次 `btnALong()`。`Serial.printf("[btnA] page=…")` 可删或保留为调试。

- [ ] **Step 5: 摇晃切页保留但尊重详情态**

`loop()` 内 IMU 摇晃分支 `page = (page + 1) % 3;` 改为:

```cpp
    if (detailProv < 0) page = (page + 1) % 3;                  // 详情态下摇晃不切页
    g_lastShake = now; active = true;
```

(即把原 `page = (page + 1) % 3; g_lastShake = now; active = true;` 拆成上面两行+条件。)

- [ ] **Step 6: 编译 + 真机交互验证**

Run: `./scripts/build.sh sketches/codey_dash`
Expected: 成功。
Run: `./scripts/flash.sh sketches/codey_dash`
On device 核对交互(对照 §5):
- BtnA 短按:仪表盘→Claude→Codex 循环;
- 列表页竖拖:会话滚动;横滑:切页;单击会话行:进该会话详情;点空白:进置顶详情;
- 详情页:横滑/BtnA 短按翻会话;双击 或 BtnA 长按:返回列表;
- 仪表盘单击:进全局最忙会话详情;
- 摇晃:非详情态切页;BtnB:语音;双键长按:设置(均不受影响)。

- [ ] **Step 7: Commit**

```bash
git add sketches/codey_dash/codey_dash.ino
git commit -m "feat(firmware): M5.Touch 手势(滚动/横滑/单双击)+ 详情进出 + BtnA 长按"
```

---

## Task 9: 解析 sessions[] 填充 Prov + 端到端联调 + 文档

> 把 `fetchState()` 真正解析每会话写进 `PROV[i].sess[]`、`active_count`/`agg`,跑通 companion → 固件全链路,补 spec 完成状态。

**Files:**
- Modify: `sketches/codey_dash/codey_dash.ino`(`fetchState()`)
- Modify: `docs/superpowers/specs/2026-06-03-agent-session-monitor-design.md`(标注完成)

- [ ] **Step 1: 加会话解析辅助**

在 `fetchState()` 之前加(把一个 JSON session 对象拷进 `Sess`):

```cpp
static void copyStr(char* dst, size_t n, const char* src) { if (!src) src = ""; strncpy(dst, src, n - 1); dst[n - 1] = 0; }

static void parseSession(JsonObject so, Sess& s) {
  copyStr(s.id,     sizeof(s.id),     so["id"]            | "");
  copyStr(s.name,   sizeof(s.name),   so["name"]          | "");
  copyStr(s.model,  sizeof(s.model),  so["model"]         | "");
  copyStr(s.branch, sizeof(s.branch), so["git"]["branch"] | "");
  copyStr(s.task,   sizeof(s.task),   so["current_task"]  | "");
  copyStr(s.effort, sizeof(s.effort), so["effort"]        | "");
  s.status   = statusFromStr(so["status"] | "waiting");
  s.ctxPct   = so["context_pct"]    | 0;
  s.ctxTok   = so["context_tokens"] | 0L;
  s.ctxWin   = so["context_window"] | 200000L;
  s.tokTotal = so["tokens_total"]   | 0L;
  s.turn     = so["turn"]           | 0;
  s.added    = so["git"]["added"]    | 0;
  s.modified = so["git"]["modified"] | 0;
  s.subagents= so["subagents"]      | 0;
  s.startedAt= so["started_at"]     | 0L;
  s.nports = 0;
  for (JsonVariant pv : so["ports"].as<JsonArray>()) { if (s.nports < MAX_PORTS) s.ports[s.nports++] = pv.as<int>(); }
}
```

- [ ] **Step 2: 在 fetchState() provider 循环里填充会话与聚合**

在 `fetchState()` 的 `for (JsonObject pr : doc["providers"]…)` 循环内,账号额度解析之后(同一个 `i>=0` 块里)追加:

```cpp
        PROV[i].activeCount = pr["active_count"]        | 0;
        PROV[i].dirtyRepos  = pr["agg"]["dirty_repos"]  | 0;
        PROV[i].tokPerMin   = pr["agg"]["tokens_per_min"]| 0L;
        int n = 0;
        for (JsonObject so : pr["sessions"].as<JsonArray>()) {
          if (n >= MAX_SESS) break;
          parseSession(so, PROV[i].sess[n]); n++;
        }
        PROV[i].nsess = n;
```

- [ ] **Step 3: 失败自愈块清零会话**

`fetchState()` 末尾失败分支里(已在 Task 3 改过账号字段),确认含 `PROV[i].nsess = 0;`,并补 `PROV[i].activeCount = 0; PROV[i].dirtyRepos = 0; PROV[i].tokPerMin = 0;`。`detailProv` 越界保护:加 `if (detailProv >= 0 && detailIdx >= PROV[detailProv].nsess) detailProv = -1;`(在 `g_haveData=true` 成功解析后也加同样一行,防会话消失后停留在越界详情)。

- [ ] **Step 4: 编译 + 启动 Python companion 联调**

Run: `./scripts/build.sh sketches/codey_dash`
Expected: 成功。

Run(另开一个终端,启动 companion):
```bash
cd companion && python3 codey_companion.py &
sleep 2
curl -s --noproxy '*' http://127.0.0.1:8787/codey/state | python3 -m json.tool | head -40
```
Expected:输出含 `providers[].sessions[]` / `active_count` / `agg`;字段名与 Step 1/2 解析一致。核对至少一个 session 的 `status/model/context_pct/tokens_total/turn/git/ports`。

> 固件侧:确保 `g_companionUrl` 指向运行 companion 的 Mac IP(设置页 WiFi/手填,或代码内 `MAC_FALLBACK_IP`)。

- [ ] **Step 5: 真机端到端核对**

Run: `./scripts/flash.sh sketches/codey_dash`
On device:在 Mac 上开 1–2 个 Claude/Codex 会话,确认手表仪表盘活跃计数、列表行、详情字段与实际一致;切页/滚动/进出详情顺畅;无会话时各页空态正常;断开 companion(Ctrl-C)后 ~1 分钟内列表清空且不崩。

- [ ] **Step 6: 标注 spec 完成**

在 `docs/superpowers/specs/2026-06-03-agent-session-monitor-design.md` 顶部状态行 `- 状态:已通过 brainstorming,待写实现计划` 改为 `- 状态:已实现(companion=Python 计划1;固件=计划2 `2026-06-04-firmware-session-pages.md`)`。

- [ ] **Step 7: Commit**

```bash
git add sketches/codey_dash/codey_dash.ino docs/superpowers/specs/2026-06-03-agent-session-monitor-design.md
git commit -m "feat(firmware): 解析 sessions[] 填充 Prov + 端到端联调"
```

---

## 完成判据
- `bash sketches/codey_dash/test/run_tests.sh` 全绿(纯函数 TDD)。
- `./scripts/build.sh sketches/codey_dash` 编译通过。
- 真机:仪表盘/Claude 列表/Codex 列表/详情四种视图齐全且与 `sim/codey-sim.html` 一致;触摸(竖拖滚动、横滑切页/会话、单击进详情、双击退出)+ 按键(BtnA 短/长按)+ 摇晃 行为符合 spec §5。
- companion(Python)无改动即可被消费;断连有空态、不崩。

## 已知简化 / 留待微调(诚实记录)
- **坐标像素级微调**:各页 y 坐标按 380→466 估算,真机需逐页微调(Task 5/6/7/8 的视觉核对步骤即为此留口)。
- **状态图标**:用 ASCII(`> * v =`)替代 ▶◆⏸✓(默认字体无这些符号)。如需符号,可自绘小三角/方块,属纯视觉增量,不改数据流。
- **大吉祥物动画**:详情页复用既有 `drawClaude/drawCodex`(含 idle 动画);列表/仪表盘用简化 `drawMiniMascot`(orb+眼)。
- **滚动惯性**:本计划为直接跟手拖拽(无惯性甩动)。spec §5 提"惯性滚动"为加分项,留待手感实测后再加(在 `g_scroll` 上叠加释放速度衰减即可,不改其它)。
- **MAX_SESS=12/端**:超出显示 `+N more`(仪表盘)或列表截断到 12;真实并发通常远小于此。若需更多,调大常量即可(注意 JSON 文档与 RAM)。
- **tokens_per_min 单位**:固件直接用 companion 的聚合值(已是 token/分钟),仅 `fmtK` 美化。

## Self-Review(已对 spec 核对)
- spec §3 仪表盘 → Task 5;§3 两列表页 → Task 6;§4 详情页 → Task 7;§5 交互(触摸+按键+摇晃)→ Task 8;§2.5 契约解析 → Task 9;表盘页移除 → Task 3。覆盖完整。
- 命名一致性:`PROV`(Prov)、`Sess`、`detailProv/detailIdx`、`g_scroll[]`、`ROW_H/LIST_TOP/LIST_BOT`、`drawArcRange/drawDualArc`、`drawMiniMascot`、`statusFromStr/statusWord/statusRank/truncCp/fmtK/fmtElapsed/modelShort` 在各 Task 间一致引用。
- 无占位符:每个代码步给出完整可编译代码;纯函数有红→绿测试;绘制/触摸有编译 + 真机核对。

---

## Execution Handoff
见下方对话中的执行方式选择。
