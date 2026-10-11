#include "display_manager.h"

#include <Wire.h>

namespace watering {

DisplayManager::DisplayManager(uint8_t sdaPin, uint8_t sclPin, uint8_t address)
    : sdaPin_(sdaPin),
      sclPin_(sclPin),
      address_(address),
      display_(WIDTH, HEIGHT, &Wire, -1) {}

void DisplayManager::begin() {
  Wire.begin(sdaPin_, sclPin_);
  Wire.setClock(400000);
  Wire.setTimeOut(5);

  // Do not let the display library restart Wire with different pins.
  available_ = display_.begin(SSD1306_SWITCHCAPVCC, address_, true, false);
  if (!available_) {
    Serial.printf(
        "SSD1306 OLED not found at I2C address 0x%02X; continuing without "
        "display\n",
        address_);
    return;
  }

  display_.clearDisplay();
  display_.setTextColor(SSD1306_WHITE);
  display_.setTextSize(1);
  display_.setTextWrap(false);
  display_.display();
  Serial.printf("SSD1306 OLED ready at I2C address 0x%02X\n", address_);
}

void DisplayManager::update(const WateringController &controller,
                            bool wifiConnected, bool backendConnected,
                            uint32_t now, bool force) {
  if (testActive_) {
    if (!controller.debugSessionActive() || controller.debugLeaseRemainingMs(now) == 0) {
      finishTest("cancelled", now, "session ended");
      force = true;
    } else if (now - testStartedAt_ >= testDurationMs_) {
      finishTest("completed", now, "display test completed");
      force = true;
    } else return;
  }
  if (!available_ ||
      (!force && now - lastRefreshAt_ < REFRESH_INTERVAL_MS)) {
    return;
  }

  lastRefreshAt_ = now;
  draw(controller, wifiConnected, backendConnected);
  const uint32_t hash = frameHash();
  if (force || !hasFrame_ || hash != lastFrameHash_) {
    display_.display();
    lastFrameHash_ = hash;
    hasFrame_ = true;
  }
}

void DisplayManager::draw(const WateringController &controller,
                          bool wifiConnected, bool backendConnected) {
  display_.clearDisplay();
  display_.setTextColor(SSD1306_WHITE);
  display_.setTextSize(1);

  display_.setCursor(0, 0);
  display_.print(controller.debugMode() == DebugMode::Simulation ? F("SIMULATION") :
                 controller.automationHeld() ? F("DEBUG HOLD") : F("WATERING"));
  display_.setCursor(82, 0);
  display_.print(wifiConnected ? F("W+") : F("W-"));
  display_.setCursor(106, 0);
  display_.print(backendConnected ? F("B+") : F("B-"));
  display_.drawFastHLine(0, 9, WIDTH, SSD1306_WHITE);

  if (controller.mainTankLow()) {
    display_.fillRect(0, 11, 51, 9, SSD1306_WHITE);
    display_.setTextColor(SSD1306_BLACK);
    display_.setCursor(2, 12);
    display_.print(F("TANK LOW"));
    display_.setTextColor(SSD1306_WHITE);
  } else {
    display_.setCursor(0, 12);
    display_.print(F("TANK OK"));
  }
  display_.setCursor(82, 12);
  display_.print(controller.automationHeld() ? F("HOLD") :
                 controller.config().automaticWateringEnabled ? F("AUTO")
                                                               : F("MAN"));

  for (size_t zone = 0; zone < ZONE_COUNT; ++zone) {
    const SensorState &sensor = controller.sensorState(zone);
    const ZoneState &state = controller.zoneState(zone);
    char line[24];
    if (sensor.valid) {
      snprintf(line, sizeof(line), "Z%u %3u%% %s",
               static_cast<unsigned>(zone + 1),
               static_cast<unsigned>(sensor.moisturePercent),
               zoneStatus(state));
    } else {
      snprintf(line, sizeof(line), "Z%u  --%% %s",
               static_cast<unsigned>(zone + 1), zoneStatus(state));
    }
    display_.setCursor(0, 22 + static_cast<int16_t>(zone) * 10);
    display_.print(line);
  }
}


bool DisplayManager::startTest(const char *pattern, uint32_t durationMs,
                               const char *requestId, const char *sessionId,
                               uint32_t now, const char *&error) {
  if (!available_) { error = "OLED is unavailable"; return false; }
  if (testActive_ || testEventPending_) { error = "another OLED test is pending"; return false; }
  if (durationMs < 100 || durationMs > 10000 ||
      (strcmp(pattern, "checkerboard") != 0 && strcmp(pattern, "text") != 0 &&
       strcmp(pattern, "off") != 0)) { error = "invalid OLED test"; return false; }
  testEvent_ = DebugActionEvent{};
  strlcpy(testEvent_.sessionId, sessionId, sizeof(testEvent_.sessionId));
  strlcpy(testEvent_.requestId, requestId, sizeof(testEvent_.requestId));
  strlcpy(testEvent_.command, "debug.oled.test", sizeof(testEvent_.command));
  testStartedAt_ = now;
  testDurationMs_ = durationMs;
  testActive_ = true;
  display_.clearDisplay();
  if (strcmp(pattern, "checkerboard") == 0) {
    for (int16_t y = 0; y < HEIGHT; y += 8)
      for (int16_t x = 0; x < WIDTH; x += 8)
        if (((x + y) / 8) % 2 == 0) display_.fillRect(x, y, 8, 8, SSD1306_WHITE);
  } else if (strcmp(pattern, "text") == 0) {
    display_.setTextColor(SSD1306_WHITE);
    display_.setTextSize(1);
    display_.setCursor(0, 0);
    display_.print(F("DIAGNOSTICS 0.8.0\nOLED 128x64\nGPIO SDA21 SCL22\n0123456789 ABCDEF"));
    display_.drawRect(0, 0, WIDTH, HEIGHT, SSD1306_WHITE);
  }
  display_.display();
  hasFrame_ = false;
  error = nullptr;
  return true;
}

void DisplayManager::finishTest(const char *status, uint32_t now, const char *reason) {
  if (!testActive_) return;
  testActive_ = false;
  strlcpy(testEvent_.status, status, sizeof(testEvent_.status));
  strlcpy(testEvent_.message, reason, sizeof(testEvent_.message));
  testEvent_.uptimeMs = now;
  testEventPending_ = true;
  hasFrame_ = false;
}
void DisplayManager::cancelTest(uint32_t now, const char *reason) { finishTest("cancelled", now, reason); }
bool DisplayManager::consumeTestEvent(DebugActionEvent &event) {
  if (!testEventPending_) return false;
  event = testEvent_;
  testEventPending_ = false;
  return true;
}
bool DisplayManager::available() const { return available_; }
uint8_t DisplayManager::sdaPin() const { return sdaPin_; }
uint8_t DisplayManager::sclPin() const { return sclPin_; }
uint8_t DisplayManager::address() const { return address_; }
size_t DisplayManager::scanI2c(uint8_t *addresses, size_t capacity) {
  size_t count = 0;
  Wire.setTimeOut(5);
  for (uint8_t address = 1; address < 127; ++address) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0 && count < capacity) addresses[count++] = address;
    // Give the independent relay/lease callbacks CPU time during a bus scan.
    delay(1);
  }
  return count;
}

