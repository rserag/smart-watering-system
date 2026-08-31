#pragma once

#include <Adafruit_SSD1306.h>
#include <Arduino.h>

#include "watering_controller.h"

namespace watering {

class DisplayManager {
 public:
  DisplayManager(uint8_t sdaPin, uint8_t sclPin, uint8_t address);

  void begin();
  void update(const WateringController &controller, bool wifiConnected,
              bool backendConnected, uint32_t now, bool force = false);

 private:
  static constexpr int16_t WIDTH = 128;
  static constexpr int16_t HEIGHT = 64;
  static constexpr uint32_t REFRESH_INTERVAL_MS = 500;

  uint8_t sdaPin_;
  uint8_t sclPin_;
  uint8_t address_;
  Adafruit_SSD1306 display_;
  bool available_ = false;
  bool hasFrame_ = false;
  uint32_t lastRefreshAt_ = 0;
  uint32_t lastFrameHash_ = 0;

  void draw(const WateringController &controller, bool wifiConnected,
            bool backendConnected);
  uint32_t frameHash();
  static const char *zoneStatus(const ZoneState &state);
};

}  // namespace watering
