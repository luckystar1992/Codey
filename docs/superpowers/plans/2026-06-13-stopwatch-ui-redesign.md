# Codey StopWatch UI 重构(abtop 对齐 + VLW 抗锯齿)Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 abtop 级会话信息(状态/配额/上下文/token 细分/摘要)以适合 466×466 圆屏的方式搬上 StopWatch,文字改 VLW 抗锯齿,交互改全触摸为主。

**Architecture:** 渐进增强——保留「主页⇄列表⇄详情」三级骨架与两个 mascot 动画。companion(Python)侧暴露已解析的 token4/summary、新增 compaction/limited 并把 waiting 排到最前;固件侧加 3 行卡片列表、VLW 抗锯齿字体、token4 详情、等待横幅。设备仍只轮询 `/codey/state`。

**Tech Stack:** Python 标准库(companion);Arduino/ESP32-S3 + LovyanGFX(M5Unified);freetype-py(VLW 字体生成);OFL 字体 JetBrains Mono / Space Grotesk。

**Spec:** `docs/superpowers/specs/2026-06-13-stopwatch-ui-redesign-design.md`

---

## File Structure

| 文件 | 责任 | 动作 |
|---|---|---|
| `companion/codey/transcript_claude.py` | Claude transcript 解析 + 新增 compaction 计数 | Modify |
| `companion/codey/build_session.py` | session dict 加 summary/tokens{}/compactions | Modify |
| `companion/codey/state.py` | provider 加 limited;`_RANK` waiting 优先 | Modify |
| `companion/codey/config.py` | DEFAULTS.columns 加 summary/branch | Modify |
| `companion/web/admin.html` | 配置台加 summary/branch 勾选 | Modify |
| `companion/tests/test_*.py` | 上述各项单测 | Modify |
| `scripts/gen_vlw.py` | freetype-py → .vlw → C 头数组生成器(+自检) | Create |
| `sketches/codey_dash/fonts/*.h` | 生成的 VLW 字体数组 | Create(生成物) |
| `sketches/codey_dash/codey_ui.h` | 纯函数:composeMetaLine、DispCol 扩展、statusRank | Modify |
| `sketches/codey_dash/test/codey_ui_test.cpp` | 纯函数单测 | Modify |
| `sketches/codey_dash/session_store.h` | Sess 加 summary/tok4/compactions 字段 | Modify |
| `sketches/codey_dash/codey_dash.ino` | JSON 解析 + 字体加载 + 三页渲染 + 交互 | Modify |

执行顺序:**Phase A 数据层(可独立测)→ Phase B 字体管线 → Phase C 固件纯函数 → Phase D 固件渲染 → Phase E 整体验证**。Phase A/B/C 互不依赖,可并行;Phase D 依赖 B、C。

---

## Phase A — 数据层(companion,TDD)

### Task 1: transcript 压缩计数

**Files:**
- Modify: `companion/codey/transcript_claude.py:27-33`(init dict)、`:48-57`(assistant 分支)
- Test: `companion/tests/test_transcript_claude.py`

- [ ] **Step 1: 写失败测试**(追加到 `TestTranscriptClaude` 类内)

```python
    def test_compaction_count(self):
        # 三个 assistant turn:94k -> 120k(涨,不算)-> 40k(掉 >30%,算 1 次)
        def asst(ctx_in):
            return A({"input_tokens": ctx_in, "output_tokens": 1,
                      "cache_read_input_tokens": 0, "cache_creation_input_tokens": 0},
                     [{"type": "text", "text": "ok"}])
        text = "\n".join([asst(94000), asst(120000), asst(40000)])
        r = parse_claude_transcript(text)
        self.assertEqual(r["compactions"], 1)

    def test_compaction_none_when_growing(self):
        def asst(ctx_in):
            return A({"input_tokens": ctx_in, "output_tokens": 1,
                      "cache_read_input_tokens": 0, "cache_creation_input_tokens": 0},
                     [{"type": "text", "text": "ok"}])
        r = parse_claude_transcript("\n".join([asst(1000), asst(2000), asst(3000)]))
        self.assertEqual(r["compactions"], 0)
```

- [ ] **Step 2: 运行测试确认失败**

Run: `cd companion && python3 -m pytest tests/test_transcript_claude.py -k compaction -v`
Expected: FAIL — `KeyError: 'compactions'`

- [ ] **Step 3: 实现**

`transcript_claude.py` init dict(第 27-33 行的 `r = {...}`)增加键:

```python
        "last_user_ts_ms": 0, "pending_tool": False,
        "compactions": 0, "_prev_ctx": 0,
```

在 assistant 分支里,`ctx` 计算之后、`r["last_context_tokens"] = ctx` 之前,插入压缩检测:

```python
            ctx = (inp + cc) if (cr == 0 and cc > 0) else (inp + cr)
            if r["_prev_ctx"] > 0 and ctx < r["_prev_ctx"] * 0.7:
                r["compactions"] += 1
            r["_prev_ctx"] = ctx
            r["last_context_tokens"] = ctx
```

返回前删除内部临时键(保持契约干净)。在 `return r` 之前加:

```python
    r.pop("_prev_ctx", None)
    return r
```

- [ ] **Step 4: 运行测试确认通过 + 全量回归**

Run: `cd companion && python3 -m pytest tests/test_transcript_claude.py -v && python3 -m pytest -q`
Expected: 新测试 PASS;全量仍全绿(已有 91 测试)。

- [ ] **Step 5: Commit**

```bash
git add companion/codey/transcript_claude.py companion/tests/test_transcript_claude.py
git commit -m "feat(companion): transcript 统计上下文压缩次数(掉 >30%)"
```

---

### Task 2: build_session 增 summary/tokens/compactions

**Files:**
- Modify: `companion/codey/build_session.py`(两个 build 函数)
- Test: `companion/tests/test_build_session.py`

- [ ] **Step 1: 写失败测试**(追加到 `TestBuildSession` 类内)

