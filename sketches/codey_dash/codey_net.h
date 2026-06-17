// sketches/codey_dash/codey_net.h — Companion state fetch/parse + core0 netTask (HTTP/WS/语音上行).
// 单 TU 习惯:netTask 的前向声明在 setup() 前;其调用的 readClock/resolveMac/wsConnect/maybeRepointWs/
// wsListen 在本头 #include 之前已定义;读写 .ino 的网络/会话全局。
#pragma once
#include <M5Unified.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <sys/time.h>
#include "codey_ui.h"
#include "session_store.h"

// voiceApplyStt 定义在 codey_dash.ino(#include "codey_net.h" 之前),此处仅为让 usbOnFrame 可见。
static void voiceApplyStt(const char* text, bool final, int seq, bool hasSeq);

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
  s.tokIn     = so["tokens"]["in"]      | 0L;
  s.tokOut    = so["tokens"]["out"]     | 0L;
  s.tokCacheR = so["tokens"]["cache_r"] | 0L;
  s.tokCacheW = so["tokens"]["cache_w"] | 0L;
  s.compactions = so["compactions"]     | 0;
  copyStr(s.summary, sizeof(s.summary), so["summary"] | "");
  s.turn     = so["turn"]           | 0;
  s.added    = so["git"]["added"]    | 0;
  s.modified = so["git"]["modified"] | 0;
  s.subagents= so["subagents"]      | 0;
  s.startedAt= so["started_at"]     | 0L;
  s.memKb    = so["memory"]         | 0L;
  s.nports = 0;
  for (JsonVariant pv : so["ports"].as<JsonArray>()) { if (s.nports < MAX_PORTS) s.ports[s.nports++] = pv.as<int>(); }
}

// state doc 解析落地(HTTP fetchState 与 USB U_STATE 共用):从 doc 更新所有全局显示/会话状态。
static void applyStateDoc(JsonDocument& doc) {
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
      d.col[DC_SUMMARY] = cols["summary"] | true;
      d.col[DC_BRANCH]  = cols["branch"]  | true;
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
    PROV[i].limited   = pr["limited"] | false;
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
  { JsonVariantConst ch = doc["chime"];               // 持久最新完成 {agent,seq};主loop 据 seq 增量响铃
    if (!ch.isNull()) { g_chimeSeq = ch["seq"] | 0; g_chimeClaude = (strcmp(ch["agent"] | "", "codex") != 0); } }
  if (detailProv >= 0 && detailIdx >= PROV[detailProv].nsess) detailProv = -1;
  long ts = doc["ts"] | 0L;                      // Mac epoch -> set the device clock (NTP-independent)
  if (epochSane(ts)) {
    struct timeval tv; tv.tv_sec = (time_t)ts; tv.tv_usec = 0; settimeofday(&tv, nullptr);
    readClock();
  }
  Serial.printf("[state] claude %d/%d  codex %d/%d  stale=%d\n",
                PROV[0].sessUsed, PROV[0].weekUsed, PROV[1].sessUsed, PROV[1].weekUsed, g_stale);
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
      applyStateDoc(doc);
      g_haveData = true; ok = true;
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

// USB 入站帧落地:更新在线时戳;HELLO_ACK→标在线;STATE→复用 applyStateDoc;STT→复用 voiceApplyStt。
static void usbOnFrame(uint8_t type, const uint8_t* payload, uint16_t len) {
  g_usbLastRx = millis();
  if (type == U_HELLO_ACK) { g_usbActive = true; g_companionOk = true; return; }
  if (!g_bootReady) return;        // 启动握手窗口内只认 HELLO_ACK;STATE/STT 等主循环就绪再处理(防早启核1崩→黑屏)
  if (type == U_STATE) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, len)) return;          // 解析失败丢弃(与 STT 同风格)
    applyStateDoc(doc); g_haveData = true;
  } else if (type == U_STT) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, len)) return;
    voiceApplyStt(doc["text"] | "", doc["final"] | false, doc["seq"] | 0, doc["seq"].is<int>());
  }
}

// ---- 300ms 音频切分:netTask 把 32ms 片段累积成 300ms 整块再发(WS/USB 共用)----
static const size_t CHUNK_BYTES = 9600;        // 300ms @ 16k/mono/int16 = 4800 样本 ×2
static uint8_t g_sendBuf[CHUNK_BYTES];         // 静态(不压 netTask 栈)
static size_t  g_sendLen = 0;

typedef void (*PcmSink)(const uint8_t*, size_t);
static void sinkUsb(const uint8_t* p, size_t n) { usbSendFrame(U_PCM, p, (uint16_t)n); }
static void sinkWs (const uint8_t* p, size_t n) { g_ws.sendBIN((uint8_t*)p, n); }

