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
#include <WiFiClientSecure.h>  // HTTPS(ngrok 远程隧道)— TLS 客户端
#include <ArduinoJson.h>    // parse it
#include <WebSocketsClient.h>  // stream mic PCM to the sherpa-onnx ASR server, receive live text
#include <mbedtls/base64.h>    // base64(user:pass) -> HTTP Basic auth
#include <ESPmDNS.h>        // resolve the Mac by hostname (survives DHCP IP changes)
#include <Preferences.h>    // persist brightness/volume settings in NVS
#include <sys/time.h>       // settimeofday() — set the clock from the Companion's epoch
#include "freertos/stream_buffer.h"  // 语音 PCM 跨核(主loop -> netTask)
#include "wifi_store.h"     // 多 WiFi 记忆:历史网络(SSID/密码/连接次数)的 NVS 数据层
#include "codey_ui.h"
#include "session_store.h"

// 触摸手势抽象动作(声明置顶:Arduino 生成的函数原型被提到 include 之后,需先见到此类型)
enum TouchAction { ACT_NONE, ACT_TAP, ACT_DOUBLE_TAP, ACT_SWIPE_L, ACT_SWIPE_R, ACT_SWIPE_UP, ACT_SWIPE_DOWN };

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

// companion 下发的显示配置(GET /codey/state -> "display");缺省全开 = 旧行为不变。
// 单写者:仅 netTask 在 fetchState() 里整体替换;主loop 渲染只读。结构是 POD,赋值是逐字段拷贝;
// 读到“半写”状态最坏只是某帧少/多一列(下帧即纠正),不会越界/崩溃,故沿用其它 g_* 状态的无锁单写模式。
static DispCfg g_disp = dispDefault();

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
static volatile uint16_t g_voiceSeq = 0;   // 每次会话自增;只认当前 seq 的 stt(丢弃上次迟到结果)
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
// ---- remote access (ngrok):rhost 非空 -> HTTPS state + WSS ASR;空 -> 局域网(不变) ----
static String         g_remoteHost = "";          // ngrok 域名(如 your.ngrok-free.app);空=局域网模式
static String         g_remoteAuthRaw = "";       // 原始 "user:pass"(门户回填用;空=无认证)
static String         g_remoteAuthB64 = "";       // base64("user:pass"),空=无 Basic 认证(boot 时算)
static char           g_asrUrl[160] = {0};         // Companion /codey/state 下发的 wss:// ASR url(netTask 写/读)
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
static void drawClaude(int ccx, int ccy, uint32_t color, const char* mood, float t, float scale) {
  const int GW = 13, GH = 15; const float cell = (102.0f / GH) * scale;   // scale 缩放整体尺寸
  float bob = sinf(t * 1.7f) * 2.0f * scale;
  if (!strcmp(mood, "alert")) bob = -fabsf(sinf(t * 4.2f)) * 5.0f * scale;   // alert hops
  float ox = ccx - (GW * cell) / 2.0f, oy = ccy - (GH * cell) / 2.0f + bob;
  auto PX = [&](float v) { return ox + v * cell; };
  auto PY = [&](float v) { return oy + v * cell; };

  uint32_t colHi = shade(color, 0.22), colLo = shade(color, -0.26);

  drawAvatarOrb(ccx, ccy, (int)(63 * scale), color);   // 3D-shaded backing orb

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
    int bx = ccx + (int)(40 * scale), by = ccy - (int)(48 * scale);
    cv.fillRoundRect(bx - 13, by - 13, 26, 26, 7, c565(COL_WHITE));
    cv.setFont(&fonts::FreeMonoBold12pt7b); cv.setTextColor(c565(0x0a0a0a)); cv.setTextDatum(middle_center);
    cv.drawString("!", bx, by);
  }
}

// Codex mascot — a cute rounded helper-bot ported from robot-blink.svg: gray two-tone
// shell lit from the left, a dark indigo visor holding two mint eyes that blink, a little
// white smile, side ears and a rounded body. SVG(508x526) is mapped to the screen 1:1.
static void drawCodex(int ccx, int ccy, uint32_t color, const char* mood, float t, float scale) {
  (void)mood;
  const float S = 0.252f * scale;                          // SVG units -> screen px,scale 缩放整体
  const float bob = sinf(t * 1.6f) * 2.0f * scale;         // gentle idle bob
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

  // 3D-shaded backing orb
  drawAvatarOrb(ccx, (int)lroundf(ccy + bob), (int)(63 * scale), color);

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

// ---- WebSocket streaming-ASR client ----
static void wsListen(bool start) {            // listen-control messages (xiaozhi-style)
  if (!g_wsConn) return;
  if (start) {
    char m[112];
    snprintf(m, sizeof(m), "{\"type\":\"listen\",\"state\":\"start\",\"mode\":\"manual\",\"seq\":%u}",
             (unsigned)g_voiceSeq);                  // 带本轮会话序号,companion 回显到 stt
    g_ws.sendTXT(m);
  } else {
    g_ws.sendTXT("{\"type\":\"listen\",\"state\":\"stop\"}");
  }
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
      if (doc["seq"].is<int>() && (uint16_t)(doc["seq"] | 0) != g_voiceSeq) return;  // 丢弃陈旧会话的迟到 stt
      strncpy(g_transcript, (const char*)(doc["text"] | ""), sizeof(g_transcript) - 1);
      g_transcript[sizeof(g_transcript) - 1] = '\0';            // live partial / final
      if (doc["final"] | false) g_sttFinal = true;
    }
  }
}

