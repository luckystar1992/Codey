// sketches/codey_dash/codey_usb.h — USB-CDC 有线兜底:帧 C0 DE|type|len(2 LE)|payload|crc16(2 LE)。
// 与日志共用单路 HWCDC;发送加锁减少与 println 交错,接收用增量状态机。companion 端 codey/usb_frames.py 同构。
#pragma once
#include <Arduino.h>              // uint8_t/size_t/Serial(与同目录其他头一致,自含依赖)
#include <freertos/semphr.h>      // SemaphoreHandle_t / xSemaphore*

static const uint8_t USB_M0 = 0xC0, USB_M1 = 0xDE;
enum { U_HELLO = 0x01, U_STATE_REQ = 0x10, U_LISTEN = 0x20, U_PCM = 0x21, U_FOCUS = 0x30,
       U_HELLO_ACK = 0x81, U_STATE = 0x90, U_STT = 0xA0 };

static SemaphoreHandle_t g_usbTxMtx = nullptr;   // setup 里 create;序列化整帧写出

static uint16_t usbCrc16Upd(uint16_t c, const uint8_t* d, size_t n) {
  for (size_t i = 0; i < n; i++) { c ^= (uint16_t)d[i] << 8;
    for (int k = 0; k < 8; k++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1); }
  return c;
}

static const uint16_t USB_MAX_PAYLOAD = 2200;  // g_rxBuf 大小;TX/RX 对称上限

static void usbSendFrame(uint8_t type, const uint8_t* p, uint16_t n) {
  if (n > USB_MAX_PAYLOAD) return;        // TX/RX 对称:payload 不得超过接收缓冲
  uint8_t head[5] = { USB_M0, USB_M1, type, (uint8_t)(n & 0xFF), (uint8_t)(n >> 8) };
  uint16_t c = usbCrc16Upd(0xFFFF, head + 2, 3);        // crc over type+len
  c = usbCrc16Upd(c, p, n);                             //          +payload
  uint8_t tail[2] = { (uint8_t)(c & 0xFF), (uint8_t)(c >> 8) };
  if (g_usbTxMtx) xSemaphoreTake(g_usbTxMtx, portMAX_DELAY);
  Serial.write(head, 5); if (n) Serial.write(p, n); Serial.write(tail, 2);
  if (g_usbTxMtx) xSemaphoreGive(g_usbTxMtx);
}

// 增量接收状态机:每收到一整帧调 usbOnFrame()(Task 5 在 codey_net.h 里给出完整定义)。
static void usbOnFrame(uint8_t type, const uint8_t* payload, uint16_t len);   // fwd decl

static uint8_t  g_rxStage = 0;            // 0=找C0 1=见C0等DE 2=收头 3=收体+crc
static uint8_t  g_rxHdr[3];               // type, lenLo, lenHi
static uint8_t  g_rxHdrGot = 0;
static uint16_t g_rxLen = 0, g_rxGot = 0;
static uint8_t  g_rxBuf[2200];            // payload 上限(STATE json ~1-2KB)
static uint8_t  g_rxCrc[2]; static uint8_t g_rxCrcGot = 0;

static void usbRxByte(uint8_t b) {
  switch (g_rxStage) {
    case 0: if (b == USB_M0) g_rxStage = 1; break;
    case 1: g_rxStage = (b == USB_M1) ? 2 : (b == USB_M0 ? 1 : 0); g_rxHdrGot = 0; break;
    case 2:
      g_rxHdr[g_rxHdrGot++] = b;
      if (g_rxHdrGot == 3) {
        g_rxLen = g_rxHdr[1] | (g_rxHdr[2] << 8);
        if (g_rxLen > sizeof(g_rxBuf)) { g_rxStage = 0; break; }   // 超界丢弃,重同步
        g_rxGot = 0; g_rxCrcGot = 0; g_rxStage = 3;
      }
      break;
    case 3:
      if (g_rxGot < g_rxLen) { g_rxBuf[g_rxGot++] = b; break; }
      g_rxCrc[g_rxCrcGot++] = b;
      if (g_rxCrcGot == 2) {
        uint16_t want = g_rxCrc[0] | (g_rxCrc[1] << 8);
        uint16_t got = usbCrc16Upd(0xFFFF, g_rxHdr, 3);
        got = usbCrc16Upd(got, g_rxBuf, g_rxLen);
        if (got == want) usbOnFrame(g_rxHdr[0], g_rxBuf, g_rxLen);
        g_rxStage = 0;
      }
      break;
  }
}

static void usbRxPump() {                  // 非阻塞 drain
  int avail = Serial.available();
  while (avail-- > 0) { int c = Serial.read(); if (c < 0) break; usbRxByte((uint8_t)c); }
}