static void pcmAccum(const uint8_t* seg, size_t segN, PcmSink sink) {  // 累积满 300ms 即整块发
  size_t off = 0;
  while (off < segN) {
    size_t take = CHUNK_BYTES - g_sendLen; if (take > segN - off) take = segN - off;
    memcpy(g_sendBuf + g_sendLen, seg + off, take); g_sendLen += take; off += take;
    if (g_sendLen == CHUNK_BYTES) { sink(g_sendBuf, CHUNK_BYTES); g_sendLen = 0; }
  }
}
static void pcmFlush(PcmSink sink) { if (g_sendLen) { sink(g_sendBuf, g_sendLen); g_sendLen = 0; } }  // 停录发尾巴

// 所有阻塞网络 IO 都在这里(core 0):WS 维護/连接、HTTP fetch、mDNS、语音上行。主loop 永不等待。
static const uint32_t USB_ACTIVE_TIMEOUT_MS = 4000;   // 连续无帧超此值 → 判 USB 离线,回落 WiFi
static const uint32_t USB_PROBE_INTERVAL_MS = 500;    // 离线探针节流(~2/s),避免刷屏 serial / 抢 mutex

static void netTask(void*) {
  bool started = false;
  uint32_t lastFetch = 0, lastProbe = 0;
  for (;;) {
    if (g_netPause) { started = false; vTaskDelay(20 / portTICK_PERIOD_MS); continue; }  // 门户接管 WiFi:让路(回来后重连)

    usbRxPump();
    if (g_usbActive && millis() - g_usbLastRx > USB_ACTIVE_TIMEOUT_MS) g_usbActive = false;   // 超时回落 WiFi
    if (!g_usbActive && Serial && millis() - lastProbe >= USB_PROBE_INTERVAL_MS) {            // 仅 CDC 已连主机才节流探针
      lastProbe = millis(); usbSendFrame(U_HELLO, (const uint8_t*)"v1", 2);                   // 找 companion(在=优先 USB)
    }

    if (g_usbActive) {
      // —— USB 分支:不走网络;PCM 按 300ms 累积成块再发 ——
      uint8_t buf[1024]; size_t n;                                   // 先排空已录 PCM(满 300ms 即发)
      while ((n = xStreamBufferReceive(g_voiceSB, buf, sizeof(buf), 0)) > 0) pcmAccum(buf, n, sinkUsb);
      if (g_netListenReq == 1) {
        g_netListenReq = 0; g_sendLen = 0;                          // 新会话:丢上次残留累积
        String j = listenStartJson();
        usbSendFrame(U_LISTEN, (const uint8_t*)j.c_str(), (uint16_t)j.length());
      } else if (g_netListenReq == 2) {
        g_netListenReq = 0; pcmFlush(sinkUsb);                      // 停录:先 flush 尾巴(不丢尾音)
        const char* s = "{\"state\":\"stop\"}";
        usbSendFrame(U_LISTEN, (const uint8_t*)s, (uint16_t)strlen(s));
      } else if (g_netListenReq == 3) {
        g_netListenReq = 0; g_sendLen = 0;
        const char* s = "{\"state\":\"cancel\"}";
        usbSendFrame(U_LISTEN, (const uint8_t*)s, (uint16_t)strlen(s));
      }
      if (g_netFocusReq) { g_netFocusReq = false; usbSendFrame(U_FOCUS, (const uint8_t*)g_focusSid, (uint16_t)strlen(g_focusSid)); }
    } else if (g_wifi) {
      // —— 现有 WiFi 分支:原样 ——
      if (!started || g_netReconnect) { g_netReconnect = false; resolveMac(); wsConnect(); started = true; lastFetch = 0; }
      g_ws.loop();                                  // WS 维护(connect 阻塞只在本任务,不卡渲染)
      uint8_t buf[1024]; size_t n;                  // 转发主loop采集的语音 PCM(按 300ms 累积成块)
      while (g_wsConn && (n = xStreamBufferReceive(g_voiceSB, buf, sizeof(buf), 0)) > 0) pcmAccum(buf, n, sinkWs);
      if (g_netListenReq == 1)      { g_netListenReq = 0; g_sendLen = 0; wsListen(true); }
      else if (g_netListenReq == 2) { g_netListenReq = 0; pcmFlush(sinkWs); wsListen(false); }   // 停录 flush 尾巴
      else if (g_netListenReq == 3) { g_netListenReq = 0; g_sendLen = 0; wsListenCancel(); }     // 取消:清同步进输入框的文本
      if (g_netFocusReq)            { g_netFocusReq = false; wsFocus(g_focusSid); }   // 切 macOS 终端 tab
      uint32_t now = millis();                      // 定时拉 usage(语音时让路)
      if (!g_voice && (lastFetch == 0 || now - lastFetch > 30000)) { lastFetch = now; fetchState(); maybeRepointWs(); }
    } else { started = false; }
    vTaskDelay(5 / portTICK_PERIOD_MS);
  }
}
