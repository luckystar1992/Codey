const { test } = require('node:test');
const assert = require('node:assert');
const os = require('os'); const fs = require('fs'); const path = require('path');
const { pickLatestTranscript, countSubagents, discoverClaudeSessions } = require('../lib/discover');

test('pickLatestTranscript 选 mtime≥startedAt 且未占用的最新 jsonl', () => {
  const files = [
    { sid: 'old', mtimeMs: 1000, path: '/p/old.jsonl' },
    { sid: 'new', mtimeMs: 5000, path: '/p/new.jsonl' },
    { sid: 'claimed', mtimeMs: 9000, path: '/p/claimed.jsonl' },
  ];
  const r = pickLatestTranscript(files, 2000, new Set(['claimed']));
  assert.equal(r.sid, 'new');
});

test('discoverClaudeSessions:临时目录里读 session 文件 + transcript', () => {
  const home = fs.mkdtempSync(path.join(os.tmpdir(), 'codey-home-'));
  const claude = path.join(home, '.claude');
  const sessions = path.join(claude, 'sessions');
  const encoded = '-Users-zyc-code-Demo';
  const projDir = path.join(claude, 'projects', encoded);
  fs.mkdirSync(sessions, { recursive: true });
  fs.mkdirSync(projDir, { recursive: true });
  const pid = process.pid;        // 用本进程 pid 保证"存活"
  fs.writeFileSync(path.join(sessions, pid + '.json'),
    JSON.stringify({ pid, sessionId: 'sid-demo', cwd: '/Users/zyc/code/Demo', startedAt: 1000 }));
  fs.writeFileSync(path.join(projDir, 'sid-demo.jsonl'),
    JSON.stringify({ type: 'assistant', timestamp: '2026-06-03T10:00:00Z',
      message: { model: 'claude-opus-4-8', usage: { input_tokens: 50, output_tokens: 10, cache_read_input_tokens: 0, cache_creation_input_tokens: 0 }, content: [{ type: 'text', text: 'hi' }] } }));

  const out = discoverClaudeSessions({ homeDirs: [claude], aliverPids: new Set([pid]) });
  assert.equal(out.length, 1);
  assert.equal(out[0].sessionId, 'sid-demo');
  assert.equal(out[0].cwd, '/Users/zyc/code/Demo');
  assert.ok(out[0].transcriptPath.endsWith('sid-demo.jsonl'));
  fs.rmSync(home, { recursive: true, force: true });
});

test('countSubagents 数 .meta.json', () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'codey-sub-'));
  fs.writeFileSync(path.join(dir, 'a.meta.json'), '{}');
  fs.writeFileSync(path.join(dir, 'b.meta.json'), '{}');
  fs.writeFileSync(path.join(dir, 'note.txt'), 'x');
  assert.equal(countSubagents(dir), 2);
  assert.equal(countSubagents('/no/such/dir'), 0);
  fs.rmSync(dir, { recursive: true, force: true });
});
