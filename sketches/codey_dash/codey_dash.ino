// codey_dash — real-time Claude Code / Codex agent-session monitor for the M5Stack StopWatch.
//
// Consumes /codey/state (companion, :8787) and shows live agent sessions across three pages,
// switched with BtnA / shake / horizontal swipe:
//   0  Dashboard — dual edge arc (Claude weekly% left / Codex right), two idle mascots framing
//      the active-session count a·b, a cross-provider top-N session grid, and a dirty/tok-rate line.
//   1  Claude session list — single weekly% arc + two rows per session (status·name·model·ctx%·tok·turn),
//      vertically scrollable by touch drag.
//   2  Codex session list — same, green.
//   Detail — tap a session (or BtnA long-press): big animated mascot reflecting status + current
//      task / git / context / subagents·ports; swipe or BtnA short-press to page through sessions.
// Touch: drag=scroll, h-swipe=switch page/session, tap=open detail, double-tap=close.
// True-black background, rendered to a full-screen PSRAM canvas for flicker-free animation.

#include <M5Unified.h>
#include <WiFi.h>           // STA 连接 + AP 热点 + scanNetworks
#include <WebServer.h>      // 自研 WiFi 配置门户(历史列表/一键连/删除/扫描)
#include <DNSServer.h>      // captive portal:连上热点自动弹配置页
#include <HTTPClient.h>     // fetch usage JSON from the Companion
#include <ArduinoJson.h>    // parse it
#include <WebSocketsClient.h>  // stream mic PCM to the sherpa-onnx ASR server, receive live text
#include <ESPmDNS.h>        // resolve the Mac by hostname (survives DHCP IP changes)
#include <Preferences.h>    // persist brightness/volume settings in NVS
#include <sys/time.h>       // settimeofday() — set the clock from the Companion's epoch
#include "freertos/stream_buffer.h"  // 语音 PCM 跨核(主loop -> netTask)
#include "wifi_store.h"     // 多 WiFi 记忆:历史网络(SSID/密码/连接次数)的 NVS 数据层
#include "codey_ui.h"
#include "session_store.h"

// ---------- palette (RGB888) ----------
static const uint32_t COL_CLAUDE = 0xF4894F;
static const uint32_t COL_CODEX  = 0x22D3A6;
static const uint32_t COL_DANGER = 0xFF5D5D;
static const uint32_t COL_WHITE  = 0xFFFFFF;

// (SessRef / collectSorted removed — dashboard replaced by per-provider usage pages)

// ---------- provider data ----------
static Prov PROV[2] = {
  { "Claude", COL_CLAUDE, 0, 0, 0, 0, 0, 0, 0, {}, 0 },
  { "Codex",  COL_CODEX,  0, 0, 0, 0, 0, 0, 0, {}, 0 },
};
static int g_batt = 76;             // refreshed from the real battery each second
static int g_clkH = 0, g_clkM = 0;  // refreshed from the on-board RTC each second

