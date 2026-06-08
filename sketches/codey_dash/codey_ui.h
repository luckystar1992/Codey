// sketches/codey_dash/codey_ui.h — pure UI helpers (host-testable; no Arduino deps).
#pragma once
#include <stdint.h>
#include <string.h>
#include <stdio.h>

// ---- session status ----
enum SessStatus { ST_EXECUTING = 0, ST_THINKING = 1, ST_WAITING = 2, ST_DONE = 3 };

static inline SessStatus statusFromStr(const char* s) {
  if (!s) return ST_WAITING;
  if (!strcmp(s, "executing")) return ST_EXECUTING;
  if (!strcmp(s, "thinking"))  return ST_THINKING;
  if (!strcmp(s, "done"))      return ST_DONE;
  return ST_WAITING;
}
static inline const char* statusWord(SessStatus s) {
  switch (s) {
    case ST_EXECUTING: return "EXECUTING";
    case ST_THINKING:  return "THINKING";
    case ST_DONE:      return "DONE";
    default:           return "WAITING";
  }
}
// sort rank for cross-provider merge: executing < thinking < (waiting|done)
static inline int statusRank(SessStatus s) { return s == ST_EXECUTING ? 0 : s == ST_THINKING ? 1 : 2; }

// ---- remote ASR url -> host ----
// Extract the host from a websocket url: "wss://host/..." or "ws://host:port/..." ->
// strip the scheme, then copy up to the first '/' or ':' (drops path and port).
// Empty/null input -> empty output. Always NUL-terminates (truncates to n-1 bytes).
static inline void parseWssHost(const char* url, char* out, size_t n) {
  if (!out || n == 0) return;
  out[0] = 0;
  if (!url || !url[0]) return;
  const char* p = url;
  const char* sep = strstr(p, "://");          // skip scheme (ws:// / wss:// / anything://)
  if (sep) p = sep + 3;
  size_t i = 0;
  for (; p[i] && p[i] != '/' && p[i] != ':' && i < n - 1; i++) out[i] = p[i];
  out[i] = 0;
}

// ---- UTF-8 codepoint helpers ----
static inline int utf8Len(unsigned char c) { return c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1; }

static inline int cpLen(const char* s) {
  int n = 0;
  for (int i = 0; s && s[i];) { i += utf8Len((unsigned char)s[i]); n++; }
  return n;
}

// copy <= maxCp codepoints into out; if truncated, keep maxCp-1 cps + "…"(U+2026, 3 bytes)
static inline void truncCp(const char* s, int maxCp, char* out, size_t outSz) {
  if (!out || outSz == 0) return;
  if (!s) { out[0] = 0; return; }
  if (cpLen(s) <= maxCp) { snprintf(out, outSz, "%s", s); return; }
  int keep = maxCp - 1; if (keep < 0) keep = 0;
  int cut = 0, cnt = 0;
  for (int i = 0; s[i] && cnt < keep;) { i += utf8Len((unsigned char)s[i]); cnt++; cut = i; }
  size_t k = (size_t)cut; if (k > outSz - 1) k = outSz - 1;
  memcpy(out, s, k);
  // append "…" if it fits
  const char* ell = "\xE2\x80\xA6";
  if (k + 3 < outSz) { memcpy(out + k, ell, 3); out[k + 3] = 0; }
  else out[k] = 0;
}

// ---- number / duration formatting ----
static inline void fmtK(long n, char* out, size_t outSz) {
  if (n >= 1000) {
    double kf = n / 1000.0;
    if (n >= 100000) snprintf(out, outSz, "%.0fk", kf);
    else             snprintf(out, outSz, "%.1fk", kf);
  } else {
    snprintf(out, outSz, "%ld", n);
  }
}

// 中文数量级:K=千 / W=万 / M=亿 / B=十亿(M、B 保留 2 位小数;W 大于百时去小数)
static inline void fmtTokens(long n, char* out, size_t outSz) {
  if (n < 0) n = 0;
  if (n >= 1000000000L)     { snprintf(out, outSz, "%.2fB", n / 1000000000.0); }
  else if (n >= 100000000L) { snprintf(out, outSz, "%.2fM", n / 100000000.0); }   // 亿
  else if (n >= 10000)      { double w = n / 10000.0; snprintf(out, outSz, w >= 100 ? "%.0fW" : "%.1fW", w); }
  else if (n >= 1000)       { snprintf(out, outSz, "%.1fK", n / 1000.0); }
  else                      { snprintf(out, outSz, "%ld", n); }
}

// 内存:入参 KB → M(兆) / G(吉);<=0 显示 "-"
static inline void fmtMem(long kb, char* out, size_t outSz) {
  if (kb <= 0)             { snprintf(out, outSz, "-"); }
  else if (kb >= 1048576)  { snprintf(out, outSz, "%.1fG", kb / 1048576.0); }
  else if (kb >= 1024)     { snprintf(out, outSz, "%ldM", kb / 1024); }
  else                     { snprintf(out, outSz, "%ldK", kb); }
}

static inline void fmtElapsed(long secs, char* out, size_t outSz) {
  if (secs < 0) secs = 0;
  long h = secs / 3600, m = (secs % 3600) / 60;
  if (h > 0) snprintf(out, outSz, "%ldh%02ldm", h, m);
  else       snprintf(out, outSz, "%ldm", m);
}

// ---- model short name: "claude-opus-4-8"->"Opus 4.8", "gpt-5.1-codex"->"GPT-5.1" ----
static inline void modelShort(const char* full, char* out, size_t outSz) {
  if (!out || outSz == 0) return;
  if (!full || !full[0]) { out[0] = 0; return; }

  char low[64]; size_t i = 0;
  for (; full[i] && i < sizeof(low) - 1; i++) { char c = full[i]; low[i] = (c >= 'A' && c <= 'Z') ? c + 32 : c; }
  low[i] = 0;

  const char* base = nullptr; bool gpt = false;
  if (strstr(low, "opus"))        base = "Opus";
  else if (strstr(low, "sonnet")) base = "Son";
  else if (strstr(low, "haiku"))  base = "Haiku";
  else if (strstr(low, "gpt"))  { base = "GPT"; gpt = true; }

  if (!base) { snprintf(out, outSz, "%s", full); return; }   // unknown -> passthrough

  // version: first digit group, optional .second group (sep '.' or '-')
  char ver[12] = {0}; int v = 0; const int VMAX = (int)sizeof(ver) - 1;   // every write guarded by v<VMAX
  for (size_t j = 0; low[j] && v < VMAX; j++) {
    if (low[j] >= '0' && low[j] <= '9') {
      ver[v++] = low[j];                                     // major digits
      size_t kk = j + 1;
      while (low[kk] >= '0' && low[kk] <= '9' && v < VMAX) ver[v++] = low[kk++];
      if ((low[kk] == '.' || low[kk] == '-') && low[kk + 1] >= '0' && low[kk + 1] <= '9' && v < VMAX) {
        ver[v++] = '.'; kk++;
        while (low[kk] >= '0' && low[kk] <= '9' && v < VMAX) ver[v++] = low[kk++];
      }
      break;
    }
  }
  ver[v] = 0;

  if (!ver[0])      snprintf(out, outSz, "%s", base);
  else if (gpt)     snprintf(out, outSz, "%s-%s", base, ver);
  else              snprintf(out, outSz, "%s %s", base, ver);
}
