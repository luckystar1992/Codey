const fs = require('fs');
const os = require('os');
const path = require('path');

// 在一组候选 transcript 里选:mtime≥startedAt(含 5s 宽限)且 sid 未被占用的最新一个
function pickLatestTranscript(files, startedAtMs, claimedSids) {
  const grace = startedAtMs - 5000;
  return files
    .filter((f) => f.mtimeMs >= grace && !claimedSids.has(f.sid))
    .sort((a, b) => b.mtimeMs - a.mtimeMs)[0] || null;
}

function listTranscripts(projDir) {
  try {
    return fs.readdirSync(projDir)
      .filter((n) => n.endsWith('.jsonl'))
      .map((n) => ({ sid: n.replace(/\.jsonl$/, ''), path: path.join(projDir, n), mtimeMs: fs.statSync(path.join(projDir, n)).mtimeMs }));
  } catch (_) { return []; }
}

// cwd -> Claude project 目录名(把非字母数字换成 '-')
function encodeCwd(cwd) {
  return cwd.replace(/[^a-zA-Z0-9]/g, '-');
}

function countSubagents(subagentsDir) {
  try { return fs.readdirSync(subagentsDir).filter((n) => n.endsWith('.meta.json')).length; }
  catch (_) { return 0; }
}

// homeDirs: 形如 [~/.claude, ~/.claude-work];aliverPids: 存活 pid 集合
function discoverClaudeSessions({ homeDirs, aliverPids }) {
  const out = [];
  const claimed = new Set();
  for (const claude of homeDirs) {
    const sessDir = path.join(claude, 'sessions');
    let files = [];
    try { files = fs.readdirSync(sessDir).filter((n) => n.endsWith('.json')); } catch (_) { continue; }
    for (const fname of files) {
      let meta;
      try { meta = JSON.parse(fs.readFileSync(path.join(sessDir, fname), 'utf8')); } catch (_) { continue; }
      if (!meta.pid || !aliverPids.has(meta.pid)) continue;
      const projDir = path.join(claude, 'projects', encodeCwd(meta.cwd || ''));
      const transcripts = listTranscripts(projDir);
      const direct = transcripts.find((t) => t.sid === meta.sessionId);
      const chosen = direct || pickLatestTranscript(transcripts, meta.startedAt || 0, claimed);
      if (!chosen) continue;
      claimed.add(chosen.sid);
      out.push({
        configRoot: claude, pid: meta.pid, sessionId: chosen.sid, cwd: meta.cwd || '',
        startedAt: meta.startedAt || 0, transcriptPath: chosen.path,
        subagentsDir: path.join(projDir, chosen.sid, 'subagents'),
      });
    }
  }
  return out;
}

// 默认 Claude 配置目录:~/.claude* + $CLAUDE_CONFIG_DIR
function defaultClaudeHomeDirs() {
  const home = os.homedir();
  const dirs = new Set();
  if (process.env.CLAUDE_CONFIG_DIR) dirs.add(process.env.CLAUDE_CONFIG_DIR);
  try {
    for (const n of fs.readdirSync(home)) if (/^\.claude/.test(n)) {
      const p = path.join(home, n);
      if (fs.existsSync(path.join(p, 'sessions'))) dirs.add(p);
    }
  } catch (_) {}
  if (dirs.size === 0) dirs.add(path.join(home, '.claude'));
  return [...dirs];
}

// 扫 ~/.codex/sessions/**/rollout-*.jsonl,返回最近 N 个(按 mtime)
function listCodexRollouts(codexRoot, limit = 40) {
  const root = path.join(codexRoot, 'sessions');
  const out = [];
  const walk = (dir) => {
    let ents = [];
    try { ents = fs.readdirSync(dir, { withFileTypes: true }); } catch (_) { return; }
    for (const e of ents) {
      const p = path.join(dir, e.name);
      if (e.isDirectory()) walk(p);
      else if (/^rollout-.*\.jsonl$/.test(e.name)) { try { out.push({ path: p, mtimeMs: fs.statSync(p).mtimeMs }); } catch (_) {} }
    }
  };
  walk(root);
  return out.sort((a, b) => b.mtimeMs - a.mtimeMs).slice(0, limit);
}

module.exports = {
  pickLatestTranscript, listTranscripts, encodeCwd, countSubagents,
  discoverClaudeSessions, defaultClaudeHomeDirs, listCodexRollouts,
};
