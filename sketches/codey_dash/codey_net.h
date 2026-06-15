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
      g_haveData = true; ok = true;
      { JsonVariantConst ch = doc["chime"];               // 持久最新完成 {agent,seq};主loop 据 seq 增量响铃
        if (!ch.isNull()) { g_chimeSeq = ch["seq"] | 0; g_chimeClaude = (strcmp(ch["agent"] | "", "codex") != 0); } }
      if (detailProv >= 0 && detailIdx >= PROV[detailProv].nsess) detailProv = -1;
      long ts = doc["ts"] | 0L;                      // Mac epoch -> set the device clock (NTP-independent)
      if (epochSane(ts)) {
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
      else if (g_netListenReq == 3) { g_netListenReq = 0; wsListenCancel(); }          // 取消:清同步进输入框的文本
      if (g_netFocusReq)            { g_netFocusReq = false; wsFocus(g_focusSid); }   // 切 macOS 终端 tab
      uint8_t buf[1024]; size_t n;                  // 转发主loop采集的语音 PCM
      while (g_wsConn && (n = xStreamBufferReceive(g_voiceSB, buf, sizeof(buf), 0)) > 0) g_ws.sendBIN(buf, n);
      uint32_t now = millis();                      // 定时拉 usage(语音时让路)
      if (!g_voice && (lastFetch == 0 || now - lastFetch > 30000)) { lastFetch = now; fetchState(); maybeRepointWs(); }
    } else { started = false; }
    vTaskDelay(5 / portTICK_PERIOD_MS);
  }
}
