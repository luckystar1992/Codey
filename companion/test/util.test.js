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