```python
    def test_claude_summary_tokens_compactions(self):
        parsed = {
            "total_input": 180, "total_output": 30, "total_cache_read": 600, "total_cache_create": 500,
            "last_context_tokens": 94000, "max_context_tokens": 94000, "turn_count": 23,
            "model": "claude-opus-4-8", "current_task": "Edit x",
            "git_branch": "main", "first_prompt": "帮我重构 agent 的对话内存管理实现",
            "compactions": 2, "last_user_ts_ms": 0, "pending_tool": True,
        }
        s = build_claude_session(
            session_id="sid1", cwd="/Users/zyc/code/Codey", started_at=1780000000000,
            parsed=parsed, status="executing", git={"branch": "main", "added": 3, "modified": 12},
            ports=[], subagents=0, effort="high", memory_kb=0)
        self.assertEqual(s["tokens"], {"in": 180, "out": 30, "cache_r": 600, "cache_w": 500})
        self.assertEqual(s["compactions"], 2)
        self.assertTrue(s["summary"].startswith("帮我重构"))
        self.assertLessEqual(len(s["summary"].encode("utf-8")), 64 + 3)  # 截断在 64 字节量级

    def test_codex_cache_w_zero(self):
        parsed = {"session_id": "x", "cwd": "/p", "model": "gpt-5.1-codex",
                  "total_input": 10, "total_output": 5, "total_cache_read": 7,
                  "last_context_tokens": 100, "context_window": 272000, "turn_count": 2,
                  "git_branch": "main", "first_prompt": "hi", "current_task": "", "effort": "",
                  "compactions": 0}
        s = build_codex_session(parsed=parsed, started_at=0, status="waiting",
                                git=None, ports=[], subagents=0, memory_kb=0)
        self.assertEqual(s["tokens"]["cache_w"], 0)
        self.assertEqual(s["summary"], "hi")
```

- [ ] **Step 2: 运行确认失败**

Run: `cd companion && python3 -m pytest tests/test_build_session.py -k "summary or cache_w" -v`
Expected: FAIL — `KeyError: 'tokens'`

- [ ] **Step 3: 实现**

在 `build_session.py` 顶部 `from .util import ...` 下加一个截断工具:

```python
def _summary(first_prompt):
    """首条用户提示截到 64 字节(UTF-8 安全,不切半个字)。"""
    s = " ".join((first_prompt or "").split())   # 折叠换行/多空格
    b = s.encode("utf-8")
    if len(b) <= 64:
        return s
    return b[:64].decode("utf-8", "ignore")
```

`build_claude_session` return dict 内,`"memory": memory_kb or 0,` 之后加:

```python
        "summary": _summary(parsed.get("first_prompt")),
        "tokens": {
            "in": parsed.get("total_input") or 0,
            "out": parsed.get("total_output") or 0,
            "cache_r": parsed.get("total_cache_read") or 0,
            "cache_w": parsed.get("total_cache_create") or 0,
        },
        "compactions": parsed.get("compactions") or 0,
```

`build_codex_session` return dict 内同位置加(Codex 无 cache_create,cache_w 恒 0):

```python
        "summary": _summary(parsed.get("first_prompt")),
        "tokens": {
            "in": parsed.get("total_input") or 0,
            "out": parsed.get("total_output") or 0,
            "cache_r": parsed.get("total_cache_read") or 0,
            "cache_w": 0,
        },
        "compactions": parsed.get("compactions") or 0,
```

- [ ] **Step 4: 运行确认通过 + 回归**

Run: `cd companion && python3 -m pytest tests/test_build_session.py -v && python3 -m pytest -q`
Expected: PASS;全量全绿。

- [ ] **Step 5: Commit**

```bash
git add companion/codey/build_session.py companion/tests/test_build_session.py
git commit -m "feat(companion): session 下发 summary/tokens 四件套/compactions"
```

---

### Task 3: state.py provider.limited + waiting 置顶排序

**Files:**
- Modify: `companion/codey/state.py:8`(`_RANK`)、claude/codex provider dict
- Test: `companion/tests/test_state.py`

- [ ] **Step 1: 写失败测试**(追加到 `TestState` 类内)

```python
    def test_provider_limited_flag(self):
        cache = {"claude": [], "codex": []}
        tok = {"claude": {"prev": None, "val": 0}, "codex": {"prev": None, "val": 0}}
        five = {"used_percentage": 100, "resets_at": 0}
        with mock.patch.object(state_mod, "read_real_usage",
                               return_value={"five_hour": five, "seven_day": None, "fresh": True}), \
             mock.patch.object(state_mod, "read_codex_usage", return_value=None):
            st = state_mod.build_state(cache, tok)
        self.assertTrue(st["providers"][0]["limited"])

    def test_sort_waiting_first(self):
        # waiting 应排到 executing 之前
        from codey.state import _sort_sessions
        arr = [
            {"id": "e", "status": "executing", "context_pct": 10},
            {"id": "w", "status": "waiting", "context_pct": 5},
            {"id": "t", "status": "thinking", "context_pct": 90},
        ]
        order = [s["id"] for s in _sort_sessions(arr)]
        self.assertEqual(order, ["w", "e", "t"])
```

- [ ] **Step 2: 运行确认失败**

Run: `cd companion && python3 -m pytest tests/test_state.py -k "limited or waiting_first" -v`
Expected: FAIL(`limited` 缺键 / 排序为 e,t,w)

- [ ] **Step 3: 实现**

`state.py` 顶部 `_RANK`(第 8 行)改为 waiting 最前、done 最后:

```python
_RANK = {"waiting": 0, "executing": 1, "thinking": 2, "done": 3}
```

`_sort_sessions` 的默认值与 tie-break 改为(waiting 已是 0,默认给未知态 1.5 居中即可,保留 ctx 次序):

```python
def _sort_sessions(arr):
    return sorted(arr, key=lambda s: (_RANK.get(s["status"], 2), -s.get("context_pct", 0)))
```

claude provider dict(`"pending_reviews": 0,` 所在 dict)加 `limited`:

```python
        "limited": bool(five_ok and clamp_pct(five["used_percentage"]) >= 100),
```

codex provider dict 同样加(用其 session 窗):

```python
        "limited": bool(cx_sess and (cx_sess["pct"] or 0) >= 100),
```

> 注意:已有测试 `test_sort_active_first`(test_state.py:59)断言 executing 在 waiting 前——它现在语义反了,需更新。把该测试里的期望顺序改为 waiting 优先,或重命名为 `test_sort_waiting_first` 并删旧的。检查 test_state.py:59 附近,改其断言与新规则一致。

- [ ] **Step 4: 运行确认通过 + 回归**

