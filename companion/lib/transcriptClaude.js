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
