const { execFile } = require('child_process');

function parseGitShortstat(text) {
  const s = String(text || '');
  const ins = s.match(/(\d+) insertion/);
  const del = s.match(/(\d+) deletion/);
  return { added: ins ? Number(ins[1]) : 0, modified: del ? Number(del[1]) : 0 };
}

function git(cwd, args, timeout = 4000) {
  return new Promise((resolve) => {
    execFile('git', ['-C', cwd, ...args], { timeout, maxBuffer: 4 * 1024 * 1024 },
      (err, stdout) => resolve(err ? '' : stdout.trim()));
  });
}

async function readGitStats(cwd) {
  const branch = await git(cwd, ['rev-parse', '--abbrev-ref', 'HEAD']);
  const stat = await git(cwd, ['diff', '--shortstat']);
  return { branch: branch || '', ...parseGitShortstat(stat) };
}

module.exports = { parseGitShortstat, readGitStats };