Run: `cd companion && python3 -m pytest tests/test_state.py -v && python3 -m pytest -q`
Expected: PASS;全量全绿(若 `test_sort_active_first` 红,按 Step 3 注释更新它)。

- [ ] **Step 5: Commit**

```bash
git add companion/codey/state.py companion/tests/test_state.py
git commit -m "feat(companion): provider.limited 限流标志 + 会话排序 waiting 置顶"
```

---

### Task 4: config + 配置台 summary/branch 列开关

**Files:**
- Modify: `companion/codey/config.py:23-27`(DEFAULTS.display.columns)
- Modify: `companion/web/admin.html`(配置标签页列开关区)
- Test: `companion/tests/test_config.py`

- [ ] **Step 1: 写失败测试**(追加到 test_config.py 的配置类内)

```python
    def test_default_columns_include_summary_branch(self):
        from codey import config
        cols = config.get("display")["columns"]
        self.assertIn("summary", cols)
        self.assertIn("branch", cols)
        self.assertTrue(cols["summary"])
        self.assertTrue(cols["branch"])
```

- [ ] **Step 2: 运行确认失败**

Run: `cd companion && python3 -m pytest tests/test_config.py -k summary_branch -v`
Expected: FAIL — `assertIn('summary', cols)` KeyError/缺键

- [ ] **Step 3: 实现**

`config.py` DEFAULTS 的 columns 增两键:

```python
        "columns": {"status": True, "model": True, "ctx": True,
                    "tokens": True, "memory": True, "turn": True,
                    "summary": True, "branch": True},
```

> `_deep_merge_display` 用 `for k in merged[group]:` 迭代,新键自动深合并,无需改它。

`web/admin.html`:在列开关勾选区(现有 status/model/ctx/tokens/memory/turn 的 checkbox 组)末尾加两项,沿用同样的绑定属性命名:

```html
<label><input type="checkbox" data-col="summary"> 摘要</label>
<label><input type="checkbox" data-col="branch"> 分支</label>
```

（具体属性名以现有 6 个 checkbox 的写法为准——打开 admin.html 找到 `data-col="status"` 那段,在 `turn` 之后照抄两行,改 key 与中文标签。）

- [ ] **Step 4: 运行确认通过 + 回归 + 目视配置台**

Run: `cd companion && python3 -m pytest tests/test_config.py -v && python3 -m pytest -q`
Expected: PASS;全量全绿。
手动:`./deploy.sh start --bg` 后浏览器开 `/codey/config` 标签,确认「摘要」「分支」勾选项出现且可存。

- [ ] **Step 5: Commit**

```bash
git add companion/codey/config.py companion/web/admin.html companion/tests/test_config.py
git commit -m "feat(companion): 配置台增 summary/branch 列开关"
```

---

## Phase B — VLW 字体管线

### Task 5: VLW 生成器 `scripts/gen_vlw.py`

VLW 格式已源码核实(TFT_eSPI `Smooth_font.cpp` + LovyanGFX `lgfx_fonts.cpp::VLWfont`):全字段 32-bit big-endian;文件头 6×int32(glyphCount/version/fontSize/mboxY/ascent/descent);每字形 7×int32(unicode/h/w/xadvance/gdY/gdX/pad),**按 unicode 升序**(LovyanGFX 用 `lower_bound` 查找);位图区紧跟所有 metric 之后,每字形 w×h 字节 0–255 alpha,无 pitch 填充。

**Files:**
- Create: `scripts/gen_vlw.py`

- [ ] **Step 1: 装依赖**

Run: `python3 -m pip install freetype-py`
Expected: 安装 `freetype-py`(≥2.13)。

- [ ] **Step 2: 写生成器(含逆向自检)**

Create `scripts/gen_vlw.py`:

```python
#!/usr/bin/env python3
"""TTF 子集 -> VLW(LovyanGFX 8-bit alpha 抗锯齿字体)-> C 头数组。

VLW 格式(全 32-bit big-endian,源码核实自 TFT_eSPI/LovyanGFX):
  头:glyphCount, version(11), fontSize(px), mboxY(0), ascent(px), descent(px)
  每字形(unicode 升序):unicode, height, width, xAdvance, gdY(bitmap_top), gdX(bitmap_left), pad
  位图:所有 metric 之后连续,每字形 w*h 字节 0-255 alpha,无 pitch 填充。
用法:gen_vlw.py <ttf> <size_px> <out.h> <array_name> [--text STR | --ascii]
"""
import argparse
import struct
import sys

import freetype


def _i32(v):
    return struct.pack(">i", int(v))


def render_vlw(ttf_path, codepoints, size_px, face_index=0):
    face = freetype.Face(ttf_path, index=face_index)
    face.set_pixel_sizes(0, size_px)
    cps = sorted(set(int(c) for c in codepoints))   # 升序去重(LovyanGFX 硬性要求)
    glyphs, max_a, max_d = [], 0, 0
    for cp in cps:
        face.load_char(cp, freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_NORMAL)
        g = face.glyph
        bmp = g.bitmap
        w, h, pitch, src = bmp.width, bmp.rows, bmp.pitch, bmp.buffer
        pix = bytearray(w * h)
        for row in range(h):
            base = row * pitch
            pix[row * w:row * w + w] = bytes(src[base:base + w])
        glyphs.append({"u": cp & 0xFFFF, "h": h, "w": w,
                       "adv": g.advance.x >> 6, "dy": g.bitmap_top, "dx": g.bitmap_left,
                       "bmp": bytes(pix)})
        max_a = max(max_a, g.bitmap_top)
        max_d = max(max_d, h - g.bitmap_top)
    out = bytearray()
    out += _i32(len(glyphs)) + _i32(11) + _i32(size_px) + _i32(0) + _i32(max_a) + _i32(max_d)
    for g in glyphs:
        out += _i32(g["u"]) + _i32(g["h"]) + _i32(g["w"]) + _i32(g["adv"]) + _i32(g["dy"]) + _i32(g["dx"]) + _i32(0)
    for g in glyphs:
        out += g["bmp"]
    return bytes(out), glyphs


def parse_vlw_check(data, glyphs):
    """逆向校验:头/metric/bitmap 终点与升序,失败抛 AssertionError。"""
    gc = struct.unpack(">i", data[0:4])[0]
    assert gc == len(glyphs), "glyphCount mismatch"
    off = 24
    us = []
    total_bmp = 0
    for _ in range(gc):
        u, h, w = struct.unpack(">iii", data[off:off + 12])
        us.append(u)
        total_bmp += w * h
        off += 28
    assert us == sorted(us), "glyphs not ascending"
    assert off + total_bmp == len(data), "bitmap region size mismatch"


def to_header(data, name):
    lines = [f"#pragma once", f"// generated by scripts/gen_vlw.py — do not edit",
             f"static const unsigned char {name}[] = {{"]
    for i in range(0, len(data), 16):
        lines.append("  " + ",".join(str(b) for b in data[i:i + 16]) + ",")
    lines.append("};")
    lines.append(f"static const unsigned long {name}_len = {len(data)};")
    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ttf")
    ap.add_argument("size", type=int)
    ap.add_argument("out")
    ap.add_argument("name")
    ap.add_argument("--text", default="")
    ap.add_argument("--ascii", action="store_true")
    ap.add_argument("--face-index", type=int, default=0)
    a = ap.parse_args()
    cps = set()
    if a.ascii:
        cps |= set(range(0x20, 0x7F))        # 可打印 ASCII
        cps |= {0x00B7, 0x2026}              # · 中点(行2分隔) 与 … 省略号
    cps |= {ord(c) for c in a.text}
    if not cps:
        print("no codepoints (use --ascii or --text)", file=sys.stderr)
        sys.exit(2)
    data, glyphs = render_vlw(a.ttf, cps, a.size, a.face_index)
    parse_vlw_check(data, glyphs)            # 自检
    with open(a.out, "w") as f:
        f.write(to_header(data, a.name))
    print(f"{a.out}: {len(glyphs)} glyphs, {len(data)} bytes -> {a.name}")


if __name__ == "__main__":
    main()
```