uint32_t DisplayManager::frameHash() {
  const uint8_t *buffer = display_.getBuffer();
  constexpr size_t BUFFER_BYTES = WIDTH * HEIGHT / 8;
  uint32_t hash = 2166136261UL;
  for (size_t index = 0; index < BUFFER_BYTES; ++index) {
    hash ^= buffer[index];
    hash *= 16777619UL;
  }
  return hash;
}

const char *DisplayManager::zoneStatus(const ZoneState &state) {
  switch (state.phase) {
    case ZonePhase::Disabled:
      return "DISABLED";
    case ZonePhase::Stabilizing:
      return "STARTING";
    case ZonePhase::Monitoring:
      return "MONITOR";
    case ZonePhase::DryConfirming:
      return "DRY CHECK";
    case ZonePhase::Queued:
      return "QUEUED";
    case ZonePhase::Watering:
      return "WATERING";
    case ZonePhase::ManualWatering:
      return "MANUAL";
    case ZonePhase::Soaking:
      return "SOAKING";
    case ZonePhase::Cooldown:
      return "COOLDOWN";
    case ZonePhase::Paused:
      return "PAUSED";
    case ZonePhase::Fault:
      switch (state.fault) {
        case FaultCode::SensorInvalid:
          return "FAULT:SENSOR";
        case FaultCode::MaximumWateringReached:
          return "FAULT:LIMIT";
        case FaultCode::EmergencyStop:
          return "FAULT:E-STOP";
        case FaultCode::None:
          return "FAULT";
      }
  }
  return "UNKNOWN";
}

}  // namespace watering
