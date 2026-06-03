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