- [ ] **Step 3: 冒烟自检(用任意系统 TTF 验证生成器不报错)**

Run:
```bash
python3 scripts/gen_vlw.py "/System/Library/Fonts/Supplemental/Arial.ttf" 20 /tmp/smoke.h Smoke20 --ascii
```
Expected: 打印 `/tmp/smoke.h: ~97 glyphs, NNNN bytes -> Smoke20`,无 AssertionError(self-check 通过)。

- [ ] **Step 4: Commit**

```bash
git add scripts/gen_vlw.py
git commit -m "feat(tools): VLW 抗锯齿字体生成器(freetype-py,含格式自检)"
```

---

### Task 6: 生成 VLW 字体头

**Files:**
- Create: `sketches/codey_dash/fonts/jbmono_{16,20,28}.h`、`sketches/codey_dash/fonts/grotesk_{20,28}.h`

- [ ] **Step 1: 取 OFL 字体 TTF**(放 `/tmp`,不入库)

Run(GitHub 走代理):
```bash
mkdir -p /tmp/ttf
curl -L --proxy 127.0.0.1:7892 -o /tmp/ttf/JetBrainsMono-Medium.ttf \
  https://github.com/JetBrains/JetBrainsMono/raw/master/fonts/ttf/JetBrainsMono-Medium.ttf
curl -L --proxy 127.0.0.1:7892 -o /tmp/ttf/SpaceGrotesk-Medium.ttf \
  https://github.com/floriankarsten/space-grotesk/raw/master/fonts/ttf/SpaceGrotesk-Medium.ttf
```
Expected: 两个 TTF 下载到 `/tmp/ttf/`(各几十~几百 KB)。

- [ ] **Step 2: 生成 5 个 VLW 头(Latin 子集 + · …)**

Run:
```bash
cd /Users/zyc/code/Codey && mkdir -p sketches/codey_dash/fonts
python3 scripts/gen_vlw.py /tmp/ttf/JetBrainsMono-Medium.ttf 16 sketches/codey_dash/fonts/jbmono_16.h JBMono16 --ascii
python3 scripts/gen_vlw.py /tmp/ttf/JetBrainsMono-Medium.ttf 20 sketches/codey_dash/fonts/jbmono_20.h JBMono20 --ascii
python3 scripts/gen_vlw.py /tmp/ttf/JetBrainsMono-Medium.ttf 28 sketches/codey_dash/fonts/jbmono_28.h JBMono28 --ascii
python3 scripts/gen_vlw.py /tmp/ttf/SpaceGrotesk-Medium.ttf 20 sketches/codey_dash/fonts/grotesk_20.h Grotesk20 --ascii
python3 scripts/gen_vlw.py /tmp/ttf/SpaceGrotesk-Medium.ttf 28 sketches/codey_dash/fonts/grotesk_28.h Grotesk28 --ascii
ls -la sketches/codey_dash/fonts/
```
Expected: 5 个 `.h`,各打印 ~97 glyphs;合计目测 < 250KB。

- [ ] **Step 3: Commit**(字体头入库,生成器可复现)

```bash
git add sketches/codey_dash/fonts/
git commit -m "feat(firmware): 生成 VLW 抗锯齿字体头(JetBrains Mono + Space Grotesk)"
```

---

## Phase C — 固件纯函数(host-testable,TDD)

### Task 7: composeMetaLine + DispCol 扩展 + statusRank

**Files:**
- Modify: `sketches/codey_dash/codey_ui.h`(DispCol enum、statusRank、新增 composeMetaLine)
- Test: `sketches/codey_dash/test/codey_ui_test.cpp`

- [ ] **Step 1: 写失败测试**(在 codey_ui_test.cpp 的 `statusRank` 断言行下方替换 + 追加)

把现有这一行:
```cpp
  assert(statusRank(ST_EXECUTING) == 0 && statusRank(ST_THINKING) == 1 && statusRank(ST_WAITING) == 2 && statusRank(ST_DONE) == 2);
```
改为(waiting 优先):
```cpp
  assert(statusRank(ST_WAITING) == 0 && statusRank(ST_EXECUTING) == 1 && statusRank(ST_THINKING) == 2 && statusRank(ST_DONE) == 3);
```

