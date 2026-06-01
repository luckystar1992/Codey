// Codey Companion — serves the Codey watch over the LAN.
//
//   GET  /codey/state   normalized Claude (+ Codex) usage as JSON
//   POST /codey/asr      16k/16-bit/mono WAV in -> { text } (whisper.cpp)
//
// Claude usage is the REAL rate-limit % (the same numbers /status and claude-hud show),
// captured from the statusline into ~/.claude/codey-usage.json by codey-statusline.sh,
// with ccusage as a token-based fallback. ASR runs against a long-lived whisper-server
// (model preloaded) so streaming partials stay fast.
//
// Run:   node server.js     (keep it running while you use the watch)

const http = require('http');
const os = require('os');
const fs = require('fs');
const path = require('path');
const { execFile, spawn } = require('child_process');
let Solar = null; try { Solar = require('lunar-javascript').Solar; } catch (e) { console.error('lunar-javascript missing'); }

// Chinese lunar date / zodiac / solar-term for the watch face (computed from the Mac's date)
function lunarInfo() {
  if (!Solar) return null;
  try {
    const l = Solar.fromDate(new Date()).getLunar();
    return {
      date: l.getMonthInChinese() + '月' + l.getDayInChinese(),   // 四月十五
      ganzhi: l.getYearInGanZhi(),                                // 丙午
      zodiac: l.getYearShengXiao(),                               // 马
      jieqi: l.getJieQi() || '',                                  // 节气 (if today)
    };
  } catch (e) { return null; }
}

const PORT = Number(process.env.CODEY_PORT) || 8787;
const SESSION_LIMIT = Number(process.env.CODEY_SESSION_LIMIT) || 0;
const WEEKLY_LIMIT = Number(process.env.CODEY_WEEKLY_LIMIT) || 0;
const PROXY = process.env.CODEY_PROXY || 'http://127.0.0.1:7892';
const CACHE_MS = 45000;
const REAL_FRESH_MS = 30 * 60 * 1000;                  // real usage older than this -> stale
const REAL_USAGE_FILE = path.join(os.homedir(), '.claude', 'codey-usage.json');
const CODEX_HISTORY = path.join(os.homedir(), 'Library', 'Application Support', 'com.steipete.codexbar', 'history', 'codex.json');

const WHISPER_CLI = process.env.CODEY_WHISPER || '/opt/homebrew/bin/whisper-cli';
const WHISPER_SERVER = process.env.CODEY_WHISPER_SERVER || '/opt/homebrew/bin/whisper-server';
const MODEL = process.env.CODEY_WHISPER_MODEL || path.join(__dirname, 'models', 'ggml-small.bin');
const WS_HOST = '127.0.0.1';
const WS_PORT = Number(process.env.CODEY_WHISPER_PORT) || 8090;
const PROMPT = '简体中文普通话。Claude Code,Codex,额度,周额度,还剩多少,什么时候重置,切换到,待审阅任务,电量。';

const env = { ...process.env, https_proxy: PROXY, http_proxy: PROXY, all_proxy: PROXY };
const clampPct = (x) => Math.max(0, Math.min(100, Math.round(x)));

// ---- transcript de-hallucination (whisper invents filler on faint/short/non-speech audio) ----
const HALLUC = [/^(你好)+$/, /欢迎(收看|观看)/, /请[^ ]{0,3}(订阅|关注|点赞)/, /谢谢(大家)?(观看|收看)/,
                /^字幕/, /明镜|点点栏目/, /thanks? for watching/i, /^(嗯|啊|呃|哦)+$/];
function collapseAndDeny(text) {
  text = String(text || '').replace(/\s+/g, ' ').trim();
  for (let L = 2; L <= 4; L++) { const u = text.slice(0, L); if (u && text.length % L === 0 && text === u.repeat(text.length / L)) { text = u; break; } }
  for (const re of HALLUC) if (re.test(text)) return '';
  return text;
}
function cleanText(vj) {
  const segs = vj.segments || [];
  const parts = [];
  for (const s of segs) {
    if (typeof s.no_speech_prob === 'number' && s.no_speech_prob > 0.6) continue;   // whisper: silence
    if (typeof s.avg_logprob === 'number' && s.avg_logprob < -1.2) continue;        // very low confidence
    let t = '';
    if (Array.isArray(s.words) && s.words.length) {                                 // collapse repeated words
      let prev = null;
      for (const w of s.words) { const ww = (w.word || '').trim(); if (ww && ww !== prev) t += ww; prev = ww; }
    } else t = (s.text || '').trim();
    if (t) parts.push(t);
  }
  return collapseAndDeny(parts.join(''));
}

