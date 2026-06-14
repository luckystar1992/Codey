// sketches/codey_dash/codey_widgets.h — reusable chrome (header/dots/wifi/meter/tile/session-count/
// provider-arc) + 状态/心情/格式 helpers。单 TU 习惯:依赖 .ino 的全局(cv/g_batt/g_wifi/g_companionOk/
// g_ssid/PROV/g_ringA·B/g_ringAok·Bok)与 codey_mascot.h 的 drawArc —— 本头在它们之后 #include。
#pragma once
#include <M5Unified.h>
#include "codey_theme.h"
#include "codey_ui.h"
#include "session_store.h"

// ---------- header (dot + name · clock · battery) ----------
static void drawHeader(const Prov& p, const String& clock) {
  uint16_t cc = c565(p.color);
  cv.setFont(&fonts::FreeSansBold12pt7b); cv.setTextSize(1);
  int nameW = cv.textWidth(p.name);
  int total = 8 + 8 + nameW, sx = CX - total / 2;
  cv.fillCircle(sx + 4, 54, 4, cc);
  cv.setTextColor(c565(COL_WHITE)); cv.setTextDatum(middle_left);
  cv.drawString(p.name, sx + 16, 54);

  cv.setFont(&fonts::FreeMono9pt7b); cv.setTextSize(1);
  int clkW = cv.textWidth(clock.c_str());
  bool warn = g_batt <= 20;
  uint16_t bc = warn ? c565(COL_DANGER) : c565(0xD0D0D0);
  int rowW = clkW + 22 + (22 + 6 + 20), rx = CX - rowW / 2, ry = 84;
  cv.setTextColor(c565(0x808080)); cv.setTextDatum(middle_left);
  cv.drawString(clock.c_str(), rx, ry);
  int dvx = rx + clkW + 11;
  cv.drawFastVLine(dvx, ry - 5, 11, c565(0x303030));
  int bx = dvx + 11, by = ry - 6;
  cv.drawRoundRect(bx, by, 22, 12, 2, bc);
  cv.fillRect(bx + 22, by + 4, 2, 4, bc);
  cv.fillRect(bx + 1, by + 1, max(2, (int)((22 - 3) * (g_batt / 100.0f))), 10, bc);
  cv.setTextColor(bc);
  cv.drawString(String(g_batt).c_str(), bx + 28, ry);
}


// ---------- page dots ----------
static void drawDots(int active, uint32_t color) {
  int n = 2, gap = 7, dotW = 7, longW = 18, y = 446;
  int total = 0; for (int i = 0; i < n; i++) total += (i == active ? longW : dotW) + (i ? gap : 0);
  int x = CX - total / 2;
  for (int i = 0; i < n; i++) {
    int w = (i == active ? longW : dotW);
    cv.fillRoundRect(x, y - 3, w, 7, 3, i == active ? c565(color) : c565(0x3a3c41));
    x += w + gap;
  }
}

// ---------- WiFi status row (bottom): status dot + SSID, marquee-scrolls if the name is too long ----------
static void drawWifiStatus(int y) {
  const uint16_t wifiDot = g_wifi ? c565(0x3CCB7F) : c565(0x6a6d74);          // 前灯: WiFi 连接
  const uint16_t usbDot  = g_companionOk ? c565(0x3CCB7F) : c565(COL_DANGER); // 后灯: usage 接口可达
  String ssid = g_wifi ? (g_ssid[0] ? String(g_ssid) : String("WiFi")) : String("No WiFi");
  cv.setFont(&fonts::efontCN_16); cv.setTextSize(1);   // CJK-capable (中文 SSID)
  cv.setTextColor(c565(0xB8BAC0));
  const int maxW = 286, dotR = 3, gap = 8;
  int tw = cv.textWidth(ssid.c_str());
  if (tw <= maxW) {                                   // fits -> [wifiDot] SSID [usageDot], centered
    int total = dotR * 2 + gap + tw + gap + dotR * 2, sx = CX - total / 2;
    cv.fillCircle(sx + dotR, y, dotR, wifiDot);
    cv.setTextDatum(middle_left);
    cv.drawString(ssid.c_str(), sx + dotR * 2 + gap, y);
    cv.fillCircle(sx + dotR * 2 + gap + tw + gap + dotR, y, dotR, usbDot);
  } else {                                            // too long -> fixed dots + scrolling marquee
    int winL = CX - maxW / 2, winR = CX + maxW / 2;
    cv.fillCircle(winL - 9, y, dotR, wifiDot);
    cv.fillCircle(winR + 9, y, dotR, usbDot);
    int scrollW = tw + 48;
    int off = (int)((millis() / 40) % (uint32_t)scrollW);
    cv.setClipRect(winL, y - 11, maxW, 22);
    cv.setTextDatum(middle_left);
    cv.drawString(ssid.c_str(), winL - off, y);
    cv.drawString(ssid.c_str(), winL - off + scrollW, y);   // 2nd copy -> seamless loop
    cv.clearClipRect();
  }
}

// 倒计时格式(d/h/m/s 紧凑)
static String fmtDur(long secs) {
  if (secs < 0) secs = 0;
  long d = secs / 86400, h = (secs % 86400) / 3600, m = (secs % 3600) / 60, s = secs % 60;
  char b[16];
  if (d > 0)      snprintf(b, sizeof(b), "%ldd", d);
  else if (h > 0) snprintf(b, sizeof(b), "%ldh%02ldm", h, m);
  else if (m > 0) snprintf(b, sizeof(b), "%ldm", m);
  else            snprintf(b, sizeof(b), "%lds", s);
  return String(b);
}

