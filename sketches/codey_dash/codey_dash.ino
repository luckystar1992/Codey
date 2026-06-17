// codey_dash — real-time Claude Code / Codex agent-session monitor for the M5Stack StopWatch
// (circular 466x466 AMOLED). Consumes /codey/state from the companion and shows a three-tier UI
// per provider, navigated by touch (swipe/tap) + buttons + shake:
//   Usage  — provider edge arc (weekly%), animated mascot, cross-end waiting banner, usage/weekly
//            meters, model, session run/total, tok/min, wifi/battery.  (Claude / Codex, h-swipe)
//   List   — three-line session cards: status·name·branch / model·ctx%·tok·turn / task-or-summary.
//   Detail — big mascot + status, CTX/TURN/TOKENS tiles, token-four, ctx budget, task, git·ports.
// Touch: tap=drill in (banner→waiting session), h-swipe=switch provider / page session in detail,
//        swipe-up/down=descend/ascend tier (long list: page the scroll; swipe-down at top exits).
//        Buttons: BtnA short=back a tier / wake (cancel during voice), long=Settings;
//        BtnB hold=push-to-talk voice (release=submit). Shake=next provider. Completion chime via state.chime.
// True-black background, full-screen PSRAM canvas for flicker-free animation; VLW AA + efont CJK.

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
#include "fonts/jbmono_16.h"
#include "fonts/jbmono_20.h"
#include "fonts/jbmono_28.h"
#include "fonts/grotesk_20.h"
#include "fonts/grotesk_28.h"
#include "codey_theme.h"   // palette / SIZE·CX·CY / c565 / shade

// 触摸手势抽象动作(声明置顶:Arduino 生成的函数原型被提到 include 之后,需先见到此类型)
enum TouchAction { ACT_NONE, ACT_TAP, ACT_DOUBLE_TAP, ACT_SWIPE_L, ACT_SWIPE_R, ACT_SWIPE_UP, ACT_SWIPE_DOWN };

// (SessRef / collectSorted removed — dashboard replaced by per-provider usage pages)

// ---------- provider data ----------
static Prov PROV[2] = {
  { "Claude", COL_CLAUDE, 0, 0, 0, 0, 0, 0, 0, false, {}, 0 },
  { "Codex",  COL_CODEX,  0, 0, 0, 0, 0, 0, 0, false, {}, 0 },
};
static int g_batt = 76;             // refreshed from the real battery each second
static int g_clkH = 0, g_clkM = 0;  // refreshed from the on-board RTC each second

// companion 下发的显示配置(GET /codey/state -> "display");缺省全开 = 旧行为不变。
// 单写者:仅 netTask 在 fetchState() 里整体替换;主loop 渲染只读。结构是 POD,赋值是逐字段拷贝;
// 读到“半写”状态最坏只是某帧少/多一列(下帧即纠正),不会越界/崩溃,故沿用其它 g_* 状态的无锁单写模式。
static DispCfg g_disp = dispDefault();

