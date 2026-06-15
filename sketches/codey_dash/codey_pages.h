// sketches/codey_dash/codey_pages.h — page renderers + render() dispatcher + waiting banner.
// 单 TU 习惯:依赖 .ino 的全局(PROV/g_disp/g_model/g_codexModel/g_clkH·M/g_batt/bootMs/page/detailProv/
// detailIdx/g_listView/g_list*/g_voice)与前向声明(drawVoiceOverlay/ensureProvVisible 在本头 #include 之前可见)。
#pragma once
#include <M5Unified.h>
#include "codey_theme.h"
#include "codey_ui.h"
#include "session_store.h"
#include "codey_mascot.h"
#include "codey_widgets.h"

// ---------- compose one page ----------
static void render() {
  if (g_voice) { drawVoiceOverlay(); cv.pushSprite(0, 0); return; }
  ensureProvVisible();                       // 当前页落在被禁用 provider 上 → 吸附到启用页
  if (detailProv >= 0)      renderDetailPage();
  else if (g_listView)      renderListPage(page);
  else                      renderUsagePage(page);
  cv.pushSprite(0, 0);
}

// 跨端等待提醒:首个 waiting 会话画橙色胶囊;limited 则红色 RATE LIMITED。
// 命中区写入 g_banner*(供点击直达详情)。无 waiting 且未 limited 返回 false。
static int g_bannerTop = 0, g_bannerH = 0, g_bannerProv = -1, g_bannerIdx = -1;
static bool drawWaitBanner(int provIdx) {
  const Prov& p = PROV[provIdx];
  // 跨端统计 waiting 会话总数(R-HOME-02:多会话变体);记录首个供点击直达
  int waitCount = 0, firstProv = -1, firstIdx = -1;
  for (int pi = 0; pi < DISP_NPROV; pi++) {
    const Prov& pp = PROV[pi];
    for (int i = 0; i < pp.nsess; i++)
      if ((SessStatus)pp.sess[i].status == ST_WAITING) {
        if (firstProv < 0) { firstProv = pi; firstIdx = i; }
        waitCount++;
      }
  }
  g_bannerProv = -1; g_bannerIdx = -1;
  if (waitCount == 0 && !p.limited) return false;
  const int by = 70, bh = 26; g_bannerTop = by - bh / 2; g_bannerH = bh;
  char buf[64];
  uint32_t col = p.limited ? 0xff5d5d : 0xffa94d;
  if (p.limited) snprintf(buf, sizeof(buf), "RATE LIMITED");
  else if (waitCount == 1) {
    g_bannerProv = firstProv; g_bannerIdx = firstIdx;
    char nm[24]; truncCp(PROV[firstProv].sess[firstIdx].name, 10, nm, sizeof(nm));
    snprintf(buf, sizeof(buf), "%s 等你输入", nm);
  } else {
    g_bannerProv = firstProv; g_bannerIdx = firstIdx;   // 多会话:点进第一个等待会话
    snprintf(buf, sizeof(buf), "%d 个会话在等你", waitCount);
  }
  cv.fillRoundRect(CX - 110, g_bannerTop, 220, bh, 13, c565(shade(col, -0.78f)));
  cv.drawRoundRect(CX - 110, g_bannerTop, 220, bh, 13, c565(col));
  cv.fillSmoothCircle(CX - 92, by, 4, c565(col));
  cv.setFont(&fonts::efontCN_16); cv.setTextDatum(middle_center); cv.setTextColor(c565(col));
  cv.drawString(buf, CX + 6, by);
  return true;
}

