# Companion 会话采集层 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 abtop 的本地会话采集算法移植进 Codey companion,扩展 `GET /codey/state`,为每个 Claude/Codex 会话输出 name/status/model/context/token/turn 及详情字段。

**Architecture:** 在 `companion/lib/` 新建一组**纯函数小模块**(parser + derivation),逐个 TDD;文件系统/进程的副作用 IO 收敛在薄封装里。`server.js` 把这些模块组装进现有 `buildState()`,给每个 provider 增加 `sessions[]` / `active_count` / `agg`,账号额度(边缘弧)逻辑保持不变。

**Tech Stack:** Node.js(CommonJS),内置 `node:test` + `node:assert`(零新依赖),`ps`/`lsof`/`git`(execFile)。

参考设计:`docs/superpowers/specs/2026-06-03-agent-session-monitor-design.md`
参考算法源:`/Users/zyc/code/abtop`(只读,勿改)。

---

## 文件结构

```
companion/
  server.js                 # 修改:buildState() 组装 sessions[],新增端点数据
  lib/
    util.js                 # 新建:clampPct, truncate, projectName
    contextWindow.js        # 新建:contextWindowFor(model, maxTok)
    syntheticUser.js        # 新建:isSyntheticUserMessage(entry)
    transcriptClaude.js     # 新建:parseClaudeTranscript(text)
    codexRollout.js         # 新建:parseCodexRollout(text)
    deriveStatus.js         # 新建:deriveStatus({...}) + classifyClaudeTurnState
    processTree.js          # 新建:parsePs, descendantsOf, parseLsofListen
    gitStats.js             # 新建:parseGitShortstat + readGitStats(cwd) IO
    buildSession.js         # 新建:buildClaudeSession / buildCodexSession (纯组装)
    discover.js             # 新建:发现会话(文件系统 IO)
  test/
    *.test.js               # 每模块一份
```

约定:所有 parser 为**纯函数**(入参是字符串/已解析对象,出参是新对象,不读文件、不变异入参)。

---

## Task 0: 测试框架接线

**Files:**
- Modify: `companion/package.json`

- [ ] **Step 1: 加 test 脚本**

把 `package.json` 的 `scripts` 改为:

```json
  "scripts": {
    "start": "node server.js",
    "test": "node --test"
  },
```

- [ ] **Step 2: 验证 runner 可用**

Run: `cd companion && npm test`
Expected: 退出码 0,输出类似 `tests 0 / pass 0`(暂无测试文件时 node --test 报 “no test files found” 也可,继续)。

- [ ] **Step 3: Commit**

```bash
git add companion/package.json
git commit -m "chore(companion): 接入 node --test 测试框架"
```

---

## Task 1: util — clampPct / truncate / projectName

**Files:**
- Create: `companion/lib/util.js`
- Test: `companion/test/util.test.js`

- [ ] **Step 1: 写失败测试**

```js
// companion/test/util.test.js
const { test } = require('node:test');
const assert = require('node:assert');
const { clampPct, truncate, projectName } = require('../lib/util');

test('clampPct 取整并夹到 0..100', () => {
  assert.equal(clampPct(-5), 0);
  assert.equal(clampPct(47.6), 48);
  assert.equal(clampPct(150), 100);
});

test('truncate 超长加省略号(按码点)', () => {
  assert.equal(truncate('abcdef', 4), 'abc…');
  assert.equal(truncate('短', 4), '短');
  assert.equal(truncate('项目名称很长啊', 4), '项目名…');
});

test('projectName 取 cwd 末段', () => {
  assert.equal(projectName('/Users/zyc/code/Codey'), 'Codey');
  assert.equal(projectName('/Users/zyc/code/Codey/'), 'Codey');
  assert.equal(projectName(''), '');
});
```

- [ ] **Step 2: 运行,确认失败**

Run: `cd companion && node --test test/util.test.js`
Expected: FAIL — `Cannot find module '../lib/util'`.

- [ ] **Step 3: 实现**

```js
// companion/lib/util.js
const clampPct = (x) => Math.max(0, Math.min(100, Math.round(Number(x) || 0)));

// 按码点截断,超出加 '…'(总长含省略号 <= max)
function truncate(s, max) {
  const str = String(s || '');
  const cp = Array.from(str);
  if (cp.length <= max) return str;
  return cp.slice(0, Math.max(0, max - 1)).join('') + '…';
}

function projectName(cwd) {
  const parts = String(cwd || '').split('/').filter(Boolean);
  return parts.length ? parts[parts.length - 1] : '';
}

module.exports = { clampPct, truncate, projectName };
```

- [ ] **Step 4: 运行,确认通过**

