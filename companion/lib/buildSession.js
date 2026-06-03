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
