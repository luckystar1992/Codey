const { test } = require('node:test');
const assert = require('node:assert');
const { hasActiveDescendant, aggregateProvider, portsForTree } = require('../lib/collectSessions');

test('hasActiveDescendant:任一后代 CPU>5% 为真', () => {
  const psMap = new Map([
    [100, { pid: 100, ppid: 1, cpu: 0.0 }],
    [200, { pid: 200, ppid: 100, cpu: 7.0 }],
  ]);
  assert.equal(hasActiveDescendant(psMap, 100, 5), true);
  assert.equal(hasActiveDescendant(psMap, 100, 9), false);
});

test('aggregateProvider:active_count + dirty + tokens', () => {
  const sessions = [
    { status: 'executing', git: { added: 1, modified: 2 }, tokens_total: 1000 },
    { status: 'waiting', git: { added: 0, modified: 0 }, tokens_total: 500 },
    { status: 'thinking', git: { added: 0, modified: 3 }, tokens_total: 700 },
  ];
  const agg = aggregateProvider(sessions);
  assert.equal(agg.active_count, 2);            // executing + thinking
  assert.equal(agg.dirty_repos, 2);             // 两个有改动
  assert.equal(agg.tokens_total, 2200);
});

test('portsForTree:并集 pid+后代 的端口并去重排序', () => {
  const psMap = new Map([
    [100, { pid: 100, ppid: 1, cpu: 0 }],
    [200, { pid: 200, ppid: 100, cpu: 0 }],
  ]);
  const portsMap = new Map([[100, [5173]], [200, [3000, 5173]]]);
  assert.deepEqual(portsForTree(psMap, portsMap, 100), [3000, 5173]);
  assert.deepEqual(portsForTree(psMap, portsMap, 999), []);
});
