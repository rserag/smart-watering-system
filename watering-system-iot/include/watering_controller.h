#pragma once

#include <Arduino.h>
#include <esp_timer.h>

#include "watering_types.h"

namespace watering {

class WateringController {
 public:
  WateringController(const uint8_t (&relayPins)[ZONE_COUNT],
                     const uint8_t (&sensorPins)[ZONE_COUNT]);

  void begin(const SystemConfig &config);
  void loop(uint32_t now);
  void applyConfig(const SystemConfig &config, uint32_t now);
  void prepareForConfigUpdate(uint32_t now);

  bool requestManualWater(size_t zone, uint32_t durationMs, uint32_t now,
                          const char *&error);
  bool stopZone(size_t zone, uint32_t now, const char *&error);
  void emergencyStop(uint32_t now);
  bool clearFault(size_t zone, uint32_t now, const char *&error);

  const SensorState &sensorState(size_t zone) const;
  const ZoneState &zoneState(size_t zone) const;
  const SystemConfig &config() const;
  bool consumeStateChanged();
  int8_t activeZone() const;

 private:
  static constexpr uint8_t SAMPLES_PER_READING = 16;
  static constexpr uint8_t REQUIRED_STARTUP_SAMPLES = 3;
  static constexpr uint8_t INVALID_SAMPLES_BEFORE_FAULT = 3;

  uint8_t relayPins_[ZONE_COUNT];
  uint8_t sensorPins_[ZONE_COUNT];
  SystemConfig config_{};
  SensorState sensors_[ZONE_COUNT]{};
  ZoneState zones_[ZONE_COUNT]{};

  uint32_t lastSampleAt_ = 0;
  bool hasSampled_ = false;
  bool stateChanged_ = false;
  int8_t activeZone_ = -1;
  esp_timer_handle_t relaySafetyTimer_ = nullptr;
  volatile bool relaySafetyCutoffTriggered_ = false;
  volatile int8_t relaySafetyCutoffZone_ = -1;

  uint16_t readAveragedRaw(uint8_t pin);
  void sampleAllSensors(uint32_t now);
  void evaluateZoneAfterSample(size_t zone, uint32_t now);
  void updateTimedStates(uint32_t now);
  void scheduleNextZone(uint32_t now);
  void startQueuedZone(size_t zone, uint32_t now);
  void setRelay(size_t zone, bool on);
  void initializeRelaySafetyTimer();
  void armRelaySafetyTimer(size_t zone, uint32_t durationMs);
  void cancelRelaySafetyTimer();
  void handleRelaySafetyCutoff();
  static void relaySafetyTimerCallback(void *argument);
  void setPhase(size_t zone, ZonePhase phase, uint32_t now,
                FaultCode fault = FaultCode::None);
  void markFault(size_t zone, FaultCode fault, uint32_t now);
  void resetRuntimeState(uint32_t now);
};

}  // namespace watering
