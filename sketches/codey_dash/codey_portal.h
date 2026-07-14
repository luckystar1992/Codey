// sketches/codey_dash/codey_portal.h — boot/setup screens + WiFi auto-connect + captive config portal.
// 单 TU 习惯:依赖 .ino 的全局(cv/g_prefs/g_manualMac/g_remoteHost/g_remoteAuthRaw/g_netPause)与
// wifi_store.h 的历史网络 API(g_nets/g_netCount/wifiStoreTouch/Remove/Pass)—— 本头在它们之后 #include。
#pragma once
#include <M5Unified.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ArduinoJson.h>
#include <qrcode.h>          // ESP-IDF 自带的 espressif/qrcode 组件(m5stack:esp32 core 已捆绑,免装外部库)
#include "codey_theme.h"
#include "wifi_store.h"

// 门户开机画面画 WIFI: 二维码,扫码直连热点。热点是开放网络(见 wifiConfigPortal 的 softAP(...)
// 无密码),故 QR 里 T=nopass。esp_qrcode_generate 是同步回调 API(编码完就调 display_func),
// 用两个文件作用域变量把目标画布参数带进回调——单次调用、非重入,不需要线程安全。
static int g_qrPixelSize = 0, g_qrCenterY = 0;

static void qrDisplayCb(esp_qrcode_handle_t qrcode) {
  int n = esp_qrcode_get_size(qrcode);
  int scale = max(1, g_qrPixelSize / n);
  int actual = scale * n;
  int x0 = CX - actual / 2, y0 = g_qrCenterY - actual / 2;

  cv.fillRoundRect(x0 - 10, y0 - 10, actual + 20, actual + 20, 8, c565(COL_WHITE));
  for (int y = 0; y < n; y++) for (int x = 0; x < n; x++)
    if (esp_qrcode_get_module(qrcode, x, y)) cv.fillRect(x0 + x * scale, y0 + y * scale, scale, scale, c565(0x000000));
}

static void drawWifiQr(const char* ssid, int qrCenterY, int qrPixelSize) {
  g_qrCenterY = qrCenterY; g_qrPixelSize = qrPixelSize;
  String payload = String("WIFI:T:nopass;S:") + ssid + ";P:;;";
  esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
  cfg.display_func = qrDisplayCb;
  cfg.max_qrcode_version = 4;                          // 定长 payload,4 够用且留余量
  esp_qrcode_generate(&cfg, payload.c_str());
}

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

static void showPortalScreen(const char* ip) {            // 全部连不上 -> 提示扫码/浏览器配置新网络
  cv.fillSprite(c565(0x000000));
  cv.setTextDatum(middle_center);
  cv.setFont(&fonts::efontCN_16); cv.setTextColor(c565(COL_CODEX));
  cv.drawString("扫码连接配网热点", CX, CY - 158);

  drawWifiQr("Codey-Setup", CY - 8, 150);                  // 圆屏安全区内:QR 居中,上下留字

  cv.setFont(&fonts::FreeSansBold12pt7b); cv.setTextColor(c565(COL_WHITE));
  cv.drawString("Codey-Setup", CX, CY + 96);
  cv.setFont(&fonts::efontCN_16); cv.setTextColor(c565(0xC8C8C8));
  cv.drawString("或浏览器打开", CX, CY + 124);
  cv.setFont(&fonts::FreeSansBold9pt7b); cv.setTextColor(c565(COL_WHITE));
  cv.drawString(ip, CX, CY + 148);
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