// ---------------------------------------------------------------- usage (state)

function ccusage(args) {
  return new Promise((resolve, reject) => {
    execFile('npx', ['-y', 'ccusage@latest', ...args],
      { maxBuffer: 64 * 1024 * 1024, env, timeout: 60000 },
      (err, stdout) => {
        if (err) return reject(err);
        try { resolve(JSON.parse(stdout)); } catch (e) { reject(e); }
      });
  });
}

function endOfWeekEpoch() {                 // next Monday 00:00 local time
  const d = new Date();
  const day = (d.getDay() + 6) % 7;         // Mon=0 .. Sun=6
  const e = new Date(d.getFullYear(), d.getMonth(), d.getDate() + (7 - day), 0, 0, 0, 0);
  return Math.floor(e.getTime() / 1000);
}

// Token-based fallback (used only when the real statusline data is unavailable).
async function buildFallback() {
  const now = Math.floor(Date.now() / 1000);
  const blk = await ccusage(['blocks', '--json']);
  const blocks = (blk.blocks || []).filter((b) => !b.isGap);
  const active = blocks.find((b) => b.isActive);
  const maxBlock = Math.max(1, ...blocks.map((b) => b.totalTokens || 0));
  const sLimit = SESSION_LIMIT || maxBlock;
  const sUsed = active ? clampPct((active.totalTokens / sLimit) * 100) : 0;
  const sReset = active ? Math.floor(new Date(active.endTime).getTime() / 1000) : now;

  let weeks = [];
  try {
    const wk = await ccusage(['weekly', '--json']);
    weeks = Array.isArray(wk) ? wk : (wk.weekly || wk.data || []);
  } catch (e) { /* leave empty -> weekly 0 */ }
  const curWeek = weeks[weeks.length - 1] || {};
  const wLimit = WEEKLY_LIMIT || Math.max(1, ...weeks.map((w) => w.totalTokens || 0));
  const wUsed = clampPct(((curWeek.totalTokens || 0) / wLimit) * 100);

  return { sUsed, sReset, wUsed, wReset: endOfWeekEpoch() };
}

let fallback = null;
let refreshing = false;
async function refresh() {
  if (refreshing) return;
  refreshing = true;
  try { fallback = await buildFallback(); }
  catch (e) { console.error('ccusage fallback failed:', e.message); }
  refreshing = false;
}
refresh();
setInterval(refresh, CACHE_MS);

// Real usage captured from the Claude Code statusline (the authoritative numbers).
function readRealUsage() {
  try {
    const j = JSON.parse(fs.readFileSync(REAL_USAGE_FILE, 'utf8'));
    const fresh = typeof j.ts === 'number' && (Date.now() - j.ts * 1000) < REAL_FRESH_MS;
    return { ...j, fresh };
  } catch (e) { return null; }
}

// Real Codex usage from the codexbar menu-bar app's history cache (it refreshes continuously).
// session (5h window) + weekly, each {usedPercent, resetsAt}.
function readCodexUsage() {
  try {
    const j = JSON.parse(fs.readFileSync(CODEX_HISTORY, 'utf8'));
    const acc = j.accounts[j.preferredAccountKey] || [];
    const pick = (name) => {
      const w = acc.find((x) => x.name === name);
      const e = w && w.entries && w.entries[w.entries.length - 1];
      return e ? { pct: clampPct(e.usedPercent), reset: Math.floor(new Date(e.resetsAt).getTime() / 1000) } : null;
    };
    return { session: pick('session'), weekly: pick('weekly') };
  } catch (e) { return null; }
}

