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