并在 `// ---- fmtK ----` 块之前追加 composeMetaLine 测试:
```cpp
  // ---- composeMetaLine: 行2点分串,按列开关取段 ----
  {
    DispCfg d = dispDefault();                 // 全开
    char line[96];
    composeMetaLine(&d, "Opus4.8", 71, "229M", 567, "59M", line, sizeof(line));
    assert(!strcmp(line, "Opus4.8 \xC2\xB7 71% \xC2\xB7 229M \xC2\xB7 t567 \xC2\xB7 59M"));

    d.col[DC_MODEL] = false; d.col[DC_MEMORY] = false;   // 关 model + memory
    composeMetaLine(&d, "Opus4.8", 71, "229M", 567, "59M", line, sizeof(line));
    assert(!strcmp(line, "71% \xC2\xB7 229M \xC2\xB7 t567"));

    DispCfg e = dispDefault();
    for (int i = 0; i < DISP_NCOL; i++) e.col[i] = false; // 全关 -> 空串
    composeMetaLine(&e, "Opus4.8", 71, "229M", 567, "59M", line, sizeof(line));
    assert(line[0] == 0);
  }
```

- [ ] **Step 2: 运行确认失败**

Run: `cd sketches/codey_dash/test && ./run_tests.sh`
Expected: 编译失败(`composeMetaLine` / `DC_*` 未定义)或断言失败。

- [ ] **Step 3: 实现**

`codey_ui.h`:DispCol enum 扩展两列(`DC_SUMMARY`/`DC_BRANCH`),`DISP_NCOL` 6→8:
```cpp
enum DispCol { DC_STATUS = 0, DC_MODEL = 1, DC_CTX = 2, DC_TOKENS = 3,
               DC_MEMORY = 4, DC_TURN = 5, DC_SUMMARY = 6, DC_BRANCH = 7 };
static const int DISP_NCOL  = 8;
```

statusRank 改 waiting 优先:
```cpp
static inline int statusRank(SessStatus s) {
  return s == ST_WAITING ? 0 : s == ST_EXECUTING ? 1 : s == ST_THINKING ? 2 : 3;
}
```

> `layoutColumns` 及常量 `DISP_COL_W`/`DISP_BAND_CENTER`/`DISP_STATUS_WORD_DX` 在 Task 10 列表改造后不再被引用,届时删除(本任务先保留以免编译断裂——它们用 `DISP_NCOL` 维度的数组,把 `DISP_COL_W` 也补到 8 个元素:`{66,92,38,58,58,44,0,0}`,summary/branch 不参与旧定位)。

新增纯函数(放 statusRank 下方):
```cpp
// 行2 元信息点分串:按列开关取 model/ctx/tokens/turn/memory 段,分隔符 " · "(U+00B7)。
// summary/branch 不在此行(分别走行3/行1)。返回写入长度。
static inline int composeMetaLine(const DispCfg* cfg, const char* modelShort, int ctxPct,
                                  const char* tokensStr, int turn, const char* memStr,
                                  char* out, size_t n) {
  if (!out || n == 0) return 0;
  out[0] = 0;
  size_t len = 0;
  char seg[40];
  for (int col = DC_MODEL; col <= DC_MEMORY; col++) {
    if (!cfg->col[col]) continue;
    switch (col) {
      case DC_MODEL:  snprintf(seg, sizeof(seg), "%s", modelShort ? modelShort : ""); break;
      case DC_CTX:    snprintf(seg, sizeof(seg), "%d%%", ctxPct); break;
      case DC_TOKENS: snprintf(seg, sizeof(seg), "%s", tokensStr ? tokensStr : ""); break;
      case DC_TURN:   snprintf(seg, sizeof(seg), "t%d", turn); break;
      case DC_MEMORY: snprintf(seg, sizeof(seg), "%s", memStr ? memStr : ""); break;
      default: continue;
    }
    if (seg[0] == 0) continue;
    const char* sep = (len > 0) ? " \xC2\xB7 " : "";   // " · " 仅在非首段前
    int w = snprintf(out + len, n - len, "%s%s", sep, seg);
    if (w < 0 || (size_t)w >= n - len) break;          // 截断保护
    len += w;
  }
  return (int)len;
}
```

- [ ] **Step 4: 运行确认通过**

Run: `cd sketches/codey_dash/test && ./run_tests.sh`
Expected: `codey_ui tests: ALL PASS`

- [ ] **Step 5: Commit**

```bash
git add sketches/codey_dash/codey_ui.h sketches/codey_dash/test/codey_ui_test.cpp
git commit -m "feat(firmware): composeMetaLine 行2点分串 + DispCol 增 summary/branch + statusRank waiting 优先"
```

---

## Phase D — 固件渲染(依赖 B、C;真机目视)

### Task 8: Sess 结构 + JSON 解析新字段

**Files:**
- Modify: `sketches/codey_dash/session_store.h`(Sess 结构)
- Modify: `sketches/codey_dash/codey_dash.ino:1283-1299`(parseSession)、`:1334-1340`(display 列解析)

- [ ] **Step 1: Sess 结构加字段**

`session_store.h` 的 `struct Sess` 内(在 `long tokTotal;` 附近)加:
```cpp
  long    tokIn, tokOut, tokCacheR, tokCacheW;   // token 四件套(与 tokTotal 同 long)
  int     compactions;                            // 上下文压缩次数
  char    summary[64];                            // 会话摘要(首条提示截断)
```

- [ ] **Step 2: parseSession 解析新字段**

`codey_dash.ino` 的 parseSession(第 1283-1301 行区块),在 `s.tokTotal = ...` 之后加:
```cpp
  s.tokIn     = so["tokens"]["in"]      | 0L;
  s.tokOut    = so["tokens"]["out"]     | 0L;
  s.tokCacheR = so["tokens"]["cache_r"] | 0L;
  s.tokCacheW = so["tokens"]["cache_w"] | 0L;
  s.compactions = so["compactions"]     | 0;
  copyStr(s.summary, sizeof(s.summary), so["summary"] | "");
```

- [ ] **Step 3: display 列解析加 summary/branch**

`codey_dash.ino` 的 display 解析(第 1335-1340 行,`d.col[DC_*] = cols[...]` 那组)末尾加:
```cpp
          d.col[DC_SUMMARY] = cols["summary"] | true;
          d.col[DC_BRANCH]  = cols["branch"]  | true;
```

- [ ] **Step 4: 编译验证**

