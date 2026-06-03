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