function buildState() {
  const now = Math.floor(Date.now() / 1000);
  const real = readRealUsage();
  const fb = fallback || { sUsed: 0, sReset: now, wUsed: 0, wReset: endOfWeekEpoch() };

  const fiveH = real && real.five_hour && typeof real.five_hour.used_percentage === 'number';
  const sevenD = real && real.seven_day && typeof real.seven_day.used_percentage === 'number';

  const claude = {
    id: 'claude', name: 'Claude Code',
    session: {
      used_pct: fiveH ? clampPct(real.five_hour.used_percentage) : fb.sUsed,
      reset_epoch: (fiveH && real.five_hour.resets_at) ? real.five_hour.resets_at : fb.sReset,
    },
    weekly: {
      used_pct: sevenD ? clampPct(real.seven_day.used_percentage) : fb.wUsed,
      reset_epoch: (sevenD && real.seven_day.resets_at) ? real.seven_day.resets_at : fb.wReset,
    },
    pending_reviews: 0,
    model: (real && real.model) ? real.model : null,
    _src: fiveH ? 'statusline' : 'ccusage',
  };

  const cx = readCodexUsage();
  const codex = {
    id: 'codex', name: 'Codex',
    session: { used_pct: cx && cx.session ? cx.session.pct : 0, reset_epoch: cx && cx.session ? cx.session.reset : now },
    weekly: { used_pct: cx && cx.weekly ? cx.weekly.pct : 0, reset_epoch: cx && cx.weekly ? cx.weekly.reset : endOfWeekEpoch() },
    pending_reviews: 0,
    _src: cx ? 'codexbar' : 'none',
  };

  return {
    ts: now,
    stale: !(real && real.fresh) && !fiveH,
    battery: { pct: 0, charging: false },         // the device uses its own real battery
    providers: [claude, codex],
    lunar: lunarInfo(),                           // 农历 / 生肖 / 节气 for the watch face
  };
}

// ------------------------------------------------------------------- ASR (voice)

// Long-lived whisper-server with the model preloaded -> fast repeated/partial transcripts.
let wsReady = false;
let wsChild = null;
let wsBackoff = 2000;
let wsProbe = null;
process.on('exit', () => { try { if (wsChild) wsChild.kill(); } catch (e) {} });   // registered once

function startWhisperServer() {
  if (!fs.existsSync(WHISPER_SERVER) || !fs.existsSync(MODEL)) {
    console.error('whisper-server or model missing; ASR will fall back to whisper-cli');
    return;
  }
  if (wsProbe) { clearInterval(wsProbe); wsProbe = null; }
  // -sns (suppress non-speech) + greedy, NO domain prompt: empirically the cleanest on real
  // device audio (the domain prompt actually hurt, e.g. dropped "Codex"; tuned on a captured clip).
  const args = ['-m', MODEL, '-l', 'zh', '-nt', '-t', '4', '-bo', '1', '-sns',
                '--host', WS_HOST, '--port', String(WS_PORT)];
  wsChild = spawn(WHISPER_SERVER, args, { stdio: ['ignore', 'pipe', 'pipe'] });
  const watch = (buf) => {
    const s = String(buf);
    if (/listening|HTTP server|server is listening|loaded the model/i.test(s)) { wsReady = true; wsBackoff = 2000; }
    if (/address already in use|EADDRINUSE/i.test(s)) console.error(`whisper-server: port ${WS_PORT} already in use`);
  };
  wsChild.stdout.on('data', watch);
  wsChild.stderr.on('data', watch);
  wsChild.on('exit', (code) => {
    wsReady = false; wsChild = null;
    wsBackoff = Math.min(wsBackoff * 2, 60000);                  // exponential backoff, capped (no hot loop)
    console.error(`whisper-server exited (${code}); retrying in ${wsBackoff}ms`);
    setTimeout(startWhisperServer, wsBackoff);
  });
  // probe readiness in case the banner text differs across builds (self-clears once ready)
  wsProbe = setInterval(() => {
    const req = http.request({ host: WS_HOST, port: WS_PORT, path: '/', method: 'GET', timeout: 800 },
      () => { wsReady = true; clearInterval(wsProbe); wsProbe = null; });
    req.on('error', () => {}); req.on('timeout', () => req.destroy()); req.end();
  }, 1000);
}
startWhisperServer();

function multipart(wav) {
  const b = '----codey' + Math.random().toString(16).slice(2);
  const field = (name, val) => Buffer.from(`--${b}\r\nContent-Disposition: form-data; name="${name}"\r\n\r\n${val}\r\n`);
  const filePart = Buffer.from(`--${b}\r\nContent-Disposition: form-data; name="file"; filename="a.wav"\r\nContent-Type: audio/wav\r\n\r\n`);
  const body = Buffer.concat([
    filePart, wav, Buffer.from('\r\n'),
    field('response_format', 'verbose_json'),
    field('language', 'zh'),
    field('temperature', '0'),
    Buffer.from(`--${b}--\r\n`),
  ]);
  return { body, contentType: `multipart/form-data; boundary=${b}` };
}

