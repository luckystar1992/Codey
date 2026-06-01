#pragma once
// 多 WiFi 记忆:历史网络(SSID + 密码 + 连接次数),NVS 持久化,按连接次数降序。
// 单 sketch = 单编译单元,故 static 全局安全。被 codey_dash.ino include。
// 数据流:wifiStoreLoad(开机) -> wifiStoreTouch(每次连上 count++/新增,满则淘汰最少用的)
//        -> wifiStoreRemove(门户删除) -> wifiStoreSave(每次变更落盘)。
#include <Arduino.h>
#include <Preferences.h>
#include <ArduinoJson.h>

static const int WIFI_MAX_NETS = 10;            // 历史列表上限

struct WifiNet {
  char     ssid[33];
  char     pass[65];
  uint16_t count;                               // 成功连接次数(排序键)
};

static WifiNet g_nets[WIFI_MAX_NETS];
static int     g_netCount = 0;

static void wifiStoreSort() {                   // 按 count 降序(插入排序,n<=10)
  for (int i = 1; i < g_netCount; i++) {
    WifiNet key = g_nets[i]; int j = i - 1;
    while (j >= 0 && g_nets[j].count < key.count) { g_nets[j + 1] = g_nets[j]; j--; }
    g_nets[j + 1] = key;
  }
}

static int wifiStoreFind(const char* ssid) {
  for (int i = 0; i < g_netCount; i++) if (strcmp(g_nets[i].ssid, ssid) == 0) return i;
  return -1;
}

static void wifiStoreLoad(Preferences& p) {
  g_netCount = 0;
  String js = p.getString("nets", "[]");
  JsonDocument doc;
  if (deserializeJson(doc, js)) return;
  for (JsonObject o : doc.as<JsonArray>()) {
    if (g_netCount >= WIFI_MAX_NETS) break;
    const char* s = o["s"] | "";
    if (!s[0]) continue;
    WifiNet& n = g_nets[g_netCount++];
    strncpy(n.ssid, s,            sizeof(n.ssid) - 1); n.ssid[sizeof(n.ssid) - 1] = 0;
    strncpy(n.pass, o["p"] | "",  sizeof(n.pass) - 1); n.pass[sizeof(n.pass) - 1] = 0;
    n.count = o["c"] | 1;
  }
  wifiStoreSort();
}

static void wifiStoreSave(Preferences& p) {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < g_netCount; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["s"] = g_nets[i].ssid; o["p"] = g_nets[i].pass; o["c"] = g_nets[i].count;
  }
  String js; serializeJson(doc, js);
  p.putString("nets", js);
}

// 成功连接后调用:已存在 -> count++ & 更新密码;不存在 -> 新增(满则淘汰 count 最低的)
static void wifiStoreTouch(Preferences& p, const char* ssid, const char* pass) {
  int idx = wifiStoreFind(ssid);
  if (idx >= 0) {
    if (g_nets[idx].count < 65535) g_nets[idx].count++;
    if (pass && pass[0]) { strncpy(g_nets[idx].pass, pass, sizeof(g_nets[idx].pass) - 1); g_nets[idx].pass[sizeof(g_nets[idx].pass) - 1] = 0; }
  } else {
    if (g_netCount >= WIFI_MAX_NETS) { wifiStoreSort(); idx = g_netCount - 1; }   // 覆盖 count 最低的
    else idx = g_netCount++;
    WifiNet& n = g_nets[idx];
    strncpy(n.ssid, ssid,             sizeof(n.ssid) - 1); n.ssid[sizeof(n.ssid) - 1] = 0;
    strncpy(n.pass, pass ? pass : "", sizeof(n.pass) - 1); n.pass[sizeof(n.pass) - 1] = 0;
    n.count = 1;
  }
  wifiStoreSort();
  wifiStoreSave(p);
}

static void wifiStoreRemove(Preferences& p, const char* ssid) {
  int idx = wifiStoreFind(ssid);
  if (idx < 0) return;
  for (int i = idx; i < g_netCount - 1; i++) g_nets[i] = g_nets[i + 1];
  g_netCount--;
  wifiStoreSave(p);
}

// 历史里的密码(门户「一键连」用);无则返回 nullptr
static const char* wifiStorePass(const char* ssid) {
  int idx = wifiStoreFind(ssid);
  return idx >= 0 ? g_nets[idx].pass : nullptr;
}
