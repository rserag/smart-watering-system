#pragma once

#include "Wire.h"
#include "display_test_state.h"

constexpr uint8_t SSD1306_SWITCHCAPVCC = 2;
constexpr uint8_t SSD1306_WHITE = 1;
constexpr uint8_t SSD1306_BLACK = 0;

class Adafruit_SSD1306 {
 public:
  Adafruit_SSD1306(int16_t, int16_t, TwoWire *, int8_t) {}
  bool begin(uint8_t, uint8_t, bool, bool) { return true; }
  void clearDisplay() {}
  void setTextColor(uint16_t) {}
  void setTextSize(uint8_t) {}
  void setTextWrap(bool) {}
  void setCursor(int16_t, int16_t) {}
  void drawFastHLine(int16_t, int16_t, int16_t, uint16_t) {}
  void fillRect(int16_t, int16_t, int16_t, int16_t, uint16_t) {}
  void drawRect(int16_t, int16_t, int16_t, int16_t, uint16_t) {}
  template <typename Value> void print(const Value &) {}
  void display() {
    controller_test::now += display_test::renderDelayMs;
    display_test::renderFinishedAt = millis();
    ++display_test::renderCount;
  }
  uint8_t *getBuffer() { return buffer_; }

 private:
  uint8_t buffer_[1024]{};
};
