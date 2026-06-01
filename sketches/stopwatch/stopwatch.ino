// stopwatch — a real stopwatch for the M5Stack StopWatch (C152, ESP32-S3).
//
// Controls:
//   BtnA : Start / Pause / Resume   (toggle)
//   BtnB : Lap   (while running)  /  Reset (while paused or ready)
//
// Display (round 466x466 AMOLED): big MM:SS.CC timer (auto-switches to HH:MM:SS
// past one hour), a seconds-sweep progress ring, a status line, and the most
// recent laps. Everything is drawn to an off-screen PSRAM canvas and pushed once
// per frame, so updates are flicker-free.

#include <M5Unified.h>

enum class State { READY, RUNNING, PAUSED };

static State    state          = State::READY;
static uint32_t segmentStartMs = 0;   // millis() when the current running segment began
static uint32_t accumulatedMs  = 0;   // elapsed time banked before the current segment

static const int MAX_LAPS      = 3;   // most-recent laps kept for display
static uint32_t  lapSplits[MAX_LAPS]; // newest at index 0
static int       lapCount      = 0;   // total laps taken
static uint32_t  lastLapTotalMs = 0;  // elapsed at the previous lap

static M5Canvas canvas(&M5.Display);  // off-screen frame buffer
static int      cx = 0, cy = 0, ringR = 0;
static float    timeScale = 1.0f;     // Font7 scale that fits "00:00.00" to the screen

// ---------- time helpers ----------

static uint32_t elapsedMs() {
  return (state == State::RUNNING)
       ? accumulatedMs + (millis() - segmentStartMs)
       : accumulatedMs;
}

static void formatTime(uint32_t ms, char* out, size_t n) {
  const uint32_t cs       = (ms / 10) % 100;
  const uint32_t totalSec = ms / 1000;
  const uint32_t s        = totalSec % 60;
  const uint32_t m        = (totalSec / 60) % 60;
  const uint32_t h        = totalSec / 3600;
  if (h > 0) snprintf(out, n, "%02u:%02u:%02u", (unsigned)h, (unsigned)m, (unsigned)s);
  else       snprintf(out, n, "%02u:%02u.%02u", (unsigned)m, (unsigned)s, (unsigned)cs);
}

// ---------- state transitions ----------

static void startOrToggle() {
  switch (state) {
    case State::READY:
    case State::PAUSED:
      segmentStartMs = millis();
      state = State::RUNNING;
      break;
    case State::RUNNING:
      accumulatedMs += millis() - segmentStartMs;
      state = State::PAUSED;
      break;
  }
}

static void lapOrReset() {
  if (state == State::RUNNING) {
    const uint32_t total = elapsedMs();
    for (int i = MAX_LAPS - 1; i > 0; --i) lapSplits[i] = lapSplits[i - 1];
    lapSplits[0] = total - lastLapTotalMs;
    lastLapTotalMs = total;
    ++lapCount;
  } else {                                   // READY or PAUSED -> reset
    state = State::READY;
    accumulatedMs = 0;
    lastLapTotalMs = 0;
    lapCount = 0;
    for (int i = 0; i < MAX_LAPS; ++i) lapSplits[i] = 0;
  }
}

// ---------- look ----------

static uint16_t accentColor() {
  switch (state) {
    case State::RUNNING: return TFT_GREEN;
    case State::PAUSED:  return TFT_ORANGE;
    default:             return TFT_CYAN;
  }
}

static const char* statusText() {
  switch (state) {
    case State::RUNNING: return "RUNNING";
    case State::PAUSED:  return "PAUSED";
    default:             return "READY";
  }
}

// ---------- rendering ----------

static void drawRing(uint32_t e) {
  const int rOuter = ringR;
  const int rInner = ringR - 16;
  canvas.fillArc(cx, cy, rInner, rOuter, 0, 360, 0x2104);        // faint track
  const int sweep = (int)(((e % 60000) / 60000.0f) * 360.0f + 0.5f);  // 1 turn / minute
  if (sweep > 0) canvas.fillArc(cx, cy, rInner, rOuter, -90, -90 + sweep, accentColor());
}

static void drawStatus() {
  canvas.setFont(&fonts::FreeSansBold12pt7b);
  canvas.setTextSize(1);
  canvas.setTextDatum(middle_center);
  canvas.setTextColor(accentColor(), TFT_BLACK);
  canvas.drawString(statusText(), cx, cy - (int)(ringR * 0.42f));
}

static void drawTime(uint32_t e) {
  char buf[16];
  formatTime(e, buf, sizeof(buf));
  canvas.setFont(&fonts::Font7);
  canvas.setTextSize(timeScale);
  canvas.setTextDatum(middle_center);
  canvas.setTextColor(accentColor(), TFT_BLACK);
  canvas.drawString(buf, cx, cy);
}

static void drawLapsOrHint() {
  canvas.setFont(&fonts::FreeSansBold9pt7b);
  canvas.setTextSize(1);
  canvas.setTextDatum(middle_center);
  const int baseY = cy + (int)(ringR * 0.40f);

  if (lapCount == 0) {
    const char* hint = (state == State::RUNNING) ? "A: Pause    B: Lap"
                     : (state == State::PAUSED)  ? "A: Resume   B: Reset"
                                                 : "A: Start";
    canvas.setTextColor(TFT_DARKGREY, TFT_BLACK);
    canvas.drawString(hint, cx, baseY);
    return;
  }

  canvas.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  const int shown = (lapCount < MAX_LAPS) ? lapCount : MAX_LAPS;
  char line[24], t[16];
  for (int i = 0; i < shown; ++i) {
    formatTime(lapSplits[i], t, sizeof(t));
    snprintf(line, sizeof(line), "L%d  %s", lapCount - i, t);
    canvas.drawString(line, cx, baseY + i * 26);
  }
}

static void render() {
  const uint32_t e = elapsedMs();
  canvas.fillSprite(TFT_BLACK);
  drawRing(e);
  drawStatus();
  drawTime(e);
  drawLapsOrHint();
  canvas.pushSprite(0, 0);
}

// ---------- Arduino entry points ----------

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(0);

  Serial.begin(115200);
  Serial.println("stopwatch booted");

  cx = M5.Display.width() / 2;
  cy = M5.Display.height() / 2;
  ringR = (cx < cy ? cx : cy) - 6;

  canvas.setColorDepth(16);
  canvas.setPsram(true);                       // 466x466x16bpp needs PSRAM (~434KB)
  if (!canvas.createSprite(M5.Display.width(), M5.Display.height())) {
    Serial.println("ERROR: canvas allocation failed");
  }

  // auto-fit the 8-char time string to ~82% of the screen width using Font7
  canvas.setFont(&fonts::Font7);
  canvas.setTextSize(1.0f);
  const float baseW = canvas.textWidth("00:00.00");
  timeScale = (baseW > 0) ? (M5.Display.width() * 0.82f / baseW) : 1.0f;

  render();
}

void loop() {
  M5.update();

  bool dirty = false;
  if (M5.BtnA.wasPressed()) { startOrToggle(); dirty = true; Serial.printf("[btnA] -> %s\n", statusText()); }
  if (M5.BtnB.wasPressed()) { lapOrReset();    dirty = true; Serial.printf("[btnB] laps=%d state=%s\n", lapCount, statusText()); }

  if (state == State::RUNNING || dirty) render();   // free-run while timing; else redraw on change
  delay(16);                                        // ~60 fps cap
}