static inline uint16_t c565(uint32_t rgb) {
  uint8_t r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
static uint32_t shade(uint32_t rgb, float f) {  // f>0 lighten toward white, f<0 darken
  int r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
  if (f >= 0) { r += (255 - r) * f; g += (255 - g) * f; b += (255 - b) * f; }
  else        { r *= (1 + f);       g *= (1 + f);       b *= (1 + f); }
  r = constrain(r, 0, 255); g = constrain(g, 0, 255); b = constrain(b, 0, 255);
  return ((uint32_t)r << 16) | (g << 8) | b;
}

// ---------- geometry ----------
static const int SIZE = 466, CX = 233, CY = 233;

// ---------- canvas + state ----------
static M5Canvas cv(&M5.Display);
static M5Canvas g_ringA(&M5.Display), g_ringB(&M5.Display);   // 仪表盘/列表各缓存一张 AA 环;每页 render 用函数内 static 记 pct 缓存键
static bool     g_ringAok = false, g_ringBok = false;
static int      page = 0;
static uint32_t bootMs = 0;
static uint32_t lastSecMs = 0;
static volatile bool g_voice = false; // voice overlay active
static uint32_t g_voiceT0 = 0;       // millis() when the voice overlay started
static volatile bool g_wifi = false; // WiFi connected (主loop 写, netTask 读)
static char     g_ssid[48] = {0};    // connected SSID (bottom, marquee if long)
static bool     g_micOK = false;     // microphone available
static int16_t  g_micBuf[256];       // (legacy, unused)
static float    g_micLevel = 0.12f;  // smoothed mic level (0..1)
// ---- streaming voice: 主loop 采集 -> StreamBuffer -> netTask sendBIN -> sherpa partials ----
static int      g_vphase = 0;        // 0 off, 1 listening/streaming, 2 finalizing, 3 result
static char     g_transcript[256] = {0};   // live/final transcript (netTask 写, 主loop 读)
static const int    REC_RATE = 16000;
static const size_t MAX_SAMPLES = (size_t)(REC_RATE * 15);     // 15s max listen window
static const size_t STREAM_CHUNK = 512;                        // samples per WS frame (~32ms)
static int16_t* g_audioBuf = nullptr;// continuous mic-capture buffer (PSRAM)
static size_t   g_sentSamples = 0;   // 主loop 已写入 StreamBuffer 的样本位置
static size_t   g_recEnd = 0;        // capture length at stop (flush up to here)
static bool     g_heardSpeech = false;
static uint32_t g_silenceT0 = 0;     // start of trailing silence -> auto-stop
static uint32_t g_resultT0 = 0;      // when the result was shown (dismiss timeout)
static uint32_t g_finalReqT0 = 0;    // when listen-stop was requested (final timeout)
static volatile bool g_sttFinal = false;     // server sent a final stt
static float    g_noiseFloor = 0.06f;// adaptive VAD noise floor
// ---- network: 所有阻塞 IO 都在 netTask/core0;主loop 不等待 ----
static WebSocketsClient g_ws;        // ONLY touched by netTask
static volatile bool g_wsConn = false;
static const char*    MAC_HOSTNAME    = "testnull-2";
static const char*    MAC_FALLBACK_IP = "10.100.0.89";
static String         g_macIp = "10.100.0.89";   // netTask only
static char           g_manualMac[24] = {0};      // 手填 Companion IP(NVS;门户写/netTask读)
static const uint16_t ASR_PORT = 8788;
static char     g_model[48] = {0};   // Claude 模型名(头像下),netTask 写 主loop 读
static char     g_codexModel[48] = {0};  // Codex 最常用模型名(头像下)
static volatile int  g_netListenReq = 0;     // 1=start 2=stop (主loop -> netTask)
static volatile bool g_netReconnect = false; // reconfigWiFi 后让 netTask 重连
static volatile bool g_netPause = false;     // 门户运行时暂停 netTask 的 WiFi 操作(避免双核争用 WiFi 栈)
static StreamBufferHandle_t g_voiceSB = nullptr;  // 语音 PCM (主loop -> netTask)
static bool     g_rtcSynced = false;
// ---- settings page ----
static Preferences g_prefs;
static bool     g_inSettings = false;
static int      g_setSel = 0;
static uint8_t  g_bright = 255;      // display brightness 0-255 (persisted)
static uint8_t  g_volume = 50;       // speaker volume 0-100 (persisted)
static const char* SET_ITEMS[4] = { "WiFi 配置", "亮度", "音量", "返回" };
static const int   SET_N = 4;
static uint32_t lastActiveMs = 0;    // last interaction/motion (screen-dim timeout)
static bool     g_dim = false;       // screen dimmed
static float    g_accMag = 1.0f;     // last accel magnitude (motion detection)
static uint32_t g_lastShake = 0;     // shake-gesture debounce
// ---- live data from the Companion ----
static String g_companionUrl = "http://10.100.0.89:8787/codey/state";   // rebuilt from g_macIp at boot
static volatile bool g_haveData = false;
static volatile bool g_companionOk = false;   // usage 接口(Companion)是否可达 -> 尾灯 绿/红
static bool     g_stale = false;
static uint32_t g_lastFetch = 0;
static int      g_fetchFails = 0;    // consecutive fetch failures -> re-resolve Mac (IP drift self-heal)

// ---------- animation state (shared) ----------
static bool     aBlink = false;
static uint32_t aBlinkNext = 0, aBlinkEnd = 0;
static float    aGlance = 0, aGlanceTarget = 0;
static uint32_t aGlanceNext = 0;

static void updateAnim(uint32_t now) {
  if (!aBlink && now >= aBlinkNext) { aBlink = true; aBlinkEnd = now + 120; }
  if (aBlink && now >= aBlinkEnd)   { aBlink = false; aBlinkNext = now + 1500 + random(2800); }
  if (now >= aGlanceNext) { aGlanceTarget = (random(201) - 100) / 100.0f; aGlanceNext = now + 1000 + random(1800); }
  aGlance += (aGlanceTarget - aGlance) * 0.15f;
}

// ---------- the edge usage arc — a clean ring drawn with overlapping circles ----------
// Custom anti-aliased arc: per-pixel sub-pixel coverage on the band's radial edges (M5GFX has
// no native smooth arc). Convention-free — the angle is computed from the pixel itself
// (design space: 0=top, clockwise), so the gap is always centered at the bottom. Background is
// black here, so coverage blends by scaling the colour toward black.

// 通用 AA 弧:沿 [startDeg, startDeg+sweepDeg] 画轨道,按 pct 填充;reverse=true 时从末端起填。
static void drawArcRange(M5Canvas& dst, uint32_t color, int pct,
                         float startDeg, float sweepDeg, bool reverse) {
  const float rIn = 218.0f, rOut = 232.0f;   // 紧贴屏幕最外一圈(半径 233)
  const float p = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
  const float fillDeg = sweepDeg * p / 100.0f;
  const float loR = (rIn - 0.7f) * (rIn - 0.7f), hiR = (rOut + 0.7f) * (rOut + 0.7f);
  const int Rb = (int)rOut + 1;
  for (int dy = -Rb; dy <= Rb; dy++) {
    int py = CY + dy; if ((unsigned)py >= (unsigned)SIZE) continue;
    float fy = (float)dy, fyy = fy * fy;
    for (int dx = -Rb; dx <= Rb; dx++) {
      float r2 = (float)dx * dx + fyy;
      if (r2 > hiR || r2 < loR) continue;
      int px = CX + dx; if ((unsigned)px >= (unsigned)SIZE) continue;
      float rr = sqrtf(r2);
      float cov = fminf(rr - (rIn - 0.5f), (rOut + 0.5f) - rr);
      if (cov <= 0.0f) continue; if (cov > 1.0f) cov = 1.0f;
      float d = atan2f((float)dx, -fy) * 57.2957795f;          // design angle (0=top, cw)
      float dn = d - startDeg; if (dn < 0) dn += 360.0f;
      if (dn > sweepDeg) continue;                             // outside this segment
      float along = reverse ? (sweepDeg - dn) : dn;            // distance from the "fill origin"
      uint32_t rgb = (along <= fillDeg) ? color : 0x23262c;
      dst.drawPixel(px, py, c565(shade(rgb, -(1.0f - cov))));
    }
  }
  if (pct > 0) {                                               // glowing cap at the progress tip
    float tip = reverse ? (startDeg + sweepDeg - fillDeg) : (startDeg + fillDeg);
    float a = (tip - 90.0f) * DEG_TO_RAD;
    int hx = CX + 224 * cosf(a), hy = CY + 224 * sinf(a);
    dst.fillSmoothCircle(hx, hy, 9, c565(COL_WHITE));
    dst.fillSmoothCircle(hx, hy, 6, c565(color));
  }
}

// 旧整段环(底部 84° 缺口居中):列表/详情页单弧沿用。
static void drawArc(M5Canvas& dst, uint32_t color, int pct) {
  drawArcRange(dst, color, pct, -138.0f, 276.0f, false);
}

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

// ---------- mascot helpers ----------
static float eyeOpenFor(const char* mood) {
  if (!strcmp(mood, "sleepy")) return 0.10f;
  if (!strcmp(mood, "tired"))  return 0.42f;
  if (!strcmp(mood, "focused"))return 0.60f;
  if (!strcmp(mood, "worried"))return 1.15f;
  if (!strcmp(mood, "alert"))  return 1.30f;
  return 1.0f;
}

// a 3D-shaded backing orb for the avatar — dark base, lit toward the upper-left (sphere look)
static void drawAvatarOrb(int cx, int cy, int R, uint32_t color) {
  cv.fillSmoothCircle(cx, cy, R, c565(shade(color, -0.82f)));
  cv.fillSmoothCircle(cx - R / 5, cy - R / 5, (int)(R * 0.72f), c565(shade(color, -0.66f)));
  cv.fillSmoothCircle(cx - (int)(R * 0.33f), cy - (int)(R * 0.33f), (int)(R * 0.42f), c565(shade(color, -0.50f)));
}

// Claude pixel creature (13x15 grid), lit from the upper-left for a 3D look.
static void drawClaude(int ccx, int ccy, uint32_t color, const char* mood, float t) {
  const int GW = 13, GH = 15; const float cell = 102.0f / GH;   // 10% smaller
  float bob = sinf(t * 1.7f) * 2.0f;
  if (!strcmp(mood, "alert")) bob = -fabsf(sinf(t * 4.2f)) * 5.0f;   // alert hops
  float ox = ccx - (GW * cell) / 2.0f, oy = ccy - (GH * cell) / 2.0f + bob;
  auto PX = [&](float v) { return ox + v * cell; };
  auto PY = [&](float v) { return oy + v * cell; };

  uint32_t colHi = shade(color, 0.22), colLo = shade(color, -0.26);

  drawAvatarOrb(ccx, ccy, 63, color);   // 3D-shaded backing orb (10% smaller)

  float legCols[4] = { 2.4f, 4.4f, 7.6f, 9.6f };
  for (int i = 0; i < 4; i++) {
    float ph = sinf(t * 6 + i * PI) * 0.5f + 0.5f;
    float len = 2.2f - ph * 0.5f;
    cv.fillRect(PX(legCols[i] - 0.5f), PY(9.9f), cell + 1, len * cell + 1, c565(colLo));
  }
  const float L = 1, R = 12, T = 1, Bo = 10, rad = 2.7f;
  for (int cy = 0; cy < GH; cy++) for (int cx = 0; cx < GW; cx++) {
    float px = cx + 0.5f, py = cy + 0.5f;
    if (px < L || px > R || py < T || py > Bo) continue;
    float nx = constrain(px, L + rad, R - rad), ny = constrain(py, T + rad, Bo - rad);
    if ((px - nx) * (px - nx) + (py - ny) * (py - ny) > rad * rad + 0.02f) continue;
    float fx = (px - L) / (R - L), fy = (py - T) / (Bo - T);
    float lit = (1.0f - fx) * 0.5f + (1.0f - fy) * 0.5f;          // upper-left lit, lower-right shaded
    uint32_t c = shade(color, (lit - 0.5f) * 0.95f);             // smooth diagonal gradient -> 3D volume
    cv.fillRect(PX(cx), PY(cy), cell + 1, cell + 1, c565(c));
  }
  cv.fillRect(PX(2.0f), PY(1.6f), cell * 1.2f, cell * 1.0f, c565(shade(color, 0.6f)));   // glossy highlight
  cv.fillSmoothCircle(PX(2.6f), PY(2.0f), cell * 0.34f, c565(0xF6F6F6));                  // specular dot

  float open = aBlink ? 0.08f : eyeOpenFor(mood);
  float ew = 1.4f, eh = 2.0f * open, ey = 4.7f + (2.0f - eh) / 2.0f, ex = aGlance * 0.9f;
  float exC[2] = { 3.5f, 8.5f };
  for (int k = 0; k < 2; k++) {
    cv.fillRect(PX(exC[k] - ew / 2 + ex), PY(ey), ew * cell + 1, eh * cell + 1, c565(0x0c0c0e));
    if (open > 0.5f) cv.fillRect(PX(exC[k] - ew / 2 + ex + 0.1f), PY(ey + 0.12f), cell * 0.42f, cell * 0.42f, c565(0xD8D8D8));
  }
  if (!strcmp(mood, "alert")) {
    int bx = ccx + 40, by = ccy - 48;
    cv.fillRoundRect(bx - 13, by - 13, 26, 26, 7, c565(COL_WHITE));
    cv.setFont(&fonts::FreeMonoBold12pt7b); cv.setTextColor(c565(0x0a0a0a)); cv.setTextDatum(middle_center);
    cv.drawString("!", bx, by);
  }
}

// Codex mascot — a cute rounded helper-bot ported from robot-blink.svg: gray two-tone
// shell lit from the left, a dark indigo visor holding two mint eyes that blink, a little
// white smile, side ears and a rounded body. SVG(508x526) is mapped to the screen 1:1.
static void drawCodex(int ccx, int ccy, uint32_t color, const char* mood, float t) {
  (void)mood;
  const float S = 0.252f;                                  // SVG units -> screen px (10% smaller, matches Claude)
  const float bob = sinf(t * 1.6f) * 2.0f;                 // gentle idle bob
  auto X = [&](float sx) { return (int)lroundf(ccx + (sx - 260.0f) * S); };
  auto Y = [&](float sy) { return (int)lroundf(ccy + (sy - 274.0f) * S + bob); };
  auto W = [&](float w)  { return (int)lroundf(w * S); };
  const int seam = X(260);                                 // light/shade split (SVG x=260)

  const uint16_t shellHi = c565(0xE4E6E1), shellLo = c565(0xB9B6AF);   // lit / shaded gray
  const uint16_t ear     = c565(0xBEBBB4);
  const uint16_t visorHi = c565(0x272044), visorLo = c565(0x12122C);   // visor indigo
  const uint16_t eyeCol  = c565(0x8AD8C7);                              // mint eyes
  const uint16_t eyeGlow = c565(shade(0x8AD8C7, -0.45f));
  const uint16_t white   = c565(0xFFFFFF);

  // 3D-shaded backing orb (10% smaller)
  drawAvatarOrb(ccx, (int)lroundf(ccy + bob), 63, color);

  // two-tone rounded rect split vertically at `seam` (left lit, right shaded)
  auto twoTone = [&](int x, int y, int w, int h, int r, uint16_t hi, uint16_t lo) {
    cv.setClipRect(x, y, max(0, seam - x), h);          cv.fillRoundRect(x, y, w, h, r, hi);
    cv.setClipRect(seam, y, max(0, x + w - seam), h);   cv.fillRoundRect(x, y, w, h, r, lo);
    cv.clearClipRect();
  };

  // side ears
  cv.fillRoundRect(X(58),  Y(140), W(40), W(64), W(10), ear);
  cv.fillRoundRect(X(424), Y(140), W(40), W(64), W(10), ear);

  // body (drawn first; head overlaps its top = clean neck), with arm nubs + a shoulder joint
  twoTone(X(150), Y(316), W(220), W(176), W(54), shellHi, shellLo);
  cv.fillRoundRect(X(118), Y(392), W(40), W(48), W(16), shellLo);
  cv.fillRoundRect(X(350), Y(392), W(40), W(48), W(16), shellLo);
  cv.fillCircle(X(212), Y(375), W(13), shellLo);
  cv.fillSmoothCircle(X(188), Y(360), W(22), c565(shade(0xE4E6E1, 0.45f)));   // body specular (3D)

  // head shell (two-tone)
  twoTone(X(105), Y(58), W(326), W(238), W(74), shellHi, shellLo);

  // dark visor (two-tone) + a tiny chin notch
  twoTone(X(125), Y(102), W(269), W(174), W(58), visorHi, visorLo);
  cv.fillTriangle(X(205), Y(248), X(227), Y(248), X(216), Y(278), visorLo);
  // glossy reflection on the visor (upper-left) -> 3D glass look
  cv.setClipRect(X(125), Y(102), W(269), W(174));
  cv.fillEllipse(X(180), Y(132), W(50), W(15), c565(shade(0x272044, 0.7f)));
  cv.clearClipRect();

  // eyes — mint, blink by flattening vertically; subtle glance + glint
  const int gx = (int)lroundf(aGlance * 4);
  const int eyR = W(22), ry = aBlink ? max(2, W(3)) : eyR, eyeY = Y(175);
  const int ex[2] = { X(190) + gx, X(330) + gx };
  for (int s = 0; s < 2; s++) {
    cv.fillEllipse(ex[s], eyeY, eyR + 3, ry + 3, eyeGlow);                   // glow
    cv.fillEllipse(ex[s], eyeY, eyR, ry, eyeCol);                           // eye
    if (!aBlink) cv.fillCircle(ex[s] - eyR / 3, eyeY - eyR / 3, max(2, W(6)), white);  // glint
  }

  // little white smile (lower half of an ellipse) under the eyes
  { int sx = X(260) + gx, sy = Y(202), sw = W(30), sh = W(24);
    cv.setClipRect(sx - sw, sy, sw * 2, sh + 2);
    cv.fillEllipse(sx, sy, sw, sh, white);
    cv.clearClipRect();
  }
}

// 小号吉祥物:3D orb + 两只眼,用于仪表盘/列表(大号 drawClaude/drawCodex 留给详情页)
static void drawMiniMascot(int cx, int cy, int R, uint32_t color, bool isClaude, uint8_t status) {
  drawAvatarOrb(cx, cy, R, color);
  float open = (status == ST_EXECUTING) ? 1.15f : (status == ST_THINKING) ? 0.85f : 0.55f;
  if (aBlink) open = 0.12f;
  int ex = (int)(R * 0.34f), ey = cy - (int)(R * 0.05f);
  int ew = (int)(R * 0.20f), eh = (int)(R * 0.30f * open) + 2;
  uint16_t ec = isClaude ? c565(0x140a04) : c565(0x8AD8C7);
  if (isClaude) {
    cv.fillRoundRect(cx - ex - ew / 2, ey - eh / 2, ew, eh, 2, ec);
    cv.fillRoundRect(cx + ex - ew / 2, ey - eh / 2, ew, eh, 2, ec);
  } else {
    cv.fillSmoothCircle(cx - ex, ey, max(2, eh / 2), ec);
    cv.fillSmoothCircle(cx + ex, ey, max(2, eh / 2), ec);
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
static void drawActivePill(int cy, int n, uint32_t color) {
  bool on = n > 0; uint16_t cc = c565(color);
  char num[4]; snprintf(num, sizeof(num), "%d", n);
  cv.setFont(&fonts::FreeMonoBold9pt7b); int nW = cv.textWidth(num);
  cv.setFont(&fonts::FreeSans9pt7b);     int tW = cv.textWidth("ACTIVE");
  int contentW = 7 + 7 + nW + 8 + tW;
  int w = contentW + 26, x = CX - w / 2, h = 30, y = cy - h / 2;
  cv.fillRoundRect(x, y, w, h, 15, on ? c565(shade(color, -0.7)) : c565(0x0e0f12));
  cv.drawRoundRect(x, y, w, h, 15, on ? cc : c565(0x2a2c31));
  int ix = x + 13;
  cv.fillCircle(ix + 3, cy, 3, on ? cc : c565(0x4d4f55));
  cv.setFont(&fonts::FreeMonoBold9pt7b);
  cv.setTextColor(on ? c565(COL_WHITE) : c565(0x8a8c92)); cv.setTextDatum(middle_left);
  cv.drawString(num, ix + 11, cy);
  cv.setFont(&fonts::FreeSans9pt7b); cv.setTextColor(c565(0x808288)); cv.setTextDatum(middle_left);
  cv.drawString("ACTIVE", ix + 11 + nW + 8, cy);
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

// ---- WebSocket streaming-ASR client ----
static void wsListen(bool start) {            // listen-control messages (xiaozhi-style)
  if (!g_wsConn) return;
  g_ws.sendTXT(start ? "{\"type\":\"listen\",\"state\":\"start\",\"mode\":\"manual\"}"
                     : "{\"type\":\"listen\",\"state\":\"stop\"}");
}

static void wsEvent(WStype_t type, uint8_t* payload, size_t len) {
  if (type == WStype_CONNECTED) {
    g_wsConn = true;
    g_ws.sendTXT("{\"type\":\"hello\",\"version\":1,\"transport\":\"websocket\","
                 "\"audio_params\":{\"format\":\"pcm\",\"sample_rate\":16000,\"channels\":1}}");
    Serial.println("[ws] connected");
  } else if (type == WStype_DISCONNECTED) {
    g_wsConn = false;
    Serial.println("[ws] disconnected");
  } else if (type == WStype_TEXT) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, len)) return;
    if (strcmp(doc["type"] | "", "stt") == 0) {
      strncpy(g_transcript, (const char*)(doc["text"] | ""), sizeof(g_transcript) - 1);
      g_transcript[sizeof(g_transcript) - 1] = '\0';            // live partial / final
      if (doc["final"] | false) g_sttFinal = true;
    }
  }
}

// ---------- voice overlay (LISTENING animation, triggered by the right button) ----------
// A multi-colour particle orb that pulses with the mic level (real if available, else simulated).
static void drawVoiceParticles(int cx, int cy, float amp, float t) {
  static const int N = 80;
  static const uint32_t PAL[6] = { 0xF4894F, 0x22D3A6, 0x35B8FF, 0xFF6BB0, 0xFFD24A, 0xB48CFF };
  static float pa[N], prad[N], pspd[N], pz[N], ptw[N];
  static uint8_t pcix[N];
  static bool init = false;
  if (!init) {
    for (int i = 0; i < N; i++) {
      pa[i]   = (random(1000) / 1000.0f) * TWO_PI;
      prad[i] = 0.30f + (random(1000) / 1000.0f) * 0.70f;        // fraction of orb radius
      pspd[i] = (0.2f + (random(1000) / 1000.0f) * 0.8f) * (random(2) ? 1 : -1);
      pz[i]   = random(1000) / 1000.0f;
      ptw[i]  = (random(1000) / 1000.0f) * TWO_PI;
      pcix[i] = random(6);                                       // pick a palette colour
    }
    init = true;
  }
  const float R = 98.0f * (0.85f + amp * 0.5f);                  // orb expands with level
  for (int i = 0; i < N; i++) {
    pa[i]  += 0.012f * pspd[i] * (0.6f + amp);                   // spin faster when louder
    ptw[i] += 0.12f;
    float wob = sinf(t * 2 + ptw[i]) * (1.0f + amp * 2.0f) * 4.0f;
    float rad = prad[i] * R + wob;
    float x = cx + cosf(pa[i]) * rad;
    float y = cy + sinf(pa[i]) * rad * 0.92f;
    float depth = 0.4f + 0.6f * ((sinf(pa[i]) + 1.0f) * 0.5f);
    int r = (int)((0.9f + pz[i] * 2.2f) * depth) + 1;
    float br = (0.35f + 0.65f * depth) * (0.7f + 0.3f * sinf(ptw[i]));
    cv.fillCircle((int)x, (int)y, r, c565(shade(PAL[pcix[i]], -(1.0f - br) * 0.55f)));
  }
}

// draw a UTF-8 (CJK) string centered at (cx,cy), wrapping by codepoint to fit maxW
static void drawWrappedCJK(const String& s, int cx, int cy, int maxW, int lineH) {
  const int n = s.length();
  String lines[6]; int nl = 0; String cur = "";
  int i = 0;
  while (i < n && nl < 6) {
    unsigned char c = (unsigned char)s[i];
    int len = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
    if (i + len > n) len = n - i;
    String ch = s.substring(i, i + len);
    String cand = cur + ch;
    if (cur.length() > 0 && cv.textWidth(cand.c_str()) > maxW) { lines[nl++] = cur; cur = ch; }
    else cur = cand;
    i += len;
  }
  if (nl < 6 && cur.length() > 0) lines[nl++] = cur;
  else if (nl == 6 && (i < n || cur.length() > 0)) lines[5] += "…";   // mark truncated text
  cv.setTextDatum(middle_center);
  int y = cy - (nl * lineH) / 2 + lineH / 2;
  for (int k = 0; k < nl; k++) { cv.drawString(lines[k].c_str(), cx, y); y += lineH; }
}

static void drawVoiceOverlay() {
  cv.fillSprite(c565(0x060608));                              // dark takeover
  const uint32_t color = PROV[page].color;
  const float t = (millis() - g_voiceT0) / 1000.0f;
  const bool hasText = g_transcript[0] != 0;

  // particle orb: pulses with the real mic level while listening, calm otherwise
  float amp = (g_vphase == 1) ? constrain(g_micLevel, 0.06f, 1.0f) : 0.16f;
  drawVoiceParticles(CX, hasText ? 120 : CY - 8, amp, t);

  // live (partial) -> final transcript, streamed in as you speak
  if (hasText) {
    cv.setFont(&fonts::efontCN_24); cv.setTextSize(1);
    cv.setTextColor(c565(0xFFFFFF));
    drawWrappedCJK(String(g_transcript), CX, 305, 420, 34);
  }

  cv.setTextDatum(middle_center);
  if (g_vphase == 1 || g_vphase == 2) {                       // LISTENING / RECOGNIZING label
    const char* label = (g_vphase == 1) ? "LISTENING" : "RECOGNIZING";
    int dots = ((int)(t * 2)) % 4;
    char title[20]; snprintf(title, sizeof(title), "%s%.*s", label, dots, "...");
    cv.setFont(&fonts::FreeSansBold18pt7b); cv.setTextSize(1);
    cv.setTextColor(c565(shade(color, 0.2f)));
    cv.drawString(title, CX, 410);
  } else {                                                    // RESULT
    cv.setFont(&fonts::FreeSans9pt7b); cv.setTextColor(c565(0x6a6d74));
    cv.drawString("press right to dismiss", CX, 420);
  }
}


// forward decls
static void renderUsagePage(int provIdx);
static void renderListPage(int provIdx);
static void renderDetailPage();

// detail 视图状态:active<0 表示不在详情;否则 detailProv(0/1) + detailIdx
static int detailProv = -1, detailIdx = 0;
static bool g_listView = false;   // true: 当前 provider 的会话列表(从主页滑入)

// 列表页布局(466 屏内)
static const int ROW_H = 47, LIST_TOP = 116, LIST_BOT = 56;
static int g_scroll[2] = { 0, 0 };                       // claude/codex 各自的滚动像素偏移

// 触摸手势(466 屏内阈值)
static const int  TAP_MOVE = 12, SWIPE_MIN = 56; static const uint32_t DBL_MS = 300;
static bool     g_tDown = false; static int g_tx0 = 0, g_ty0 = 0; static int g_tStartScroll = 0;
static char     g_tAxis = 0;                 // 0 未定 / 'x' / 'y'
static int      g_tProv = -1;                // 竖拖作用的列表 provIdx(-1=非列表页)
static uint32_t g_lastTapMs = 0;             // 双击判定
static uint32_t g_pendTapMs = 0; static int g_pendTapRow = -2;   // 待派发的单击(row:-1空白,-2无)

static int listViewH() { return SIZE - LIST_TOP - LIST_BOT; }
static int maxScrollFor(int provIdx) {
  int content = PROV[provIdx].nsess * ROW_H;
  int m = content - listViewH();
  return m > 0 ? m : 0;
}

// ---------- 触摸调试 HUD(临时:确认触摸硬件是否响应)----------
static int g_dbgTouchN = 0;                       // wasPressed 累计次数
static int g_dbgTouchX = -1, g_dbgTouchY = -1;    // 最近触点
static uint32_t g_dbgTouchMs = 0;                 // 最近触摸时刻
static char g_dbgGest[16] = "-";                  // 最近手势(tap/dtap/swipeL/R)

static void drawTouchHud() {
  char b[48];
  snprintf(b, sizeof(b), "C%d N%d %s", (int)M5.Touch.getCount(), g_dbgTouchN, g_dbgGest);
  cv.setFont(&fonts::FreeMono9pt7b); cv.setTextSize(1); cv.setTextDatum(middle_center);
  cv.setTextColor(c565(0x00ff88)); cv.drawString(b, CX, 16);
  if (millis() - g_dbgTouchMs < 900 && g_dbgTouchX >= 0) {   // 触点光圈(跟手)
    cv.drawCircle(g_dbgTouchX, g_dbgTouchY, 16, c565(0x00ff88));
    cv.fillSmoothCircle(g_dbgTouchX, g_dbgTouchY, 5, c565(0x00ff88));
  }
}

// ---------- compose one page ----------
static void render() {
  if (g_voice) { drawVoiceOverlay(); cv.pushSprite(0, 0); return; }
  if (detailProv >= 0)      renderDetailPage();
  else if (g_listView)      renderListPage(page);
  else                      renderUsagePage(page);
  drawTouchHud();                                  // 临时:触摸诊断叠层
  cv.pushSprite(0, 0);
}

static void renderUsagePage(int provIdx) {
  const Prov& p = PROV[provIdx];
  blitProviderArc(provIdx);
  char clk[8]; snprintf(clk, sizeof(clk), "%02d:%02d", g_clkH, g_clkM);
  drawHeader(p, String(clk));
  float t = (millis() - bootMs) / 1000.0f;
  const char* mood = moodForUsage(max(p.sessUsed, p.weekUsed), p.activeCount, g_batt);
  if (provIdx == 0) drawClaude(CX, 150, p.color, mood, t);
  else              drawCodex(CX, 150, p.color, mood, t);
  const char* mdl = (provIdx == 0) ? g_model : g_codexModel;
  if (mdl[0]) {
    cv.setFont(&fonts::FreeSans9pt7b); cv.setTextSize(1);
    cv.setTextDatum(middle_center); cv.setTextColor(c565(0x8a8d94));
    cv.drawString(mdl, CX, 226);
  }
  long nowE = time(nullptr); bool epochOK = nowE > 1700000000L;
  String sR = (epochOK && p.sessReset > nowE) ? fmtDur(p.sessReset - nowE) : String("");
  String wR = (epochOK && p.weekReset > nowE) ? fmtDur(p.weekReset - nowE) : String("");
  drawMeter(250, "usage",  p.sessUsed, sR, p.color);
  drawMeter(286, "weekly", p.weekUsed, wR, p.color);
  drawActivePill(344, p.activeCount, p.color);
  drawWifiStatus(406);
  drawDots(provIdx, p.color);
}
static void renderListPage(int provIdx) {
  const Prov& p = PROV[provIdx];
  uint32_t color = p.color;

  blitProviderArc(provIdx);

  // 头部:小吉祥物 + "CLAUDE · N"
  drawMiniMascot(CX - 52, 60, 22, color, provIdx == 0, ST_THINKING);
  char hd[24]; snprintf(hd, sizeof(hd), "%s · %d", provIdx == 0 ? "CLAUDE" : "CODEX", p.nsess);
  cv.setFont(&fonts::FreeSans9pt7b); cv.setTextSize(1); cv.setTextDatum(middle_left);
  cv.setTextColor(c565(0xcfd2d8)); cv.drawString(hd, CX - 20, 60);

  // 空态
  if (p.nsess == 0) {
    cv.setFont(&fonts::FreeSans9pt7b); cv.setTextColor(c565(0x6f757d)); cv.setTextDatum(middle_center);
    cv.drawString("no sessions", CX, CY);
    drawDots(provIdx, color);
    return;
  }

  // 裁剪窗 + 滚动绘制
  int sc = g_scroll[provIdx];
  if (sc > maxScrollFor(provIdx)) { sc = maxScrollFor(provIdx); g_scroll[provIdx] = sc; }
  cv.setClipRect(40, LIST_TOP, SIZE - 80, listViewH());
  for (int i = 0; i < p.nsess; i++) {
    int y = LIST_TOP - sc + i * ROW_H;
    if (y + ROW_H < LIST_TOP || y > SIZE - LIST_BOT) continue;     // 屏外跳过
    const Sess& s = p.sess[i];
    SessStatus st = (SessStatus)s.status;
    const char* ico = st == ST_EXECUTING ? ">" : st == ST_THINKING ? "*" : st == ST_DONE ? "v" : "=";
    uint16_t tint = c565(st == ST_EXECUTING ? shade(color, 0.25f) : st == ST_THINKING ? 0xffd479 : 0x8b9097);

    char nm[40]; truncCp(s.name, 16, nm, sizeof(nm));
    // 行1
    cv.setFont(&fonts::FreeSansBold12pt7b); cv.setTextDatum(middle_left);
    int x = 48;
    cv.setTextColor(c565(0xe6e8ec));
    char l1[48]; snprintf(l1, sizeof(l1), "%s %s", ico, nm);
    cv.drawString(l1, x, y + 14);
    int wl1 = cv.textWidth(l1);
    cv.setFont(&fonts::FreeSans9pt7b); cv.setTextColor(tint);
    cv.drawString(statusWord(st), x + wl1 + 10, y + 14);
    // 行2
    char md[24]; modelShort(s.model, md, sizeof(md));
    char kt[16]; fmtK(s.tokTotal, kt, sizeof(kt));
    char l2[64]; snprintf(l2, sizeof(l2), "%s · %d%% · %s · t%d", md, s.ctxPct, kt, s.turn);
    cv.setFont(&fonts::FreeMono9pt7b); cv.setTextColor(c565(0x8a8d94));
    cv.drawString(l2, x, y + 34);
    // 分隔线
    cv.drawFastHLine(48, y + ROW_H - 1, SIZE - 96, c565(0x1a1c20));
  }
  cv.clearClipRect();

  // 顶/底渐隐(纯色淡出条,提示可滚动)
  if (sc > 0)                       cv.fillRect(40, LIST_TOP, SIZE - 80, 8, c565(0x000000));
  if (sc < maxScrollFor(provIdx))   cv.fillRect(40, SIZE - LIST_BOT - 8, SIZE - 80, 8, c565(0x000000));

  drawDots(provIdx, color);
}
// 数据小块:顶部小标签 + 中部大数值
static void drawStatTile(int cx, int cy, int w, int h, const char* label, const char* value, uint32_t color) {
  cv.fillRoundRect(cx - w / 2, cy - h / 2, w, h, 7, c565(shade(color, -0.80f)));
  cv.drawRoundRect(cx - w / 2, cy - h / 2, w, h, 7, c565(shade(color, -0.42f)));
  cv.setFont(&fonts::Font0); cv.setTextDatum(middle_center);
  cv.setTextColor(c565(0x8a9097)); cv.drawString(label, cx, cy - h / 2 + 11);
  cv.setFont(&fonts::FreeSansBold12pt7b); cv.setTextColor(c565(COL_WHITE));
  cv.drawString(value, cx, cy + 5);
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

  // 头部:provider 点 + 名称 · PROVIDER
  char nm[40]; truncCp(s.name, 12, nm, sizeof(nm));
  char title[56]; snprintf(title, sizeof(title), "%s · %s", nm, detailProv == 0 ? "CLAUDE" : "CODEX");
  cv.setFont(&fonts::FreeSansBold12pt7b); cv.setTextSize(1); cv.setTextDatum(middle_center);
  int tWid = cv.textWidth(title);
  cv.fillCircle(CX - tWid / 2 - 11, 44, 4, c565(color));
  cv.setTextColor(c565(0xcfd2d8)); cv.drawString(title, CX, 44);

  // 第二行:model · 已运行时长
  long nowE = time(nullptr); bool epochOK = nowE > 1700000000L;
  long elapsed = (epochOK && s.startedAt > 0) ? (nowE - s.startedAt) : 0;
  char md[24]; modelShort(s.model, md, sizeof(md));
  char el[16]; fmtElapsed(elapsed, el, sizeof(el));
  char l2[48]; snprintf(l2, sizeof(l2), "%s · %s", md, el);
  cv.setFont(&fonts::FreeMono9pt7b); cv.setTextDatum(middle_center);
  cv.setTextColor(c565(0x7d828a)); cv.drawString(l2, CX, 68);

  // 小号动画吉祥物 + 状态词
  drawMiniMascot(CX, 122, 42, color, detailProv == 0, s.status);
  cv.setFont(&fonts::FreeSansBold12pt7b); cv.setTextDatum(middle_center);
  uint16_t sw = c565(st == ST_EXECUTING ? shade(color, 0.25f) : st == ST_THINKING ? 0xffd479 : 0x8b9097);
  cv.setTextColor(sw); cv.drawString(statusWord(st), CX, 180);

  // 三宫格:ctx / turn / tokens
  char ctxv[8]; snprintf(ctxv, sizeof(ctxv), "%d%%", s.ctxPct);
  char turnv[8]; snprintf(turnv, sizeof(turnv), "%d", s.turn);
  char tokv[12]; fmtK(s.tokTotal, tokv, sizeof(tokv));
  const int tw = 96, th = 50, gap = 10, ty = 228;
  drawStatTile(CX - tw - gap, ty, tw, th, "CTX",    ctxv,  color);
  drawStatTile(CX,            ty, tw, th, "TURN",   turnv, color);
  drawStatTile(CX + tw + gap, ty, tw, th, "TOKENS", tokv,  color);

  // 任务行 + git/subagents 行(居中,裁剪防溢出圆屏)
  char buf[96];
  cv.setFont(&fonts::FreeMono9pt7b); cv.setTextDatum(middle_center);
  if (s.task[0]) { cv.setTextColor(c565(0xe6e8ec)); snprintf(buf, sizeof(buf), "* %s", s.task); }
  else           { cv.setTextColor(c565(0x6f757d)); snprintf(buf, sizeof(buf), "* idle"); }
  cv.setClipRect(48, 278, SIZE - 96, 20); cv.drawString(buf, CX, 288); cv.clearClipRect();

  int n = snprintf(buf, sizeof(buf), "git %s +%d ~%d", s.branch[0] ? s.branch : "-", s.added, s.modified);
  if (s.subagents > 0) n += snprintf(buf + n, sizeof(buf) - n, " · %dsub", s.subagents);
  if (s.nports > 0)    snprintf(buf + n, sizeof(buf) - n, " · :%d", s.ports[0]);
  cv.setTextColor(c565(0xc3c7cd));
  cv.setClipRect(48, 302, SIZE - 96, 20); cv.drawString(buf, CX, 312); cv.clearClipRect();

  // 底部:位置点 + i/N
  int nd = p.nsess > 9 ? 9 : p.nsess;
  char pos[12]; snprintf(pos, sizeof(pos), "%d/%d", detailIdx + 1, p.nsess);
  cv.setFont(&fonts::FreeSans9pt7b); int posW = cv.textWidth(pos);
  const int dotW = 7, dgap = 6;
  int dotsW = nd * dotW + (nd - 1) * dgap;
  int x0 = CX - (dotsW + 12 + posW) / 2;
  for (int i = 0; i < nd; i++) {
    bool cur = (i == detailIdx) || (detailIdx >= 9 && i == 8);
    cv.fillSmoothCircle(x0 + i * (dotW + dgap) + dotW / 2, 410, cur ? 3 : 2, c565(cur ? color : 0x44474e));
  }
  cv.setTextDatum(middle_left); cv.setTextColor(c565(0x6f757d));
  cv.drawString(pos, x0 + dotsW + 12, 410);
}

// ---------- Arduino entry points ----------
static void showSetupScreen(const char* l1, const char* l2, const char* l3) {
  cv.fillSprite(c565(0x000000));
  cv.setTextDatum(middle_center);
  cv.setFont(&fonts::FreeSansBold12pt7b); cv.setTextColor(c565(COL_CODEX));
  cv.drawString(l1, CX, CY - 40);
  cv.setFont(&fonts::FreeSans9pt7b); cv.setTextColor(c565(0xC8C8C8));
  if (l2 && l2[0]) cv.drawString(l2, CX, CY + 6);
  if (l3 && l3[0]) cv.drawString(l3, CX, CY + 36);
  cv.pushSprite(0, 0);
}

// ---------- WiFi: 多网络自动连接 + 自研配置门户 ----------
static void showConnecting(const char* ssid) {            // 开机逐个尝试历史网络时的中文提示
  cv.fillSprite(c565(0x000000));
  cv.setTextDatum(middle_center);
  cv.setFont(&fonts::efontCN_24); cv.setTextColor(c565(COL_CODEX));
  cv.drawString("正在连接", CX, CY - 34);
  cv.setFont(&fonts::efontCN_24); cv.setTextColor(c565(COL_WHITE));
  cv.drawString(ssid, CX, CY + 6);
  cv.setFont(&fonts::efontCN_16); cv.setTextColor(c565(0x808080));
  cv.drawString("请稍候…", CX, CY + 44);
  cv.pushSprite(0, 0);
}

static void showPortalScreen(const char* ip) {            // 全部连不上 -> 提示用 web 配置新网络
  cv.fillSprite(c565(0x000000));
  cv.setTextDatum(middle_center);
  cv.setFont(&fonts::efontCN_24); cv.setTextColor(c565(COL_CODEX));
  cv.drawString("WiFi 配置", CX, CY - 58);
  cv.setFont(&fonts::efontCN_16); cv.setTextColor(c565(0xC8C8C8));
  cv.drawString("手机连接热点", CX, CY - 20);
  cv.setFont(&fonts::FreeSansBold12pt7b); cv.setTextColor(c565(COL_WHITE));
  cv.drawString("Codey-Setup", CX, CY + 6);
  cv.setFont(&fonts::efontCN_16); cv.setTextColor(c565(0xC8C8C8));
  cv.drawString("浏览器打开", CX, CY + 40);
  cv.setFont(&fonts::FreeSansBold12pt7b); cv.setTextColor(c565(COL_WHITE));
  cv.drawString(ip, CX, CY + 66);
  cv.pushSprite(0, 0);
}

static bool wifiTryConnect(const char* ssid, const char* pass, uint32_t timeoutMs) {
  WiFi.begin(ssid, pass);
  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    if (WiFi.status() == WL_CONNECTED) return true;
    delay(120);
  }
  return false;
}

