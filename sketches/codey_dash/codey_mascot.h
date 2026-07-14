// sketches/codey_dash/codey_mascot.h — AA edge-arc + Claude/Codex mascots + idle animation.
// 单 TU 习惯:依赖全局 canvas `cv`(定义在 .ino,本头在其后 #include)+ codey_theme.h + fonts/*。
#pragma once
#include <M5Unified.h>
#include "codey_theme.h"
#include "codey_spring.h"

// ---------- animation state (shared) ----------
static bool     aBlink = false;
static uint32_t aBlinkNext = 0, aBlinkEnd = 0;
static Spring1D aGlanceSpring;      // 视线偏移:临界阻尼弹簧,替代写死的 *0.15f 逐帧衰减
static uint32_t aGlanceNext = 0;
static uint32_t aAnimLastMs = 0;    // updateAnim 相邻两次调用的时间戳,算 dt 喂给弹簧

static void updateAnim(uint32_t now) {
  if (!aBlink && now >= aBlinkNext) { aBlink = true; aBlinkEnd = now + 120; }
  if (aBlink && now >= aBlinkEnd)   { aBlink = false; aBlinkNext = now + 1500 + random(2800); }
  if (now >= aGlanceNext) { aGlanceSpring.to((random(201) - 100) / 100.0f); aGlanceNext = now + 1000 + random(1800); }
  uint32_t dt = aAnimLastMs ? (now - aAnimLastMs) : 0; aAnimLastMs = now;
  aGlanceSpring.halfLifeMs = 90.0f;   // 手感参数:多久追上目标的一半,帧率无关
  aGlanceSpring.update((float)dt);
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
  float ew = 1.4f, eh = 2.0f * open, ey = 4.7f + (2.0f - eh) / 2.0f, ex = aGlanceSpring.value * 0.9f;
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
  const int gx = (int)lroundf(aGlanceSpring.value * 4);
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

// 按 provider 分发吉祥物(0=Claude 网格脸 / 1=Codex 机器人)
static void drawMascot(int provIdx, int ccx, int ccy, uint32_t color, const char* mood, float t, float scale) {
  if (provIdx == 0) drawClaude(ccx, ccy, color, mood, t, scale);
  else              drawCodex(ccx, ccy, color, mood, t, scale);
}
