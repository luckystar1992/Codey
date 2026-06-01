// hello_stopwatch — minimal M5Unified validation sketch for the M5Stack StopWatch (C152).
//
// Verifies the full path: M5Unified init -> round AMOLED (via M5GFX) -> button input
// -> USB-serial output. It depends ONLY on M5Unified + M5GFX (no board-specific libs),
// so a successful compile doubles as a toolchain smoke test before the device is connected.
//
// Note: the StopWatch's vibration motor / power LED are driven by the M5PM1 / M5IOE1
// chips. We'll wire those in once the device is in hand and those libraries are installed.

#include <M5Unified.h>

static int counter = 0;

static void drawCounter() {
  M5.Display.fillScreen(TFT_BLACK);
  const int cx = M5.Display.width() / 2;
  const int cy = M5.Display.height() / 2;
  M5.Display.drawString("StopWatch", cx, cy - 30);

  char buf[32];
  snprintf(buf, sizeof(buf), "Count: %d", counter);
  M5.Display.drawString(buf, cx, cy + 10);
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);

  M5.Display.setRotation(0);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextSize(2);

  Serial.begin(115200);
  Serial.println("hello_stopwatch booted");

  drawCounter();
}

void loop() {
  M5.update();

  if (M5.BtnA.wasPressed()) {
    counter++;
    Serial.printf("BtnA pressed -> count=%d\n", counter);
    drawCounter();
    // TODO(device): trigger the vibration motor via M5PM1 / M5IOE1 here.
  }

  delay(10);
}