// 开机自动连:扫描周边 -> 在历史(已按 count 降序)中取可见者,逐个尝试,屏显进度。
static bool wifiAutoConnect() {
  WiFi.mode(WIFI_STA);
  if (g_netCount == 0) {                               // 历史为空(如刚刷机) -> 试 ESP32 底层上次凭证,成功则导入
    WiFi.begin();
    showSetupScreen("WiFi", "connecting...", "");
    uint32_t t0 = millis();
    while (millis() - t0 < 6000 && WiFi.status() != WL_CONNECTED) delay(120);
    if (WiFi.status() == WL_CONNECTED) {
      String s = WiFi.SSID(), p = WiFi.psk();
      if (s.length()) wifiStoreTouch(g_prefs, s.c_str(), p.c_str());   // 迁移进历史(含密码)
      Serial.printf("[wifi] adopted saved creds: %s\n", s.c_str());
      return true;
    }
  }
  showSetupScreen("WiFi", "scanning...", "");
  int n = WiFi.scanNetworks();
  for (int i = 0; i < g_netCount; i++) {
    bool visible = false;
    for (int j = 0; j < n; j++) if (WiFi.SSID(j) == g_nets[i].ssid) { visible = true; break; }
    if (!visible) continue;                                       // 不在附近 -> 跳过(省时)
    Serial.printf("[wifi] try %s (count=%u)\n", g_nets[i].ssid, g_nets[i].count);
    showConnecting(g_nets[i].ssid);
    if (wifiTryConnect(g_nets[i].ssid, g_nets[i].pass, 8000)) {
      wifiStoreTouch(g_prefs, g_nets[i].ssid, g_nets[i].pass);     // count++ & 落盘
      WiFi.scanDelete();
      return true;
    }
  }
  WiFi.scanDelete();
  return false;
}

