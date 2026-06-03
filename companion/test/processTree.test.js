const { test } = require('node:test');
const assert = require('node:assert');
const { parsePs, descendantsOf, parseLsofListen } = require('../lib/processTree');

const PS = `  PID  PPID    RSS %CPU COMM
    1     0   1000  0.0 /sbin/launchd
  100     1  50000  7.5 node
  200   100  20000  0.1 npm
  300   200  10000  0.0 esbuild
  400     1   5000  0.0 other`;

test('parsePs 建 pid->info', () => {
  const m = parsePs(PS);
  assert.equal(m.get(100).ppid, 1);
  assert.equal(m.get(100).cpu, 7.5);
  assert.equal(m.get(100).rssKb, 50000);
  assert.equal(m.get(100).comm, 'node');
});

test('descendantsOf 递归收集', () => {
  const m = parsePs(PS);
  const d = descendantsOf(m, 100).sort((a, b) => a - b);
  assert.deepEqual(d, [200, 300]);
  assert.deepEqual(descendantsOf(m, 400), []);
});

test('parseLsofListen 解析 pid->ports', () => {
  const LSOF = `COMMAND   PID USER   FD   TYPE             DEVICE SIZE/OFF NODE NAME
node      300 zyc   25u  IPv4 0x1234      0t0  TCP *:3000 (LISTEN)
node      300 zyc   26u  IPv6 0x5678      0t0  TCP [::1]:5173 (LISTEN)
node      999 zyc   27u  IPv4 0x9999      0t0  TCP 127.0.0.1:8787 (LISTEN)`;
  const p = parseLsofListen(LSOF);
  assert.deepEqual(p.get(300).sort((a, b) => a - b), [3000, 5173]);
  assert.deepEqual(p.get(999), [8787]);
});
