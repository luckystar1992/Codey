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