// ---- 自研配置门户(AP「Codey-Setup」+ captive DNS + WebServer) ----
static WebServer*    g_portalSrv  = nullptr;
static volatile bool g_portalDone = false;

static String urlencode(const String& s) {
  String o; char b[4];
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') o += c;
    else { snprintf(b, sizeof(b), "%%%02X", (unsigned char)c); o += b; }
  }
  return o;
}

static String portalHtml() {
  String h = F("<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Codey WiFi</title><style>"
    "body{font-family:-apple-system,sans-serif;background:#0b0c0e;color:#e8e8ea;margin:0;padding:16px}"
    "h2{color:#22d3a6;font-size:20px;margin:4px 0 12px}h3{color:#9aa;font-size:14px;margin:18px 0 8px}"
    ".row{display:flex;align-items:center;justify-content:space-between;background:#16181c;border-radius:10px;padding:10px 12px;margin:6px 0}"
    ".ss{font-size:15px}.ct{color:#7a7d84;font-size:12px;margin-left:8px}"
    "input,select{width:100%;box-sizing:border-box;background:#16181c;border:1px solid #303236;color:#fff;border-radius:8px;padding:11px;margin:5px 0;font-size:15px}"
    "button{border:0;border-radius:8px;padding:9px 14px;font-weight:600;font-size:14px}"
    ".pri{background:#22d3a6;color:#04110d;width:100%;padding:12px;margin-top:6px}.lk{background:#2d6cf6;color:#fff}.del{background:#ff5d5d;color:#fff}"
    "</style><h2>Codey WiFi 配置</h2>");
  h += F("<h3>已记住的网络（按连接次数）</h3>");
  if (g_netCount == 0) h += F("<div class=ct>暂无历史网络</div>");
  for (int i = 0; i < g_netCount; i++) {
    h += "<div class=row><div><span class=ss>" + String(g_nets[i].ssid) + "</span><span class=ct>×" + String(g_nets[i].count) + "</span></div><div>";
    h += "<form style='display:inline' method=POST action=/connect><input type=hidden name=ssid value=\"" + String(g_nets[i].ssid) + "\"><button class=lk>连接</button></form> ";
    h += "<a href='/del?ssid=" + urlencode(g_nets[i].ssid) + "'><button class=del>删除</button></a></div></div>";
  }
  h += F("<h3>周边网络</h3><select id=scan onchange=\"document.getElementById('ssid').value=this.value\"><option>点「扫描」刷新…</option></select>"
         "<button class=lk style=width:100% onclick=doScan()>扫描周边 WiFi</button>");
  h += "<h3>连接 / 新增网络</h3><form method=POST action=/connect>"
       "<input id=ssid name=ssid placeholder=SSID>"
       "<input name=pass type=password placeholder='密码（连接历史网络可留空）'>"
       "<input name=macip placeholder='Companion Mac IP（留空=自动 mDNS）' value=\"" + String(g_manualMac) + "\">"
       "<button class=pri type=submit>保存并连接</button></form>";
  h += F("<script>function doScan(){let s=document.getElementById('scan');s.innerHTML='<option>扫描中…</option>';"
         "fetch('/scan').then(r=>r.json()).then(d=>{s.innerHTML='';d.sort((a,b)=>b.r-a.r);"
         "d.forEach(n=>{let o=document.createElement('option');o.value=n.s;o.text=n.s+'  ('+n.r+'dBm)';s.add(o)});"
         "if(d.length){document.getElementById('ssid').value=d[0].s}})}</script>");
  return h;
}