Run: `cd /Users/zyc/code/Codey && ./scripts/build.sh sketches/codey_dash 2>&1 | tail -4`
Expected: 编译通过(字段已声明并解析,暂未用于渲染)。

- [ ] **Step 5: Commit**

```bash
git add sketches/codey_dash/session_store.h sketches/codey_dash/codey_dash.ino
git commit -m "feat(firmware): Sess 解析 tokens 四件套/compactions/summary + display 增列"
```

---

### Task 9: VLW 字体加载 + 主页(等待横幅 + 速率)

> **字体切换策略(先确认):** LovyanGFX 一张 canvas 同时只持有一个 loadFont 字体。为避免每帧反复 `loadFont`(重解析头 + 重建索引),策略是**按字体分组绘制**:每页渲染时,需要某 VLW 时 `cv.loadFont(JBMono16)` 一次、画完该字体所有文本、再 `cv.loadFont` 下一个;中文段切回 `cv.unloadFont(); cv.setFont(&fonts::efontCN_16)`。先在本任务里跑通一次「loadFont→drawString→unloadFont」确认渲染正确,再铺到各页。

**Files:**
- Modify: `sketches/codey_dash/codey_dash.ino`(include 字体头、renderUsagePage、新增 drawWaitBanner)

- [ ] **Step 1: include 字体头**

`codey_dash.ino` 顶部 include 区(`#include "session_store.h"` 之后)加:
```cpp
#include "fonts/jbmono_16.h"
#include "fonts/jbmono_20.h"
#include "fonts/jbmono_28.h"
#include "fonts/grotesk_20.h"
#include "fonts/grotesk_28.h"
```

- [ ] **Step 2: 新增等待横幅绘制函数**(放 renderUsagePage 之前)

```cpp
// 跨端等待提醒:返回首个 waiting 会话,画橙色胶囊;limited 则红色 RATE LIMITED。
// 命中区写入 g_bannerHit(供 handleAction 点击直达)。无 waiting 返回 false。
static int g_bannerTop = 0, g_bannerH = 0, g_bannerProv = -1, g_bannerIdx = -1;
static bool drawWaitBanner(int provIdx) {
  const Prov& p = PROV[provIdx];
  int widx = -1;
  for (int i = 0; i < p.nsess; i++) if ((SessStatus)p.sess[i].status == ST_WAITING) { widx = i; break; }
  g_bannerProv = -1; g_bannerIdx = -1;
  if (widx < 0 && !p.limited) return false;
  const int by = 70, bh = 26; g_bannerTop = by - bh/2; g_bannerH = bh;
  char buf[64];
  uint32_t col = p.limited ? 0xff5d5d : 0xffa94d;
  if (p.limited) snprintf(buf, sizeof(buf), "RATE LIMITED");
  else { g_bannerProv = provIdx; g_bannerIdx = widx;
         char nm[24]; truncCp(p.sess[widx].name, 10, nm, sizeof(nm));
         snprintf(buf, sizeof(buf), "%s 等你输入", nm); }
  cv.fillRoundRect(CX - 110, g_bannerTop, 220, bh, 13, c565(shade(col, -0.78f)));
  cv.drawRoundRect(CX - 110, g_bannerTop, 220, bh, 13, c565(col));
  cv.fillSmoothCircle(CX - 92, by, 4, c565(col));
  cv.setFont(&fonts::efontCN_16); cv.setTextDatum(middle_center); cv.setTextColor(c565(col));
  cv.drawString(buf, CX + 6, by);
  return true;
}
```
> `Prov` 需有 `limited` 字段:在 Prov 结构里加 `bool limited;`,并在 provider JSON 解析处(第 1352 行附近)加 `PROV[i].limited = pr["limited"] | false;`。

- [ ] **Step 3: renderUsagePage 接入横幅 + 速率 + VLW 数字**

在 `renderUsagePage` 里:mascot 绘制之后调用 `drawWaitBanner(provIdx);`(横幅在 mascot 上方,有 waiting 才显示)。

把 `drawSessionCount` 那行的渲染改为带速率:在 `drawSessionCount(344, ...)` 之后补一行速率(VLW 等宽):
```cpp
  { char rate[24]; fmtTokens(p.tokPerMin, rate, sizeof(rate));
    char line[32]; snprintf(line, sizeof(line), "%s/min", rate);
    cv.loadFont(JBMono16); cv.setTextDatum(middle_center); cv.setTextColor(c565(0x8a8d94));
    cv.drawString(line, CX, 368); cv.unloadFont(); }
```

- [ ] **Step 4: 编译 + flash 占用检查**

Run: `cd /Users/zyc/code/Codey && ./scripts/build.sh sketches/codey_dash 2>&1 | tail -4`
Expected: 编译通过;打印 flash 占用 < 90%(若超,回 Task 6 减字号)。

- [ ] **Step 5: Commit**

```bash
git add sketches/codey_dash/codey_dash.ino
git commit -m "feat(firmware): 主页等待横幅(limited 红/waiting 橙)+ tok/min 速率 + VLW 数字"
```

---

### Task 10: 列表页 3 行卡片行 + waiting 高亮 + 退役 layoutColumns

**Files:**
- Modify: `sketches/codey_dash/codey_dash.ino:690-799`(renderListPage)、rowHitAt 命中几何
- Modify: `sketches/codey_dash/codey_ui.h`(删 layoutColumns 及其常量)

- [ ] **Step 1: 重写 renderListPage 行布局**

把 `ROW_H` 40→64、`LIST_MAX_VIS` 6→4(第 637 行 `static const int ROW_H = 40, LIST_MAX_VIS = 6;`):
```cpp
static const int ROW_H = 64, LIST_MAX_VIS = 4;
```

renderListPage 内每行三行文本(替换原 layoutColumns 列循环为固定三行绘制)。每行 `y` 起点不变(居中带 + 滚动逻辑保留),行内:
- **行1**(y+14):状态点(`statusDotColor`)+ 名称(Grotesk20)+ 若 `DC_BRANCH` 开则 ` <branch> +a~m`(灰,efontCN_16,因分支名可能含非 ASCII 保险用 efont 或 JBMono16)+ 状态词右对齐(JBMono16,状态色)。
- **行2**(y+34):`composeMetaLine(...)` 串,**JBMono16**;含 ` · ` 故必须 JBMono16(子集含 U+00B7)。
- **行3**(y+52):若 executing 且 task 非空 → `⚙ <task>`(efontCN_16,因 task 可能中文);否则 summary(efontCN_16)。截断省略号 `truncCp`。