static void renderUsagePage(int provIdx) {
  const Prov& p = PROV[provIdx];
  blitProviderArc(provIdx);
  char clk[8]; snprintf(clk, sizeof(clk), "%02d:%02d", g_clkH, g_clkM);
  drawHeader(p, String(clk));
  float t = (millis() - bootMs) / 1000.0f;
  const char* mood = moodForUsage(max(p.sessUsed, p.weekUsed), p.activeCount, g_batt);
  drawMascot(provIdx, CX, 150, p.color, mood, t, 1.0f);
  drawWaitBanner(provIdx);
  const char* mdl = (provIdx == 0) ? g_model : g_codexModel;
  if (mdl[0]) {
    cv.loadFont(JBMono16);                              // 模型名走 VLW
    cv.setTextDatum(middle_center); cv.setTextColor(c565(0x8a8d94));
    cv.drawString(mdl, CX, 226); cv.unloadFont();
  }
  long nowE = time(nullptr); bool epochOK = epochSane(nowE);
  String sR = (epochOK && p.sessReset > nowE) ? fmtDur(p.sessReset - nowE) : String("");
  String wR = (epochOK && p.weekReset > nowE) ? fmtDur(p.weekReset - nowE) : String("");
  drawMeter(250, "usage",  p.sessUsed, sR, p.color);
  drawMeter(286, "weekly", p.weekUsed, wR, p.color);
  drawSessionCount(344, p.activeCount, p.nsess);    // session 运行/总数(运行数绿)
  { char rate[24]; fmtTokens(p.tokPerMin, rate, sizeof(rate));
    char line[32]; snprintf(line, sizeof(line), "%s/min", rate);
    cv.loadFont(JBMono16); cv.setTextDatum(middle_center); cv.setTextColor(c565(0x8a8d94));
    cv.drawString(line, CX, 368); cv.unloadFont(); }
  drawWifiStatus(406);
  drawDots(provIdx, p.color);
}
static void renderListPage(int provIdx) {
  const Prov& p = PROV[provIdx];
  uint32_t color = p.color;

  blitProviderArc(provIdx);

  // 顶部:与首页同款小机器人(provider 标识)
  { float t = (millis() - bootMs) / 1000.0f;
    const char* mood = moodForUsage(max(p.sessUsed, p.weekUsed), p.activeCount, g_batt);
    drawMascot(provIdx, CX, 42, color, mood, t, 0.32f); }

  // 空态
  if (p.nsess == 0) {
    cv.setFont(&fonts::FreeSans9pt7b); cv.setTextColor(c565(0x6f757d)); cv.setTextDatum(middle_center);
    cv.drawString("no sessions", CX, CY);
    drawDots(provIdx, color);
    return;
  }

  // 居中几何:三行卡片列表以屏幕中线 CY 为对称轴居中
  int visN = p.nsess < LIST_MAX_VIS ? p.nsess : LIST_MAX_VIS;
  int bandH = visN * ROW_H;
  int bandTop = CY - bandH / 2;
  bool scrollable = p.nsess > LIST_MAX_VIS;
  int contentH = p.nsess * ROW_H;
  int maxScroll = scrollable ? (contentH - bandH) : 0;
  if (g_listScroll < 0) g_listScroll = 0;
  if (g_listScroll > maxScroll) g_listScroll = maxScroll;          // 钳制(数据变少时自愈)
  int off = g_listScroll;                                          // 用户可控偏移(swipe 翻页)
  g_listBandTop = bandTop; g_listBandH = bandH; g_listOff = off;   // 供 rowHitAt 命中检测
  g_listVisN = visN; g_listAuto = scrollable; g_listMaxScroll = maxScroll;

  // 行(按 g_listScroll 偏移,裁剪到行带;用户 swipe 翻页控制,不再定时自动滚动)
  cv.setClipRect(40, bandTop, SIZE - 80, bandH);
  {
    int base = bandTop - off;
    for (int i = 0; i < p.nsess; i++) {
      int y = base + i * ROW_H;
      if (y + ROW_H < bandTop || y > bandTop + bandH) continue;
      const Sess& s = p.sess[i];
      SessStatus st = (SessStatus)s.status;
      uint16_t dc = c565(statusDotColor(st));

      if (st == ST_WAITING) {
        cv.fillRoundRect(46, y + 3, SIZE - 92, ROW_H - 6, 6, c565(shade(0xffa94d, -0.82f)));
        cv.fillRect(46, y + 3, 4, ROW_H - 6, c565(0xffa94d));
      } else if (st == ST_EXECUTING) {
        cv.fillRoundRect(46, y + 3, SIZE - 92, ROW_H - 6, 6, c565(shade(color, -0.82f)));
      }

      // 行1:状态点 + 名称 + 分支摘要 + 状态词
      if (g_disp.col[DC_STATUS]) {
        if (st == ST_EXECUTING || st == ST_WAITING) cv.fillSmoothCircle(58, y + 17, 5, dc);
        else { cv.drawCircle(58, y + 17, 5, dc); cv.drawCircle(58, y + 17, 4, dc); }
      }
      char nm[28]; truncCp(s.name, 13, nm, sizeof(nm));
      cv.loadFont(Grotesk20); cv.setTextDatum(middle_left); cv.setTextColor(c565(0xe6e8ec));
      cv.drawString(nm, 72, y + 17);
      int nameW = cv.textWidth(nm); cv.unloadFont();
      if (g_disp.col[DC_BRANCH] && s.branch[0]) {
        char br[36]; truncCp(s.branch, 12, br, sizeof(br));
        char gb[48]; snprintf(gb, sizeof(gb), " %s +%d~%d", br, s.added, s.modified);
        cv.setFont(&fonts::efontCN_16); cv.setTextDatum(middle_left);
        cv.setTextColor(c565(0x7d828a));
        cv.drawString(gb, min(250, 76 + nameW), y + 17);
      }
      if (g_disp.col[DC_STATUS]) {
        cv.loadFont(JBMono16); cv.setTextDatum(middle_right); cv.setTextColor(dc);
        cv.drawString(statusWord(st), SIZE - 52, y + 17); cv.unloadFont();
      }

      // 行2:model/ctx/tokens/turn/memory 的点分串
      char md[16]; modelShort(s.model, md, sizeof(md));
      for (char* a = md, *b = md; ; ++a) { if (*a != ' ') *b++ = *a; if (!*a) break; }
      char tb[12]; fmtTokens(s.tokTotal, tb, sizeof(tb));
      char mb[10]; fmtMem(s.memKb, mb, sizeof(mb));
      char meta[96]; composeMetaLine(&g_disp, md, s.ctxPct, tb, s.turn, mb, meta, sizeof(meta));
      if (meta[0]) {
        cv.loadFont(JBMono16); cv.setTextDatum(middle_left);
        cv.setTextColor(c565(st == ST_WAITING ? 0xffc180 : 0x8a8d94));
        cv.drawString(meta, 72, y + 36); cv.unloadFont();
      }

      // 行3:执行任务优先;否则按 summary 列显示首条提示摘要
      char line3[80];
      if (s.task[0]) snprintf(line3, sizeof(line3), "* %s", s.task);
      else if (g_disp.col[DC_SUMMARY] && s.summary[0]) snprintf(line3, sizeof(line3), "%s", s.summary);
      else line3[0] = 0;
      if (line3[0]) {
        char short3[80]; truncCp(line3, 26, short3, sizeof(short3));
        cv.setFont(&fonts::efontCN_16); cv.setTextDatum(middle_left);
        cv.setTextColor(c565(s.task[0] ? 0xcfd2d8 : 0x757a82));
        cv.drawString(short3, 72, y + 54);
      }
    }
  }
  cv.clearClipRect();

  drawDots(provIdx, color);
}
// 数据小块:顶部小标签 + 中部大数值
static void drawStatTile(int cx, int cy, int w, int h, const char* label, const char* value, uint32_t color) {
  cv.fillRoundRect(cx - w / 2, cy - h / 2, w, h, 7, c565(shade(color, -0.80f)));
  cv.drawRoundRect(cx - w / 2, cy - h / 2, w, h, 7, c565(shade(color, -0.42f)));
  cv.setFont(&fonts::Font0); cv.setTextDatum(middle_center);
  cv.setTextColor(c565(0x8a9097)); cv.drawString(label, cx, cy - h / 2 + 11);
  cv.loadFont(JBMono20); cv.setTextColor(c565(COL_WHITE));   // 数值走 VLW(去颗粒)
  cv.drawString(value, cx, cy + 5); cv.unloadFont();
}

