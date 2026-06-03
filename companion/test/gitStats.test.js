const { test } = require('node:test');
const assert = require('node:assert');
const { parseGitShortstat } = require('../lib/gitStats');

test('解析 insertions/deletions', () => {
  assert.deepEqual(parseGitShortstat(' 3 files changed, 12 insertions(+), 5 deletions(-)'), { added: 12, modified: 5 });
  assert.deepEqual(parseGitShortstat(' 1 file changed, 4 insertions(+)'), { added: 4, modified: 0 });
  assert.deepEqual(parseGitShortstat(''), { added: 0, modified: 0 });
});