waiting 行高亮:在行背景处,`if (st == ST_WAITING)` 画橙色左边条 + 淡橙底:
```cpp
      if (st == ST_WAITING) {
        cv.fillRoundRect(46, y + 3, SIZE - 92, ROW_H - 6, 6, c565(shade(0xffa94d, -0.82f)));
        cv.fillRect(46, y + 3, 4, ROW_H - 6, c565(0xffa94d));
      } else if (st == ST_EXECUTING) {
        cv.fillRoundRect(46, y + 3, SIZE - 92, ROW_H - 6, 6, c565(shade(color, -0.82f)));
      }
```
> 字体切换按「行2 用 JBMono load 一段、行1/3 中文段 unloadFont+efont」分组;同一帧内同字体的行尽量连续绘制以减少 loadFont 次数(可接受每帧 ~2 次 load 切换)。

- [ ] **Step 2: 同步 rowHitAt 命中几何**

`rowHitAt`(列表点击命中行)用到的 `g_listBandTop/g_listOff/ROW_H`——ROW_H 改了自动跟随,确认命中检测仍用 `ROW_H` 计算行索引(检查 rowHitAt 函数体,把任何硬编码 40 改 ROW_H,无硬编码则不动)。

- [ ] **Step 3: 删除 layoutColumns 死代码**

`codey_ui.h` 删除 `layoutColumns` 函数及常量 `DISP_COL_W`/`DISP_BAND_CENTER`/`DISP_STATUS_WORD_DX`(grep 确认 .ino 无引用后删):
Run: `grep -rn "layoutColumns\|DISP_COL_W\|DISP_BAND_CENTER\|DISP_STATUS_WORD_DX" sketches/codey_dash/`
Expected: 删除后仅剩定义处→一并删;若 .ino 仍有引用先清理引用。同步删 codey_ui_test.cpp 里 layoutColumns 的测试(若有)。

- [ ] **Step 4: 编译 + 主机测试**

Run: `cd /Users/zyc/code/Codey && ./scripts/build.sh sketches/codey_dash 2>&1 | tail -3 && (cd sketches/codey_dash/test && ./run_tests.sh)`
Expected: 编译通过;`codey_ui tests: ALL PASS`。

- [ ] **Step 5: Commit**

```bash
git add sketches/codey_dash/codey_dash.ino sketches/codey_dash/codey_ui.h sketches/codey_dash/test/codey_ui_test.cpp
git commit -m "feat(firmware): 列表页 3 行卡片(摘要/分支)+ waiting 置顶高亮;退役 layoutColumns"
```

---

### Task 11: 详情页 token4 + 摘要 + 压缩角标 + WAITING 转橙

**Files:**
- Modify: `sketches/codey_dash/codey_dash.ino:814-908`(renderDetailPage)

- [ ] **Step 1: WAITING 状态词转橙**

renderDetailPage 内状态词颜色(原 `st == ST_THINKING ? 0xffd479 : 0x8b9097`)把 waiting 改橙:
```cpp
  uint16_t sw = c565(st == ST_EXECUTING ? shade(color, 0.25f)
                   : st == ST_THINKING ? 0xffd479
                   : st == ST_WAITING ? 0xffa94d : 0x8b9097);
```

- [ ] **Step 2: CTX 宫格压缩角标**

三宫格绘制 CTX 块后,若 `s.compactions > 0` 在其右上角标 `Nc`(橙,JBMono16)。在 `drawStatTile(... "CTX" ...)` 之后:
```cpp
  if (g_disp.col[DC_CTX] && s.compactions > 0) {
    char cc[8]; snprintf(cc, sizeof(cc), "%dc", s.compactions);
    cv.loadFont(JBMono16); cv.setTextDatum(top_right); cv.setTextColor(c565(0xffa94d));
    cv.drawString(cc, ctxTileRightX, 210); cv.unloadFont();   // ctxTileRightX = CTX 块右边缘 x
  }
```
> `ctxTileRightX` 用 CTX 宫格的 `cx0 + 0*(tw+gap) + tw/2` 算右边缘(参照三宫格几何 tw=96)。

- [ ] **Step 3: token 四件套行(放空闲带 y≈268,三宫格 228 与任务行 288 之间)**

现详情垂直布局(已核实):头部 20–68、mascot 118、状态词 180、三宫格中心 228(h=50→约 203–253)、任务跑马灯 288、git/sub/ports 312、底部点 410。空闲带 ~258–278 放 token4:
```cpp
  { char a[12], b[12], cr[12], cw[12];
    fmtTokens(s.tokIn, a, sizeof(a)); fmtTokens(s.tokOut, b, sizeof(b));
    fmtTokens(s.tokCacheR, cr, sizeof(cr)); fmtTokens(s.tokCacheW, cw, sizeof(cw));
    char line[64];
    if (detailProv == 1) snprintf(line, sizeof(line), "in %s  out %s  cR %s", a, b, cr);  // codex 无 cW
    else                 snprintf(line, sizeof(line), "in %s out %s cR %s cW %s", a, b, cr, cw);
    cv.loadFont(JBMono16); cv.setTextDatum(middle_center); cv.setTextColor(c565(0x8a8d94));
    cv.drawString(line, CX, 268); cv.unloadFont(); }
```

- [ ] **Step 4: 摘要复用任务行(不新增行,避免拥挤)**

现状任务跑马灯行(y=288)逻辑是 `s.task[0] ? "* <task>" : "* idle"`。把 idle 兜底改为显示摘要——**无活动任务时该行展示会话摘要**,有任务时仍显任务。改 renderDetailPage 内构造 `buf` 那两行:
```cpp
  if (s.task[0])          snprintf(buf, sizeof(buf), "* %s", s.task);
  else if (s.summary[0])  snprintf(buf, sizeof(buf), "\xE2\x80\x9C%s\xE2\x80\x9D", s.summary);  // “摘要”
  else                    snprintf(buf, sizeof(buf), "* idle");
```
（跑马灯/居中/字体逻辑不变,efontCN 已能渲染中文与引号。）

- [ ] **Step 5: 编译 + flash 检查**

