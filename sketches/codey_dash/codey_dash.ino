// codey_dash — Claude Code / Codex usage-monitor dashboard for the M5Stack StopWatch.
//
// A faithful port of the "Codey 圆屏仪表盘" static design (milestone M1) + subtle
// character idle-animation. Two pages (Claude orange / Codex green), switch with BtnA.
// Per page: edge usage arc with ticks, header (dot+name · clock · battery), an animated
// mascot, USAGE/WEEKLY 14-segment meters with live reset countdowns, a "N TO REVIEW"
// pill, and page dots. Clock = on-board RTC, battery = real level. True-black background.
//
// Rendered to a full-screen PSRAM canvas for flicker-free animation.

#include <M5Unified.h>
#include <WiFiManager.h>   // web config portal: AP "Codey-Setup" -> pick WiFi in a browser
#include <HTTPClient.h>     // fetch usage JSON from the Companion
#include <ArduinoJson.h>    // parse it
#include <WebSocketsClient.h>  // stream mic PCM to the sherpa-onnx ASR server, receive live text
#include <ESPmDNS.h>        // resolve the Mac by hostname (survives DHCP IP changes)
#include <Preferences.h>    // persist brightness/volume settings in NVS
#include <sys/time.h>       // settimeofday() — set the clock from the Companion's epoch

// ---------- palette (RGB888) ----------
static const uint32_t COL_CLAUDE = 0xF4894F;
static const uint32_t COL_CODEX  = 0x22D3A6;
static const uint32_t COL_DANGER = 0xFF5D5D;
static const uint32_t COL_WHITE  = 0xFFFFFF;

