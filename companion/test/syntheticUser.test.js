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