static void portalHandleRoot() { g_portalSrv->send(200, "text/html; charset=utf-8", portalHtml()); }

static void portalHandleScan() {
  int n = WiFi.scanNetworks();
  JsonDocument doc; JsonArray a = doc.to<JsonArray>();
  for (int i = 0; i < n && i < 24; i++) { JsonObject o = a.add<JsonObject>(); o["s"] = WiFi.SSID(i); o["r"] = WiFi.RSSI(i); }
  String js; serializeJson(doc, js);
  WiFi.scanDelete();
  g_portalSrv->send(200, "application/json", js);
}

static void portalHandleDel() {
  String ssid = g_portalSrv->arg("ssid");
  if (ssid.length()) wifiStoreRemove(g_prefs, ssid.c_str());
  g_portalSrv->sendHeader("Location", "/");
  g_portalSrv->send(302, "text/plain", "");
}

static void portalHandleConnect() {
  WebServer& s = *g_portalSrv;
  String ssid = s.arg("ssid"), pass = s.arg("pass"), mac = s.arg("macip");
  if (mac.length()) { strncpy(g_manualMac, mac.c_str(), sizeof(g_manualMac) - 1); g_manualMac[sizeof(g_manualMac) - 1] = 0; g_prefs.putString("macip", g_manualMac); }
  if (ssid.length() == 0) { s.send(200, "text/html; charset=utf-8", "<meta charset=utf-8>请填写 SSID。<a href=/>返回</a>"); return; }
  if (pass.length() == 0) { const char* hp = wifiStorePass(ssid.c_str()); if (hp) pass = hp; }   // 一键连:用历史密码
  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t t0 = millis(); bool ok = false;
  while (millis() - t0 < 12000) { if (WiFi.status() == WL_CONNECTED) { ok = true; break; } delay(150); }
  if (ok) {
    wifiStoreTouch(g_prefs, ssid.c_str(), pass.c_str());
    s.send(200, "text/html; charset=utf-8", "<meta charset=utf-8><h2 style='font-family:sans-serif'>已连接 " + ssid + " ✓</h2><p>设备继续启动,可关闭本页。</p>");
    g_portalDone = true;
  } else {
    s.send(200, "text/html; charset=utf-8", "<meta charset=utf-8><h2 style='font-family:sans-serif'>连接 " + ssid + " 失败</h2><p>请检查密码后重试。<a href=/>返回</a></p>");
  }
}