// ---------- canvas + state ----------
static M5Canvas cv(&M5.Display);
static M5Canvas g_ringA(&M5.Display), g_ringB(&M5.Display);   // 仪表盘/列表各缓存一张 AA 环;每页 render 用函数内 static 记 pct 缓存键
static bool     g_ringAok = false, g_ringBok = false;
static M5Canvas g_voiceBg(&M5.Display);                       // 语音时缓存「暗化的 session 底页」,每帧只 blit+叠层(不重渲整页)→ 按键响应快
static bool     g_voiceBgOk = false;                          // sprite 是否分配成功(失败则退化为每帧重渲)
static bool     g_voiceBgDirty = true;                        // 需重建底图缓存(语音开始时置位)
static int      page = 0;
static uint32_t bootMs = 0;
static uint32_t lastSecMs = 0;
static volatile bool g_voice = false; // voice overlay active
static uint32_t g_voiceT0 = 0;       // millis() when the voice overlay started
static volatile bool g_wifi = false; // WiFi connected (主loop 写, netTask 读)
static char     g_ssid[48] = {0};    // connected SSID (bottom, marquee if long)
static bool     g_micOK = false;     // microphone available
static float    g_micLevel = 0.12f;  // smoothed mic level (0..1)
// ---- streaming voice: 小段双缓冲连续录音 -> StreamBuffer -> netTask sendBIN -> sherpa partials ----
// 关键:M5.Mic.record(buf,N) 要录满 N 才放槽;用一次大 N(20s)会让连续开停时第3次 record() 阻塞主loop,
// 且 end() 在录音中会死等当前槽录完。改为每段 32ms、双缓冲(始终2段在录),槽 ≤64ms 内释放 → 不卡不冻。
static int      g_vphase = 0;        // 0 off, 1 listening/streaming, 2 finalizing, 3 result
static volatile uint16_t g_voiceSeq = 0;   // 每次会话自增;只认当前 seq 的 stt(丢弃上次迟到结果)
static char     g_transcript[256] = {0};   // live/final transcript (netTask 写, 主loop 读)
static const int    REC_RATE = 16000;
static const size_t STREAM_CHUNK = 512;                        // 每段/每 WS 帧样本数(~32ms)
static int16_t  g_seg[2][STREAM_CHUNK];                        // 双缓冲小段录音(FIFO ping-pong)
static int      g_segHead = 0;                                 // 下一个待消费(队首)的段缓冲
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
static volatile bool g_netFocusReq = false;  // true=发 focus(详情页点屏切到此会话;主loop -> netTask)
static char          g_focusSid[40] = {0};   // 要切到的会话 id(主loop 写,netTask 读)
static char          g_voiceSid[40] = {0};   // 本轮语音的目标会话 id(详情页发起则非空;流式同步到它的 Mac 输入)
static volatile bool g_netReconnect = false; // reconfigWiFi 后让 netTask 重连
static volatile bool g_netPause = false;     // 门户运行时暂停 netTask 的 WiFi 操作(避免双核争用 WiFi 栈)
static volatile bool g_usbActive = false;     // USB 有线链路在线(netTask 写/读;在=优先 USB,不走网络)
static volatile uint32_t g_usbLastRx = 0;     // 最近一帧时间(ms);超时判离线
static volatile bool g_bootReady = false;     // setup 跑完置真;之前 USB 只认 HELLO_ACK,不处理 STATE/STT(避免早启核1崩→黑屏)
static StreamBufferHandle_t g_voiceSB = nullptr;  // 语音 PCM (主loop -> netTask)
static bool     g_rtcSynced = false;
// ---- 完成提示音(chime):companion 下发持久 {agent,seq};seq 增长 → 响一次 ----
static volatile uint32_t g_chimeSeq = 0;     // 最新完成事件 seq(netTask 写,主loop 读)
static volatile bool     g_chimeClaude = true;  // 该完成属 claude(880Hz)/codex(660Hz)
static uint32_t g_chimePlayed = 0;           // 主loop 已响过的 seq
static bool     g_chimeSynced = false;       // 首拉对齐:不为开机前的旧完成补响
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

#include "codey_mascot.h"   // anim(updateAnim/aBlink/aGlance)+ AA arc(drawArcRange/drawArc)+ Claude/Codex/drawMascot

#include "codey_widgets.h"   // header/dots/wifi/meter/tile-helpers/session-count/blitProviderArc + 状态/心情/格式

// ---- WebSocket streaming-ASR client ----
static void wsListen(bool start) {            // listen-control messages (xiaozhi-style)
  if (!g_wsConn) return;
  if (start) {
    char m[160];
    snprintf(m, sizeof(m),                          // 带会话序号 + 目标会话 id(流式同步到该会话的 Mac 输入)
             "{\"type\":\"listen\",\"state\":\"start\",\"mode\":\"manual\",\"seq\":%u,\"session\":\"%.48s\"}",
             (unsigned)g_voiceSeq, g_voiceSid);
    g_ws.sendTXT(m);
  } else {
    g_ws.sendTXT("{\"type\":\"listen\",\"state\":\"stop\"}");
  }
}

// 取消:让 companion 清掉已流式同步进目标会话输入框的文本
static void wsListenCancel() {
  if (!g_wsConn) return;
  g_ws.sendTXT("{\"type\":\"listen\",\"state\":\"cancel\"}");
}

// 详情页点屏「切到此会话」:把会话 id 发给 companion,由其切 macOS 终端 tab。
static void wsFocus(const char* sid) {
  if (!g_wsConn || !sid || !sid[0]) return;
  char m[80];
  snprintf(m, sizeof(m), "{\"type\":\"focus\",\"session\":\"%.48s\"}", sid);
  g_ws.sendTXT(m);
}

