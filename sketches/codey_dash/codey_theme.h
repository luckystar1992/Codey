// sketches/codey_dash/codey_theme.h — palette, geometry, color helpers (presentation-wide).
// 仅依赖 <stdint.h> + Arduino 的 constrain 宏(本头在 .ino 里于 M5Unified 之后 #include)。
#pragma once
#include <stdint.h>

// ---------- palette (RGB888) ----------
static const uint32_t COL_CLAUDE = 0xF4894F;
static const uint32_t COL_CODEX  = 0x22D3A6;
static const uint32_t COL_DANGER = 0xFF5D5D;
static const uint32_t COL_WHITE  = 0xFFFFFF;

// ---------- geometry (circular 466x466) ----------
static const int SIZE = 466, CX = 233, CY = 233;

// ---------- color helpers ----------
static inline uint16_t c565(uint32_t rgb) {
  uint8_t r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
static inline uint32_t shade(uint32_t rgb, float f) {  // f>0 lighten toward white, f<0 darken
  int r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
  if (f >= 0) { r += (255 - r) * f; g += (255 - g) * f; b += (255 - b) * f; }
  else        { r *= (1 + f);       g *= (1 + f);       b *= (1 + f); }
  r = constrain(r, 0, 255); g = constrain(g, 0, 255); b = constrain(b, 0, 255);
  return ((uint32_t)r << 16) | (g << 8) | b;
}