// 阻塞式配置门户:连上 / 3 分钟超时 / 双键 返回。运行时调用前用 g_netPause 让 netTask 放开 WiFi 栈。
static bool wifiConfigPortal() {
  g_netPause = true; delay(120);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("Codey-Setup");
  IPAddress apIP = WiFi.softAPIP();
  DNSServer dns; dns.start(53, "*", apIP);
  WebServer srv(80); g_portalSrv = &srv; g_portalDone = false;
  srv.on("/", portalHandleRoot);
  srv.on("/scan", portalHandleScan);
  srv.on("/connect", HTTP_POST, portalHandleConnect);
  srv.on("/del", portalHandleDel);
  srv.onNotFound([]() { g_portalSrv->sendHeader("Location", "/"); g_portalSrv->send(302, "text/plain", ""); });
  srv.begin();
  showPortalScreen(apIP.toString().c_str());
  uint32_t t0 = millis();
  while (!g_portalDone && millis() - t0 < 180000) {
    dns.processNextRequest();
    srv.handleClient();
    M5.update();
    if (M5.BtnA.isPressed() && M5.BtnB.isPressed()) { delay(400); break; }    // 双键放弃配置
    delay(2);
  }
  srv.stop(); dns.stop();
  bool ok = (WiFi.status() == WL_CONNECTED);
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  g_portalSrv = nullptr;
  g_netPause = false;
  return ok;
}

static bool timeSane(time_t e) { return e > 1700000000L && e < 1900000000L; }   // ~2023..2030, rejects garbage RTC

// clock = Beijing time (UTC+8), 24h. Prefer the system epoch (set from the Companion's ts / NTP;
// localtime applies the +8 offset set by configTime); fall back to the on-board RTC otherwise.
static void readClock() {
  time_t e = time(nullptr);
  if (timeSane(e)) { struct tm ti; localtime_r(&e, &ti); g_clkH = ti.tm_hour; g_clkM = ti.tm_min; }
  else { auto dt = M5.Rtc.getDateTime(); g_clkH = dt.time.hours; g_clkM = dt.time.minutes; }
}
// write the (correct) system time into the on-board RTC so the clock survives going offline
static void syncRtcFromSystem() {
  time_t e = time(nullptr);
  if (!timeSane(e)) return;
  struct tm ti; localtime_r(&e, &ti);
  m5::rtc_datetime_t dt;
  dt.date.year = ti.tm_year + 1900; dt.date.month = ti.tm_mon + 1; dt.date.date = ti.tm_mday; dt.date.weekDay = ti.tm_wday;
  dt.time.hours = ti.tm_hour; dt.time.minutes = ti.tm_min; dt.time.seconds = ti.tm_sec;
  M5.Rtc.setDateTime(dt);
  Serial.printf("RTC synced (Beijing): %04d-%02d-%02d %02d:%02d\n", dt.date.year, dt.date.month, dt.date.date, dt.time.hours, dt.time.minutes);
}
// find the Companion Mac on the LAN by mDNS hostname (robust to DHCP IP changes); else fixed fallback
static void resolveMac() {
  String ip = "";
  if (MDNS.begin("codey-watch")) {                 // 1) mDNS (家用网/不拦多播的网)
    IPAddress a = MDNS.queryHost(MAC_HOSTNAME, 2000);
    if (a != IPAddress((uint32_t)0)) ip = a.toString();
  }
  if (ip.length() == 0 && g_manualMac[0]) ip = String(g_manualMac);   // 2) 手填(公司网 mDNS 被拦)
  if (ip.length() == 0) ip = MAC_FALLBACK_IP;                       // 3) 兜底
  g_macIp = ip;
  g_companionUrl = "http://" + g_macIp + ":8787/codey/state";
  Serial.printf("Companion Mac -> %s\n", g_macIp.c_str());
}
// (re)point the streaming-ASR WebSocket at the current Mac IP (after IP change / WiFi switch)
static void wsConnect() {
  g_ws.disconnect();
  g_ws.begin(g_macIp.c_str(), ASR_PORT, "/");
  g_ws.onEvent(wsEvent);
  g_ws.setReconnectInterval(3000);
}

static void netTask(void*);   // 定义在后面(core0 网络任务);setup 里 xTaskCreate 需先声明

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(0);
  Serial.begin(115200);
  Serial.println("codey_dash booted");
  Serial.printf("[touch] enabled=%d  count=%d\n", M5.Touch.isEnabled(), M5.Touch.getCount());

  g_prefs.begin("codey", false);                  // persisted settings (brightness/volume/macip)
  g_bright = g_prefs.getUChar("bright", 255);
  g_volume = g_prefs.getUChar("vol", 50);
  { String m = g_prefs.getString("macip", ""); strncpy(g_manualMac, m.c_str(), sizeof(g_manualMac) - 1); g_manualMac[sizeof(g_manualMac) - 1] = 0; }
  M5.Display.setBrightness(g_bright);

  randomSeed(micros());
  cv.setColorDepth(16);
  cv.setPsram(true);
  if (!cv.createSprite(SIZE, SIZE)) Serial.println("ERROR: canvas alloc failed");
  g_ringA.setColorDepth(16); g_ringA.setPsram(true); g_ringAok = (g_ringA.createSprite(SIZE, SIZE) != nullptr);
  g_ringB.setColorDepth(16); g_ringB.setPsram(true); g_ringBok = (g_ringB.createSprite(SIZE, SIZE) != nullptr);
  Serial.printf("ring sprites: %d %d\n", g_ringAok, g_ringBok);

  M5.Speaker.end();                       // free the shared codec for the mic
  g_micOK = M5.Mic.begin();
  g_audioBuf = (int16_t*) ps_malloc(MAX_SAMPLES * 2);     // continuous mic-capture buffer (PSRAM)
  Serial.printf("Mic begin=%d  IMU enabled=%d  audioBuf=%p\n", g_micOK, M5.Imu.isEnabled(), g_audioBuf);
  if (!g_audioBuf) Serial.println("ERROR: audio buffer alloc failed (PSRAM) — voice disabled");

  // ---- WiFi: 多网络自动连接(记忆历史) -> 都失败则自研配置门户 ----
  wifiStoreLoad(g_prefs);                          // 历史网络(SSID/密码/连接次数),按 count 降序
  g_wifi = wifiAutoConnect();                      // 扫描周边 + 按连接次数依次尝试记住的网络
  if (!g_wifi) g_wifi = wifiConfigPortal();        // 都连不上 -> 提示用 web 配置新网络
  if (g_wifi) {
    { String s = WiFi.SSID(); strncpy(g_ssid, s.c_str(), sizeof(g_ssid) - 1); g_ssid[sizeof(g_ssid) - 1] = 0; }
    Serial.printf("WiFi connected: %s (%s)\n", WiFi.localIP().toString().c_str(), g_ssid);
    configTime(8 * 3600, 0, "ntp.aliyun.com", "ntp.tencent.com", "pool.ntp.org");  // UTC+8 offset for localtime
    showSetupScreen("WiFi Connected", WiFi.localIP().toString().c_str(), "");
    // 网络(mDNS 解析 / WS 连接 / fetch / 对时)全部交给 netTask,主loop 不在此阻塞
  } else {
    Serial.println("WiFi not connected - running offline");
  }

  int b = M5.Power.getBatteryLevel(); if (b >= 0) g_batt = constrain(b, 0, 100);
  readClock();
  Serial.printf("battery=%d  clock=%02d:%02d\n", g_batt, g_clkH, g_clkM);

  g_voiceSB = xStreamBufferCreate(8192, 1);                                  // 语音 PCM:主loop -> netTask
  xTaskCreatePinnedToCore(netTask, "net", 16384, nullptr, 1, nullptr, 0);    // 所有阻塞网络 IO 在 core0

  bootMs = millis();
  lastActiveMs = millis();
  aBlinkNext = millis() + 1500;
  render();
}

static void copyStr(char* dst, size_t n, const char* src) { if (!src) src = ""; strncpy(dst, src, n - 1); dst[n - 1] = 0; }

static void parseSession(JsonObject so, Sess& s) {
  copyStr(s.id,     sizeof(s.id),     so["id"]            | "");
  copyStr(s.name,   sizeof(s.name),   so["name"]          | "");
  copyStr(s.model,  sizeof(s.model),  so["model"]         | "");
  copyStr(s.branch, sizeof(s.branch), so["git"]["branch"] | "");
  copyStr(s.task,   sizeof(s.task),   so["current_task"]  | "");
  copyStr(s.effort, sizeof(s.effort), so["effort"]        | "");
  s.status   = statusFromStr(so["status"] | "waiting");
  s.ctxPct   = so["context_pct"]    | 0;
  s.ctxTok   = so["context_tokens"] | 0L;
  s.ctxWin   = so["context_window"] | 200000L;
  s.tokTotal = so["tokens_total"]   | 0L;
  s.turn     = so["turn"]           | 0;
  s.added    = so["git"]["added"]    | 0;
  s.modified = so["git"]["modified"] | 0;
  s.subagents= so["subagents"]      | 0;
  s.startedAt= so["started_at"]     | 0L;
  s.nports = 0;
  for (JsonVariant pv : so["ports"].as<JsonArray>()) { if (s.nports < MAX_PORTS) s.ports[s.nports++] = pv.as<int>(); }
}

