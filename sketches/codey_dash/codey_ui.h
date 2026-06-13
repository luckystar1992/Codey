// sketches/codey_dash/codey_ui.h — pure UI helpers (host-testable; no Arduino deps).
#pragma once
#include <stdint.h>
#include <string.h>
#include <stdio.h>

// ---- session status ----
enum SessStatus { ST_EXECUTING = 0, ST_THINKING = 1, ST_WAITING = 2, ST_DONE = 3 };

// ---- companion-driven display config (GET /codey/state -> "display") ----
// 6 列(会话列表表头/详情字段)+ 2 个 provider 页;缺省全开(旧 companion / 当前行为不变)。
enum DispCol { DC_STATUS = 0, DC_MODEL = 1, DC_CTX = 2, DC_TOKENS = 3,
               DC_MEMORY = 4, DC_TURN = 5, DC_SUMMARY = 6, DC_BRANCH = 7 };
static const int DISP_NCOL  = 8;
static const int DISP_NPROV = 2;   // 0=claude 1=codex

struct DispCfg {
  bool col[DISP_NCOL];     // 列开关(STATUS,MODEL,CTX,TOKENS,MEMORY,TURN,SUMMARY,BRANCH)
  bool prov[DISP_NPROV];   // provider 页开关(claude,codex)
};

// 全开默认值;display 缺失或某键缺失 → 该项 true。
static inline DispCfg dispDefault() {
  DispCfg d;
  for (int i = 0; i < DISP_NCOL; i++)  d.col[i]  = true;
  for (int i = 0; i < DISP_NPROV; i++) d.prov[i] = true;
  return d;
}

// ---- 动态列布局(纯函数,host 可测) ----
// 原 6 列的自然槽宽(像素)(后 2 列 summary/branch 不参与槽定位,置 0);全开时槽起点正好 = 原固定列 x(46,112,204,242,300,358)。
// STATUS 槽含左侧状态点(点画在 slotX,状态词画在 slotX+DISP_STATUS_WORD_DX)。
static const int DISP_COL_W[DISP_NCOL]   = { 66, 92, 38, 58, 58, 44, 0, 0 };  // status,model,ctx,tok,mem,turn,summary,branch
static const int DISP_BAND_CENTER        = 224;   // 原列块 46..402 的中点 → 全开复现原位
static const int DISP_STATUS_WORD_DX     = 12;    // 状态词相对状态点的右移量(原 colSt-colDot = 58-46)

// 输入:8 个列开关 cfg[]。输出:启用列的有序索引 idx[] 与其槽起点 x 坐标 x[],返回启用列数 n。
// 启用列按固定顺序(STATUS<MODEL<...<TURN)排列,保留各自自然槽宽,整块以 DISP_BAND_CENTER 居中。
// 全开 → 复现原始 6 列 x;少列 → 重新铺满并居中。idx/x 容量须 >= DISP_NCOL。
static inline int layoutColumns(const bool* cfg, int* idx, int* x) {
  int n = 0, used = 0;
  for (int i = 0; i < DISP_NCOL; i++) if (cfg[i]) { idx[n++] = i; used += DISP_COL_W[i]; }
  int cur = DISP_BAND_CENTER - used / 2;   // 整块居中(used==0 时退化为中点,n=0)
  for (int k = 0; k < n; k++) { x[k] = cur; cur += DISP_COL_W[idx[k]]; }
  return n;
}

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
// sort rank for cross-provider merge: waiting < executing < thinking < done
static inline int statusRank(SessStatus s) {
  return s == ST_WAITING ? 0 : s == ST_EXECUTING ? 1 : s == ST_THINKING ? 2 : 3;
}

// 行2 元信息点分串:按列开关取 model/ctx/tokens/turn/memory 段,分隔符 " · "(U+00B7)。
// summary/branch 不在此行(分别走行3/行1)。返回写入长度。
static inline int composeMetaLine(const DispCfg* cfg, const char* modelShort, int ctxPct,
                                  const char* tokensStr, int turn, const char* memStr,
                                  char* out, size_t n) {
  if (!cfg || !out || n == 0) return 0;
  out[0] = 0;
  size_t len = 0;
  char seg[40];
  static const int ORDER[5] = { DC_MODEL, DC_CTX, DC_TOKENS, DC_TURN, DC_MEMORY };
  for (int oi = 0; oi < 5; oi++) {
    int col = ORDER[oi];
    if (!cfg->col[col]) continue;
    switch (col) {
      case DC_MODEL:  snprintf(seg, sizeof(seg), "%s", modelShort ? modelShort : ""); break;
      case DC_CTX:    snprintf(seg, sizeof(seg), "%d%%", ctxPct); break;
      case DC_TOKENS: snprintf(seg, sizeof(seg), "%s", tokensStr ? tokensStr : ""); break;
      case DC_TURN:   snprintf(seg, sizeof(seg), "t%d", turn); break;
      case DC_MEMORY: snprintf(seg, sizeof(seg), "%s", memStr ? memStr : ""); break;
      default: continue;
    }
    if (seg[0] == 0) continue;
    const char* sep = (len > 0) ? " \xC2\xB7 " : "";   // " · " 仅在非首段前
    int w = snprintf(out + len, n - len, "%s%s", sep, seg);
    if (w < 0 || (size_t)w >= n - len) break;           // 截断保护
    len += w;
  }
  return (int)len;
}

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
