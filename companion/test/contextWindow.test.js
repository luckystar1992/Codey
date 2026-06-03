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
