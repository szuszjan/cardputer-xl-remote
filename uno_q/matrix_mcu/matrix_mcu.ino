// CXL Remote - UNO Q LED matrix (MCU side)
//
// Dumb frame display for the 13x8 LED matrix: the CXL Remote server on the
// Linux side composes every frame (scrolling PIN, clock, sound spectrum) and
// sends it through the arduino-router as one RPC call, "cxl_frame", carrying
// 104 bytes (row-major, 13 columns x 8 rows, brightness 0..7).
// All the logic lives in Python, so changing what the matrix shows never
// needs this sketch to be re-flashed.
//
// Build/flash: FQBN arduino:zephyr:unoq, libraries Arduino_RouterBridge +
// Arduino_LED_Matrix (ships with the core); upload over USB (adb).

#include <Arduino_RouterBridge.h>
#include <Arduino_LED_Matrix.h>

Arduino_LED_Matrix matrix;
uint8_t frame[104];

bool cxlFrame(MsgPack::bin_t<uint8_t> px) {
  size_t n = px.size() < sizeof(frame) ? px.size() : sizeof(frame);
  memset(frame, 0, sizeof(frame));
  memcpy(frame, px.data(), n);
  matrix.draw(frame);
  return true;
}

void setup() {
  matrix.begin();
  matrix.setGrayscaleBits(3);  // 0..7 per LED
  memset(frame, 0, sizeof(frame));
  matrix.draw(frame);
  Bridge.begin();
  Bridge.provide("cxl_frame", cxlFrame);
}

void loop() {
  delay(100);
}