Run: `cd /Users/zyc/code/Codey && ./scripts/build.sh sketches/codey_dash 2>&1 | tail -4`
Expected: 编译通过;flash < 90%。

- [ ] **Step 6: Commit**

```bash
git add sketches/codey_dash/codey_dash.ino
git commit -m "feat(firmware): 详情页 token 四件套行 + 摘要 + ctx 压缩角标 + WAITING 转橙"
```

---

### Task 12: 交互——横幅点击直达 + BtnA 短按返回/长按设置

**Files:**
- Modify: `sketches/codey_dash/codey_dash.ino`(detectTouchAction/handleAction、btnAShort/btnALong)

- [ ] **Step 1: 横幅命中 → 详情直达**

`detectTouchAction`(或 handleAction 的 ACT_TAP 分支)在主页(非 list、非 detail)时,若点击落在横幅区(`g_bannerIdx>=0` 且 y 在 `[g_bannerTop, g_bannerTop+g_bannerH]`),进入该会话详情:
```cpp
      // 主页 ACT_TAP:先判等待横幅
      if (detailProv < 0 && !g_listView && g_bannerProv >= 0 &&
          g_tLastY >= g_bannerTop && g_tLastY <= g_bannerTop + g_bannerH) {
        enterDetail(g_bannerProv, g_bannerIdx); break;
      }
```
（放在 ACT_TAP 分支最前,命中则 break,不再走「主页→列表」。）

- [ ] **Step 2: BtnA 短按逐级返回 / 长按设置**

`btnAShort`(原:详情翻会话 / 否则切端)改为逐级返回(息屏先亮屏)。实际符号:dim 标志是 `g_dim`,亮度 `g_bright`,无独立 wake 函数——直接复位亮度即可(下个 IMU tick 会按静止再判 dim):
```cpp
static void btnAShort() {
  if (g_dim) { g_dim = false; M5.Display.setBrightness(g_bright); return; }   // 息屏 → 亮屏
  if (detailProv >= 0) { exitDetail(); return; }     // 详情 → 列表
  if (g_listView) { g_listView = false; return; }    // 列表 → 主页
}
```
`btnALong`(原:逐级返回)改为进设置页。设置页标志是 `g_inSettings`(line 1602 双键也切它,保留;此处多一个入口):
```cpp
static void btnALong() { g_inSettings = true; g_setSel = 0; }   // 进设置
```
> 切端原靠 BtnA 短按,现已由横滑承担(handleAction ACT_SWIPE 切端保留)。详情翻会话仍由横滑承担。符号 `g_dim`(line 130)、`g_bright`(line 125)、`g_inSettings`(line 1602)、`g_setSel`(line 124)均为现有,无需新增。

- [ ] **Step 3: 编译 + 主机测试 + 全量 companion 测试**

Run:
```bash
cd /Users/zyc/code/Codey && ./scripts/build.sh sketches/codey_dash 2>&1 | tail -3
(cd sketches/codey_dash/test && ./run_tests.sh)
(cd companion && python3 -m pytest -q)
```
Expected: 编译通过;固件主机测试 ALL PASS;companion 全绿。

- [ ] **Step 4: Commit**

```bash
git add sketches/codey_dash/codey_dash.ino
git commit -m "feat(firmware): 等待横幅点击直达 + BtnA 短按逐级返回/长按设置"
```

---

## Phase E — 整体验证

### Task 13: 全量回归 + flash 预算 + 真机清单

- [ ] **Step 1: 三套测试 + 构建全绿**

Run:
```bash
cd /Users/zyc/code/Codey
(cd companion && python3 -m pytest -q)
(cd sketches/codey_dash/test && ./run_tests.sh)
./scripts/build.sh sketches/codey_dash 2>&1 | tail -4
```
Expected: companion 全绿;固件主机测试 ALL PASS;固件编译通过,**flash 占用打印 < 90%**(超则减 Task 6 字号/字符集)。

- [ ] **Step 2: companion 端到端目视(假数据即可)**

Run: `cd companion && ./deploy.sh start --bg && curl -s --noproxy '*' http://127.0.0.1:8787/codey/state | python3 -m json.tool | grep -E "summary|tokens|compactions|limited" | head`
Expected: state JSON 含 `summary`/`tokens`(in/out/cache_r/cache_w)/`compactions`/`limited` 字段。

- [ ] **Step 3: 真机清单(USB 接上后,与现有待刷分支一并)**

`./scripts/flash.sh sketches/codey_dash` 后逐项目视:
- VLW 数字/标签抗锯齿(对比旧位图,无颗粒感)
- 列表页 3 行卡片;waiting 行置顶 + 橙高亮
- 主页等待横幅;点击横幅直达详情
- 详情页 token4 行 / 摘要 / 压缩角标 / WAITING 橙
- BtnA 短按逐级返回、长按进设置;横滑切端/翻会话
- 中文摘要(efont)与 Latin VLW 混排可读

- [ ] **Step 4: 最终 commit(如有真机微调)**

```bash
git add -A && git commit -m "chore(firmware): 真机目视微调 UI 重构"
```

---

## Self-Review

- **Spec 覆盖**:组件0 字体=Task5/6/7/9–11;组件1 数据层=Task1–4;组件2 主页=Task9;组件3 列表=Task10;组件4 详情=Task11;组件5 交互=Task12——全覆盖。
- **排序落点修正**:spec 写「firmware statusRank」,实际排序在 companion `_RANK`(Task3);firmware statusRank 当前未被引用,仍按 spec 改为 waiting 优先以保持一致(Task7),并提示更新旧测试 `test_sort_active_first`。
- **类型一致**:token 字段 companion 为 int、固件为 `long`(与 `tokTotal` 一致,Task8);`composeMetaLine` 签名 Task7 定义、Task10 调用一致;`DISP_NCOL` 6→8 在 Task7 改、Task8 解析、Task10 退役 layoutColumns 后无悬挂引用。
- **占位扫描**:无 TBD;render 任务给出具体 y 坐标与代码,标注「真机目视微调」属正常迭代而非占位。
- **风险**:① 字体切换 loadFont 每帧开销——Task9 先验证并按字体分组绘制;② 详情页 y 坐标拥挤——以真机目视微调;③ flash 预算——每个固件任务 build 检查 <90%。