function asrViaServer(wav) {
  return new Promise((resolve, reject) => {
    const { body, contentType } = multipart(wav);
    const req = http.request({
      host: WS_HOST, port: WS_PORT, path: '/inference', method: 'POST',
      headers: { 'Content-Type': contentType, 'Content-Length': body.length }, timeout: 20000,
    }, (res) => {
      const chunks = [];
      res.on('data', (c) => chunks.push(c));
      res.on('end', () => {
        const raw = Buffer.concat(chunks).toString('utf8');
        // require JSON; a foreign occupant of WS_PORT -> reject -> handleAsr falls back to the CLI
        try { resolve(cleanText(JSON.parse(raw))); }
        catch (e) { reject(new Error('whisper-server non-JSON: ' + raw.slice(0, 80))); }
      });
    });
    req.on('error', reject);
    req.on('timeout', () => { req.destroy(new Error('whisper-server timeout')); });
    req.write(body); req.end();
  });
}

function asrViaCli(wav) {
  return new Promise((resolve, reject) => {
    const tmp = path.join(os.tmpdir(), `codey_asr_${Date.now()}.wav`);
    fs.writeFileSync(tmp, wav);
    execFile(WHISPER_CLI, ['-m', MODEL, '-f', tmp, '-l', 'zh', '-nt', '-np', '-bo', '1', '-bs', '1', '-sns'],
      { timeout: 25000 }, (err, stdout) => {
        fs.unlink(tmp, () => {});
        if (err) return reject(err);
        resolve(collapseAndDeny(stdout));
      });
  });
}

function handleAsr(req, res) {
  const chunks = []; let size = 0; let aborted = false;
  req.on('data', (c) => {
    if (aborted) return;
    chunks.push(c); size += c.length;
    if (size > 8e6) {                                   // oversized -> respond 413, don't silently drop
      aborted = true;
      res.writeHead(413, { 'Content-Type': 'application/json', 'Access-Control-Allow-Origin': '*' });
      res.end(JSON.stringify({ text: '', error: 'audio too large' }));
      req.destroy();
    }
  });
  req.on('end', async () => {
    if (aborted) return;
    const wav = Buffer.concat(chunks);
    // DEBUG: dump the received audio + report its level so we can diagnose mic quality
    try {
      fs.writeFileSync('/tmp/codey_dev_last.wav', wav);
      const pcm = wav.subarray(44);
      let sum = 0, peak = 0; const nn = Math.max(1, pcm.length / 2);
      for (let i = 0; i + 1 < pcm.length; i += 2) { const v = pcm.readInt16LE(i); sum += v * v; if (Math.abs(v) > peak) peak = Math.abs(v); }
      console.log(`[asr] in ${(nn / 16000).toFixed(1)}s samples=${nn} rms=${Math.round(Math.sqrt(sum / nn))} peak=${peak}`);
    } catch (e) {}
    const t0 = Date.now();
    let text = '', err = null;
    try {
      text = (wsReady ? await asrViaServer(wav) : await asrViaCli(wav));
    } catch (e1) {
      try { text = await asrViaCli(wav); } catch (e2) { err = e2; }
    }
    res.writeHead(err ? 500 : 200, { 'Content-Type': 'application/json', 'Access-Control-Allow-Origin': '*' });
    res.end(JSON.stringify({ text, error: err ? String(err) : undefined }));
    console.log(`[asr] ${Date.now() - t0}ms ${err ? 'ERR ' + err.message : '"' + text + '"'}`);
  });
}

// ------------------------------------------------------------------- HTTP server

const server = http.createServer((req, res) => {
  if (req.method === 'POST' && req.url.startsWith('/codey/asr')) return handleAsr(req, res);
  if (req.url.startsWith('/codey/state')) {
    res.writeHead(200, { 'Content-Type': 'application/json', 'Access-Control-Allow-Origin': '*' });
    return res.end(JSON.stringify(buildState()));
  }
  res.writeHead(404); res.end('not found');
});

function lanIP() {
  for (const xs of Object.values(os.networkInterfaces()))
    for (const x of xs) if (x.family === 'IPv4' && !x.internal) return x.address;
  return '127.0.0.1';
}
server.listen(PORT, () => console.log(`Codey companion -> http://${lanIP()}:${PORT}/codey/state  (port ${PORT})`));