// ---------- voice overlay (meme-style: active mascot + streaming transcript) ----------
// Sonar rings that breathe outward with the mic level — the "listening" energy cue that
// replaces the old particle orb. The provider mascot is drawn on top by drawVoiceOverlay().
static void drawListenRings(int cx, int cy, int baseR, uint32_t color, float amp, float t) {
  const int RINGS = 3;
  for (int i = 0; i < RINGS; i++) {
    float phase = t * 0.9f - i * 0.45f;                          // staggered outward pulses
    float k = phase - floorf(phase);                             // 0..1 expansion within a pulse
    float r = baseR * (1.0f + k * (0.45f + amp * 0.9f));         // louder -> rings reach further
    float fade = (1.0f - k) * (0.30f + amp * 0.55f);             // fade as they expand; brighter when loud
    if (fade <= 0.02f) continue;
    cv.drawCircle(cx, cy, (int)r, c565(shade(color, -0.55f + fade * 0.85f)));
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
  const bool result  = (g_vphase == 3);
  const float amp = (g_vphase == 1) ? constrain(g_micLevel, 0.06f, 1.0f) : 0.16f;

  // 1) streaming transcript at the TOP — partial -> final, like meme's inline label
  if (hasText) {
    cv.setFont(&fonts::efontCN_24); cv.setTextSize(1);
    cv.setTextColor(c565(result ? 0xFFFFFF : 0xFFD27A));      // warm while live, white once settled
    drawWrappedCJK(String(g_transcript), CX, 100, 412, 34);
    cv.drawFastHLine(CX - 130, 176, 260, c565(shade(color, -0.3f)));   // divider under transcript
  }

  // 2) the provider mascot in its "active" state: breathes with the mic level + listening rings
  const int my = hasText ? 300 : CY + 6;                      // shift down when transcript present
  const float scale = (hasText ? 0.92f : 1.12f) * (1.0f + amp * 0.16f);
  if (g_vphase == 1 || g_vphase == 2)
    drawListenRings(CX, my, (int)(70 * scale), color, amp, t);
  const char* mood = result ? "happy" : "";                   // calm-attentive while listening
  if (page == 0) drawClaude(CX, my, color, mood, t, scale);
  else           drawCodex(CX, my, color, mood, t, scale);

  // 3) status line at the BOTTOM: ● LISTENING… / RECOGNIZING… / dismiss hint
  if (!result) {
    const char* label = (g_vphase == 1) ? "LISTENING" : "RECOGNIZING";
    const uint32_t dotc = (g_vphase == 1) ? 0x3CCB7F : 0xFFC24A;   // green listening, amber finalizing
    int dots = ((int)(t * 2)) % 4;
    char title[24]; snprintf(title, sizeof(title), "%s%.*s", label, dots, "...");
    char full[24];  snprintf(full,  sizeof(full),  "%s...", label);   // fixed width -> no jiggle
    cv.setFont(&fonts::FreeSansBold18pt7b); cv.setTextSize(1);
    int gw = 12 + 8 + cv.textWidth(full), sx = CX - gw / 2;           // center the ● + label group
    cv.fillSmoothCircle(sx + 6, 412, 6, c565(dotc));                  // status dot
    cv.setTextDatum(middle_left);
    cv.setTextColor(c565(shade(color, 0.25f)));
    cv.drawString(title, sx + 20, 412);
  } else {
    cv.setTextDatum(middle_center);
    cv.setFont(&fonts::FreeSans9pt7b); cv.setTextColor(c565(0x6a6d74));
    cv.drawString("press right to dismiss", CX, 420);
  }
}


// forward decls
static void renderUsagePage(int provIdx);
static void renderListPage(int provIdx);
static void renderDetailPage();
static void ensureProvVisible();   // provider-skip:把 page 吸附到启用的 provider(定义在导航区)

// detail 视图状态:active<0 表示不在详情;否则 detailProv(0/1) + detailIdx
static int detailProv = -1, detailIdx = 0;
static bool g_listView = false;   // true: 当前 provider 的会话列表(从主页滑入)

// 列表页布局(466 屏内)
static const int ROW_H = 40, LIST_MAX_VIS = 6;   // 表格行高;同屏最多行(超出则上下循环滚动)
// 列表渲染几何(渲染时写,rowHitAt 命中检测读 —— 两者一致)
static int  g_listBandTop = 0, g_listBandH = 0, g_listOff = 0, g_listVisN = 0;
static bool g_listAuto = false;

// 触摸手势 → 抽象动作(action);导航只消费 action,改交互只动 handleAction()
// 注:enum 已上移到文件顶部(Arduino 自动生成的函数原型会被提到 include 之后,需先见到此类型)
static const int  TAP_MOVE = 16, SWIPE_MIN = 50; static const uint32_t DBL_MS = 400, RELEASE_MS = 40;
static bool     g_tDown = false;
static int      g_tx0 = 0, g_ty0 = 0, g_tLastX = 0, g_tLastY = 0;
static char     g_tAxis = 0;                 // 0 未定 / 'x' / 'y'
static int      g_tProv = -1;                // 竖拖滚动作用的列表 provIdx(-1=非列表)
static uint32_t g_lastTapMs = 0;             // 双击窗(尽力,硬件多半接不住)
static int      g_tapRow = -1;               // 最近 TAP 命中行(供 handleAction)
static uint32_t g_touchLostMs = 0;           // 失触起点;去抖确认抬起(防 getCount 单帧抖动)
static bool     g_gestureFired = false;      // 本次手势是否已触发滑动(阈值即发,防重复/防误判点击)
static bool     g_forceRender = false;       // 动作发生时强制立即重绘(切页跟手)


// ---------- compose one page ----------
static void render() {
  if (g_voice) { drawVoiceOverlay(); cv.pushSprite(0, 0); return; }
  ensureProvVisible();                       // 当前页落在被禁用 provider 上 → 吸附到启用页
  if (detailProv >= 0)      renderDetailPage();
  else if (g_listView)      renderListPage(page);
  else                      renderUsagePage(page);
  cv.pushSprite(0, 0);
}

static void renderUsagePage(int provIdx) {
  const Prov& p = PROV[provIdx];
  blitProviderArc(provIdx);
  char clk[8]; snprintf(clk, sizeof(clk), "%02d:%02d", g_clkH, g_clkM);
  drawHeader(p, String(clk));
  float t = (millis() - bootMs) / 1000.0f;
  const char* mood = moodForUsage(max(p.sessUsed, p.weekUsed), p.activeCount, g_batt);
  if (provIdx == 0) drawClaude(CX, 150, p.color, mood, t, 1.0f);
  else              drawCodex(CX, 150, p.color, mood, t, 1.0f);
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
  drawSessionCount(344, p.activeCount, p.nsess);    // session 运行/总数(运行数绿)
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
    if (provIdx == 0) drawClaude(CX, 42, color, mood, t, 0.32f);
    else              drawCodex(CX, 42, color, mood, t, 0.32f); }

  // 空态
  if (p.nsess == 0) {
    cv.setFont(&fonts::FreeSans9pt7b); cv.setTextColor(c565(0x6f757d)); cv.setTextDatum(middle_center);
    cv.drawString("no sessions", CX, CY);
    drawDots(provIdx, color);
    return;
  }

  // 动态列布局:companion 下发的列开关 → 启用列的有序索引 + 槽起点 x(纯函数,host 已测)。
  // 全开 → 复现原固定列 46/112/204/242/300/358;少列 → 重新铺满并以屏中线居中。
  int colIdx[DISP_NCOL], colX[DISP_NCOL];
  int ncol = layoutColumns(g_disp.col, colIdx, colX);
  if (ncol == 0) { ncol = 1; colIdx[0] = DC_STATUS; colX[0] = DISP_BAND_CENTER - DISP_COL_W[DC_STATUS] / 2; }  // 全关兜底:至少保留状态
  // STATUS 列的状态点固定画在该列槽起点;状态词右移 DISP_STATUS_WORD_DX。
  int statusSlotX = -1;
  for (int k = 0; k < ncol; k++) if (colIdx[k] == DC_STATUS) statusSlotX = colX[k];

  // 居中几何:整张表(表头 + 可见行)以屏幕中线 CY 为对称轴居中
  const int HEAD_H = 26;
  int visN = p.nsess < LIST_MAX_VIS ? p.nsess : LIST_MAX_VIS;
  int bandH = visN * ROW_H;
  int blockTop = CY - (HEAD_H + bandH) / 2;
  int headY = blockTop + 11;
  int bandTop = blockTop + HEAD_H;
  bool autoScroll = p.nsess > LIST_MAX_VIS;
  int contentH = p.nsess * ROW_H;
  int off = autoScroll ? (int)((millis() / 40) % (uint32_t)contentH) : 0;   // ~25px/s 上下循环滚动
  g_listBandTop = bandTop; g_listBandH = bandH; g_listOff = off;            // 供 rowHitAt 命中检测
  g_listVisN = visN; g_listAuto = autoScroll;

  // 表头(居中块顶部):按启用列绘制各自标签;STATUS 标签随状态词偏移对齐其文本列。
  static const char* COL_HEAD[DISP_NCOL] = { "St", "Model", "Ctx", "Tok", "Mem", "Turn" };
  cv.setFont(&fonts::FreeMono9pt7b); cv.setTextSize(1); cv.setTextDatum(middle_left);
  cv.setTextColor(c565(0x6d6f75));
  for (int k = 0; k < ncol; k++) {
    int hx = (colIdx[k] == DC_STATUS) ? colX[k] + DISP_STATUS_WORD_DX : colX[k];
    cv.drawString(COL_HEAD[colIdx[k]], hx, headY);
  }
  cv.drawFastHLine(46, bandTop - 3, SIZE - 92, c565(0x2a2c31));

  // 行(居中静态;>LIST_MAX_VIS 时上下循环滚动,双份无缝拼接;裁剪到行带)
  cv.setClipRect(40, bandTop, SIZE - 80, bandH);
  for (int pass = 0; pass < (autoScroll ? 2 : 1); pass++) {
    int base = bandTop - off + pass * contentH;
    for (int i = 0; i < p.nsess; i++) {
      int y = base + i * ROW_H;
      if (y + ROW_H < bandTop || y > bandTop + bandH) continue;
      const Sess& s = p.sess[i];
      SessStatus st = (SessStatus)s.status;
      uint16_t dc = c565(statusDotColor(st));
      int my = y + ROW_H / 2;
      if (st == ST_EXECUTING)
        cv.fillRoundRect(46, y + 3, SIZE - 92, ROW_H - 5, 6, c565(shade(color, -0.82f)));  // 高亮条:全行宽,与列数无关
      // 状态点:仅当 STATUS 列启用时画(其余列不绘点)。
      if (statusSlotX >= 0) {
        if (st == ST_EXECUTING) cv.fillSmoothCircle(statusSlotX, my, 5, dc);   // 实心=运行
        else { cv.drawCircle(statusSlotX, my, 5, dc); cv.drawCircle(statusSlotX, my, 4, dc); }   // 空心环=其余
      }
      cv.setFont(&fonts::FreeMono9pt7b); cv.setTextSize(1); cv.setTextDatum(middle_left);
      for (int k = 0; k < ncol; k++) {
        int cx = colX[k];
        switch (colIdx[k]) {
          case DC_STATUS: {
            cv.setTextColor(dc); cv.drawString(statusShort(st), cx + DISP_STATUS_WORD_DX, my);
            break;
          }
          case DC_MODEL: {
            char md[16]; modelShort(s.model, md, sizeof(md));      // 紧凑:去空格(Opus 4.8 -> Opus4.8)
            for (char* a = md, *b = md; ; ++a) { if (*a != ' ') *b++ = *a; if (!*a) break; }
            cv.setTextColor(c565(0x8a8d94)); cv.drawString(md, cx, my);
            break;
          }
          case DC_CTX: {
            char cb[8]; snprintf(cb, sizeof(cb), "%d%%", s.ctxPct);
            cv.setTextColor(c565(ctxColor(s.ctxPct))); cv.drawString(cb, cx, my);
            break;
          }
          case DC_TOKENS: {
            char tb[12]; fmtTokens(s.tokTotal, tb, sizeof(tb));
            cv.setTextColor(c565(0xe6e8ec)); cv.drawString(tb, cx, my);
            break;
          }
          case DC_MEMORY: {
            char mb[10]; fmtMem(s.memKb, mb, sizeof(mb));
            cv.setTextColor(c565(0x8a8d94)); cv.drawString(mb, cx, my);
            break;
          }
          case DC_TURN: {
            char rb[8]; snprintf(rb, sizeof(rb), "%d", s.turn);
            cv.setTextColor(c565(0x8a8d94)); cv.drawString(rb, cx, my);
            break;
          }
          default: break;
        }
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

  // 头部:provider 色点 + 会话名称(只显示名称,不再带 · CLAUDE)
  char nm[40]; truncCp(s.name, 18, nm, sizeof(nm));
  cv.setFont(&fonts::FreeSansBold12pt7b); cv.setTextSize(1); cv.setTextDatum(middle_center);
  int tWid = cv.textWidth(nm);
  cv.fillCircle(CX - tWid / 2 - 11, 44, 4, c565(color));
  cv.setTextColor(c565(0xcfd2d8)); cv.drawString(nm, CX, 44);

  // 第二行:model · 已运行时长(efontCN 字体能渲染 ·,不再是方框)。MODEL 列关 → 只显时长。
  long nowE = time(nullptr); bool epochOK = nowE > 1700000000L;
  long elapsed = (epochOK && s.startedAt > 0) ? (nowE - s.startedAt) : 0;
  char el[16]; fmtElapsed(elapsed, el, sizeof(el));
  char l2[48];
  if (g_disp.col[DC_MODEL]) { char md[24]; modelShort(s.model, md, sizeof(md)); snprintf(l2, sizeof(l2), "%s · %s", md, el); }
  else                        snprintf(l2, sizeof(l2), "%s", el);
  cv.setFont(&fonts::efontCN_16); cv.setTextDatum(middle_center);
  cv.setTextColor(c565(0x7d828a)); cv.drawString(l2, CX, 68);

  // 缩小版的首页同款动画机器人 + 状态词
  float t = (millis() - bootMs) / 1000.0f;
  const char* mood = moodForStatus(s.status);
  if (detailProv == 0) drawClaude(CX, 118, color, mood, t, 0.62f);
  else                 drawCodex(CX, 118, color, mood, t, 0.62f);
  cv.setFont(&fonts::FreeSansBold12pt7b); cv.setTextDatum(middle_center);
  uint16_t sw = c565(st == ST_EXECUTING ? shade(color, 0.25f) : st == ST_THINKING ? 0xffd479 : 0x8b9097);
  cv.setTextColor(sw); cv.drawString(statusWord(st), CX, 180);

  // 数据宫格:CTX / TURN / TOKENS / MEMORY,各自由对应列开关控制,启用块均分居中(块越少越居中)。
  // 注:原详情页只有 CTX/TURN/TOKENS 三块;memory 列默认开 → 全开时新增 MEM 块(共 4 块,
  //     span=414px,在 466 圆屏可视范围内,左右各余 ~26px)。这是相对旧固件唯一的“默认外观”变化,
  //     符合需求「memory off → 不显示 memory 块」的语义(memory 作为可隐藏的详情块存在)。
  char ctxv[8]; snprintf(ctxv, sizeof(ctxv), "%d%%", s.ctxPct);
  char turnv[8]; snprintf(turnv, sizeof(turnv), "%d", s.turn);
  char tokv[12]; fmtTokens(s.tokTotal, tokv, sizeof(tokv));   // K千/W万/B十亿
  char memv[10]; fmtMem(s.memKb, memv, sizeof(memv));
  struct Tile { const char* label; const char* value; };
  Tile tiles[4]; int nt = 0;
  if (g_disp.col[DC_CTX])    tiles[nt++] = { "CTX",    ctxv };
  if (g_disp.col[DC_TURN])   tiles[nt++] = { "TURN",   turnv };
  if (g_disp.col[DC_TOKENS]) tiles[nt++] = { "TOKENS", tokv };
  if (g_disp.col[DC_MEMORY]) tiles[nt++] = { "MEM",    memv };
  if (nt > 0) {
    const int tw = 96, th = 50, gap = 10, ty = 228;
    int span = nt * tw + (nt - 1) * gap;
    int cx0 = CX - span / 2 + tw / 2;                       // 第一块的圆心 x
    for (int k = 0; k < nt; k++)
      drawStatTile(cx0 + k * (tw + gap), ty, tw, th, tiles[k].label, tiles[k].value, color);
  }

  // 任务/agent 名称(efontCN 可渲染中文与 ·;超宽 → 跑马灯循环)
  char buf[96];
  if (s.task[0]) snprintf(buf, sizeof(buf), "* %s", s.task);
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
  if (s.nports > 0)    snprintf(buf + n, sizeof(buf) - n, " · :%d", s.ports[0]);
  cv.setFont(&fonts::efontCN_16); cv.setTextColor(c565(0xc3c7cd)); cv.setTextDatum(middle_center);
  cv.setClipRect(48, 302, SIZE - 96, 22); cv.drawString(buf, CX, 312); cv.clearClipRect();

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

// HTML-escape a user-controlled string for safe interpolation into a value="..." attribute.
static String htmlAttr(const String& s) {
  String o; o.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '&') o += "&amp;"; else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;"; else if (c == '"') o += "&quot;";
    else o += c;
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
    h += "<form style='display:inline' method=POST action=/connect><input type=hidden name=ssid value=\"" + htmlAttr(String(g_nets[i].ssid)) + "\"><button class=lk>连接</button></form> ";
    h += "<a href='/del?ssid=" + urlencode(g_nets[i].ssid) + "'><button class=del>删除</button></a></div></div>";
  }
  h += F("<h3>周边网络</h3><select id=scan onchange=\"document.getElementById('ssid').value=this.value\"><option>点「扫描」刷新…</option></select>"
         "<button class=lk style=width:100% onclick=doScan()>扫描周边 WiFi</button>");
  h += "<h3>连接 / 新增网络</h3><form method=POST action=/connect>"
       "<input id=ssid name=ssid placeholder=SSID>"
       "<input name=pass type=password placeholder='密码（连接历史网络可留空）'>"
       "<input name=macip placeholder='Companion Mac IP（留空=自动 mDNS）' value=\"" + htmlAttr(String(g_manualMac)) + "\">"
       "<input name=rhost placeholder='Remote host（ngrok域名，留空=局域网）' value=\"" + htmlAttr(g_remoteHost) + "\">"
       "<input name=rauth type=password placeholder='Auth（user:pass，留空=无）' value=\"" + htmlAttr(g_remoteAuthRaw) + "\">"
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
  // 远程访问(ngrok):rhost 空=局域网模式。两字段总是落盘(留空即清除)。重启后由 setup 生效。
  String rhost = s.arg("rhost"); rhost.trim();
  String rauth = s.arg("rauth");
  g_prefs.putString("rhost", rhost);
  g_prefs.putString("rauth", rauth);
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
// base64-encode a string (for HTTP Basic auth "user:pass"); empty in -> empty out
static String b64(const String& in) {
  if (in.length() == 0) return String("");
  size_t inLen = in.length();
  size_t outLen = 0;
  // sizing pass (dst=NULL, dlen=0 -> required size incl. NUL in *olen)
  mbedtls_base64_encode(nullptr, 0, &outLen, (const unsigned char*)in.c_str(), inLen);
  if (outLen == 0) return String("");
  unsigned char* buf = (unsigned char*)malloc(outLen);
  if (!buf) return String("");
  size_t written = 0;
  int rc = mbedtls_base64_encode(buf, outLen, &written, (const unsigned char*)in.c_str(), inLen);
  String out = (rc == 0) ? String((const char*)buf) : String("");
  free(buf);
  return out;
}

// find the Companion Mac on the LAN by mDNS hostname (robust to DHCP IP changes); else fixed fallback
static void resolveMac() {
  if (g_remoteHost.length()) {                       // 远程模式:走 ngrok HTTPS,不做 mDNS/LAN 解析
    g_companionUrl = "https://" + g_remoteHost + "/codey/state";
    Serial.printf("Companion (remote) -> %s\n", g_companionUrl.c_str());
    return;
  }
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
// (re)point the streaming-ASR WebSocket at the current target (LAN IP or remote WSS host).
// netTask-only. Remote mode (g_remoteHost set) uses TLS via beginSSL once a wss:// url has been
// delivered in /codey/state (g_asrUrl); otherwise plain WS to the Mac on the LAN.
// g_wsExtraHdr backs setExtraHeaders — the WS lib keeps the pointer, so it must outlive the connection.
static String g_wsExtraHdr = "";          // netTask-only: backing store for setExtraHeaders
static void wsConnect() {
  g_ws.disconnect();
  if (g_remoteHost.length()) {                       // 远程模式:绝不空连 LAN(asr_url 未到则等 maybeRepointWs 触发)
    if (g_asrUrl[0]) {                               // 远程:WSS(TLS)到下发的 ASR 域名
      char host[160]; parseWssHost(g_asrUrl, host, sizeof(host));
      if (host[0]) {
        g_wsExtraHdr = "";                           // ngrok 警告页跳过 + 可选 Basic 认证
        if (g_remoteAuthB64.length()) g_wsExtraHdr += "Authorization: Basic " + g_remoteAuthB64 + "\r\n";
        g_wsExtraHdr += "ngrok-skip-browser-warning: true";
        g_ws.setExtraHeaders(g_wsExtraHdr.c_str());
        g_ws.beginSSL(host, 443, "/");               // 空 fingerprint -> ESP32 内部 setInsecure()
      }
    }
    // g_asrUrl 尚未下发 -> 这一轮什么都不连;asr_url 首次到达时由 maybeRepointWs() 发起首连
  } else {                                            // 局域网:明文 WS 到 Mac(行为不变,保留库默认 Origin 头)
    g_ws.begin(g_macIp.c_str(), ASR_PORT, "/");
  }
  g_ws.onEvent(wsEvent);
  g_ws.setReconnectInterval(3000);
}

// 远程模式下,/codey/state 下发的 ASR 域名可能变化(ngrok 重启换地址)。netTask-only。
// 记录 WS 当前指向的 host,仅当目标 host 真正变化时才断开重连(避免抖动 thrash)。
static String g_wsAsrHost = "";          // netTask-only:WS 当前指向的 host
static void maybeRepointWs() {
  if (g_remoteHost.length() == 0 || g_asrUrl[0] == 0) return;   // 局域网/尚未下发 -> 不动
  char host[160]; parseWssHost(g_asrUrl, host, sizeof(host));
  if (!host[0]) return;
  if (g_wsAsrHost == host) return;       // host 未变 -> 不重连(防抖)
  g_wsAsrHost = host;
  Serial.printf("ASR url changed -> WSS %s\n", host);
  wsConnect();                            // 内部已 disconnect() + beginSSL(host,...)
}

static void netTask(void*);   // 定义在后面(core0 网络任务);setup 里 xTaskCreate 需先声明

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(0);
  Serial.begin(115200);
  Serial.println("codey_dash booted");

  g_prefs.begin("codey", false);                  // persisted settings (brightness/volume/macip)
  g_bright = g_prefs.getUChar("bright", 255);
  g_volume = g_prefs.getUChar("vol", 50);
  { String m = g_prefs.getString("macip", ""); strncpy(g_manualMac, m.c_str(), sizeof(g_manualMac) - 1); g_manualMac[sizeof(g_manualMac) - 1] = 0; }
  // ---- remote access (ngrok):rhost 非空 -> HTTPS state + WSS ASR;空 -> 局域网(默认,行为不变) ----
  g_remoteHost    = g_prefs.getString("rhost", "");
  g_remoteAuthRaw = g_prefs.getString("rauth", "");
  g_remoteAuthB64 = g_remoteAuthRaw.length() ? b64(g_remoteAuthRaw) : String("");
  if (g_remoteHost.length()) Serial.printf("remote mode: host=%s auth=%s\n", g_remoteHost.c_str(), g_remoteAuthB64.length() ? "yes" : "no");
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
  s.memKb    = so["memory"]         | 0L;
  s.nports = 0;
  for (JsonVariant pv : so["ports"].as<JsonArray>()) { if (s.nports < MAX_PORTS) s.ports[s.nports++] = pv.as<int>(); }
}

// fetch normalized usage JSON from the Companion and update PROV with real Claude data
static void fetchState() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.setConnectTimeout(2500);
  http.setTimeout(3500);
  if (g_remoteHost.length()) {                       // 远程:HTTPS(TLS,不校验证书)+ ngrok/Basic 头
    static WiFiClientSecure tls; tls.setInsecure();
    if (!http.begin(tls, g_companionUrl)) return;
    if (g_remoteAuthB64.length()) http.addHeader("Authorization", "Basic " + g_remoteAuthB64);
    http.addHeader("ngrok-skip-browser-warning", "true");
  } else {                                            // 局域网:明文 HTTP(行为不变)
    if (!http.begin(g_companionUrl)) return;
  }
  bool ok = false;
  int code = http.GET();
  if (code == 200) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getString());
    if (!err) {
      g_stale = doc["stale"] | false;
      const char* a = doc["asr_url"] | "";           // 远程下发的 wss:// ASR url(局域网为空)
      strncpy(g_asrUrl, a, sizeof(g_asrUrl) - 1); g_asrUrl[sizeof(g_asrUrl) - 1] = 0;

      // display 配置:列开关 + provider 开关;缺省/缺键 → true(旧 companion 行为不变)。
      // 先在局部组装,最后一次性赋给 g_disp(单写者:netTask;主loop 只读)。
      {
        DispCfg d = dispDefault();
        JsonVariantConst disp = doc["display"];
        if (!disp.isNull()) {
          JsonVariantConst cols = disp["columns"];
          d.col[DC_STATUS] = cols["status"] | true;
          d.col[DC_MODEL]  = cols["model"]  | true;
          d.col[DC_CTX]    = cols["ctx"]    | true;
          d.col[DC_TOKENS] = cols["tokens"] | true;
          d.col[DC_MEMORY] = cols["memory"] | true;
          d.col[DC_TURN]   = cols["turn"]   | true;
          JsonVariantConst pv = disp["providers"];
          d.prov[0] = pv["claude"] | true;
          d.prov[1] = pv["codex"]  | true;
        }
        g_disp = d;
      }

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
      if (!g_voice && (lastFetch == 0 || now - lastFetch > 30000)) { lastFetch = now; fetchState(); maybeRepointWs(); }
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

// provider 页是否启用(companion display.providers);两者全关时兜底视作全开(至少显示一个)。
static bool provEnabled(int idx) {
  if (!g_disp.prov[0] && !g_disp.prov[1]) return true;   // 全关兜底:不跳过,正常显示
  return g_disp.prov[idx];
}
// 从 page 起按 dir(±1)找下一个启用的 provider 页;最多绕一圈,找不到则原地不动。
static int nextProv(int from, int dir) {
  for (int step = 1; step <= DISP_NPROV; step++) {
    int cand = ((from + dir * step) % DISP_NPROV + DISP_NPROV) % DISP_NPROV;
    if (provEnabled(cand)) return cand;
  }
  return from;
}
// 若当前 page 落在被禁用的 provider 上,吸附到最近的启用页(切换显示配置后自愈)。
static void ensureProvVisible() { if (!provEnabled(page)) page = nextProv(page, 1); }

// 列表页:由屏幕 y 命中会话行号(用渲染时记下的几何,自动/居中两种布局一致);-1 = 空白
static int rowHitAt(int provIdx, int ty) {
  int n = PROV[provIdx].nsess;
  if (n <= 0 || g_listBandH <= 0) return -1;
  if (ty < g_listBandTop || ty > g_listBandTop + g_listBandH) return -1;
  int rel = ty - g_listBandTop;
  if (g_listAuto) return ((g_listOff + rel) / ROW_H) % n;     // 滚动中:按当前偏移定位
  int idx = rel / ROW_H;
  return (idx >= 0 && idx < n) ? idx : -1;
}

static void enterDetail(int prov, int idx) { detailProv = prov; detailIdx = idx; }
static void exitDetail() { detailProv = -1; }

// 每帧调用;抬起沿返回一个动作(否则 ACT_NONE)。列表竖拖滚动在内部实时处理。
static TouchAction detectTouchAction(uint32_t now) {
  auto td = M5.Touch.getDetail();
  bool touching = M5.Touch.getCount() > 0;
  TouchAction act = ACT_NONE;
  if (touching) {
    g_tLastX = td.x; g_tLastY = td.y; g_touchLostMs = 0;   // 有触摸:清失触计时(吃掉单帧抖动)
    if (!g_tDown) {                                 // 按下沿(用 g_tDown,抖动恢复不会重置基准)
      g_tDown = true; g_tx0 = td.x; g_ty0 = td.y; g_tAxis = 0; g_gestureFired = false;
      g_tProv = curListProv();
    } else {                                        // 移动中
      int dx = g_tLastX - g_tx0, dy = g_tLastY - g_ty0;
      if (!g_tAxis && (abs(dx) > TAP_MOVE || abs(dy) > TAP_MOVE)) g_tAxis = (abs(dx) > abs(dy)) ? 'x' : 'y';
      if (!g_gestureFired) {                        // 阈值即触发(跟手:不等抬手就切页/返回)。列表不再手动滚动(改自动循环)
        if (g_tAxis == 'x' && abs(dx) >= SWIPE_MIN)      { act = (dx < 0) ? ACT_SWIPE_L : ACT_SWIPE_R; g_gestureFired = true; }
        else if (g_tAxis == 'y' && abs(dy) >= SWIPE_MIN) { act = (dy < 0) ? ACT_SWIPE_UP : ACT_SWIPE_DOWN; g_gestureFired = true; }
      }
    }
  } else if (g_tDown) {                             // 无触摸:去抖确认抬起(防单帧 getCount=0 抖动)
    if (g_touchLostMs == 0) g_touchLostMs = now;
    else if (now - g_touchLostMs >= RELEASE_MS) {   // 抬起沿 → 仅判点击(滑动已在移动中触发)
      int dx = g_tLastX - g_tx0, dy = g_tLastY - g_ty0;
      if (!g_gestureFired && g_tAxis == 0 && abs(dx) < TAP_MOVE && abs(dy) < TAP_MOVE) {
        g_tapRow = (g_tProv >= 0) ? rowHitAt(g_tProv, g_ty0) : -1;
        act = (now - g_lastTapMs < DBL_MS) ? ACT_DOUBLE_TAP : ACT_TAP;
        g_lastTapMs = now;
      }
      g_tDown = false; g_tAxis = 0; g_tProv = -1; g_touchLostMs = 0; g_gestureFired = false;   // 抬起即复位
    }
  }
  return act;
}

// 动作 → 导航(要改交互/重绑只改这里)
static void handleAction(TouchAction a) {
  switch (a) {
    case ACT_SWIPE_L: case ACT_SWIPE_R: {            // 横滑:详情翻会话,否则切端
      int dir = (a == ACT_SWIPE_L) ? 1 : -1;
      if (detailProv >= 0) { int n = PROV[detailProv].nsess; if (n) detailIdx = (detailIdx + dir + n) % n; }
      else page = nextProv(page, dir);   // 切端:跳过被 companion 禁用的 provider 页
      break;
    }
    case ACT_SWIPE_UP:                               // 上滑:进入下一级(主页 → 列表)
      if (detailProv < 0 && !g_listView) g_listView = true;
      break;
    case ACT_SWIPE_DOWN:                             // 下滑:返回上一级
      if (detailProv >= 0) exitDetail();             // 详情 → 列表
      else if (g_listView) g_listView = false;       // 列表 → 主页
      break;
    case ACT_TAP:                                    // 单击:主页 → 列表;列表点行 → 详情
      if (detailProv >= 0) break;
      if (g_listView) { if (PROV[page].nsess) enterDetail(page, g_tapRow >= 0 ? g_tapRow : 0); }
      else g_listView = true;
      break;
    case ACT_DOUBLE_TAP:                             // 双击(备选返回;硬件多半接不住)
      if (detailProv >= 0) exitDetail();
      else if (g_listView) g_listView = false;
      break;
    default: break;
  }
}

// 长按 BtnA:详情→列表;列表→主页;主页→列表
static void btnALong() {
  if (detailProv >= 0) { exitDetail(); return; }      // 详情 → 列表
  if (g_listView) { g_listView = false; return; }     // 列表 → 主页
  g_listView = true;                                  // 主页 → 列表
}
// 短按 BtnA:详情翻下一会话;否则切 provider(跳过被禁用的 provider 页)
static void btnAShort() {
  if (detailProv >= 0) { int n = PROV[detailProv].nsess; if (n > 0) detailIdx = (detailIdx + 1) % n; }
  else page = nextProv(page, 1);
}

void loop() {
  M5.update();
  uint32_t now = millis();
  // 网络全部在 netTask(core0);主loop 不调 g_ws.loop / fetchState,绝不等待网络

  // ---- 触摸手势(仅非设置/非语音态)----
  if (!g_inSettings && !g_voice) {
    TouchAction act = detectTouchAction(now);
    if (act != ACT_NONE) { handleAction(act); lastActiveMs = now; g_forceRender = true; }
  } else {                                   // 进入设置/语音态:清手势残留
    g_tDown = false; g_tAxis = 0; g_tProv = -1; g_touchLostMs = 0;
  }

  static uint32_t bothSince = 0; static bool bothFired = false;   // hold BOTH ~0.4s -> toggle settings
  if (M5.BtnA.isPressed() && M5.BtnB.isPressed()) {
    if (bothSince == 0) bothSince = now;
    if (!bothFired && now - bothSince > 400) {
      bothFired = true; g_setSel = 0; g_inSettings = !g_inSettings;
      if (g_inSettings) { g_voice = false; g_vphase = 0; g_voiceSeq++; } else bootMs = millis();  // 进设置=放弃语音,推进 seq 丢弃迟到结果
    }
  } else {
    bothSince = 0; bothFired = false;
    if (g_inSettings) {
      settingsButtons();                                   // BtnA = down, BtnB = confirm
    } else if (M5.BtnB.wasPressed() && !M5.BtnA.isPressed()) {   // right button -> voice command
      if (!g_voice) {                                      // start streaming
        g_voice = true; g_voiceT0 = now; g_micLevel = 0.12f; g_voiceSeq++;   // 新会话序号(隔离上次迟到结果)
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
      } else if (g_vphase == 2) {
        g_voice = false; g_vphase = 0; g_voiceSeq++;       // finalizing 时再按 -> 取消等待(丢弃迟到 final)
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
      if (g_sttFinal || now - g_finalReqT0 > 2500) {                    // got the final result (or 2.5s 超时,可按键提前取消)
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
    if (detailProv < 0 && !g_listView) page = nextProv(page, 1);   // 详情/列表态下摇晃不切 provider;跳过禁用页
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
  // 触摸采样与重绘解耦:循环高速跑(每几 ms 采一次触摸,接住快速双击的抬手间隙),
  // 重绘限到 ~30fps;语音叠层仍每帧重绘以保持粒子顺滑。
  static uint32_t lastRender = 0;
  if (g_voice || g_forceRender || now - lastRender >= 33) { render(); lastRender = now; g_forceRender = false; }
  delay(g_voice ? 2 : 5);
}