// ---------- mock provider data ----------
struct Provider {
  const char* name;
  uint32_t    color;
  int         sessionUsed, weeklyUsed, pending;
  long        sessionSeed, weeklySeed;   // countdown seeds (seconds)
};
static Provider PROV[2] = {
  { "Claude", COL_CLAUDE, 10, 10, 3, 216L * 60, 24L * 3600 },
  { "Codex",  COL_CODEX,  24, 18, 0,  41L * 60, 48L * 3600 },
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
static int      page = 0;
static uint32_t bootMs = 0;
static uint32_t lastSecMs = 0;
static bool     g_voice = false;     // voice overlay active
static uint32_t g_voiceT0 = 0;       // millis() when the voice overlay started
static bool     g_wifi = false;      // WiFi connected
static String   g_ssid = "";         // connected SSID (shown at the bottom, marquee if long)
static bool     g_micOK = false;     // microphone available
static int16_t  g_micBuf[256];       // mic capture buffer
static float    g_micLevel = 0.12f;  // smoothed mic level (0..1)
// ---- streaming voice: continuous mic capture -> WebSocket PCM -> sherpa-onnx live partials ----
static int      g_vphase = 0;        // 0 off, 1 listening/streaming, 2 finalizing, 3 result
static String   g_transcript = "";   // live partial, then final transcript (set by wsEvent)
static const int    REC_RATE = 16000;
static const size_t MAX_SAMPLES = (size_t)(REC_RATE * 15);     // 15s max listen window
static const size_t STREAM_CHUNK = 512;                        // samples per WS frame (~32ms)
static int16_t* g_audioBuf = nullptr;// continuous mic-capture buffer (PSRAM, no WAV header)
static size_t   g_sentSamples = 0;   // streaming position (samples already sent)
static size_t   g_recEnd = 0;        // capture length at stop (flush up to here)
static bool     g_heardSpeech = false;
static uint32_t g_silenceT0 = 0;     // start of trailing silence (after speech) -> auto-stop
static uint32_t g_resultT0 = 0;      // when the result was shown (dismiss timeout)
static uint32_t g_finalReqT0 = 0;    // when listen-stop was sent (final-result timeout)
static volatile bool g_sttFinal = false;     // server sent a final stt for this utterance
static float    g_noiseFloor = 0.06f;// adaptive VAD noise floor
// WebSocket to the streaming-ASR server
static WebSocketsClient g_ws;
static volatile bool g_wsConn = false;
static const char*    MAC_HOSTNAME    = "testnull-2";      // Mac's mDNS name (resolves to its LAN IP)
static const char*    MAC_FALLBACK_IP = "192.168.1.29";    // used if mDNS fails
static String         g_macIp = "192.168.1.29";            // resolved Mac IP (mDNS, else fallback)
static const uint16_t ASR_PORT = 8788;
static String g_model = "";          // Claude model name (from statusline) shown under the avatar
static String g_lunar = "";          // 农历 e.g. 四月十五 (from Companion)
static String g_zodiac = "";         // 生肖 e.g. 马 (from Companion)
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
static String g_companionUrl = "http://192.168.1.29:8787/codey/state";   // rebuilt from g_macIp at boot
static bool     g_haveData = false;
static bool     g_stale = false;
static long     g_sReset[2] = {0, 0}, g_wReset[2] = {0, 0};   // real reset epochs (0 = use mock)
static uint32_t g_lastFetch = 0;

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

// ---------- time helpers ----------
static String fmtDur(long secs) {
  if (secs < 0) secs = 0;
  long d = secs / 86400, h = (secs % 86400) / 3600, m = (secs % 3600) / 60, s = secs % 60;
  char b[16];
  if (d > 0)      snprintf(b, sizeof(b), "%ldd", d);          // compact: 1d
  else if (h > 0) snprintf(b, sizeof(b), "%ldh%02ldm", h, m); // 3h36m
  else if (m > 0) snprintf(b, sizeof(b), "%ldm", m);          // 40m
  else            snprintf(b, sizeof(b), "%lds", s);          // 30s
  return String(b);
}
static long remain(long seed, long elapsed) { long r = seed - (elapsed % seed); return r == seed ? seed : r; }

static const char* moodFor(int used, int pending, int battery) {
  if (battery <= 12) return "sleepy";
  if (pending > 0)   return "alert";
  if (used >= 88)    return "worried";
  if (used >= 65)    return "tired";
  if (used >= 40)    return "focused";
  return "happy";
}

// ---------- the edge usage arc — a clean ring drawn with overlapping circles ----------
// Custom anti-aliased arc: per-pixel sub-pixel coverage on the band's radial edges (M5GFX has
// no native smooth arc). Convention-free — the angle is computed from the pixel itself
// (design space: 0=top, clockwise), so the gap is always centered at the bottom. Background is
// black here, so coverage blends by scaling the colour toward black.
static void drawArc(uint32_t color, int pct) {
  const float rIn = 209.0f, rOut = 223.0f;
  const float startDeg = -138.0f, sweepDeg = 276.0f;       // gap (84°) centered at the bottom
  const float p = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
  const float fillDeg = sweepDeg * p / 100.0f;
  const float loR = (rIn - 0.7f) * (rIn - 0.7f), hiR = (rOut + 0.7f) * (rOut + 0.7f);
  const int Rb = (int)rOut + 1;
  for (int dy = -Rb; dy <= Rb; dy++) {
    int py = CY + dy; if ((unsigned)py >= (unsigned)SIZE) continue;
    float fy = (float)dy, fyy = fy * fy;
    for (int dx = -Rb; dx <= Rb; dx++) {
      float r2 = (float)dx * dx + fyy;
      if (r2 > hiR || r2 < loR) continue;                  // outside the band (+~1px) -> skip (cheap)
      int px = CX + dx; if ((unsigned)px >= (unsigned)SIZE) continue;
      float rr = sqrtf(r2);
      float cov = fminf(rr - (rIn - 0.5f), (rOut + 0.5f) - rr);   // radial sub-pixel coverage (AA edges)
      if (cov <= 0.0f) continue; if (cov > 1.0f) cov = 1.0f;
      float d = atan2f((float)dx, -fy) * 57.2957795f;            // design angle (0=top, clockwise)
      float dn = d - startDeg; if (dn < 0) dn += 360.0f;
      if (dn > sweepDeg) continue;                               // inside the bottom gap
      uint32_t rgb = (dn <= fillDeg) ? color : 0x23262c;         // progress vs track
      cv.drawPixel(px, py, c565(shade(rgb, -(1.0f - cov))));     // blend toward black = anti-alias
    }
  }
  if (pct > 0) {                                                 // glowing AA cap dot at the progress tip
    float a = (startDeg + fillDeg - 90.0f) * DEG_TO_RAD;
    int hx = CX + 216 * cosf(a), hy = CY + 216 * sinf(a);
    cv.fillSmoothCircle(hx, hy, 9, c565(COL_WHITE));
    cv.fillSmoothCircle(hx, hy, 6, c565(color));
  }
}

// ---------- header (dot + name · clock · battery) ----------
static void drawHeader(const Provider& p, const String& clock) {
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

// ---------- a USAGE/WEEKLY meter row ----------
static void drawMeter(int y, const char* label, int used, const String& reset, uint32_t color) {
  const int segs = 10;                 // 10 segments (per-segment size unchanged)
  const int labelX = 56;               // lowercase label, moved left
  const float pitch = 12.0f;           // per-segment pitch (drawn width stays 9px)
  const int barX = 120;
  const int pctRightX = 306;           // percentage right-aligned here
  const int timeRightX = 378;          // remaining time at the far right, after the %
  int filled = constrain((int)roundf(used / 100.0f * segs), 0, segs);
  bool hot = used >= 85;
  uint16_t segc = c565(hot ? COL_DANGER : color);
  uint16_t empty = c565(0x1b1c20);

  cv.setFont(&fonts::FreeSans9pt7b); cv.setTextSize(1);
  cv.setTextColor(c565(0x9a9ca2)); cv.setTextDatum(middle_left);
  cv.drawString(label, labelX, y);

  for (int i = 0; i < segs; i++)
    cv.fillRoundRect(barX + (int)(i * pitch), y - 5, (int)pitch - 3, 10, 2, i < filled ? segc : empty);

  char pc[8]; snprintf(pc, sizeof(pc), "%d%%", used);
  cv.setFont(&fonts::FreeMonoBold12pt7b);
  cv.setTextColor(c565(COL_WHITE)); cv.setTextDatum(middle_right);
  cv.drawString(pc, pctRightX, y);

  cv.setFont(&fonts::FreeMono9pt7b);
  cv.setTextColor(c565(0x6d6f75)); cv.setTextDatum(middle_right);
  cv.drawString(reset.c_str(), timeRightX, y);
}

// ---------- "N TO REVIEW" pill ----------
static void drawPill(int cy, int pending, uint32_t color) {
  bool on = pending > 0;
  uint16_t cc = c565(color);
  char num[4]; snprintf(num, sizeof(num), "%d", pending);
  cv.setFont(&fonts::FreeMonoBold9pt7b);  int nW = cv.textWidth(num);
  cv.setFont(&fonts::FreeSans9pt7b);      int tW = cv.textWidth("TO REVIEW");
  int contentW = 7 + 7 + nW + 8 + tW;
  int w = contentW + 26, x = CX - w / 2, h = 30, y = cy - h / 2;
  cv.fillRoundRect(x, y, w, h, 15, on ? c565(shade(color, -0.7)) : c565(0x0e0f12));
  cv.drawRoundRect(x, y, w, h, 15, on ? cc : c565(0x2a2c31));
  int ix = x + 13;
  cv.fillCircle(ix + 3, cy, 3, on ? cc : c565(0x4d4f55));
  cv.setFont(&fonts::FreeMonoBold9pt7b);
  cv.setTextColor(on ? c565(COL_WHITE) : c565(0x8a8c92)); cv.setTextDatum(middle_left);
  cv.drawString(num, ix + 11, cy);
  cv.setFont(&fonts::FreeSans9pt7b);
  cv.setTextColor(c565(0x808288)); cv.setTextDatum(middle_left);
  cv.drawString("TO REVIEW", ix + 11 + nW + 8, cy);
}

// ---------- page dots ----------
static void drawDots(int active, uint32_t color) {
  int n = 3, gap = 7, dotW = 7, longW = 18, y = 446;
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
  const uint16_t dotc = g_wifi ? c565(0x3CCB7F) : c565(0x6a6d74);
  String ssid = g_wifi ? (g_ssid.length() ? g_ssid : String("WiFi")) : String("No WiFi");
  cv.setFont(&fonts::FreeSans9pt7b); cv.setTextSize(1);
  cv.setTextColor(c565(0xB8BAC0));
  const int maxW = 300, dotR = 3;
  int tw = cv.textWidth(ssid.c_str());
  if (tw <= maxW) {                                   // fits -> centered [dot] SSID
    int total = dotR * 2 + 8 + tw, sx = CX - total / 2;
    cv.fillCircle(sx + dotR, y, dotR, dotc);
    cv.setTextDatum(middle_left);
    cv.drawString(ssid.c_str(), sx + dotR * 2 + 8, y);
  } else {                                            // too long -> fixed dot + scrolling marquee
    int winL = CX - maxW / 2;
    cv.fillCircle(winL - 9, y, dotR, dotc);
    int scrollW = tw + 48;
    int off = (int)((millis() / 40) % (uint32_t)scrollW);
    cv.setClipRect(winL, y - 13, maxW, 26);
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
      g_transcript = String((const char*)(doc["text"] | ""));   // live partial / final
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
  const bool hasText = g_transcript.length() > 0;

  // particle orb: pulses with the real mic level while listening, calm otherwise
  float amp = (g_vphase == 1) ? constrain(g_micLevel, 0.06f, 1.0f) : 0.16f;
  drawVoiceParticles(CX, hasText ? 120 : CY - 8, amp, t);

  // live (partial) -> final transcript, streamed in as you speak
  if (hasText) {
    cv.setFont(&fonts::efontCN_24); cv.setTextSize(1);
    cv.setTextColor(c565(0xFFFFFF));
    drawWrappedCJK(g_transcript, CX, 305, 420, 34);
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

// 270° dot gauge (opening at the bottom), lit up to pct, with an icon-less numeric center
static void drawGaugeDots(int cx, int cy, int r, int pct, uint32_t color) {
  const int n = 11;
  for (int i = 0; i < n; i++) {
    float th = (-135.0f + 270.0f * i / (n - 1)) * DEG_TO_RAD;
    int x = cx + (int)(r * sinf(th)), y = cy - (int)(r * cosf(th));
    bool lit = ((float)i / (n - 1)) <= pct / 100.0f + 0.001f;
    cv.fillSmoothCircle(x, y, 2, lit ? c565(color) : c565(0x33363c));
  }
}
// moon-phase disc: frac 0=new .. 0.5=full .. 1=new (lit half + terminator ellipse)
static void drawMoon(int cx, int cy, int R, float frac) {
  const uint16_t lit = c565(0xF1F0DA), dark = c565(0x1b1d23);
  bool waxing = frac < 0.5f;
  cv.fillSmoothCircle(cx, cy, R, dark);
  cv.setClipRect(waxing ? cx : cx - R, cy - R, R + 1, 2 * R + 1);   // lit half (right if waxing)
  cv.fillSmoothCircle(cx, cy, R, lit);
  cv.clearClipRect();
  float ct = cosf(2.0f * PI * frac); int ew = (int)(R * fabsf(ct));
  if (ew >= 1) cv.fillEllipse(cx, cy, ew, R, ct >= 0 ? dark : lit);   // terminator
  cv.drawCircle(cx, cy, R, c565(0x44474e));
}
static float moonFrac() {
  struct timeval tv; gettimeofday(&tv, nullptr);
  if (!timeSane(tv.tv_sec)) return 0.5f;
  double jd = tv.tv_sec / 86400.0 + 2440587.5;                       // epoch -> Julian Day
  double f = (jd - 2451550.1) / 29.530588853; f -= floor(f); if (f < 0) f += 1.0;   // since 2000 new moon
  return (float)f;
}

// ---------- page 3: rich analog watch face (mechanical complications + Apple-Watch sweep) ----------
static void drawWatchFace() {
  const int cx = CX, cy = CY;
  struct timeval tv; gettimeofday(&tv, nullptr);
  struct tm ti; time_t e = tv.tv_sec; float sec;
  if (timeSane(e)) { localtime_r(&e, &ti); sec = ti.tm_sec + tv.tv_usec / 1000000.0f; }
  else { auto dt = M5.Rtc.getDateTime(); ti.tm_hour = dt.time.hours; ti.tm_min = dt.time.minutes; ti.tm_sec = dt.time.seconds;
         ti.tm_mday = dt.date.date; ti.tm_mon = dt.date.month - 1; ti.tm_wday = dt.date.weekDay; sec = ti.tm_sec + (millis() % 1000) / 1000.0f; }
  const float fmin = ti.tm_min + sec / 60.0f, fhour = (ti.tm_hour % 12) + fmin / 60.0f;

  const uint16_t silver = c565(0xEDEDF2), faint = c565(0x53565d), edge = c565(0x26282d), dimtxt = c565(0x9598a0);
  const uint32_t accent = COL_CLAUDE;
  auto P = [&](float deg, float r, float& ox, float& oy) { float a = deg * DEG_TO_RAD; ox = cx + r * sinf(a); oy = cy - r * cosf(a); };

  cv.drawCircle(cx, cy, 228, edge); cv.drawCircle(cx, cy, 227, edge);               // chapter ring
  for (int i = 0; i < 60; i++) {                                                    // minute railroad
    float ox, oy, ix, iy; bool major = (i % 5 == 0);
    P(i * 6.0f, 223, ox, oy); P(i * 6.0f, major ? 209 : 217, ix, iy);
    cv.drawWideLine((int)ix, (int)iy, (int)ox, (int)oy, major ? 2.0f : 0.7f, major ? silver : faint);
  }
  for (int h = 0; h < 12; h++) {                                                    // baton hour indices
    float ox, oy, ix, iy; P(h * 30.0f, 203, ox, oy); P(h * 30.0f, 181, ix, iy);
    cv.drawWideLine((int)ix, (int)iy, (int)ox, (int)oy, h == 0 ? 4.0f : 2.6f, silver);
  }
  { float ax, ay, bx, by, tx, ty; P(0, 175, tx, ty); P(-2.3f, 201, ax, ay); P(2.3f, 201, bx, by);
    cv.fillTriangle((int)ax, (int)ay, (int)bx, (int)by, (int)tx, (int)ty, c565(accent)); }   // 12 marker

  // top: weekday + Gregorian date, then 农历 + 生肖
  static const char* WD[7] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
  char l1[48]; snprintf(l1, sizeof(l1), "%s  %d月%d日", WD[ti.tm_wday & 7], ti.tm_mon + 1, ti.tm_mday);
  cv.setFont(&fonts::efontCN_24); cv.setTextSize(1); cv.setTextDatum(middle_center);
  cv.setTextColor(silver); cv.drawString(l1, cx, 96);
  if (g_lunar.length()) {
    String l2 = g_lunar + (g_zodiac.length() ? ("  " + g_zodiac + "年") : String(""));
    cv.setTextColor(dimtxt); cv.drawString(l2.c_str(), cx, 126);
  }

  // subdials: battery (9 o'clock), volume (3 o'clock), moon phase (6 o'clock)
  auto subNum = [&](int sx, int sy, int val, uint32_t col, const char* tag) {
    drawGaugeDots(sx, sy, 30, val, col);
    cv.setFont(&fonts::FreeSansBold9pt7b); cv.setTextDatum(middle_center); cv.setTextColor(silver);
    char b[6]; snprintf(b, sizeof(b), "%d", val); cv.drawString(b, sx, sy - 3);
    cv.setFont(&fonts::Font0); cv.setTextColor(dimtxt); cv.drawString(tag, sx, sy + 13);
  };
  subNum(cx - 112, cy, g_batt, 0x3CCB7F, "BAT");
  subNum(cx + 112, cy, g_volume, 0x35B8FF, "VOL");
  cv.drawCircle(cx, cy + 112, 34, edge); drawMoon(cx, cy + 112, 22, moonFrac());

  // hands (AA, tapered)
  float hx, hy, mx, my, sx, sy, tlx, tly;
  P(fhour * 30.0f, 100, hx, hy); P(fhour * 30.0f + 180.0f, 22, tlx, tly);
  cv.drawWedgeLine(cx, cy, (int)hx, (int)hy, 4.4f, 1.8f, c565(0xF2F2F6));
  cv.drawWedgeLine(cx, cy, (int)tlx, (int)tly, 4.4f, 2.2f, c565(0xF2F2F6));
  P(fmin * 6.0f, 150, mx, my); P(fmin * 6.0f + 180.0f, 26, tlx, tly);
  cv.drawWedgeLine(cx, cy, (int)mx, (int)my, 3.6f, 1.2f, silver);
  cv.drawWedgeLine(cx, cy, (int)tlx, (int)tly, 3.6f, 1.6f, silver);
  P(sec * 6.0f, 170, sx, sy); P(sec * 6.0f + 180.0f, 46, tlx, tly);
  cv.drawWideLine((int)tlx, (int)tly, (int)sx, (int)sy, 1.2f, c565(accent));
  { float bxp, byp; P(sec * 6.0f + 180.0f, 32, bxp, byp); cv.fillSmoothCircle((int)bxp, (int)byp, 5, c565(accent)); }

  cv.fillSmoothCircle(cx, cy, 7, silver);                                           // jeweled center cap
  cv.fillSmoothCircle(cx, cy, 4, c565(0x141619));
  cv.fillSmoothCircle(cx, cy, 2, c565(accent));
}

// ---------- compose one page ----------
static void render() {
  if (g_voice) {
    drawVoiceOverlay();
    cv.pushSprite(0, 0);
    return;
  }
  if (page == 2) {                          // pure analog watch face
    cv.fillSprite(c565(0x000000));
    drawWatchFace();
    cv.pushSprite(0, 0);
    return;
  }
  long elapsed = (millis() - bootMs) / 1000;
  const Provider& p = PROV[page];
  int arcPct = max(p.sessionUsed, p.weeklyUsed);
  const char* mood = moodFor(arcPct, p.pending, g_batt);

  char clk[8]; snprintf(clk, sizeof(clk), "%02d:%02d", g_clkH, g_clkM);
  float t = (millis() - bootMs) / 1000.0f;

  cv.fillSprite(c565(0x000000));
  drawArc(p.color, arcPct);
  drawHeader(p, String(clk));
  if (page == 0) drawClaude(CX, 150, p.color, mood, t);
  else           drawCodex(CX, 150, p.color, mood, t);
  if (page == 0 && g_model.length()) {              // model name under the avatar (e.g. "Opus 4.8")
    cv.setFont(&fonts::FreeSans9pt7b); cv.setTextSize(1);
    cv.setTextDatum(middle_center); cv.setTextColor(c565(0x8a8d94));
    cv.drawString(g_model.c_str(), CX, 226);
  }
  long nowE = time(nullptr);
  bool epochOK = nowE > 1700000000L;   // NTP set -> real epoch
  String sR = (g_haveData && epochOK && g_sReset[page] > 0) ? fmtDur(g_sReset[page] - nowE) : fmtDur(remain(p.sessionSeed, elapsed));
  String wR = (g_haveData && epochOK && g_wReset[page] > 0) ? fmtDur(g_wReset[page] - nowE) : fmtDur(remain(p.weeklySeed,  elapsed));
  drawMeter(250, "usage",  p.sessionUsed, sR, p.color);
  drawMeter(286, "weekly", p.weeklyUsed,  wR, p.color);
  drawPill(344, p.pending, p.color);
  drawWifiStatus(406);
  drawDots(page, p.color);
  cv.pushSprite(0, 0);
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
  if (MDNS.begin("codey-watch")) {
    IPAddress ip = MDNS.queryHost(MAC_HOSTNAME, 2500);
    if (ip != IPAddress((uint32_t)0)) g_macIp = ip.toString();
  }
  if (g_macIp.length() == 0) g_macIp = String(MAC_FALLBACK_IP);
  g_companionUrl = "http://" + g_macIp + ":8787/codey/state";
  Serial.printf("Companion Mac -> %s\n", g_macIp.c_str());
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(0);
  Serial.begin(115200);
  Serial.println("codey_dash booted");

  g_prefs.begin("codey", false);                  // persisted settings (brightness/volume)
  g_bright = g_prefs.getUChar("bright", 255);
  g_volume = g_prefs.getUChar("vol", 50);
  M5.Display.setBrightness(g_bright);

  randomSeed(micros());
  cv.setColorDepth(16);
  cv.setPsram(true);
  if (!cv.createSprite(SIZE, SIZE)) Serial.println("ERROR: canvas alloc failed");

  M5.Speaker.end();                       // free the shared codec for the mic
  g_micOK = M5.Mic.begin();
  g_audioBuf = (int16_t*) ps_malloc(MAX_SAMPLES * 2);     // continuous mic-capture buffer (PSRAM)
  Serial.printf("Mic begin=%d  IMU enabled=%d  audioBuf=%p\n", g_micOK, M5.Imu.isEnabled(), g_audioBuf);
  if (!g_audioBuf) Serial.println("ERROR: audio buffer alloc failed (PSRAM) — voice disabled");

  // ---- WiFi provisioning via web config portal ----
  showSetupScreen("WiFi", "connecting...", "");
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);                 // give up after 3 min -> run offline
  wm.setAPCallback([](WiFiManager*) {
    showSetupScreen("WiFi Setup", "join hotspot:  Codey-Setup", "then open  192.168.4.1");
  });
  g_wifi = wm.autoConnect("Codey-Setup");          // saved creds, else open portal
  if (g_wifi) {
    g_ssid = WiFi.SSID();
    Serial.printf("WiFi connected: %s (%s)\n", WiFi.localIP().toString().c_str(), g_ssid.c_str());
    configTime(8 * 3600, 0, "ntp.aliyun.com", "ntp.tencent.com", "pool.ntp.org");  // UTC+8 offset for localtime
    showSetupScreen("WiFi Connected", WiFi.localIP().toString().c_str(), "");
    resolveMac();                                  // find the Companion Mac (mDNS, else fallback IP)
    fetchState();                                  // pulls usage AND sets the clock from the Mac's ts
    syncRtcFromSystem();                           // write the (now-correct) Beijing time into the RTC
    g_ws.begin(g_macIp.c_str(), ASR_PORT, "/");    // streaming-ASR server (persistent + auto-reconnect)
    g_ws.onEvent(wsEvent);
    g_ws.setReconnectInterval(3000);
  } else {
    Serial.println("WiFi not connected (portal timeout) - running offline");
  }

  int b = M5.Power.getBatteryLevel(); if (b >= 0) g_batt = constrain(b, 0, 100);
  readClock();
  Serial.printf("battery=%d  clock=%02d:%02d\n", g_batt, g_clkH, g_clkM);

  bootMs = millis();
  lastActiveMs = millis();
  aBlinkNext = millis() + 1500;
  render();
}

// fetch normalized usage JSON from the Companion and update PROV with real Claude data
static void fetchState() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.setConnectTimeout(2500);
  http.setTimeout(3500);
  if (!http.begin(g_companionUrl)) return;
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
        PROV[i].sessionUsed = pr["session"]["used_pct"] | PROV[i].sessionUsed;
        PROV[i].weeklyUsed  = pr["weekly"]["used_pct"]  | PROV[i].weeklyUsed;
        PROV[i].pending     = pr["pending_reviews"]     | PROV[i].pending;
        g_sReset[i] = pr["session"]["reset_epoch"] | 0L;
        g_wReset[i] = pr["weekly"]["reset_epoch"]  | 0L;
        if (i == 0) { const char* m = pr["model"] | ""; if (m[0]) g_model = String(m); }
      }
      g_haveData = true;
      JsonObject lu = doc["lunar"];                  // 农历 / 生肖 for the watch face
      if (!lu.isNull()) { g_lunar = String((const char*)(lu["date"] | "")); g_zodiac = String((const char*)(lu["zodiac"] | "")); }
      long ts = doc["ts"] | 0L;                      // Mac epoch -> set the device clock (NTP-independent)
      if (ts > 1700000000L && ts < 1900000000L) {
        struct timeval tv; tv.tv_sec = (time_t)ts; tv.tv_usec = 0; settimeofday(&tv, nullptr);
        readClock();
      }
      Serial.printf("[fetch] ok  claude %d/%d  codex %d/%d  stale=%d\n",
                    PROV[0].sessionUsed, PROV[0].weeklyUsed, PROV[1].sessionUsed, PROV[1].weeklyUsed, g_stale);
    } else {
      Serial.printf("[fetch] json err: %s\n", err.c_str());
    }
  } else {
    Serial.printf("[fetch] HTTP %d\n", code);
  }
  http.end();
}