Run: `cd companion && node --test test/util.test.js`
Expected: PASS(3 tests）。

- [ ] **Step 5: Commit**

```bash
git add companion/lib/util.js companion/test/util.test.js
git commit -m "feat(companion): util — clampPct/truncate/projectName"
```

---

## Task 2: contextWindowFor — 上下文窗口判定

**Files:**
- Create: `companion/lib/contextWindow.js`
- Test: `companion/test/contextWindow.test.js`

abtop 规则:模型名含 `[1m]` 或观测到的 max context tokens > 200_000 → 1_000_000,否则 200_000。

- [ ] **Step 1: 写失败测试**

```js
// companion/test/contextWindow.test.js
const { test } = require('node:test');
const assert = require('node:assert');
const { contextWindowFor } = require('../lib/contextWindow');

test('普通模型 = 200k', () => {
  assert.equal(contextWindowFor('claude-opus-4-8', 0), 200000);
  assert.equal(contextWindowFor('sonnet', 150000), 200000);
});

test('[1m] 后缀 = 1M', () => {
  assert.equal(contextWindowFor('sonnet[1m]', 0), 1000000);
});

test('观测 token 超 200k = 1M', () => {
  assert.equal(contextWindowFor('whatever', 250000), 1000000);
});
```

- [ ] **Step 2: 运行,确认失败**

Run: `cd companion && node --test test/contextWindow.test.js`
Expected: FAIL — 模块缺失。

- [ ] **Step 3: 实现**

```js
// companion/lib/contextWindow.js
function contextWindowFor(model, maxObservedTokens) {
  const m = String(model || '').toLowerCase();
  if (m.includes('[1m]') || Number(maxObservedTokens) > 200000) return 1000000;
  return 200000;
}
module.exports = { contextWindowFor };
```

- [ ] **Step 4: 运行,确认通过**

Run: `cd companion && node --test test/contextWindow.test.js`
Expected: PASS。

- [ ] **Step 5: Commit**

```bash
git add companion/lib/contextWindow.js companion/test/contextWindow.test.js
git commit -m "feat(companion): contextWindowFor 上下文窗口判定"
```

---

## Task 3: isSyntheticUserMessage — 合成 user 行识别

**Files:**
- Create: `companion/lib/syntheticUser.js`
- Test: `companion/test/syntheticUser.test.js`

合成 user 行(不算"用户在等模型回复"):`isMeta:true`、全部 block 为 `tool_result`、或文本以 `<command-name>`/`<bash-input>`/`<bash-stdout>`/`<local-command-...>` 开头。

- [ ] **Step 1: 写失败测试**

```js
// companion/test/syntheticUser.test.js
const { test } = require('node:test');
const assert = require('node:assert');
const { isSyntheticUserMessage } = require('../lib/syntheticUser');

test('真实用户 prompt 不是合成', () => {
  assert.equal(isSyntheticUserMessage({ type: 'user', message: { content: '帮我改 bug' } }), false);
  assert.equal(isSyntheticUserMessage({ type: 'user', message: { content: [{ type: 'text', text: 'hi' }] } }), false);
});

test('isMeta / tool_result / 命令包裹 都是合成', () => {
  assert.equal(isSyntheticUserMessage({ type: 'user', isMeta: true, message: { content: 'x' } }), true);
  assert.equal(isSyntheticUserMessage({ type: 'user', message: { content: [{ type: 'tool_result', content: 'ok' }] } }), true);
  assert.equal(isSyntheticUserMessage({ type: 'user', message: { content: '<command-name>/clear</command-name>' } }), true);
  assert.equal(isSyntheticUserMessage({ type: 'user', message: { content: [{ type: 'text', text: '<bash-input>ls</bash-input>' }] } }), true);
});
```

- [ ] **Step 2: 运行,确认失败**

Run: `cd companion && node --test test/syntheticUser.test.js`
Expected: FAIL — 模块缺失。

- [ ] **Step 3: 实现**

```js
// companion/lib/syntheticUser.js
const CMD_PREFIX = /^\s*<(command-name|command-message|command-args|bash-input|bash-stdout|bash-stderr|local-command-[a-z]+)>/;

function textOf(content) {
  if (typeof content === 'string') return content;
  if (Array.isArray(content)) {
    const t = content.find((b) => b && b.type === 'text');
    return t ? String(t.text || '') : '';
  }
  return '';
}

function isSyntheticUserMessage(entry) {
  if (!entry || entry.type !== 'user') return false;
  if (entry.isMeta === true) return true;
  const content = entry.message && entry.message.content;
  if (Array.isArray(content) && content.length > 0 && content.every((b) => b && b.type === 'tool_result')) return true;
  if (CMD_PREFIX.test(textOf(content))) return true;
  return false;
}

module.exports = { isSyntheticUserMessage, textOf };
```

- [ ] **Step 4: 运行,确认通过**

Run: `cd companion && node --test test/syntheticUser.test.js`
Expected: PASS。

- [ ] **Step 5: Commit**

```bash
git add companion/lib/syntheticUser.js companion/test/syntheticUser.test.js
git commit -m "feat(companion): isSyntheticUserMessage 合成行识别"
```

---

## Task 4: parseClaudeTranscript — token / 轮次 / 模型 / 当前任务

**Files:**
- Create: `companion/lib/transcriptClaude.js`
- Test: `companion/test/transcriptClaude.test.js`

逐行解析 JSONL。累计 token;turn=assistant 轮数;model=最后一条 assistant 的 model;currentTask=最新 assistant 轮里最后一个 tool_use 的 `"{tool} {arg}"`(每个新 assistant 轮清空);gitBranch/version 取最后非空;firstPrompt=首条非合成 user 文本;lastContextTokens 见设计 §2.2;lastUserTsMs=末尾若是非合成 user 行则其时间戳否则 0;pendingTool=末尾是带 tool_use 的 assistant。

- [ ] **Step 1: 写失败测试**

```js
// companion/test/transcriptClaude.test.js
const { test } = require('node:test');
const assert = require('node:assert');
const { parseClaudeTranscript } = require('../lib/transcriptClaude');

const A = (usage, content, model = 'claude-opus-4-8', ts = '2026-06-03T10:00:00.000Z') =>
  JSON.stringify({ type: 'assistant', timestamp: ts, message: { model, usage, content } });
const U = (content, extra = {}) =>
  JSON.stringify({ type: 'user', timestamp: '2026-06-03T10:00:05.000Z', version: '1.2.3', gitBranch: 'main', message: { content }, ...extra });

test('累计 token、turn、model', () => {
  const text = [
    U('帮我改 server.js'),
    A({ input_tokens: 100, output_tokens: 20, cache_read_input_tokens: 0, cache_creation_input_tokens: 500 },
      [{ type: 'text', text: '好的' }]),
    U([{ type: 'tool_result', content: 'done' }]),
    A({ input_tokens: 80, output_tokens: 10, cache_read_input_tokens: 600, cache_creation_input_tokens: 0 },
      [{ type: 'tool_use', name: 'Edit', input: { file_path: '/Users/zyc/code/Codey/companion/server.js' } }]),
  ].join('\n');
  const r = parseClaudeTranscript(text);
  assert.equal(r.turnCount, 2);
  assert.equal(r.model, 'claude-opus-4-8');
  assert.equal(r.totalInput, 180);
  assert.equal(r.totalOutput, 30);
  assert.equal(r.totalCacheRead, 600);
  assert.equal(r.totalCacheCreate, 500);
  assert.equal(r.version, '1.2.3');
  assert.equal(r.gitBranch, 'main');
  assert.equal(r.firstPrompt, '帮我改 server.js');
});

test('currentTask = 末轮最后一个 tool_use,取路径末两段', () => {
  const text = [
    A({ input_tokens: 80, output_tokens: 10, cache_read_input_tokens: 600, cache_creation_input_tokens: 0 },
      [{ type: 'tool_use', name: 'Edit', input: { file_path: '/Users/zyc/code/Codey/companion/server.js' } }]),
  ].join('\n');
  const r = parseClaudeTranscript(text);
  assert.equal(r.currentTask, 'Edit companion/server.js');
  assert.equal(r.pendingTool, true);
});

test('lastContextTokens:cache_read 为 0 且有 cache_create 用 input+create,否则 input+cache_read', () => {
  const fresh = parseClaudeTranscript(
    A({ input_tokens: 100, output_tokens: 0, cache_read_input_tokens: 0, cache_creation_input_tokens: 500 }, [{ type: 'text', text: 'x' }]));
  assert.equal(fresh.lastContextTokens, 600);
  const warm = parseClaudeTranscript(
    A({ input_tokens: 80, output_tokens: 0, cache_read_input_tokens: 600, cache_creation_input_tokens: 0 }, [{ type: 'text', text: 'x' }]));
  assert.equal(warm.lastContextTokens, 680);
});

test('末尾是真实 user → modelGenerating(lastUserTsMs>0);坏行被跳过', () => {
  const text = [
    'this is not json',
    A({ input_tokens: 10, output_tokens: 5, cache_read_input_tokens: 0, cache_creation_input_tokens: 0 }, [{ type: 'text', text: 'hi' }]),
    U('再帮我看一下'),
  ].join('\n');
  const r = parseClaudeTranscript(text);
  assert.ok(r.lastUserTsMs > 0);
  assert.equal(r.pendingTool, false);
});

test('末尾合成 user(tool_result)不算 modelGenerating', () => {
  const text = [
    A({ input_tokens: 10, output_tokens: 5, cache_read_input_tokens: 0, cache_creation_input_tokens: 0 },
      [{ type: 'tool_use', name: 'Bash', input: { command: 'npm test\n更多' } }]),
    U([{ type: 'tool_result', content: 'ok' }]),
  ].join('\n');
  const r = parseClaudeTranscript(text);
  assert.equal(r.lastUserTsMs, 0);
});
```

- [ ] **Step 2: 运行,确认失败**

Run: `cd companion && node --test test/transcriptClaude.test.js`
Expected: FAIL — 模块缺失。

- [ ] **Step 3: 实现**

```js
// companion/lib/transcriptClaude.js
const { isSyntheticUserMessage, textOf } = require('./syntheticUser');

function toolArg(name, input) {
  const i = input || {};
  if (name === 'Read' || name === 'Edit' || name === 'Write') {
    const segs = String(i.file_path || '').split('/').filter(Boolean);
    return segs.slice(-2).join('/');
  }
  if (name === 'Bash') return String(i.command || '').split('\n')[0].slice(0, 40);
  if (name === 'Grep' || name === 'Glob') return String(i.pattern || '');
  return '';
}

function parseClaudeTranscript(text) {
  const lines = String(text || '').split('\n');
  const r = {
    totalInput: 0, totalOutput: 0, totalCacheRead: 0, totalCacheCreate: 0,
    lastContextTokens: 0, maxContextTokens: 0, turnCount: 0,
    model: '', version: '', gitBranch: '', currentTask: '', firstPrompt: '',
    lastUserTsMs: 0, pendingTool: false,
  };
  for (const line of lines) {
    const s = line.trim();
    if (!s) continue;
    let e;
    try { e = JSON.parse(s); } catch (_) { continue; }      // 坏行跳过

    if (e.type === 'assistant' && e.message) {
      const u = e.message.usage || {};
      const inp = Number(u.input_tokens) || 0;
      const out = Number(u.output_tokens) || 0;
      const cr = Number(u.cache_read_input_tokens) || 0;
      const cc = Number(u.cache_creation_input_tokens) || 0;
      r.totalInput += inp; r.totalOutput += out; r.totalCacheRead += cr; r.totalCacheCreate += cc;
      const ctx = (cr === 0 && cc > 0) ? (inp + cc) : (inp + cr);
      r.lastContextTokens = ctx;
      if (ctx > r.maxContextTokens) r.maxContextTokens = ctx;
      if (e.message.model) r.model = e.message.model;
      r.turnCount += 1;
      // currentTask = 本轮最后一个 tool_use(新轮覆盖)
      let task = '';
      let hasTool = false;
      for (const b of (e.message.content || [])) {
        if (b && b.type === 'tool_use') { hasTool = true; task = (b.name + ' ' + toolArg(b.name, b.input)).trim(); }
      }
      r.currentTask = task;
      r.pendingTool = hasTool;           // 末尾若是该 assistant 则仍 pending(后续 user 行会覆盖)
      r.lastUserTsMs = 0;                // assistant 之后清除 user 等待标记
    } else if (e.type === 'user') {
      if (e.version) r.version = e.version;
      if (e.gitBranch) r.gitBranch = e.gitBranch;
      const synthetic = isSyntheticUserMessage(e);
      if (!synthetic && !r.firstPrompt) r.firstPrompt = textOf(e.message && e.message.content).trim();
      r.pendingTool = false;             // user 行(含 tool_result)闭合上一个 assistant 的工具
      r.lastUserTsMs = synthetic ? 0 : (Date.parse(e.timestamp) || 0);
    }
  }
  return r;
}

module.exports = { parseClaudeTranscript, toolArg };
```

- [ ] **Step 4: 运行,确认通过**

Run: `cd companion && node --test test/transcriptClaude.test.js`
Expected: PASS(5 tests）。

- [ ] **Step 5: Commit**

```bash
git add companion/lib/transcriptClaude.js companion/test/transcriptClaude.test.js
git commit -m "feat(companion): parseClaudeTranscript token/turn/model/任务解析"
```

---

## Task 5: deriveStatus — 会话状态判定

**Files:**
- Create: `companion/lib/deriveStatus.js`
- Test: `companion/test/deriveStatus.test.js`

`deriveStatus({ hasActiveDescendant, pendingTool, modelGenerating, done })` → `'executing' | 'thinking' | 'waiting' | 'done'`。优先级:done > executing(有后代 CPU 活跃 或 pendingTool) > thinking(modelGenerating) > waiting。

- [ ] **Step 1: 写失败测试**

```js
// companion/test/deriveStatus.test.js
const { test } = require('node:test');
const assert = require('node:assert');
const { deriveStatus } = require('../lib/deriveStatus');

test('优先级', () => {
  assert.equal(deriveStatus({ done: true }), 'done');
  assert.equal(deriveStatus({ hasActiveDescendant: true }), 'executing');
  assert.equal(deriveStatus({ pendingTool: true }), 'executing');
  assert.equal(deriveStatus({ modelGenerating: true }), 'thinking');
  assert.equal(deriveStatus({}), 'waiting');
  assert.equal(deriveStatus({ pendingTool: true, modelGenerating: true }), 'executing');
});
```

- [ ] **Step 2: 运行,确认失败**

Run: `cd companion && node --test test/deriveStatus.test.js`
Expected: FAIL — 模块缺失。

- [ ] **Step 3: 实现**

```js
// companion/lib/deriveStatus.js
function deriveStatus({ hasActiveDescendant, pendingTool, modelGenerating, done } = {}) {
  if (done) return 'done';
  if (hasActiveDescendant || pendingTool) return 'executing';
  if (modelGenerating) return 'thinking';
  return 'waiting';
}
module.exports = { deriveStatus };
```

- [ ] **Step 4: 运行,确认通过**

Run: `cd companion && node --test test/deriveStatus.test.js`
Expected: PASS。

- [ ] **Step 5: Commit**

```bash
git add companion/lib/deriveStatus.js companion/test/deriveStatus.test.js
git commit -m "feat(companion): deriveStatus 会话状态判定"
```

---

## Task 6: processTree — ps / lsof 解析

**Files:**
- Create: `companion/lib/processTree.js`
- Test: `companion/test/processTree.test.js`

macOS:`ps -axo pid,ppid,rss,pcpu,comm`、`lsof -nP -iTCP -sTCP:LISTEN`。这里只测**纯解析**与后代收集。

- [ ] **Step 1: 写失败测试**

```js
// companion/test/processTree.test.js
const { test } = require('node:test');
const assert = require('node:assert');
const { parsePs, descendantsOf, parseLsofListen } = require('../lib/processTree');

const PS = `  PID  PPID    RSS %CPU COMM
    1     0   1000  0.0 /sbin/launchd
  100     1  50000  7.5 node
  200   100  20000  0.1 npm
  300   200  10000  0.0 esbuild
  400     1   5000  0.0 other`;

test('parsePs 建 pid->info', () => {
  const m = parsePs(PS);
  assert.equal(m.get(100).ppid, 1);
  assert.equal(m.get(100).cpu, 7.5);
  assert.equal(m.get(100).rssKb, 50000);
  assert.equal(m.get(100).comm, 'node');
});

test('descendantsOf 递归收集', () => {
  const m = parsePs(PS);
  const d = descendantsOf(m, 100).sort((a, b) => a - b);
  assert.deepEqual(d, [200, 300]);
  assert.deepEqual(descendantsOf(m, 400), []);
});

test('parseLsofListen 解析 pid->ports', () => {
  const LSOF = `COMMAND   PID USER   FD   TYPE             DEVICE SIZE/OFF NODE NAME
node      300 zyc   25u  IPv4 0x1234      0t0  TCP *:3000 (LISTEN)
node      300 zyc   26u  IPv6 0x5678      0t0  TCP [::1]:5173 (LISTEN)
node      999 zyc   27u  IPv4 0x9999      0t0  TCP 127.0.0.1:8787 (LISTEN)`;
  const p = parseLsofListen(LSOF);
  assert.deepEqual(p.get(300).sort((a, b) => a - b), [3000, 5173]);
  assert.deepEqual(p.get(999), [8787]);
});
```

- [ ] **Step 2: 运行,确认失败**

Run: `cd companion && node --test test/processTree.test.js`
Expected: FAIL — 模块缺失。

- [ ] **Step 3: 实现**

```js
// companion/lib/processTree.js
const { execFile } = require('child_process');

function parsePs(text) {
  const map = new Map();
  const lines = String(text || '').split('\n');
  for (const line of lines) {
    const m = line.trim().match(/^(\d+)\s+(\d+)\s+(\d+)\s+([\d.]+)\s+(.*)$/);
    if (!m) continue;                               // 跳过表头/空行
    const pid = Number(m[1]);
    map.set(pid, { pid, ppid: Number(m[2]), rssKb: Number(m[3]), cpu: Number(m[4]), comm: m[5].trim() });
  }
  return map;
}

function descendantsOf(map, rootPid) {
  const childrenByParent = new Map();
  for (const info of map.values()) {
    if (!childrenByParent.has(info.ppid)) childrenByParent.set(info.ppid, []);
    childrenByParent.get(info.ppid).push(info.pid);
  }
  const out = [];
  const seen = new Set([rootPid]);
  const stack = [...(childrenByParent.get(rootPid) || [])];
  while (stack.length) {
    const pid = stack.pop();
    if (seen.has(pid)) continue;
    seen.add(pid);
    out.push(pid);
    for (const c of (childrenByParent.get(pid) || [])) stack.push(c);
  }
  return out;
}

function parseLsofListen(text) {
  const ports = new Map();
  for (const line of String(text || '').split('\n')) {
    if (!/\(LISTEN\)/.test(line)) continue;
    const pidM = line.match(/^\S+\s+(\d+)/);
    const portM = line.match(/:(\d+)\s+\(LISTEN\)/);
    if (!pidM || !portM) continue;
    const pid = Number(pidM[1]); const port = Number(portM[1]);
    if (!ports.has(pid)) ports.set(pid, []);
    if (!ports.get(pid).includes(port)) ports.get(pid).push(port);
  }
  return ports;
}

// --- IO 封装(运行期用,不在单测覆盖) ---
function run(cmd, args, timeout = 4000) {
  return new Promise((resolve) => {
    execFile(cmd, args, { timeout, maxBuffer: 8 * 1024 * 1024 }, (err, stdout) => resolve(err ? '' : stdout));
  });
}
const readPs = async () => parsePs(await run('ps', ['-axo', 'pid,ppid,rss,pcpu,comm']));
const readListenPorts = async () => parseLsofListen(await run('lsof', ['-nP', '-iTCP', '-sTCP:LISTEN']));

module.exports = { parsePs, descendantsOf, parseLsofListen, readPs, readListenPorts };
```

- [ ] **Step 4: 运行,确认通过**

Run: `cd companion && node --test test/processTree.test.js`
Expected: PASS（3 tests）。

- [ ] **Step 5: Commit**

```bash
git add companion/lib/processTree.js companion/test/processTree.test.js
git commit -m "feat(companion): processTree — ps/lsof 解析与后代收集"
```

---

## Task 7: gitStats — diff --shortstat 解析

**Files:**
- Create: `companion/lib/gitStats.js`
- Test: `companion/test/gitStats.test.js`

- [ ] **Step 1: 写失败测试**

```js
// companion/test/gitStats.test.js
const { test } = require('node:test');
const assert = require('node:assert');
const { parseGitShortstat } = require('../lib/gitStats');

test('解析 insertions/deletions', () => {
  assert.deepEqual(parseGitShortstat(' 3 files changed, 12 insertions(+), 5 deletions(-)'), { added: 12, modified: 5 });
  assert.deepEqual(parseGitShortstat(' 1 file changed, 4 insertions(+)'), { added: 4, modified: 0 });
  assert.deepEqual(parseGitShortstat(''), { added: 0, modified: 0 });
});
```

- [ ] **Step 2: 运行,确认失败**

Run: `cd companion && node --test test/gitStats.test.js`
Expected: FAIL — 模块缺失。

- [ ] **Step 3: 实现**

```js
// companion/lib/gitStats.js
const { execFile } = require('child_process');

function parseGitShortstat(text) {
  const s = String(text || '');
  const ins = s.match(/(\d+) insertion/);
  const del = s.match(/(\d+) deletion/);
  return { added: ins ? Number(ins[1]) : 0, modified: del ? Number(del[1]) : 0 };
}

function git(cwd, args, timeout = 4000) {
  return new Promise((resolve) => {
    execFile('git', ['-C', cwd, ...args], { timeout, maxBuffer: 4 * 1024 * 1024 },
      (err, stdout) => resolve(err ? '' : stdout.trim()));
  });
}

async function readGitStats(cwd) {
  const branch = await git(cwd, ['rev-parse', '--abbrev-ref', 'HEAD']);
  const stat = await git(cwd, ['diff', '--shortstat']);
  return { branch: branch || '', ...parseGitShortstat(stat) };
}

module.exports = { parseGitShortstat, readGitStats };
```

- [ ] **Step 4: 运行,确认通过**

Run: `cd companion && node --test test/gitStats.test.js`
Expected: PASS。

- [ ] **Step 5: Commit**

```bash
git add companion/lib/gitStats.js companion/test/gitStats.test.js
git commit -m "feat(companion): gitStats — shortstat 解析 + readGitStats"
```

---

## Task 8: parseCodexRollout — Codex rollout 解析

**Files:**
- Create: `companion/lib/codexRollout.js`
- Test: `companion/test/codexRollout.test.js`

Codex JSONL 事件:`session_meta{ id, cwd, cli_version, originator, git.branch }`、`turn_context{ model, effort, model_context_window }`、`event_msg.token_count{ info.total_token_usage, info.last_token_usage, rate_limits }`、`event_msg.{user_message,agent_message}`、`event_msg.task_complete`、`response_item.function_call{ name, arguments }`。Codex 的 `input_tokens` 已含 `cached_input_tokens` → totalInput 存非缓存部分,cache_read 单列。rate_limits 按 `window_minutes<=300` 分 5h/周。

- [ ] **Step 1: 写失败测试**

```js
// companion/test/codexRollout.test.js
const { test } = require('node:test');
const assert = require('node:assert');
const { parseCodexRollout } = require('../lib/codexRollout');

test('解析 meta / turn_context / token_count / rate_limits', () => {
  const text = [
    JSON.stringify({ type: 'session_meta', payload: { id: 'abc', cwd: '/Users/zyc/code/webapp', cli_version: '0.9', originator: 'codex-cli', git: { branch: 'dev' } } }),
    JSON.stringify({ type: 'turn_context', payload: { model: 'gpt-5.1-codex', effort: 'high', model_context_window: 272000 } }),
    JSON.stringify({ type: 'event_msg', payload: { type: 'user_message', message: '加个接口' } }),
    JSON.stringify({ type: 'response_item', payload: { type: 'function_call', name: 'shell', arguments: '{"command":"ls"}' } }),
    JSON.stringify({ type: 'event_msg', payload: { type: 'token_count',
      info: { total_token_usage: { input_tokens: 1000, cached_input_tokens: 600, output_tokens: 200 },
              last_token_usage: { input_tokens: 800, cached_input_tokens: 600 } },
      rate_limits: { primary: { window_minutes: 300, used_percent: 42, resets_at: '2026-06-03T12:00:00Z' },
                     secondary: { window_minutes: 10080, used_percent: 18, resets_at: '2026-06-09T00:00:00Z' } } } }),
  ].join('\n');
  const r = parseCodexRollout(text);
  assert.equal(r.sessionId, 'abc');
  assert.equal(r.cwd, '/Users/zyc/code/webapp');
  assert.equal(r.model, 'gpt-5.1-codex');
  assert.equal(r.effort, 'high');
  assert.equal(r.contextWindow, 272000);
  assert.equal(r.gitBranch, 'dev');
  assert.equal(r.firstPrompt, '加个接口');
  assert.equal(r.currentTask, 'shell ls');
  assert.equal(r.totalInput, 400);          // 1000 - 600 cached
  assert.equal(r.totalCacheRead, 600);
  assert.equal(r.totalOutput, 200);
  assert.equal(r.lastContextTokens, 800);
  assert.equal(r.turnCount, 1);
  assert.equal(r.fiveHourPct, 42);
  assert.equal(r.weeklyPct, 18);
  assert.equal(r.done, false);
});

test('task_complete → done', () => {
  const text = JSON.stringify({ type: 'event_msg', payload: { type: 'task_complete' } });
  assert.equal(parseCodexRollout(text).done, true);
});
```

- [ ] **Step 2: 运行,确认失败**

Run: `cd companion && node --test test/codexRollout.test.js`
Expected: FAIL — 模块缺失。

- [ ] **Step 3: 实现**

```js
// companion/lib/codexRollout.js
function codexToolArg(name, argsJson) {
  try {
    const a = JSON.parse(argsJson || '{}');
    if (a.command) return String(Array.isArray(a.command) ? a.command.join(' ') : a.command).split('\n')[0].slice(0, 40);
    if (a.path) return String(a.path).split('/').slice(-2).join('/');
    if (a.pattern) return String(a.pattern);
  } catch (_) {}
  return '';
}

function parseCodexRollout(text) {
  const r = {
    sessionId: '', cwd: '', cliVersion: '', originator: '', model: '', effort: '',
    contextWindow: 0, gitBranch: '', firstPrompt: '', currentTask: '',
    totalInput: 0, totalOutput: 0, totalCacheRead: 0, lastContextTokens: 0, turnCount: 0,
    fiveHourPct: null, fiveHourResetsAt: null, weeklyPct: null, weeklyResetsAt: null, done: false,
  };
  for (const line of String(text || '').split('\n')) {
    const s = line.trim(); if (!s) continue;
    let e; try { e = JSON.parse(s); } catch (_) { continue; }
    const p = e.payload || {};
    if (e.type === 'session_meta') {
      r.sessionId = p.id || r.sessionId; r.cwd = p.cwd || r.cwd;
      r.cliVersion = p.cli_version || r.cliVersion; r.originator = p.originator || r.originator;
      if (p.git && p.git.branch) r.gitBranch = p.git.branch;
    } else if (e.type === 'turn_context') {
      if (p.model) r.model = p.model;
      if (p.effort) r.effort = p.effort;
      if (p.model_context_window) r.contextWindow = Number(p.model_context_window) || r.contextWindow;
    } else if (e.type === 'response_item' && p.type === 'function_call') {
      r.currentTask = (String(p.name || '') + ' ' + codexToolArg(p.name, p.arguments)).trim();
    } else if (e.type === 'event_msg') {
      if (p.type === 'user_message') { r.turnCount += 1; if (!r.firstPrompt) r.firstPrompt = String(p.message || '').trim(); }
      else if (p.type === 'task_complete') r.done = true;
      else if (p.type === 'token_count') {
        const tot = (p.info && p.info.total_token_usage) || {};
        const last = (p.info && p.info.last_token_usage) || {};
        const cached = Number(tot.cached_input_tokens) || 0;
        r.totalInput = Math.max(0, (Number(tot.input_tokens) || 0) - cached);
        r.totalCacheRead = cached;
        r.totalOutput = Number(tot.output_tokens) || 0;
        if (last.input_tokens != null) r.lastContextTokens = Number(last.input_tokens) || 0;
        const rl = p.rate_limits || {};
        for (const slot of ['primary', 'secondary']) {
          const w = rl[slot]; if (!w) continue;
          const resets = w.resets_at ? Math.floor(Date.parse(w.resets_at) / 1000) || null : null;
          if (Number(w.window_minutes) <= 300) { r.fiveHourPct = Number(w.used_percent); r.fiveHourResetsAt = resets; }
          else { r.weeklyPct = Number(w.used_percent); r.weeklyResetsAt = resets; }
        }
      }
    }
  }
  if (!r.contextWindow) r.contextWindow = 272000;     // Codex 默认窗口兜底
  return r;
}

module.exports = { parseCodexRollout, codexToolArg };
```

- [ ] **Step 4: 运行,确认通过**

Run: `cd companion && node --test test/codexRollout.test.js`
Expected: PASS（2 tests）。

- [ ] **Step 5: Commit**

```bash
git add companion/lib/codexRollout.js companion/test/codexRollout.test.js
git commit -m "feat(companion): parseCodexRollout 解析 Codex 会话"
```

---

## Task 9: buildSession — 纯组装为对外 Session 对象

**Files:**
- Create: `companion/lib/buildSession.js`
- Test: `companion/test/buildSession.test.js`

把解析结果 + 进程/git/端口/子agent 组装成 §2.5 的 session 对象。纯函数,便于单测。

- [ ] **Step 1: 写失败测试**

```js
// companion/test/buildSession.test.js
const { test } = require('node:test');
const assert = require('node:assert');
const { buildClaudeSession, buildCodexSession } = require('../lib/buildSession');

test('buildClaudeSession 组装并算 context_pct', () => {
  const parsed = {
    totalInput: 180, totalOutput: 30, totalCacheRead: 600, totalCacheCreate: 500,
    lastContextTokens: 94000, maxContextTokens: 94000, turnCount: 23,
    model: 'claude-opus-4-8', currentTask: 'Edit companion/server.js',
    gitBranch: 'main', firstPrompt: '帮我改', lastUserTsMs: 0, pendingTool: true,
  };
  const s = buildClaudeSession({
    sessionId: 'sid1', cwd: '/Users/zyc/code/Codey', startedAt: 1780000000000,
    parsed, status: 'executing', git: { branch: 'main', added: 3, modified: 12 },
    ports: [3000, 5173], subagents: 2, effort: 'high',
  });
  assert.equal(s.id, 'sid1');
  assert.equal(s.name, 'Codey');
  assert.equal(s.status, 'executing');
  assert.equal(s.model, 'claude-opus-4-8');
  assert.equal(s.context_window, 200000);
  assert.equal(s.context_tokens, 94000);
  assert.equal(s.context_pct, 47);                       // 94000/200000
  assert.equal(s.tokens_total, 180 + 30 + 600 + 500);
  assert.equal(s.turn, 23);
  assert.deepEqual(s.git, { branch: 'main', added: 3, modified: 12 });
  assert.equal(s.current_task, 'Edit companion/server.js');
  assert.deepEqual(s.ports, [3000, 5173]);
  assert.equal(s.subagents, 2);
  assert.equal(s.effort, 'high');
  assert.equal(s.started_at, 1780000000);                // 秒
});

test('buildCodexSession 用 rollout 字段', () => {
  const parsed = {
    sessionId: 'cx1', cwd: '/Users/zyc/code/webapp', model: 'gpt-5.1-codex', effort: 'high',
    contextWindow: 272000, gitBranch: 'dev', currentTask: 'shell ls',
    totalInput: 400, totalOutput: 200, totalCacheRead: 600, lastContextTokens: 136000, turnCount: 5,
  };
  const s = buildCodexSession({ parsed, startedAt: 1780000000000, status: 'thinking',
    git: { branch: 'dev', added: 0, modified: 0 }, ports: [], subagents: 0 });
  assert.equal(s.id, 'cx1');
  assert.equal(s.name, 'webapp');
  assert.equal(s.context_window, 272000);
  assert.equal(s.context_pct, 50);                       // 136000/272000
  assert.equal(s.tokens_total, 400 + 200 + 600);
  assert.equal(s.turn, 5);
});
```

- [ ] **Step 2: 运行,确认失败**

Run: `cd companion && node --test test/buildSession.test.js`
Expected: FAIL — 模块缺失。

- [ ] **Step 3: 实现**

```js
// companion/lib/buildSession.js
const { clampPct, projectName } = require('./util');
const { contextWindowFor } = require('./contextWindow');

function buildClaudeSession({ sessionId, cwd, startedAt, parsed, status, git, ports, subagents, effort }) {
  const cw = contextWindowFor(parsed.model, parsed.maxContextTokens);
  return {
    id: sessionId,
    name: projectName(cwd),
    status,
    model: parsed.model || '',
    context_pct: clampPct((parsed.lastContextTokens / cw) * 100),
    context_tokens: parsed.lastContextTokens || 0,
    context_window: cw,
    tokens_total: (parsed.totalInput || 0) + (parsed.totalOutput || 0) + (parsed.totalCacheRead || 0) + (parsed.totalCacheCreate || 0),
    turn: parsed.turnCount || 0,
    git: git || { branch: parsed.gitBranch || '', added: 0, modified: 0 },
    current_task: parsed.currentTask || '',
    subagents: subagents || 0,
    ports: ports || [],
    started_at: Math.floor((startedAt || 0) / 1000),
    effort: effort || '',
  };
}

function buildCodexSession({ parsed, startedAt, status, git, ports, subagents }) {
  const cw = parsed.contextWindow || 272000;
  return {
    id: parsed.sessionId,
    name: projectName(parsed.cwd),
    status,
    model: parsed.model || '',
    context_pct: clampPct((parsed.lastContextTokens / cw) * 100),
    context_tokens: parsed.lastContextTokens || 0,
    context_window: cw,
    tokens_total: (parsed.totalInput || 0) + (parsed.totalOutput || 0) + (parsed.totalCacheRead || 0),
    turn: parsed.turnCount || 0,
    git: git || { branch: parsed.gitBranch || '', added: 0, modified: 0 },
    current_task: parsed.currentTask || '',
    subagents: subagents || 0,
    ports: ports || [],
    started_at: Math.floor((startedAt || 0) / 1000),
    effort: parsed.effort || '',
  };
}

module.exports = { buildClaudeSession, buildCodexSession };
```

- [ ] **Step 4: 运行,确认通过**

Run: `cd companion && node --test test/buildSession.test.js`
Expected: PASS（2 tests）。

- [ ] **Step 5: Commit**

```bash
git add companion/lib/buildSession.js companion/test/buildSession.test.js
git commit -m "feat(companion): buildSession 纯组装为对外 session 对象"
```

---

## Task 10: discover — 文件系统发现会话(IO)

**Files:**
- Create: `companion/lib/discover.js`
- Test: `companion/test/discover.test.js`

发现层做 IO(读 sessions/*.json、选 transcript、扫 codex rollout、数 subagents)。把可纯化的部分(选最新 transcript、数 subagents)抽成纯函数测试;整体发现用临时目录做集成测试。

- [ ] **Step 1: 写失败测试**

```js
// companion/test/discover.test.js
const { test } = require('node:test');
const assert = require('node:assert');
const os = require('os'); const fs = require('fs'); const path = require('path');
const { pickLatestTranscript, countSubagents, discoverClaudeSessions } = require('../lib/discover');

test('pickLatestTranscript 选 mtime≥startedAt 且未占用的最新 jsonl', () => {
  const files = [
    { sid: 'old', mtimeMs: 1000, path: '/p/old.jsonl' },
    { sid: 'new', mtimeMs: 5000, path: '/p/new.jsonl' },
    { sid: 'claimed', mtimeMs: 9000, path: '/p/claimed.jsonl' },
  ];
  const r = pickLatestTranscript(files, 2000, new Set(['claimed']));
  assert.equal(r.sid, 'new');
});

test('discoverClaudeSessions:临时目录里读 session 文件 + transcript', () => {
  const home = fs.mkdtempSync(path.join(os.tmpdir(), 'codey-home-'));
  const claude = path.join(home, '.claude');
  const sessions = path.join(claude, 'sessions');
  const encoded = '-Users-zyc-code-Demo';
  const projDir = path.join(claude, 'projects', encoded);
  fs.mkdirSync(sessions, { recursive: true });
  fs.mkdirSync(projDir, { recursive: true });
  const pid = process.pid;        // 用本进程 pid 保证“存活”
  fs.writeFileSync(path.join(sessions, pid + '.json'),
    JSON.stringify({ pid, sessionId: 'sid-demo', cwd: '/Users/zyc/code/Demo', startedAt: 1000 }));
  fs.writeFileSync(path.join(projDir, 'sid-demo.jsonl'),
    JSON.stringify({ type: 'assistant', timestamp: '2026-06-03T10:00:00Z',
      message: { model: 'claude-opus-4-8', usage: { input_tokens: 50, output_tokens: 10, cache_read_input_tokens: 0, cache_creation_input_tokens: 0 }, content: [{ type: 'text', text: 'hi' }] } }));

  const out = discoverClaudeSessions({ homeDirs: [claude], aliverPids: new Set([pid]) });
  assert.equal(out.length, 1);
  assert.equal(out[0].sessionId, 'sid-demo');
  assert.equal(out[0].cwd, '/Users/zyc/code/Demo');
  assert.ok(out[0].transcriptPath.endsWith('sid-demo.jsonl'));
  fs.rmSync(home, { recursive: true, force: true });
});

test('countSubagents 数 .meta.json', () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'codey-sub-'));
  fs.writeFileSync(path.join(dir, 'a.meta.json'), '{}');
  fs.writeFileSync(path.join(dir, 'b.meta.json'), '{}');
  fs.writeFileSync(path.join(dir, 'note.txt'), 'x');
  assert.equal(countSubagents(dir), 2);
  assert.equal(countSubagents('/no/such/dir'), 0);
  fs.rmSync(dir, { recursive: true, force: true });
});
```

- [ ] **Step 2: 运行,确认失败**

Run: `cd companion && node --test test/discover.test.js`
Expected: FAIL — 模块缺失。

- [ ] **Step 3: 实现**

```js
// companion/lib/discover.js
const fs = require('fs');
const os = require('os');
const path = require('path');

// 在一组候选 transcript 里选:mtime≥startedAt(含 5s 宽限)且 sid 未被占用的最新一个
function pickLatestTranscript(files, startedAtMs, claimedSids) {
  const grace = startedAtMs - 5000;
  return files
    .filter((f) => f.mtimeMs >= grace && !claimedSids.has(f.sid))
    .sort((a, b) => b.mtimeMs - a.mtimeMs)[0] || null;
}

function listTranscripts(projDir) {
  try {
    return fs.readdirSync(projDir)
      .filter((n) => n.endsWith('.jsonl'))
      .map((n) => ({ sid: n.replace(/\.jsonl$/, ''), path: path.join(projDir, n), mtimeMs: fs.statSync(path.join(projDir, n)).mtimeMs }));
  } catch (_) { return []; }
}

// cwd -> Claude project 目录名(把非字母数字换成 '-')
function encodeCwd(cwd) {
  return cwd.replace(/[^a-zA-Z0-9]/g, '-');
}

function countSubagents(subagentsDir) {
  try { return fs.readdirSync(subagentsDir).filter((n) => n.endsWith('.meta.json')).length; }
  catch (_) { return 0; }
}

// homeDirs: 形如 [~/.claude, ~/.claude-work];aliverPids: 存活 pid 集合
function discoverClaudeSessions({ homeDirs, aliverPids }) {
  const out = [];
  const claimed = new Set();
  for (const claude of homeDirs) {
    const sessDir = path.join(claude, 'sessions');
    let files = [];
    try { files = fs.readdirSync(sessDir).filter((n) => n.endsWith('.json')); } catch (_) { continue; }
    for (const fname of files) {
      let meta;
      try { meta = JSON.parse(fs.readFileSync(path.join(sessDir, fname), 'utf8')); } catch (_) { continue; }
      if (!meta.pid || !aliverPids.has(meta.pid)) continue;
      const projDir = path.join(claude, 'projects', encodeCwd(meta.cwd || ''));
      const transcripts = listTranscripts(projDir);
      const direct = transcripts.find((t) => t.sid === meta.sessionId);
      const chosen = direct || pickLatestTranscript(transcripts, meta.startedAt || 0, claimed);
      if (!chosen) continue;
      claimed.add(chosen.sid);
      out.push({
        configRoot: claude, pid: meta.pid, sessionId: chosen.sid, cwd: meta.cwd || '',
        startedAt: meta.startedAt || 0, transcriptPath: chosen.path,
        subagentsDir: path.join(projDir, chosen.sid, 'subagents'),
      });
    }
  }
  return out;
}

// 默认 Claude 配置目录:~/.claude* + $CLAUDE_CONFIG_DIR
function defaultClaudeHomeDirs() {
  const home = os.homedir();
  const dirs = new Set();
  if (process.env.CLAUDE_CONFIG_DIR) dirs.add(process.env.CLAUDE_CONFIG_DIR);
  try {
    for (const n of fs.readdirSync(home)) if (/^\.claude/.test(n)) {
      const p = path.join(home, n);
      if (fs.existsSync(path.join(p, 'sessions'))) dirs.add(p);
    }
  } catch (_) {}
  if (dirs.size === 0) dirs.add(path.join(home, '.claude'));
  return [...dirs];
}

// 扫 ~/.codex/sessions/**/rollout-*.jsonl,返回最近 N 个(按 mtime)
function listCodexRollouts(codexRoot, limit = 40) {
  const root = path.join(codexRoot, 'sessions');
  const out = [];
  const walk = (dir) => {
    let ents = [];
    try { ents = fs.readdirSync(dir, { withFileTypes: true }); } catch (_) { return; }
    for (const e of ents) {
      const p = path.join(dir, e.name);
      if (e.isDirectory()) walk(p);
      else if (/^rollout-.*\.jsonl$/.test(e.name)) { try { out.push({ path: p, mtimeMs: fs.statSync(p).mtimeMs }); } catch (_) {} }
    }
  };
  walk(root);
  return out.sort((a, b) => b.mtimeMs - a.mtimeMs).slice(0, limit);
}

module.exports = {
  pickLatestTranscript, listTranscripts, encodeCwd, countSubagents,
  discoverClaudeSessions, defaultClaudeHomeDirs, listCodexRollouts,
};
```

- [ ] **Step 4: 运行,确认通过**

Run: `cd companion && node --test test/discover.test.js`
Expected: PASS（3 tests）。

- [ ] **Step 5: Commit**

```bash
git add companion/lib/discover.js companion/test/discover.test.js
git commit -m "feat(companion): discover — 发现 Claude/Codex 会话(文件系统)"
```

---

## Task 11: collectSessions — 编排出两端 sessions[](IO 编排)

**Files:**
- Create: `companion/lib/collectSessions.js`
- Test: `companion/test/collectSessions.test.js`

把发现 + 解析 + 进程/端口/git/subagent + buildSession 串起来,产出 `{ claude: [...], codex: [...] }`。其中状态判定的 `hasActiveDescendant` 用进程树 CPU>5%。这里测**纯编排逻辑** `assembleStatusInputs`(给定 parsed + 后代 cpu),整体 collect 走运行期。

- [ ] **Step 1: 写失败测试**

```js
// companion/test/collectSessions.test.js
const { test } = require('node:test');
const assert = require('node:assert');
const { hasActiveDescendant, aggregateProvider } = require('../lib/collectSessions');

test('hasActiveDescendant:任一后代 CPU>5% 为真', () => {
  const psMap = new Map([
    [100, { pid: 100, ppid: 1, cpu: 0.0 }],
    [200, { pid: 200, ppid: 100, cpu: 7.0 }],
  ]);
  assert.equal(hasActiveDescendant(psMap, 100, 5), true);
  assert.equal(hasActiveDescendant(psMap, 100, 9), false);
});

test('aggregateProvider:active_count + dirty + tokens', () => {
  const sessions = [
    { status: 'executing', git: { added: 1, modified: 2 }, tokens_total: 1000 },
    { status: 'waiting', git: { added: 0, modified: 0 }, tokens_total: 500 },
    { status: 'thinking', git: { added: 0, modified: 3 }, tokens_total: 700 },
  ];
  const agg = aggregateProvider(sessions);
  assert.equal(agg.active_count, 2);            // executing + thinking
  assert.equal(agg.dirty_repos, 2);             // 两个有改动
  assert.equal(agg.tokens_total, 2200);
});
```

- [ ] **Step 2: 运行,确认失败**

Run: `cd companion && node --test test/collectSessions.test.js`
Expected: FAIL — 模块缺失。

- [ ] **Step 3: 实现**

```js
// companion/lib/collectSessions.js
const fs = require('fs');
const os = require('os');
const path = require('path');
const { parseClaudeTranscript } = require('./transcriptClaude');
const { parseCodexRollout } = require('./codexRollout');
const { deriveStatus } = require('./deriveStatus');
const { buildClaudeSession, buildCodexSession } = require('./buildSession');
const { readPs, readListenPorts, descendantsOf } = require('./processTree');
const { readGitStats } = require('./gitStats');
const discover = require('./discover');

function hasActiveDescendant(psMap, pid, threshold = 5) {
  for (const d of descendantsOf(psMap, pid)) {
    const info = psMap.get(d);
    if (info && Number(info.cpu) > threshold) return true;
  }
  return false;
}

function portsForTree(psMap, portsMap, pid) {
  const set = new Set();
  for (const d of [pid, ...descendantsOf(psMap, pid)]) for (const p of (portsMap.get(d) || [])) set.add(p);
  return [...set].sort((a, b) => a - b);
}

function aggregateProvider(sessions) {
  return {
    active_count: sessions.filter((s) => s.status === 'executing' || s.status === 'thinking').length,
    dirty_repos: sessions.filter((s) => s.git && (s.git.added > 0 || s.git.modified > 0)).length,
    tokens_total: sessions.reduce((sum, s) => sum + (s.tokens_total || 0), 0),
  };
}

async function collectSessions() {
  const psMap = await readPs();
  const portsMap = await readListenPorts();
  const aliverPids = new Set([...psMap.keys()]);

  // ---- Claude ----
  const claude = [];
  for (const d of discover.discoverClaudeSessions({ homeDirs: discover.defaultClaudeHomeDirs(), aliverPids })) {
    let text = ''; try { text = fs.readFileSync(d.transcriptPath, 'utf8'); } catch (_) {}
    const parsed = parseClaudeTranscript(text);
    const status = deriveStatus({
      hasActiveDescendant: hasActiveDescendant(psMap, d.pid),
      pendingTool: parsed.pendingTool,
      modelGenerating: parsed.lastUserTsMs > 0,
    });
    const git = await readGitStats(d.cwd);
    claude.push(buildClaudeSession({
      sessionId: d.sessionId, cwd: d.cwd, startedAt: d.startedAt, parsed, status, git,
      ports: portsForTree(psMap, portsMap, d.pid),
      subagents: discover.countSubagents(d.subagentsDir),
      effort: '',
    }));
  }

  // ---- Codex ----
  const codex = [];
  const codexRoot = path.join(os.homedir(), '.codex');
  for (const roll of discover.listCodexRollouts(codexRoot)) {
    let text = ''; try { text = fs.readFileSync(roll.path, 'utf8'); } catch (_) {}
    const parsed = parseCodexRollout(text);
    if (!parsed.sessionId) continue;
    const fresh = (Date.now() - roll.mtimeMs) < 5 * 60 * 1000;   // 5 分钟内才算活跃/近期
    if (!fresh) continue;
    const status = deriveStatus({ done: parsed.done, modelGenerating: false });
    const git = await readGitStats(parsed.cwd);
    codex.push(buildCodexSession({ parsed, startedAt: roll.mtimeMs, status, git, ports: [], subagents: 0 }));
  }

  return { claude, codex };
}

module.exports = { collectSessions, hasActiveDescendant, aggregateProvider, portsForTree };
```

- [ ] **Step 4: 运行,确认通过**

Run: `cd companion && node --test test/collectSessions.test.js`
Expected: PASS（2 tests）。

- [ ] **Step 5: Commit**

```bash
git add companion/lib/collectSessions.js companion/test/collectSessions.test.js
git commit -m "feat(companion): collectSessions 编排两端会话采集"
```

---

## Task 12: 接入 server.js — buildState 输出 sessions[]/active_count/agg

**Files:**
- Modify: `companion/server.js`(导入模块、缓存采集、组装进 providers)

采集较重(读多文件 + ps/lsof/git),用与 `fallback` 相同的**定时缓存**模式,不在每次请求时同步采集。

- [ ] **Step 1: 在 server.js 顶部导入并加缓存刷新**

在 `const clampPct = ...` 之后新增:

```js
const { collectSessions, aggregateProvider } = require('./lib/collectSessions');

// 会话采集缓存(异步定时刷新,避免每次请求阻塞)
let sessionCache = { claude: [], codex: [] };
let sessionRefreshing = false;
async function refreshSessions() {
  if (sessionRefreshing) return;
  sessionRefreshing = true;
  try { sessionCache = await collectSessions(); }
  catch (e) { console.error('collectSessions failed:', e.message); }
  sessionRefreshing = false;
}
refreshSessions();
setInterval(refreshSessions, 2000);                 // 快 tick ~2s
```

- [ ] **Step 2: 在 buildState() 里把会话挂到 provider**

找到 `buildState()` 中构造 `claude` 对象的位置,在其 `return` 之前插入排序工具,并给 `claude`/`codex` 对象补字段。具体:在 `function buildState()` 体内、`const claude = {...}` 之后加:

```js
  const sortSessions = (arr) => [...arr].sort((a, b) => {
    const rank = (s) => (s.status === 'executing' ? 0 : s.status === 'thinking' ? 1 : 2);
    return rank(a) - rank(b) || b.context_pct - a.context_pct;
  });
  const claudeSessions = sortSessions(sessionCache.claude);
  const codexSessions = sortSessions(sessionCache.codex);
```

然后把 `claude` 与 `codex` 对象各扩展(在它们的对象字面量里追加属性):

```js
    // claude 对象内追加:
    sessions: claudeSessions,
    active_count: aggregateProvider(claudeSessions).active_count,
    agg: { dirty_repos: aggregateProvider(claudeSessions).dirty_repos,
           tokens_per_min: 0 },              // tokens/min 见 Step 3(暂置 0,先打通)
```
```js
    // codex 对象内追加:
    sessions: codexSessions,
    active_count: aggregateProvider(codexSessions).active_count,
    agg: { dirty_repos: aggregateProvider(codexSessions).dirty_repos, tokens_per_min: 0 },
```

> 注意:`claude`/`codex` 当前已是 `const ... = { ... }`,直接在字面量里加这些键即可;`claudeSessions/codexSessions` 必须在该字面量之前声明。若 `const claude = {...}` 在前,则把上面 `sortSessions` 段移到 `const claude` 之前。

- [ ] **Step 3: 验证服务能起、端点含 sessions**

Run:
```bash
cd companion && node -e "require('./lib/collectSessions')" && \
( node server.js & echo $! > /tmp/codey_srv.pid; sleep 3; \
  curl -s --noproxy '*' http://127.0.0.1:8787/codey/state | node -e "let s='';process.stdin.on('data',d=>s+=d).on('end',()=>{const j=JSON.parse(s);console.log('providers:',j.providers.map(p=>p.id+':'+(p.sessions?p.sessions.length:'NA')+' active='+p.active_count));})"; \
  kill $(cat /tmp/codey_srv.pid) )
```
Expected: 打印形如 `providers: [ 'claude:N active=…', 'codex:M active=…' ]`,无异常退出(N/M 取决于此刻你本机是否开着 agent,可能为 0)。

- [ ] **Step 4: 跑全量单测**

Run: `cd companion && npm test`
Expected: 所有测试 PASS。

- [ ] **Step 5: Commit**

```bash
git add companion/server.js
git commit -m "feat(companion): /codey/state 输出每会话 sessions[]/active_count/agg"
```

---

## Task 13: tokens_per_min 速率(聚合) + README 契约说明

**Files:**
- Modify: `companion/server.js`(用相邻两次采集的 token 差 / 时间差)
- Modify: `companion/lib/collectSessions.js`(导出本次总 token 便于求差)
- (可选)Modify: `readme.md` 或 `readme_zh.md`:补 `/codey/state` 新字段说明

- [ ] **Step 1: 写失败测试(速率纯函数)**

```js
// companion/test/rate.test.js
const { test } = require('node:test');
const assert = require('node:assert');
const { tokensPerMin } = require('../lib/collectSessions');

test('tokensPerMin = Δtoken / Δ分钟,夹到 >=0', () => {
  assert.equal(tokensPerMin({ tokens: 100000, at: 0 }, { tokens: 130000, at: 60000 }), 30000);
  assert.equal(tokensPerMin({ tokens: 100, at: 0 }, { tokens: 50, at: 60000 }), 0);   // 切换会话致下降→0
  assert.equal(tokensPerMin(null, { tokens: 100, at: 60000 }), 0);
});
```

- [ ] **Step 2: 运行,确认失败**

Run: `cd companion && node --test test/rate.test.js`
Expected: FAIL — `tokensPerMin` 未导出。

- [ ] **Step 3: 实现并导出**

在 `companion/lib/collectSessions.js` 增加并 `module.exports` 追加 `tokensPerMin`:

```js
function tokensPerMin(prev, cur) {
  if (!prev || !cur) return 0;
  const dt = (cur.at - prev.at) / 60000;
  if (dt <= 0) return 0;
  return Math.max(0, Math.round((cur.tokens - prev.tokens) / dt));
}
```

- [ ] **Step 4: 在 server.js 用上(每端各算一次)**

把 Step 2(Task 12)里写死的 `tokens_per_min: 0` 替换为基于缓存历史的计算。在 `refreshSessions` 里记录每端 token 总量与时间戳:

```js
let tokRate = { claude: { prev: null, val: 0 }, codex: { prev: null, val: 0 } };
// refreshSessions() 末尾(成功后)追加:
for (const id of ['claude', 'codex']) {
  const total = (sessionCache[id] || []).reduce((s, x) => s + (x.tokens_total || 0), 0);
  const cur = { tokens: total, at: Date.now() };
  tokRate[id].val = tokensPerMin(tokRate[id].prev, cur);
  tokRate[id].prev = cur;
}
```
并把 buildState 里 `tokens_per_min: 0` 改为 `tokens_per_min: tokRate.claude.val`(codex 同理用 `tokRate.codex.val`)。记得在顶部 `require` 里加入 `tokensPerMin`。

- [ ] **Step 5: 跑测试 + 手测端点**

Run: `cd companion && npm test`
Expected: 全 PASS。
Run:(可选)重启 server,`curl` `/codey/state`,确认 `providers[].agg.tokens_per_min` 字段存在(初次为 0,二次刷新后可能 >0)。

- [ ] **Step 6: Commit**

```bash
git add companion/lib/collectSessions.js companion/server.js companion/test/rate.test.js
git commit -m "feat(companion): agg.tokens_per_min 速率聚合"
```

---

## 完成判据(计划 1)
- `cd companion && npm test` 全绿。
- `GET /codey/state` 的每个 provider 含 `sessions[]`(每项有 id/name/status/model/context_pct/context_tokens/context_window/tokens_total/turn/git/current_task/subagents/ports/started_at/effort)、`active_count`、`agg{dirty_repos,tokens_per_min}`;原有 `session/weekly/model`(账号额度,边缘弧)保持不变。
- 这是固件(计划 2)消费的契约,字段名以本计划与 spec §2.5 为准。

## 已知简化(相对 spec,先打通后优化)
- **增量解析**:本计划每 tick 全量读 transcript(正确但偏重)。spec §2.4 的 offset 增量解析是**性能优化**,留待实测变慢后再加(在 `parseClaudeTranscript` 外包一层 offset 缓存即可,不改契约)。
- **Claude effort**:暂置 `''`;spec §2.3 的 settings 优先级链(env/项目/全局 `effortLevel`)后续在 discover 里补。
- **compaction 次数**:暂未输出(详情页 §4 必需行未含它),后续需要再加。
- **Codex started_at**:用 rollout 文件 mtime 近似(可改为解析 `session_meta.timestamp`)。
- **Codex Desktop 窗口**:spec §2.1 的 `originator:"Codex Desktop"` + 30 分钟窗口分支未实现,仅统一用 5 分钟 freshness;桌面版超 5 分钟的会话会被丢。plan 2 不要假设它已实现。
- **status `done`**:Codex `task_complete` 且 5 分钟内的会话会上报 `status:"done"`(第 4 个枚举值,不计入 active_count,列表排序归为"其他")。固件需识别该值。

## 后续(计划 2,另写)
固件 `codey_dash.ino`:仪表盘页 + Claude/Codex 会话列表页 + 单会话详情页 + `M5.Touch`(滑动滚动 / 点按进详情)。待计划 1 契约稳定后再写。