// fetch normalized usage JSON from the Companion and update PROV with real Claude data
static void fetchState() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.setConnectTimeout(2500);
  http.setTimeout(3500);
  if (!http.begin(g_companionUrl)) return;
  bool ok = false;
  int code = http.GET();
  if (code == 200) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getString());
    if (!err) {
      g_stale = doc["stale"] | false;
      for (JsonObject pr : doc["providers"].as<JsonArray>()) {
        const char* id = pr["id"] | "";
        int i = (strcmp(id, "claude") == 0) ? 0 : (strcmp(id, "codex") == 0 ? 1 : -1);
        if (i < 0) continue;
        PROV[i].sessUsed  = pr["session"]["used_pct"] | 0;
        PROV[i].weekUsed  = pr["weekly"]["used_pct"]  | 0;
        PROV[i].sessReset = pr["session"]["reset_epoch"] | 0L;
        PROV[i].weekReset = pr["weekly"]["reset_epoch"]  | 0L;
        if (i == 0 || i == 1) { const char* m = pr["model"] | ""; char* dst = (i == 0) ? g_model : g_codexModel; if (m[0]) { strncpy(dst, m, sizeof(g_model) - 1); dst[sizeof(g_model) - 1] = 0; } }
        PROV[i].activeCount = pr["active_count"]         | 0;
        PROV[i].dirtyRepos  = pr["agg"]["dirty_repos"]   | 0;
        PROV[i].tokPerMin   = pr["agg"]["tokens_per_min"]| 0L;
        int n = 0;
        for (JsonObject so : pr["sessions"].as<JsonArray>()) {
          if (n >= MAX_SESS) break;
          parseSession(so, PROV[i].sess[n]); n++;
        }
        PROV[i].nsess = n;
      }
      g_haveData = true; ok = true;
      if (detailProv >= 0 && detailIdx >= PROV[detailProv].nsess) detailProv = -1;
      long ts = doc["ts"] | 0L;                      // Mac epoch -> set the device clock (NTP-independent)
      if (ts > 1700000000L && ts < 1900000000L) {
        struct timeval tv; tv.tv_sec = (time_t)ts; tv.tv_usec = 0; settimeofday(&tv, nullptr);
        readClock();
      }
      Serial.printf("[fetch] ok  claude %d/%d  codex %d/%d  stale=%d\n",
                    PROV[0].sessUsed, PROV[0].weekUsed, PROV[1].sessUsed, PROV[1].weekUsed, g_stale);
    } else {
      Serial.printf("[fetch] json err: %s\n", err.c_str());
    }
  } else {
    Serial.printf("[fetch] HTTP %d\n", code);
  }
  http.end();
  if (ok) { g_fetchFails = 0; g_companionOk = true; }
  else if (++g_fetchFails >= 2) {                  // usage 接口不可达:自愈重连 + 把显示重置为 0
    g_fetchFails = 0; g_companionOk = false; g_haveData = false;
    for (int i = 0; i < 2; i++) { PROV[i].sessUsed = 0; PROV[i].weekUsed = 0; PROV[i].nsess = 0; PROV[i].activeCount = 0; PROV[i].dirtyRepos = 0; PROV[i].tokPerMin = 0; }
    detailProv = -1;
    g_model[0] = g_codexModel[0] = 0;
    resolveMac(); wsConnect();                     // IP drift self-heal (netTask 上下文)
  }
}

// 所有阻塞网络 IO 都在这里(core 0):WS 维护/连接、HTTP fetch、mDNS、语音上行。主loop 永不等待。
static void netTask(void*) {
  bool started = false;
  uint32_t lastFetch = 0;
  for (;;) {
    if (g_netPause) { started = false; vTaskDelay(20 / portTICK_PERIOD_MS); continue; }  // 门户接管 WiFi:让路(回来后重连)
    if (g_wifi) {
      if (!started || g_netReconnect) { g_netReconnect = false; resolveMac(); wsConnect(); started = true; lastFetch = 0; }
      g_ws.loop();                                  // WS 维护(connect 阻塞只在本任务,不卡渲染)
      if (g_netListenReq == 1)      { g_netListenReq = 0; wsListen(true); }
      else if (g_netListenReq == 2) { g_netListenReq = 0; wsListen(false); }
      uint8_t buf[1024]; size_t n;                  // 转发主loop采集的语音 PCM
      while (g_wsConn && (n = xStreamBufferReceive(g_voiceSB, buf, sizeof(buf), 0)) > 0) g_ws.sendBIN(buf, n);
      uint32_t now = millis();                      // 定时拉 usage(语音时让路)
      if (!g_voice && (lastFetch == 0 || now - lastFetch > 30000)) { lastFetch = now; fetchState(); }
    } else { started = false; }
    vTaskDelay(5 / portTICK_PERIOD_MS);
  }
}

static void reconfigWiFi() {                     // 设置页 -> 打开自研 WiFi 门户(历史/一键连/删除/扫描/手填)
  g_voice = false;
  bool ok = wifiConfigPortal();                  // 阻塞门户;内部已 g_netPause 让 netTask 放开 WiFi 栈
  g_wifi = (WiFi.status() == WL_CONNECTED);
  if (ok && g_wifi) {
    { String s = WiFi.SSID(); strncpy(g_ssid, s.c_str(), sizeof(g_ssid) - 1); g_ssid[sizeof(g_ssid) - 1] = 0; }
    showSetupScreen("WiFi Connected", WiFi.localIP().toString().c_str(), ""); delay(800);
    g_netReconnect = true;                       // netTask 重新解析 Mac + 重连 WS + fetch(不在主loop阻塞)
  }
  bootMs = millis();                             // reset the animation clock
}

// how many mic samples have been captured since listening started (clamped to the buffer)
static size_t capturedSamples() {
  if (!g_micOK) return 0;
  long s = (long)((millis() - g_voiceT0) / 1000.0f * REC_RATE);
  return (size_t)constrain(s, 0L, (long)MAX_SAMPLES);
}

// ---------- settings page ----------
static void savePrefs() { g_prefs.putUChar("bright", g_bright); g_prefs.putUChar("vol", g_volume); }
static uint8_t nextBright(uint8_t b) {
  const uint8_t L[4] = { 64, 128, 191, 255 };                  // ~25/50/75/100%
  for (int i = 0; i < 4; i++) if (b <= L[i]) return L[(i + 1) % 4];
  return L[0];
}
static void renderSettings() {
  cv.fillSprite(c565(0x0a0b0d));
  cv.setTextDatum(middle_center);
  cv.setFont(&fonts::efontCN_24); cv.setTextColor(c565(COL_CODEX));
  cv.drawString("设置", CX, 56);
  const int y0 = 132, dy = 66;
  for (int i = 0; i < SET_N; i++) {
    int y = y0 + i * dy; bool sel = (i == g_setSel);
    if (sel) cv.fillRoundRect(54, y - 27, SIZE - 108, 54, 12, c565(shade(COL_CLAUDE, -0.55f)));
    cv.drawRoundRect(54, y - 27, SIZE - 108, 54, 12, c565(sel ? COL_CLAUDE : 0x303236));
    cv.setFont(&fonts::efontCN_24); cv.setTextDatum(middle_left);
    cv.setTextColor(c565(sel ? COL_WHITE : 0xB0B2B8));
    cv.drawString(SET_ITEMS[i], 84, y);
    char val[16] = "";
    if (i == 1)      snprintf(val, sizeof(val), "%d%%", (g_bright * 100 + 127) / 255);
    else if (i == 2) snprintf(val, sizeof(val), "%d%%", g_volume);
    else if (i == 0) strcpy(val, "Web");
    if (val[0]) {
      cv.setFont(&fonts::FreeSansBold12pt7b); cv.setTextDatum(middle_right);
      cv.setTextColor(c565(sel ? COL_WHITE : 0x808389)); cv.drawString(val, SIZE - 84, y);
    }
  }
  cv.setFont(&fonts::efontCN_24); cv.setTextDatum(middle_center); cv.setTextColor(c565(0x70737a));
  cv.drawString("键1下移  键2确定  双键返回", CX, 422);
  cv.pushSprite(0, 0);
}
// settings buttons: BtnA(1) = move down, BtnB(2) = confirm/activate
static void settingsButtons() {
  if (M5.BtnA.wasPressed()) g_setSel = (g_setSel + 1) % SET_N;
  if (M5.BtnB.wasPressed()) {
    switch (g_setSel) {
      case 0: reconfigWiFi(); break;                                            // web WiFi portal (blocking)
      case 1: g_bright = nextBright(g_bright); M5.Display.setBrightness(g_bright); savePrefs(); break;
      case 2: g_volume = (g_volume >= 100) ? 0 : (uint8_t)min(100, g_volume + 25); savePrefs(); break;
      case 3: g_inSettings = false; bootMs = millis(); break;                    // back to dashboard
    }
  }
}

// ---- 触摸手势辅助函数 ----
static int curListProv() { return (detailProv < 0 && g_listView) ? page : -1; }

// 列表页:由屏幕 y 命中会话行号;-1 = 空白
static int rowHitAt(int provIdx, int ty) {
  if (ty < LIST_TOP || ty > SIZE - LIST_BOT) return -1;
  int idx = (g_scroll[provIdx] + (ty - LIST_TOP)) / ROW_H;
  return (idx >= 0 && idx < PROV[provIdx].nsess) ? idx : -1;
}

static void enterDetail(int prov, int idx) { detailProv = prov; detailIdx = idx; }
static void exitDetail() { detailProv = -1; }

// 横滑:详情切会话,否则切 provider 主页。dir:+1 下一 / -1 上一
static void swipePage(int dir) {
  if (detailProv >= 0) {
    int n = PROV[detailProv].nsess; if (n > 0) detailIdx = (detailIdx + dir + n) % n;
  } else {
    page = (page + dir + 2) % 2;
  }
}

// 单击派发:列表点行=该会话/空白=置顶;主页=进列表;详情不响应单击
static void doTap(int row) {
  if (detailProv >= 0) return;
  if (g_listView) { if (PROV[page].nsess) enterDetail(page, row >= 0 ? row : 0); }
  else g_listView = true;           // 主页单击 → 进该端会话列表
}

// 长按 BtnA:详情→列表;列表→主页;主页→列表
static void btnALong() {
  if (detailProv >= 0) { exitDetail(); return; }      // 详情 → 列表
  if (g_listView) { g_listView = false; return; }     // 列表 → 主页
  g_listView = true;                                  // 主页 → 列表
}
// 短按 BtnA:详情翻下一会话;否则切 provider
static void btnAShort() {
  if (detailProv >= 0) { int n = PROV[detailProv].nsess; if (n > 0) detailIdx = (detailIdx + 1) % n; }
  else page = (page + 1) % 2;
}