// 用量心情(给主页大吉祥物)
static const char* moodForUsage(int used, int active, int battery) {
  if (battery <= 12) return "sleepy";
  if (active > 0)    return "alert";
  if (used >= 88)    return "worried";
  if (used >= 65)    return "tired";
  if (used >= 40)    return "focused";
  return "happy";
}

// 会话状态 → 详情页大吉祥物心情(动画与首页同款)
static const char* moodForStatus(uint8_t status) {
  switch (status) {
    case ST_EXECUTING: return "alert";
    case ST_THINKING:  return "focused";
    case ST_DONE:      return "happy";
    default:           return "sleepy";   // waiting
  }
}

// USAGE/WEEKLY 分段表(还原旧版)
static void drawMeter(int y, const char* label, int used, const String& reset, uint32_t color) {
  const int segs = 10; const int labelX = 56; const float pitch = 12.0f;
  const int barX = 120; const int pctRightX = 306; const int timeRightX = 378;
  int filled = constrain((int)roundf(used / 100.0f * segs), 0, segs);
  bool hot = used >= 85;
  uint16_t segc = c565(hot ? COL_DANGER : color), empty = c565(0x1b1c20);
  cv.setFont(&fonts::FreeSans9pt7b); cv.setTextSize(1);
  cv.setTextColor(c565(0x9a9ca2)); cv.setTextDatum(middle_left);
  cv.drawString(label, labelX, y);
  for (int i = 0; i < segs; i++)
    cv.fillRoundRect(barX + (int)(i * pitch), y - 5, (int)pitch - 3, 10, 2, i < filled ? segc : empty);
  char pc[8]; snprintf(pc, sizeof(pc), "%d%%", used);
  cv.setFont(&fonts::FreeMonoBold12pt7b); cv.setTextColor(c565(COL_WHITE)); cv.setTextDatum(middle_right);
  cv.drawString(pc, pctRightX, y);
  cv.setFont(&fonts::FreeMono9pt7b); cv.setTextColor(c565(0x6d6f75)); cv.setTextDatum(middle_right);
  cv.drawString(reset.c_str(), timeRightX, y);
}

// 活跃会话胶囊(N ACTIVE;点它/上滑进会话列表)
// 会话状态 → 红绿灯圆点颜色
static uint32_t statusDotColor(SessStatus st) {
  switch (st) {
    case ST_EXECUTING: return 0x3CCB7F;   // 绿:运行中
    case ST_THINKING:  return 0xFFB454;   // 黄:思考中
    case ST_WAITING:   return 0xFF5D5D;   // 红:等待(需要你)
    default:           return 0x6a6d74;   // 灰:已完成/其它
  }
}

// 列表表格:短状态词 + ctx% 分级配色
static const char* statusShort(SessStatus st) {
  switch (st) {
    case ST_EXECUTING: return "Exec";
    case ST_THINKING:  return "Think";
    case ST_DONE:      return "Done";
    default:           return "Wait";
  }
}
static uint32_t ctxColor(int pct) {
  if (pct >= 85) return 0xFF6B3C;   // 高:橙红
  if (pct >= 60) return 0xFFB454;   // 中:琥珀
  return 0x7FDCA0;                  // 低:绿
}

// 首页底部:session 运行数/总数(运行数绿色)
static void drawSessionCount(int cy, int running, int total) {
  char rs[8]; snprintf(rs, sizeof(rs), "%d", running);
  char ts[12]; snprintf(ts, sizeof(ts), "/%d", total);
  cv.setFont(&fonts::FreeMonoBold12pt7b); cv.setTextSize(1);
  const char* lbl = "session ";
  int lw = cv.textWidth(lbl), rw = cv.textWidth(rs), tw = cv.textWidth(ts);
  int x = CX - (lw + rw + tw) / 2;
  cv.setTextDatum(middle_left);
  cv.setTextColor(c565(0x9a9ca2)); cv.drawString(lbl, x, cy);
  cv.setTextColor(c565(0x3CCB7F)); cv.drawString(rs, x + lw, cy);            // 运行中:绿色
  cv.setTextColor(c565(0x9a9ca2)); cv.drawString(ts, x + lw + rw, cy);
}

// 把某 provider 的弧(=max(sess,week))画进它的缓存环再贴到 cv;主页/列表共用,避免串味
static void blitProviderArc(int provIdx) {
  static int aPct = -1, bPct = -1; static uint32_t aCol = 0, bCol = 0;
  const Prov& p = PROV[provIdx];
  int pct = max(p.sessUsed, p.weekUsed);
  M5Canvas* spr = (provIdx == 0) ? &g_ringA : &g_ringB;
  bool ok = (provIdx == 0) ? g_ringAok : g_ringBok;
  int* cpct = (provIdx == 0) ? &aPct : &bPct;
  uint32_t* ccol = (provIdx == 0) ? &aCol : &bCol;
  if (!ok) { cv.fillSprite(c565(0x000000)); drawArc(cv, p.color, pct); return; }
  if (pct != *cpct || p.color != *ccol) { spr->fillSprite(c565(0x000000)); drawArc(*spr, p.color, pct); *cpct = pct; *ccol = p.color; }
  spr->pushSprite(&cv, 0, 0);
}
