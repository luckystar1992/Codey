const { execFile } = require('child_process');

function parsePs(text) {
  const map = new Map();
  const lines = String(text || '').split('\n');
  for (const line of lines) {
    const m = line.trim().match(/^(\d+)\s+(\d+)\s+(\d+)\s+([\d.]+)\s+(.*)$/);
    if (!m) continue;                               // 跳过表头/空行
    const pid = Number(m[1]);
    map.set(pid, { pid, ppid: Number(m[2]), rssKb: Number(m[3]), cpu: Number(m[4]), comm: m[5].trim() });
  }
  return map;
}

function descendantsOf(map, rootPid) {
  const childrenByParent = new Map();
  for (const info of map.values()) {
    if (!childrenByParent.has(info.ppid)) childrenByParent.set(info.ppid, []);
    childrenByParent.get(info.ppid).push(info.pid);
  }
  const out = [];
  const seen = new Set([rootPid]);
  const stack = [...(childrenByParent.get(rootPid) || [])];
  while (stack.length) {
    const pid = stack.pop();
    if (seen.has(pid)) continue;
    seen.add(pid);
    out.push(pid);
    for (const c of (childrenByParent.get(pid) || [])) stack.push(c);
  }
  return out;
}

function parseLsofListen(text) {
  const ports = new Map();
  for (const line of String(text || '').split('\n')) {
    if (!/\(LISTEN\)/.test(line)) continue;
    const pidM = line.match(/^\S+\s+(\d+)/);
    const portM = line.match(/:(\d+)\s+\(LISTEN\)/);
    if (!pidM || !portM) continue;
    const pid = Number(pidM[1]); const port = Number(portM[1]);
    if (!ports.has(pid)) ports.set(pid, []);
    if (!ports.get(pid).includes(port)) ports.get(pid).push(port);
  }
  return ports;
}

// --- IO 封装(运行期用,不在单测覆盖) ---
function run(cmd, args, timeout = 4000) {
  return new Promise((resolve) => {
    execFile(cmd, args, { timeout, maxBuffer: 8 * 1024 * 1024 }, (err, stdout) => resolve(err ? '' : stdout));
  });
}
const readPs = async () => parsePs(await run('ps', ['-axo', 'pid,ppid,rss,pcpu,comm']));
const readListenPorts = async () => parseLsofListen(await run('lsof', ['-nP', '-iTCP', '-sTCP:LISTEN']));

module.exports = { parsePs, descendantsOf, parseLsofListen, readPs, readListenPorts };
