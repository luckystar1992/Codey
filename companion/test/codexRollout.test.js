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