static void reconfigWiFi() {                     // both buttons held -> re-open the WiFi portal
  g_voice = false;
  showSetupScreen("WiFi Setup", "join hotspot:  Codey-Setup", "then open  192.168.4.1");
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  wm.startConfigPortal("Codey-Setup");           // blocking: lets you pick a new network
  g_wifi = (WiFi.status() == WL_CONNECTED);
  if (g_wifi) { showSetupScreen("WiFi Connected", WiFi.localIP().toString().c_str(), ""); delay(1000); }
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

void loop() {
  M5.update();
  uint32_t now = millis();
  if (g_wifi) g_ws.loop();                       // service the streaming-ASR WebSocket + reconnect

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
        g_transcript = ""; g_heardSpeech = false; g_silenceT0 = 0; g_noiseFloor = 0.06f;
        g_sentSamples = 0; g_recEnd = 0; g_finalReqT0 = 0; g_sttFinal = false;
        if (g_micOK && g_audioBuf && g_wsConn) {
          g_vphase = 1;
          wsListen(true);
          M5.Mic.record(g_audioBuf, MAX_SAMPLES, REC_RATE);
          Serial.println("[voice] streaming");
        } else {                                           // can't stream -> show why
          g_vphase = 3; g_resultT0 = now;
          g_transcript = String(!g_micOK ? "麦克风不可用" : !g_wsConn ? "语音服务未连接" : "缓冲不可用");
        }
      } else if (g_vphase == 1) {
        g_recEnd = capturedSamples(); g_vphase = 2;        // press again -> stop & finalize
      } else if (g_vphase == 3) {
        g_voice = false; g_vphase = 0;                     // dismiss the result
      }
    } else if (!g_voice && M5.BtnA.wasPressed() && !M5.BtnB.isPressed()) {  // left -> switch page
      page = (page + 1) % 3;                                        // Claude / Codex / watch face
      Serial.printf("[btnA] page=%d\n", page);
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
        if (g_wsConn) g_ws.sendBIN((uint8_t*)chunk, STREAM_CHUNK * 2);
        g_sentSamples += STREAM_CHUNK;
      }
      bool maxed   = (cap >= MAX_SAMPLES) || (g_micOK && !M5.Mic.isRecording());
      bool silence = g_heardSpeech && g_silenceT0 && (now - g_silenceT0 > 1300) && (now - g_voiceT0 > 1000);
      if (maxed || silence) { g_recEnd = capturedSamples(); g_vphase = 2; g_finalReqT0 = 0; }
    } else if (g_vphase == 2) {                           // FINALIZE: flush tail, await the final stt
      while (g_sentSamples < g_recEnd) {                  // flush remaining audio (not real-time bound now)
        size_t n = g_recEnd - g_sentSamples; if (n > STREAM_CHUNK) n = STREAM_CHUNK;
        if (g_wsConn) g_ws.sendBIN((uint8_t*)(g_audioBuf + g_sentSamples), n * 2);
        g_sentSamples += n;
      }
      if (g_finalReqT0 == 0) { wsListen(false); g_finalReqT0 = now; }   // ask the server to finalize (once)
      if (g_sttFinal || now - g_finalReqT0 > 4000) {                    // got the final result (or timeout)
        if (g_transcript.length() == 0) g_transcript = String("(没听清)");
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
  if (!g_dim && !g_voice && mag > 1.9f && now - g_lastShake > 900) {     // shake -> next page
    page = (page + 1) % 3; g_lastShake = now; active = true;
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
    if (g_wifi) g_ssid = WiFi.SSID();
  }
  if (g_wifi && !g_voice && now - g_lastFetch > 30000) { g_lastFetch = now; fetchState(); }  // poll real usage
  updateAnim(now);
  render();
  delay(g_voice ? 2 : 16);   // voice screen runs faster for a smoother orb
}
