const { test } = require('node:test');
const assert = require('node:assert');
const { tokensPerMin } = require('../lib/collectSessions');

test('tokensPerMin = Δtoken / Δ分钟,夹到 >=0', () => {
  assert.equal(tokensPerMin({ tokens: 100000, at: 0 }, { tokens: 130000, at: 60000 }), 30000);
  assert.equal(tokensPerMin({ tokens: 100, at: 0 }, { tokens: 50, at: 60000 }), 0);   // 切换会话致下降→0
  assert.equal(tokensPerMin(null, { tokens: 100, at: 60000 }), 0);
  assert.equal(tokensPerMin({ tokens: 100, at: 0 }, null), 0);            // cur 为 null
  assert.equal(tokensPerMin({ tokens: 100, at: 5000 }, { tokens: 200, at: 5000 }), 0);  // dt == 0
});