// USB LISTEN payload:与 wsListen(start) 同字段,去掉 "type"(companion 端补)。
static String listenStartJson() {
  String m = "{\"state\":\"start\",\"mode\":\"manual\",\"seq\":";
  m += (uint16_t)g_voiceSeq;
  m += ",\"session\":\""; m += g_voiceSid; m += "\"}";   // 与 wsListen 一致:session 字段恒在(空也发)
  return m;
}

// stt 落地(WS 与 USB 共用):seq 过滤陈旧会话 + 写 g_transcript + final 置位。
static void voiceApplyStt(const char* text, bool final, int seq, bool hasSeq) {
  if (hasSeq && (uint16_t)seq != g_voiceSeq) return;          // 丢弃陈旧会话的迟到 stt
  strncpy(g_transcript, text ? text : "", sizeof(g_transcript) - 1);
  g_transcript[sizeof(g_transcript) - 1] = '\0';
  if (final) g_sttFinal = true;
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
    if (strcmp(doc["type"] | "", "stt") == 0)
      voiceApplyStt(doc["text"] | "", doc["final"] | false, doc["seq"] | 0, doc["seq"].is<int>());
  }
}

// ---------- voice visualizer ----------
// 圆屏语音可视化:中心呼吸光球 + 一圈随麦克风电平起伏的径向声条(叠加行波 → 有机"声波环"灵动感)。
// vphase: 1 听写(provider 色,随电平灵动) / 2 识别中(琥珀,缓转、低幅) / 3 结果(平静收束)。
static void drawVoiceViz(int cx, int cy, float level, float t, uint32_t color, int vphase) {
  level = level < 0 ? 0 : (level > 1 ? 1 : level);
  const bool finalizing = (vphase == 2), result = (vphase == 3);
  uint32_t base = finalizing ? 0xFFB454 : color;                 // 识别中转琥珀
  const int N = 44; const float R0 = 60.0f, MAXLEN = 48.0f;
  float spin  = (finalizing ? 1.6f : 2.4f) * t;                  // 行波转速(识别中更缓)
  float drive = result ? 0.18f : (finalizing ? 0.32f : (0.30f + 0.70f * level));  // 整体幅度

  // 中心呼吸光球(3D 着色:暗底 + 左上高光)
  int coreR = (int)(18 + drive * 12);
  cv.fillSmoothCircle(cx, cy, coreR + 7, c565(shade(base, -0.66f)));
  cv.fillSmoothCircle(cx, cy, coreR,     c565(shade(base, result ? 0.15f : -0.04f)));
  cv.fillSmoothCircle(cx - coreR / 4, cy - coreR / 4, (int)(coreR * 0.55f), c565(shade(base, 0.38f)));

  // 径向声条:高度 = drive × 行波包络;越长越亮,端点加圆点更顺滑
  for (int i = 0; i < N; i++) {
    float a = (i / (float)N) * 2.0f * PI;
    float w = 0.5f + 0.34f * sinf(a * 4.0f + spin) + 0.16f * sinf(a * 7.0f - spin * 1.3f);   // ~[0,1] 行波
    float len = MAXLEN * drive * (0.30f + 0.70f * w); if (len < 2.0f) len = 2.0f;
    float cs = cosf(a), sn = sinf(a), r2 = R0 + len;
    uint16_t cc = c565(shade(base, -0.15f + (len / MAXLEN) * 0.6f));
    cv.drawLine(cx + (int)(R0 * cs), cy + (int)(R0 * sn), cx + (int)(r2 * cs), cy + (int)(r2 * sn), cc);
    cv.fillSmoothCircle(cx + (int)(r2 * cs), cy + (int)(r2 * sn), 2, c565(shade(base, 0.25f)));
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

// 纯 ASCII 串(无 >=0x80 字节)→ 可用 VLW 抗锯齿渲染;含 CJK → 用 efont 位图(CJK 无 VLW)。
static bool isPureAscii(const char* s) { for (; *s; ++s) if ((unsigned char)*s >= 0x80) return false; return true; }

// 转写文本:纯 Latin/数字走 VLW(Grotesk,抗锯齿),含中文走 efont。drawWrappedCJK 按当前字体测宽换行。
static void drawTranscript(const String& s, int cx, int cy, int maxW) {
  if (isPureAscii(s.c_str())) { cv.loadFont(Grotesk20); drawWrappedCJK(s, cx, cy, maxW, 28); cv.unloadFont(); }
  else                        { cv.setFont(&fonts::efontCN_24); drawWrappedCJK(s, cx, cy, maxW, 34); }
}

// 语音叠层元素(环/转写/状态):画在 render() 已铺好的「暗化底页」之上,不含压暗/底页。
static void drawVoiceOverlay() {
  const uint32_t color = PROV[page].color;
  const float t = (millis() - g_voiceT0) / 1000.0f;
  const bool hasText = g_transcript[0] != 0;
  const bool result  = (g_vphase == 3);
  const float amp = (g_vphase == 1) ? constrain(g_micLevel, 0.06f, 1.0f) : 0.16f;

  // 1) 就地「正在听」动画:圆屏径向声波环(随麦克风电平起伏 + 行波灵动)
  const int ry = hasText ? 296 : CY + 6;
  drawVoiceViz(CX, ry, (g_vphase == 1) ? amp : 0.0f, t, color, g_vphase);

  // 2) 流式转写(顶部):Latin VLW 抗锯齿 / CJK efont;warm 实时 → white 定稿
  if (hasText) {
    cv.setTextColor(c565(result ? 0xFFFFFF : 0xFFD27A));
    drawTranscript(String(g_transcript), CX, 100, 412);
    cv.drawFastHLine(CX - 130, 176, 260, c565(shade(color, -0.3f)));   // divider under transcript
  }

  // 3) 状态行(底部):● LISTENING/RECOGNIZING(VLW 抗锯齿)+ 提示
  if (!result) {
    const char* label = (g_vphase == 1) ? "LISTENING" : "RECOGNIZING";
    const uint32_t dotc = (g_vphase == 1) ? 0x3CCB7F : 0xFFC24A;   // green listening, amber finalizing
    int dots = ((int)(t * 2)) % 4;
    char title[24]; snprintf(title, sizeof(title), "%s%.*s", label, dots, "...");
    char full[24];  snprintf(full,  sizeof(full),  "%s...", label);   // fixed width -> no jiggle
    cv.loadFont(Grotesk20);
    int gw = 12 + 8 + cv.textWidth(full), sx = CX - gw / 2;           // center the ● + label group
    cv.fillSmoothCircle(sx + 6, 412, 6, c565(dotc));                  // status dot
    cv.setTextDatum(middle_left);
    cv.setTextColor(c565(shade(color, 0.25f)));
    cv.drawString(title, sx + 20, 412); cv.unloadFont();
    if (g_vphase == 1) {                                              // push-to-talk 提示(录音中)
      cv.setFont(&fonts::efontCN_16); cv.setTextDatum(middle_center); cv.setTextColor(c565(0x9498a0));
      cv.drawString("右键结束 · 左键取消", CX, 388);
    }
  } else {
    cv.setTextDatum(middle_center);
    cv.setFont(&fonts::efontCN_16); cv.setTextColor(c565(0x9498a0));
    cv.drawString("按右键关闭", CX, 420);
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
static const int ROW_H = 64, LIST_MAX_VIS = 4;   // 三行卡片行高;同屏最多行(超出则上下循环滚动)
// 列表渲染几何(渲染时写,rowHitAt 命中检测读 —— 两者一致)
static int  g_listBandTop = 0, g_listBandH = 0, g_listOff = 0, g_listVisN = 0;
static bool g_listAuto = false;
static int  g_listScroll = 0, g_listMaxScroll = 0;   // 用户可控滚动偏移(px)+ 上限;替代定时自动循环

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


#include "codey_usb.h"   // USB-CDC 有线兜底:帧编解码 + CRC16 + 收发状态机


#include "codey_pages.h"   // render() + drawWaitBanner + renderUsage/List/Detail(用 .ino 全局 + 前向声明)

#include "codey_portal.h"   // 开机/配网屏 + wifiAutoConnect + 自研配置门户(用 .ino 全局 + wifi_store)

// ---------- Arduino entry points ----------
static bool timeSane(time_t e) { return epochSane((long)e); }   // ~2023..2030, rejects garbage RTC(见 codey_ui.h)

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
  Serial.setTxTimeoutMs(0);                     // companion 没在读时写不阻塞 netTask(关键)
  g_usbTxMtx = xSemaphoreCreateMutex();
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
  g_voiceBg.setColorDepth(16); g_voiceBg.setPsram(true); g_voiceBgOk = (g_voiceBg.createSprite(SIZE, SIZE) != nullptr);
  Serial.printf("ring sprites: %d %d  voiceBg: %d\n", g_ringAok, g_ringBok, g_voiceBgOk);

  M5.Speaker.end();                       // free the shared codec for the mic
  g_micOK = M5.Mic.begin();               // 录音用双缓冲小段 g_seg[2](静态,无需 PSRAM 大缓冲)
  Serial.printf("Mic begin=%d  IMU enabled=%d\n", g_micOK, M5.Imu.isEnabled());

  // ---- 免配网 USB 启动:仅当 CDC 已连主机才握手(否则普通 WiFi 启动不空等 1.5s);在线则跳过配网门户 ----
  if (Serial) for (int i = 0; i < 30 && !g_usbActive; i++) { usbRxPump(); usbSendFrame(U_HELLO, (const uint8_t*)"v1", 2); delay(50); }

  // ---- WiFi: 多网络自动连接(记忆历史) -> 都失败则自研配置门户 ----
  wifiStoreLoad(g_prefs);                          // 历史网络(SSID/密码/连接次数),按 count 降序
  g_wifi = wifiAutoConnect();                      // 扫描周边 + 按连接次数依次尝试记住的网络
  if (!g_wifi && !g_usbActive) g_wifi = wifiConfigPortal();  // 都连不上且无 USB → 提示用 web 配置新网络
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
  g_bootReady = true;                  // 主循环就绪 → USB 才开始处理 STATE/STT(此前只认 HELLO_ACK)
}

#include "codey_net.h"   // copyStr/parseSession/fetchState/netTask(用 .ino 全局 + readClock/resolveMac/wsConnect/wsListen/maybeRepointWs)

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

// 任务完成提示音:与麦克风共享 ES8311 编解码器 —— 先停麦,响一段双音 chime,再恢复麦。
// 仅在非语音态(麦空闲)调用;g_volume=0 静音。Claude 高八度,Codex 低八度。
static void playChime(bool claude) {
  if (g_volume == 0) return;
  bool micWas = g_micOK;
  if (micWas) M5.Mic.end();                       // 释放编解码器给扬声器
  M5.Speaker.begin();
  M5.Speaker.setVolume((uint8_t)map(g_volume, 0, 100, 40, 255));
  M5.Speaker.tone(claude ? 880 : 660, 110); delay(130);   // tone 非阻塞,delay 让其播完
  M5.Speaker.tone(claude ? 1320 : 990, 110); delay(130);
  M5.Speaker.end();
  if (micWas) g_micOK = M5.Mic.begin();           // 恢复麦克风(下次语音用)
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
  int idx = (g_listOff + rel) / ROW_H;            // g_listOff = 可控滚动偏移(静态时为 0)
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
  const int LIST_PAGE = (LIST_MAX_VIS - 1) * ROW_H;   // 每次翻页滚动行数(留 1 行重叠做上下文)
  switch (a) {
    case ACT_SWIPE_L: case ACT_SWIPE_R: {            // 横滑:详情翻会话,否则切端
      int dir = (a == ACT_SWIPE_L) ? 1 : -1;
      if (detailProv >= 0) { int n = PROV[detailProv].nsess; if (n) detailIdx = (detailIdx + dir + n) % n; }
      else { page = nextProv(page, dir); g_listScroll = 0; }   // 切端:跳过禁用页 + 重置滚动
      break;
    }
    case ACT_SWIPE_UP:                               // 上滑:主页→列表;列表内→向下翻页
      if (detailProv >= 0) break;
      if (!g_listView) { g_listView = true; g_listScroll = 0; }
      else if (g_listAuto) g_listScroll = min(g_listScroll + LIST_PAGE, g_listMaxScroll);
      break;
    case ACT_SWIPE_DOWN:                             // 下滑:详情→列表;列表内有偏移→上翻,到顶→回主页
      if (detailProv >= 0) { exitDetail(); break; }  // 详情 → 列表
      if (g_listView) {
        if (g_listScroll > 0) g_listScroll = max(0, g_listScroll - LIST_PAGE);   // 列表向上翻页
        else g_listView = false;                     // 已到顶 → 列表 → 主页
      }
      break;
    case ACT_TAP:                                    // 单击:详情→切 Mac 终端 tab;主页→列表;列表点行→详情
      if (detailProv >= 0) {                          // 详情页点屏:确认切到此会话的 macOS 终端 tab
        const Prov& p = PROV[detailProv];
        if (detailIdx >= 0 && detailIdx < p.nsess && p.sess[detailIdx].id[0]) {
          strncpy(g_focusSid, p.sess[detailIdx].id, sizeof(g_focusSid) - 1);
          g_focusSid[sizeof(g_focusSid) - 1] = 0;
          g_netFocusReq = true;                        // netTask 发 focus
        }
        break;
      }
      if (!g_listView && g_bannerProv >= 0 &&
          g_tLastY >= g_bannerTop && g_tLastY <= g_bannerTop + g_bannerH) {
        enterDetail(g_bannerProv, g_bannerIdx); break;
      }
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

// 长按 BtnA:进入设置页
static void btnALong() {
  g_inSettings = true;
  g_setSel = 0;
}
// 短按 BtnA:息屏亮屏;否则逐级返回
static void btnAShort() {
  if (g_dim) { g_dim = false; M5.Display.setBrightness(g_bright); return; }
  if (detailProv >= 0) { exitDetail(); return; }      // 详情 → 列表
  if (g_listView) { g_listView = false; return; }     // 列表 → 主页
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
      if (g_inSettings) { if (g_voiceSid[0]) g_netListenReq = 3; g_voice = false; g_vphase = 0; g_voiceSeq++; } else bootMs = millis();  // 进设置=放弃语音(targeted 则发 cancel 清残留),推进 seq
    }
  } else {
    bothSince = 0; bothFired = false;
    if (g_inSettings) {
      settingsButtons();                                   // BtnA = down, BtnB = confirm
    } else if (!g_voice && M5.BtnB.wasPressed() && !M5.BtnA.isPressed()) {   // 按一下右键 → 开录(异步 toggle,免按住)
      g_voice = true; g_voiceT0 = now; g_micLevel = 0.12f; g_voiceSeq++;     // 新会话序号(隔离上次迟到结果)
      g_transcript[0] = 0; g_heardSpeech = false; g_silenceT0 = 0; g_noiseFloor = 0.06f;
      g_finalReqT0 = 0; g_sttFinal = false;
      g_voiceBgDirty = true;                               // 重建语音底图缓存(当前 session 页)
      // 目标会话:详情页发起 → 取当前会话 id(流式同步进它的 Mac 终端输入);否则空(回退:停录粘贴到前台)
      if (detailProv >= 0 && detailIdx >= 0 && detailIdx < PROV[detailProv].nsess)
        { strncpy(g_voiceSid, PROV[detailProv].sess[detailIdx].id, sizeof(g_voiceSid) - 1); g_voiceSid[sizeof(g_voiceSid) - 1] = 0; }
      else g_voiceSid[0] = 0;
      if (g_micOK && g_wsConn) {
        g_vphase = 1;
        if (g_voiceSB) xStreamBufferReset(g_voiceSB);
        g_netListenReq = 1;                                // netTask -> listen:start
        g_segHead = 0;                                     // 双缓冲:先排 2 段,始终保持 2 段在录(无缝)
        M5.Mic.record(g_seg[0], STREAM_CHUNK, REC_RATE);
        M5.Mic.record(g_seg[1], STREAM_CHUNK, REC_RATE);
        Serial.println("[voice] streaming (seg x2)");
      } else {                                             // can't stream -> show why
        g_vphase = 3; g_resultT0 = now;
        strncpy(g_transcript, !g_micOK ? "麦克风不可用" : "语音服务未连接", sizeof(g_transcript) - 1);
        g_transcript[sizeof(g_transcript) - 1] = 0;
      }
    } else if (g_voice && M5.BtnA.wasPressed()) {                            // 左键:任意阶段立即退出(全程可打断)
      if (g_vphase != 3 && g_voiceSid[0]) g_netListenReq = 3;   // 录音/识别中取消 → 清已同步进输入框的残留;结果态只关
      g_voice = false; g_vphase = 0; g_voiceSeq++;
      Serial.println("[voice] aborted (BtnA)");
    } else if (g_voice && g_vphase == 1 && M5.BtnB.wasPressed()) {           // 录音中:右键 → 停录识别
      if (now - g_voiceT0 > 250) { g_vphase = 2; g_finalReqT0 = 0; }         // <250ms 视为起录抖动,忽略;在录尾段任其自然完成
    } else if (g_voice && g_vphase == 2 && M5.BtnB.wasPressed()) {           // 识别中:右键 → 放弃等待立即退出(不卡 2.5s)
      if (g_voiceSid[0]) g_netListenReq = 3;
      g_voice = false; g_vphase = 0; g_voiceSeq++;
      Serial.println("[voice] give up finalize (BtnB)");
    } else if (g_voice && g_vphase == 3 && M5.BtnB.wasPressed()) {           // 结果/错误:右键 → 关掉(保留定稿)
      g_voice = false; g_vphase = 0;
    } else if (!g_voice && !M5.BtnB.isPressed()) {                           // 左键:短按逐级返回,长按设置
      static uint32_t aDownAt = 0; static bool aLong = false;
      if (!M5.BtnA.isPressed() && !M5.BtnA.wasReleased()) { aDownAt = 0; aLong = false; }  // 空闲重置:防按键被 BtnB 打断后残留长按态
      if (M5.BtnA.wasPressed()) { aDownAt = now; aLong = false; }
      if (M5.BtnA.isPressed() && !aLong && aDownAt && now - aDownAt > 550) { aLong = true; btnALong(); }
      if (M5.BtnA.wasReleased()) { if (!aLong) btnAShort(); aDownAt = 0; }
    }
  }

  if (g_inSettings) { renderSettings(); delay(16); return; }   // settings page owns the screen

  // ---- streaming voice: 小段双缓冲连续录音 → 每段录满即发 WS(server partials arrive in wsEvent) ----
  if (g_voice) {
    if (g_vphase == 1) {                                  // LISTENING:消费已录满的段 + 立刻补录(保持 2 段在录)
      int guard = 0;
      while (g_micOK && M5.Mic.isRecording() < 2 && guard++ < 4) {   // 有空闲槽 = 队首段 g_seg[g_segHead] 已录满
        int16_t* seg = g_seg[g_segHead];
        double s = 0; for (size_t i = 0; i < STREAM_CHUNK; i++) { double v = seg[i]; s += v * v; }
        float lvl = sqrtf(s / STREAM_CHUNK) / 2500.0f;    // VAD level + adaptive floor
        g_micLevel += (lvl - g_micLevel) * 0.4f;
        g_noiseFloor += (g_micLevel - g_noiseFloor) * (g_micLevel < g_noiseFloor ? 0.2f : 0.01f);
        if (g_micLevel > g_noiseFloor + 0.12f) g_heardSpeech = true;
        if (g_voiceSB) xStreamBufferSend(g_voiceSB, (uint8_t*)seg, STREAM_CHUNK * 2, 0);   // -> netTask
        if (!M5.Mic.record(g_seg[g_segHead], STREAM_CHUNK, REC_RATE)) break;               // 补回该槽
        g_segHead ^= 1;
      }
      if (now - g_voiceT0 > 20000) { g_vphase = 2; g_finalReqT0 = 0; }    // 20s 上限兜底(停录由按键触发)
    } else if (g_vphase == 2) {                           // FINALIZE:请求 final 并等待(在录尾段任其自然完成放槽)
      if (g_finalReqT0 == 0) { g_netListenReq = 2; g_finalReqT0 = now; }  // netTask -> listen:stop
      if (g_sttFinal || now - g_finalReqT0 > 2500) {                     // 拿到 final(或 2.5s 超时,可按键提前打断)
        if (g_transcript[0] == 0) { strncpy(g_transcript, "(没听清)", sizeof(g_transcript) - 1); g_transcript[sizeof(g_transcript) - 1] = 0; }
        g_vphase = 3; g_resultT0 = now;
      }
    } else if (g_vphase == 3) {                           // RESULT
      if (now - g_resultT0 > 9000) { g_voice = false; g_vphase = 0; }
    }
  }

  // ---- 完成提示音:companion 的 chime.seq 增长时响一次(开机首拉对齐,不补开机前的旧完成) ----
  if (!g_voice && g_haveData) {
    uint32_t cseq = g_chimeSeq;
    if (!g_chimeSynced) { g_chimePlayed = cseq; g_chimeSynced = true; }
    else if (cseq != g_chimePlayed) { g_chimePlayed = cseq; playChime(g_chimeClaude); lastActiveMs = now; }
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
  if (g_forceRender || now - lastRender >= 33) { render(); lastRender = now; g_forceRender = false; }   // 语音底图已缓存,无需强制每帧重渲;~30fps + 快循环查按键
  delay(g_voice ? 2 : 5);
}