// 会话详情:三宫格数据风(name·PROVIDER / model·时长 / 小吉祥物+状态 / ctx·turn·tok 三宫格 / 任务 / git·sub / 位置)
static void renderDetailPage() {
  const Prov& p = PROV[detailProv];
  if (detailIdx < 0 || detailIdx >= p.nsess) { cv.fillSprite(c565(0x000000)); return; }
  const Sess& s = p.sess[detailIdx];
  uint32_t color = p.color;
  SessStatus st = (SessStatus)s.status;

  cv.fillSprite(c565(0x000000));
  drawArc(cv, color, s.ctxPct);                       // 边缘弧 = ctx%

  // 头部:provider 色点 + 会话名称(只显示名称,不再带 · CLAUDE)
  char nm[40]; truncCp(s.name, 18, nm, sizeof(nm));
  cv.loadFont(Grotesk20); cv.setTextDatum(middle_center);   // 会话名走 VLW
  int tWid = cv.textWidth(nm);
  cv.fillCircle(CX - tWid / 2 - 11, 44, 4, c565(color));
  cv.setTextColor(c565(0xcfd2d8)); cv.drawString(nm, CX, 44); cv.unloadFont();

  // 第二行:model · 已运行时长(efontCN 字体能渲染 ·,不再是方框)。MODEL 列关 → 只显时长。
  long nowE = time(nullptr); bool epochOK = epochSane(nowE);
  long elapsed = (epochOK && s.startedAt > 0) ? (nowE - s.startedAt) : 0;
  char el[16]; fmtElapsed(elapsed, el, sizeof(el));
  char l2[64];
  int p2 = 0;
  if (g_disp.col[DC_MODEL]) { char md[24]; modelShort(s.model, md, sizeof(md)); p2 += snprintf(l2 + p2, sizeof(l2) - p2, "%s · ", md); }
  p2 += snprintf(l2 + p2, sizeof(l2) - p2, "%s", el);
  if (s.effort[0]) snprintf(l2 + p2, sizeof(l2) - p2, " · %s", s.effort);   // effort:此前解析后从不显示
  cv.setFont(&fonts::efontCN_16); cv.setTextDatum(middle_center);
  cv.setTextColor(c565(0x7d828a)); cv.drawString(l2, CX, 68);

  // 缩小版的首页同款动画机器人 + 状态词
  float t = (millis() - bootMs) / 1000.0f;
  const char* mood = moodForStatus(s.status);
  drawMascot(detailProv, CX, 118, color, mood, t, 0.62f);
  cv.loadFont(Grotesk20); cv.setTextDatum(middle_center);   // 状态词走 VLW
  uint16_t sw = c565(st == ST_EXECUTING ? shade(color, 0.25f)
                   : st == ST_THINKING ? 0xffd479
                   : st == ST_WAITING ? 0xffa94d : 0x8b9097);
  cv.setTextColor(sw); cv.drawString(statusWord(st), CX, 180); cv.unloadFont();

  // 数据宫格:保持原「三宫格」CTX / TURN / TOKENS,各自由对应列开关控制(关掉则隐去该块,启用块均分居中)。
  // memory 列只作用于列表表格,不在详情页加块 —— 保留刻意的三宫格默认外观不变。
  char ctxv[8]; snprintf(ctxv, sizeof(ctxv), "%d%%", s.ctxPct);
  char turnv[8]; snprintf(turnv, sizeof(turnv), "%d", s.turn);
  char tokv[12]; fmtTokens(s.tokTotal, tokv, sizeof(tokv));   // K千/W万/B十亿
  struct Tile { int col; const char* label; const char* value; };
  Tile tiles[3]; int nt = 0;
  if (g_disp.col[DC_CTX])    tiles[nt++] = { DC_CTX,    "CTX",    ctxv };
  if (g_disp.col[DC_TURN])   tiles[nt++] = { DC_TURN,   "TURN",   turnv };
  if (g_disp.col[DC_TOKENS]) tiles[nt++] = { DC_TOKENS, "TOKENS", tokv };
  if (nt > 0) {
    const int tw = 96, th = 50, gap = 10, ty = 228;
    int span = nt * tw + (nt - 1) * gap;
    int cx0 = CX - span / 2 + tw / 2;                       // 第一块的圆心 x
    for (int k = 0; k < nt; k++) {
      int tcx = cx0 + k * (tw + gap);
      drawStatTile(cx0 + k * (tw + gap), ty, tw, th, tiles[k].label, tiles[k].value, color);
      if (tiles[k].col == DC_CTX && s.compactions > 0) {
        char cc[8]; snprintf(cc, sizeof(cc), "%dc", s.compactions);
        cv.loadFont(JBMono16); cv.setTextDatum(top_right); cv.setTextColor(c565(0xffa94d));
        cv.drawString(cc, tcx + tw / 2 - 7, ty - th / 2 + 5); cv.unloadFont();
      }
    }
  }

  // token 四件套:in/out/cache_r/cache_w(Codex 无 cache_w)
  { char a[12], b[12], cr[12], cw[12];
    fmtTokens(s.tokIn, a, sizeof(a)); fmtTokens(s.tokOut, b, sizeof(b));
    fmtTokens(s.tokCacheR, cr, sizeof(cr)); fmtTokens(s.tokCacheW, cw, sizeof(cw));
    char line[72];
    if (detailProv == 1) snprintf(line, sizeof(line), "in %s  out %s  cR %s", a, b, cr);
    else                 snprintf(line, sizeof(line), "in %s out %s cR %s cW %s", a, b, cr, cw);
    cv.loadFont(JBMono16); cv.setTextDatum(middle_center); cv.setTextColor(c565(0x8a8d94));
    cv.drawString(line, CX, 268); cv.unloadFont(); }

  // 任务/agent 名称(efontCN 可渲染中文与 ·;超宽 → 跑马灯循环)
  char buf[96];
  if (s.task[0]) snprintf(buf, sizeof(buf), "* %s", s.task);
  else if (s.summary[0]) snprintf(buf, sizeof(buf), "\xE2\x80\x9C%s\xE2\x80\x9D", s.summary);
  else           snprintf(buf, sizeof(buf), "* idle");
  cv.setFont(&fonts::efontCN_16); cv.setTextColor(c565(s.task[0] ? 0xe6e8ec : 0x6f757d));
  const int taskWin = SIZE - 120, taskY = 288;            // 任务行可视窗 ~346px
  int taskW = cv.textWidth(buf);
  if (taskW <= taskWin) {                                 // 不超宽:居中
    cv.setTextDatum(middle_center); cv.drawString(buf, CX, taskY);
  } else {                                                // 超宽:跑马灯
    int winL = CX - taskWin / 2, scrollW = taskW + 48;
    int off = (int)((millis() / 40) % (uint32_t)scrollW);
    cv.setClipRect(winL, taskY - 11, taskWin, 22); cv.setTextDatum(middle_left);
    cv.drawString(buf, winL - off, taskY);
    cv.drawString(buf, winL - off + scrollW, taskY);      // 第二份 → 无缝循环
    cv.clearClipRect();
  }

  // git / subagents / ports 行(efontCN 渲染 ·)
  int n = snprintf(buf, sizeof(buf), "git %s +%d ~%d", s.branch[0] ? s.branch : "-", s.added, s.modified);
  if (s.subagents > 0) n += snprintf(buf + n, sizeof(buf) - n, " · %dsub", s.subagents);
  for (int pi = 0; pi < s.nports && n < (int)sizeof(buf) - 10; pi++)   // 全部端口(此前只显示 ports[0])
    n += snprintf(buf + n, sizeof(buf) - n, "%s:%d", pi == 0 ? " · " : " ", s.ports[pi]);
  cv.setFont(&fonts::efontCN_16); cv.setTextColor(c565(0xc3c7cd)); cv.setTextDatum(middle_center);
  cv.setClipRect(48, 302, SIZE - 96, 22); cv.drawString(buf, CX, 312); cv.clearClipRect();

  // ctx 预算原始值(把抽象 % 落到具体 token;此前 ctxTok/ctxWin 解析后从不显示)。空带 y340,圆屏安全居中。
  if (s.ctxTok > 0) {
    char ca[12], cwn[12]; fmtK(s.ctxTok, ca, sizeof(ca)); fmtK(s.ctxWin, cwn, sizeof(cwn));
    char cbud[32]; snprintf(cbud, sizeof(cbud), "ctx %s / %s", ca, cwn);
    cv.loadFont(JBMono16); cv.setTextDatum(middle_center); cv.setTextColor(c565(0x6d7077));
    cv.drawString(cbud, CX, 340); cv.unloadFont();
  }

  // 点屏切 Mac 终端 tab 的提示(仅 WS 连上时显示,避免离线误导)
  if (g_wsConn) {
    cv.setFont(&fonts::efontCN_16); cv.setTextDatum(middle_center); cv.setTextColor(c565(0x55585f));
    cv.drawString("点屏 → 切到 Mac", CX, 384);
  }

  // 底部:位置点 + i/N
  int nd = p.nsess > 9 ? 9 : p.nsess;
  char pos[12]; snprintf(pos, sizeof(pos), "%d/%d", detailIdx + 1, p.nsess);
  cv.loadFont(JBMono16); int posW = cv.textWidth(pos);     // 位置 i/N 走 VLW
  const int dotW = 7, dgap = 6;
  int dotsW = nd * dotW + (nd - 1) * dgap;
  int x0 = CX - (dotsW + 12 + posW) / 2;
  for (int i = 0; i < nd; i++) {
    bool cur = (i == detailIdx) || (detailIdx >= 9 && i == 8);
    cv.fillSmoothCircle(x0 + i * (dotW + dgap) + dotW / 2, 410, cur ? 3 : 2, c565(cur ? color : 0x44474e));
  }
  cv.setTextDatum(middle_left); cv.setTextColor(c565(0x6f757d));
  cv.drawString(pos, x0 + dotsW + 12, 410); cv.unloadFont();
}
