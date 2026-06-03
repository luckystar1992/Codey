const fs = require('fs');
const os = require('os');
const path = require('path');
const { parseClaudeTranscript } = require('./transcriptClaude');
const { parseCodexRollout } = require('./codexRollout');
const { deriveStatus } = require('./deriveStatus');
const { buildClaudeSession, buildCodexSession } = require('./buildSession');
const { readPs, readListenPorts, descendantsOf } = require('./processTree');
const { readGitStats } = require('./gitStats');
const discover = require('./discover');

function hasActiveDescendant(psMap, pid, threshold = 5) {
  for (const d of descendantsOf(psMap, pid)) {
    const info = psMap.get(d);
    if (info && Number(info.cpu) > threshold) return true;
  }
  return false;
}

function portsForTree(psMap, portsMap, pid) {
  const set = new Set();
  for (const d of [pid, ...descendantsOf(psMap, pid)]) for (const p of (portsMap.get(d) || [])) set.add(p);
  return [...set].sort((a, b) => a - b);
}

function aggregateProvider(sessions) {
  return {
    active_count: sessions.filter((s) => s.status === 'executing' || s.status === 'thinking').length,
    dirty_repos: sessions.filter((s) => s.git && (s.git.added > 0 || s.git.modified > 0)).length,
    tokens_total: sessions.reduce((sum, s) => sum + (s.tokens_total || 0), 0),
  };
}

async function collectSessions() {
  const psMap = await readPs();
  const portsMap = await readListenPorts();
  const aliverPids = new Set([...psMap.keys()]);

  // ---- Claude ----
  const claude = [];
  for (const d of discover.discoverClaudeSessions({ homeDirs: discover.defaultClaudeHomeDirs(), aliverPids })) {
    let text = ''; try { text = fs.readFileSync(d.transcriptPath, 'utf8'); } catch (_) {}
    const parsed = parseClaudeTranscript(text);
    const status = deriveStatus({
      hasActiveDescendant: hasActiveDescendant(psMap, d.pid),
      pendingTool: parsed.pendingTool,
      modelGenerating: parsed.lastUserTsMs > 0,
    });
    const git = await readGitStats(d.cwd);
    claude.push(buildClaudeSession({
      sessionId: d.sessionId, cwd: d.cwd, startedAt: d.startedAt, parsed, status, git,
      ports: portsForTree(psMap, portsMap, d.pid),
      subagents: discover.countSubagents(d.subagentsDir),
      effort: '',
    }));
  }

  // ---- Codex ----
  const codex = [];
  const codexRoot = path.join(os.homedir(), '.codex');
  for (const roll of discover.listCodexRollouts(codexRoot)) {
    let text = ''; try { text = fs.readFileSync(roll.path, 'utf8'); } catch (_) {}
    const parsed = parseCodexRollout(text);
    if (!parsed.sessionId) continue;
    const fresh = (Date.now() - roll.mtimeMs) < 5 * 60 * 1000;   // 5 分钟内才算活跃/近期
    if (!fresh) continue;
    const status = deriveStatus({ done: parsed.done, modelGenerating: false });
    const git = await readGitStats(parsed.cwd);
    codex.push(buildCodexSession({ parsed, startedAt: roll.mtimeMs, status, git, ports: [], subagents: 0 }));
  }

  return { claude, codex };
}

module.exports = { collectSessions, hasActiveDescendant, aggregateProvider, portsForTree };