void loop() {
  M5.update();
  uint32_t now = millis();
  // 网络全部在 netTask(core0);主loop 不调 g_ws.loop / fetchState,绝不等待网络

  // ---- 触摸手势(仅非设置/非语音态)----
  if (!g_inSettings && !g_voice) {
    auto td = M5.Touch.getDetail();
    if (td.isPressed()) { g_dbgTouchX = td.x; g_dbgTouchY = td.y; g_dbgTouchMs = now; }   // 原始触点(独立于手势逻辑)
    if (td.wasPressed()) {
      g_tDown = true; g_tx0 = td.x; g_ty0 = td.y; g_tAxis = 0;
      g_tProv = curListProv(); g_tStartScroll = (g_tProv >= 0) ? g_scroll[g_tProv] : 0;
      g_dbgTouchN++; snprintf(g_dbgGest, sizeof(g_dbgGest), "down");
      Serial.printf("[touch] press %d,%d\n", td.x, td.y);
    } else if (g_tDown && td.isPressed()) {
      int dx = td.x - g_tx0, dy = td.y - g_ty0;
      if (!g_tAxis && (abs(dx) > TAP_MOVE || abs(dy) > TAP_MOVE)) g_tAxis = (abs(dx) > abs(dy)) ? 'x' : 'y';
      if (g_tAxis == 'y' && g_tProv >= 0) {                     // 竖拖滚动列表
        int ns = g_tStartScroll - dy;
        int mx = maxScrollFor(g_tProv); ns = ns < 0 ? 0 : (ns > mx ? mx : ns);
        g_scroll[g_tProv] = ns;
      }
      lastActiveMs = now;
    } else if (g_tDown && td.wasReleased()) {
      int dx = td.x - g_tx0, dy = td.y - g_ty0;
      if (g_tAxis == 'x' && abs(dx) >= SWIPE_MIN) { snprintf(g_dbgGest, sizeof(g_dbgGest), "swipe%s", dx < 0 ? "L" : "R"); swipePage(dx < 0 ? 1 : -1); }   // 横滑
      else if (g_tAxis == 'y' && g_tProv < 0 && detailProv < 0 && !g_listView && dy < 0 && abs(dy) >= SWIPE_MIN) {
        snprintf(g_dbgGest, sizeof(g_dbgGest), "swipeU"); g_listView = true;      // 上划主页 → 列表
      } else if (g_tAxis == 0 && abs(dx) < TAP_MOVE && abs(dy) < TAP_MOVE) {    // 点击 -> 单/双击判定
        if (now - g_lastTapMs < DBL_MS) { g_lastTapMs = 0; g_pendTapRow = -2;   // 双击 -> 退出详情/关列表
          snprintf(g_dbgGest, sizeof(g_dbgGest), "dtap");
          if (detailProv >= 0) exitDetail(); else if (g_listView) g_listView = false; }
        else { g_lastTapMs = now;                                                // 记一次单击,延迟派发
          g_pendTapMs = now; g_pendTapRow = (g_tProv >= 0) ? rowHitAt(g_tProv, g_ty0) : -1; }
      }
      g_tDown = false; g_tAxis = 0; g_tProv = -1; lastActiveMs = now;
    }
    // 单击延迟派发(等过双击窗口确认不是双击)
    if (g_pendTapRow != -2 && now - g_pendTapMs >= DBL_MS) { snprintf(g_dbgGest, sizeof(g_dbgGest), "tap%d", g_pendTapRow); doTap(g_pendTapRow); g_pendTapRow = -2; }
  } else {                                   // 进入设置/语音态:清手势残留(防退出后用旧坐标误滚动)
    g_tDown = false; g_tAxis = 0; g_tProv = -1; g_pendTapRow = -2;
  }

  static uint32_t bothSince = 0; static bool bothFired = false;   // hold BOTH ~0.4s -> toggle settings
  if (M5.BtnA.isPressed() && M5.BtnB.isPressed()) {
    if (bothSince == 0) bothSince = now;
    if (!bothFired && now - bothSince > 400) {
      bothFired = true; g_setSel = 0; g_inSettings = !g_inSettings;
      if (g_inSettings) { g_voice = false; g_vphase = 0; } else bootMs = millis();
    }
  } else {
    bothSince = 0; bothFired = false;
    if (g_inSettings) {
      settingsButtons();                                   // BtnA = down, BtnB = confirm
    } else if (M5.BtnB.wasPressed() && !M5.BtnA.isPressed()) {   // right button -> voice command
      if (!g_voice) {                                      // start streaming
        g_voice = true; g_voiceT0 = now; g_micLevel = 0.12f;
        g_transcript[0] = 0; g_heardSpeech = false; g_silenceT0 = 0; g_noiseFloor = 0.06f;
        g_sentSamples = 0; g_recEnd = 0; g_finalReqT0 = 0; g_sttFinal = false;
        if (g_micOK && g_audioBuf && g_wsConn) {
          g_vphase = 1;
          if (g_voiceSB) xStreamBufferReset(g_voiceSB);
          g_netListenReq = 1;                              // netTask -> listen:start
          M5.Mic.record(g_audioBuf, MAX_SAMPLES, REC_RATE);
          Serial.println("[voice] streaming");
        } else {                                           // can't stream -> show why
          g_vphase = 3; g_resultT0 = now;
          strncpy(g_transcript, !g_micOK ? "麦克风不可用" : !g_wsConn ? "语音服务未连接" : "缓冲不可用", sizeof(g_transcript) - 1);
          g_transcript[sizeof(g_transcript) - 1] = 0;
        }
      } else if (g_vphase == 1) {
        g_recEnd = capturedSamples(); g_vphase = 2;        // press again -> stop & finalize
      } else if (g_vphase == 3) {
        g_voice = false; g_vphase = 0;                     // dismiss the result
      }
    } else if (!g_voice && !M5.BtnB.isPressed()) {              // 左键:短按切页/翻会话,长按进/出详情
      static uint32_t aDownAt = 0; static bool aLong = false;
      if (!M5.BtnA.isPressed() && !M5.BtnA.wasReleased()) { aDownAt = 0; aLong = false; }  // 空闲重置:防按键被 BtnB 打断后残留长按态
      if (M5.BtnA.wasPressed()) { aDownAt = now; aLong = false; }
      if (M5.BtnA.isPressed() && !aLong && aDownAt && now - aDownAt > 550) { aLong = true; btnALong(); }
      if (M5.BtnA.wasReleased()) { if (!aLong) btnAShort(); aDownAt = 0; }
    }
  }

  if (g_inSettings) { renderSettings(); delay(16); return; }   // settings page owns the screen

  // ---- streaming voice: send mic PCM chunks over WebSocket; server partials arrive in wsEvent ----
  if (g_voice) {
    if (g_vphase == 1) {                                  // LISTENING + streaming
      size_t cap = capturedSamples();
      while (g_sentSamples + STREAM_CHUNK <= cap) {       // stream each newly-captured ~32ms chunk
        int16_t* chunk = g_audioBuf + g_sentSamples;
        double s = 0; for (size_t i = 0; i < STREAM_CHUNK; i++) { double v = chunk[i]; s += v * v; }
        float lvl = sqrtf(s / STREAM_CHUNK) / 2500.0f;    // VAD level + adaptive floor (hysteresis)
        g_micLevel += (lvl - g_micLevel) * 0.4f;
        g_noiseFloor += (g_micLevel - g_noiseFloor) * (g_micLevel < g_noiseFloor ? 0.2f : 0.01f);
        float on = g_noiseFloor + 0.12f, off = g_noiseFloor + 0.06f;
        if (g_micLevel > on) { g_heardSpeech = true; g_silenceT0 = 0; }
        else if (g_micLevel < off) { if (g_heardSpeech && g_silenceT0 == 0) g_silenceT0 = now; }
        if (g_voiceSB) xStreamBufferSend(g_voiceSB, (uint8_t*)chunk, STREAM_CHUNK * 2, 0);  // -> netTask
        g_sentSamples += STREAM_CHUNK;
      }
      bool maxed   = (cap >= MAX_SAMPLES) || (g_micOK && !M5.Mic.isRecording());
      bool silence = g_heardSpeech && g_silenceT0 && (now - g_silenceT0 > 1300) && (now - g_voiceT0 > 1000);
      if (maxed || silence) { g_recEnd = capturedSamples(); g_vphase = 2; g_finalReqT0 = 0; }
    } else if (g_vphase == 2) {                           // FINALIZE: flush tail, await the final stt
      while (g_sentSamples < g_recEnd) {                  // flush remaining audio (not real-time bound now)
        size_t n = g_recEnd - g_sentSamples; if (n > STREAM_CHUNK) n = STREAM_CHUNK;
        if (g_voiceSB) xStreamBufferSend(g_voiceSB, (uint8_t*)(g_audioBuf + g_sentSamples), n * 2, 0);
        g_sentSamples += n;
      }
      if (g_finalReqT0 == 0) { g_netListenReq = 2; g_finalReqT0 = now; }  // netTask -> listen:stop (finalize)
      if (g_sttFinal || now - g_finalReqT0 > 4000) {                    // got the final result (or timeout)
        if (g_transcript[0] == 0) { strncpy(g_transcript, "(没听清)", sizeof(g_transcript) - 1); g_transcript[sizeof(g_transcript) - 1] = 0; }
        g_vphase = 3; g_resultT0 = now;
      }
    } else if (g_vphase == 3) {                           // RESULT
      if (now - g_resultT0 > 9000) { g_voice = false; g_vphase = 0; }
    }
  }

  // ---- IMU: shake to switch page + raise/move to wake the screen ----
  M5.Imu.update();
  float ax, ay, az; M5.Imu.getAccel(&ax, &ay, &az);
  float mag = sqrtf(ax * ax + ay * ay + az * az);
  float motion = fabsf(mag - g_accMag); g_accMag = mag;
  bool active = M5.BtnA.isPressed() || M5.BtnB.isPressed() || motion > 0.10f;
  if (!g_dim && !g_voice && mag > 1.9f && now - g_lastShake > 900) {     // shake -> next provider (main only)
    if (detailProv < 0 && !g_listView) page = (page + 1) % 2;   // 详情/列表态下摇晃不切 provider
    g_lastShake = now; active = true;
    Serial.printf("[shake] page=%d\n", page);
  }
  if (active) lastActiveMs = now;
  bool wantDim = (now - lastActiveMs > 20000);                           // dim after 20s idle
  if (wantDim != g_dim) { g_dim = wantDim; M5.Display.setBrightness(g_dim ? 12 : g_bright); }

  if (now - lastSecMs >= 1000) {          // refresh real time + battery + WiFi status once a second
    lastSecMs = now;
    int b = M5.Power.getBatteryLevel(); if (b >= 0) g_batt = constrain(b, 0, 100);
    readClock();
    g_wifi = (WiFi.status() == WL_CONNECTED);
    if (g_wifi) { String s = WiFi.SSID(); strncpy(g_ssid, s.c_str(), sizeof(g_ssid) - 1); g_ssid[sizeof(g_ssid) - 1] = 0; }
    if (timeSane(time(nullptr)) && !g_rtcSynced) { syncRtcFromSystem(); g_rtcSynced = true; }  // netTask set clock -> RTC once
  }
  // 不在主loop poll usage:fetchState 已搬进 netTask(异步,不阻塞渲染)
  updateAnim(now);
  render();
  delay(g_voice ? 2 : 16);   // voice screen runs faster for a smoother orb
}
