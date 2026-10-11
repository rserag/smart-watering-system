#pragma once

#include <Arduino.h>
#include <esp_timer.h>

#include "watering_types.h"

namespace watering {

class ConfigStore;

struct PumpStartEvent {
  uint8_t zoneId;
  bool manual;
  uint32_t durationMs;
  uint8_t moisturePercent;
};

class WateringController {
 public:
  WateringController(const uint8_t (&relayPins)[ZONE_COUNT],
                     const uint8_t (&sensorPins)[ZONE_COUNT],
                     uint8_t mainTankLevelPin);

  void begin(const SystemConfig &config, ConfigStore *store = nullptr);
  void loop(uint32_t now);
  void applyConfig(const SystemConfig &config, uint32_t now, bool bootLoad = false);
  void prepareForConfigUpdate(uint32_t now);

  bool requestManualWater(size_t zone, uint32_t durationMs, uint32_t now,
                          const char *&error);
  bool stopZone(size_t zone, uint32_t now, const char *&error);
  void emergencyStop(uint32_t now);
  bool clearFault(size_t zone, uint32_t now, const char *&error);
  bool clearFaultAndPause(size_t zone, uint32_t expectedGeneration,
                         uint32_t now, const char *&error);
  bool resumeZone(size_t zone, uint32_t expectedGeneration, uint32_t now,
                  const char *&error);

  const SensorState &sensorState(size_t zone) const;
  const ZoneState &zoneState(size_t zone) const;
  const SensorState &liveSensorState(size_t zone) const;
  const ZoneState &liveZoneState(size_t zone) const;
  const SystemConfig &config() const;
  bool consumeStateChanged();
  bool consumeTankStateChanged(bool &isLow);
  bool consumePumpStarted(PumpStartEvent &event);
  int8_t activeZone() const;
  bool mainTankLow() const;

  bool startDebugSession(DebugMode mode, const char *sessionId, const char *owner,
                         uint32_t now, const char *&error);
  bool renewDebugSession(const char *sessionId, uint32_t now, const char *&error);
  void endDebugSession(uint32_t now, const char *reason = "session ended");
  bool resumeAutomation(uint32_t now, const char *&error);
  bool debugPulse(size_t zone, uint32_t durationMs, const char *requestId,
                  uint32_t now, const char *&error);
  bool debugStop(int zone, uint32_t now, const char *&error, const char *reason = "stopped by operator");
  bool setDebugSoil(size_t zone, int raw, uint32_t now, const char *&error);
  bool setDebugTank(bool low, uint32_t now, const char *&error);
  bool clearDebugInput(const char *input, int zone, uint32_t now, const char *&error);
  void debugSample(uint32_t now);
  bool consumeDebugAction(DebugActionEvent &event);
  DebugMode debugMode() const;
  bool debugSessionActive() const;
  bool automationHeld() const;
  const char *debugSessionId() const;
  const char *debugOwner() const;
  uint32_t debugLeaseRemainingMs(uint32_t now) const;
  bool physicalTankLow() const;
  bool effectiveTankLow() const;
  bool pendingTankLow() const;
  uint32_t tankDebounceRemainingMs(uint32_t now) const;
  int tankOverride() const;
  int soilOverride(size_t zone) const;
  bool physicalRelayOn(size_t zone) const;
  bool virtualRelayOn(size_t zone) const;
  uint32_t pulseRemainingMs(size_t zone, uint32_t now) const;
  const char *decisionReason(size_t zone) const;
  uint8_t relayPin(size_t zone) const;
  uint8_t sensorPin(size_t zone) const;
  uint8_t tankPin() const;
  uint32_t lastLoopDurationUs() const;
  uint32_t maxLoopDurationUs() const;
  uint32_t sampleDurationUs() const;

 private:
  static constexpr uint8_t SAMPLES_PER_READING = 16;
  static constexpr uint8_t REQUIRED_STARTUP_SAMPLES = 3;
  static constexpr uint8_t INVALID_SAMPLES_BEFORE_FAULT = 3;
  static constexpr uint32_t TANK_LOW_DEBOUNCE_MS = 250;
  static constexpr uint32_t TANK_RESTORED_DEBOUNCE_MS = 2000;

  uint8_t relayPins_[ZONE_COUNT];
  uint8_t sensorPins_[ZONE_COUNT];
  uint8_t mainTankLevelPin_;
  SystemConfig config_{};
  SensorState sensors_[ZONE_COUNT]{};
  ZoneState zones_[ZONE_COUNT]{};
  ConfigStore *configStore_ = nullptr;
  uint8_t pausedZones_ = 0;

  uint32_t lastSampleAt_ = 0;
  bool hasSampled_ = false;
  bool stateChanged_ = false;
  bool tankStateChanged_ = false;
  bool pumpStarted_ = false;
  PumpStartEvent pumpStartEvent_{};
  int8_t activeZone_ = -1;
  bool mainTankLow_ = true;
  bool pendingMainTankLow_ = true;
  uint32_t tankLevelTransitionStartedAt_ = 0;
  esp_timer_handle_t relaySafetyTimer_ = nullptr;
  volatile bool relaySafetyCutoffTriggered_ = false;
  volatile int8_t relaySafetyCutoffZone_ = -1;
  static constexpr uint32_t DEBUG_LEASE_MS = 30000;
  DebugMode debugMode_ = DebugMode::Observe;
  bool automationHeld_ = false;
  bool automationHoldPersisted_ = true;
  char debugSessionId_[37]{};
  char debugOwner_[81]{};
  uint32_t debugLeaseRenewedAt_ = 0;
  esp_timer_handle_t debugLeaseTimer_ = nullptr;
  volatile bool debugLeaseExpired_ = false;
  esp_timer_handle_t debugTankTimer_ = nullptr;
  volatile bool debugTankCutoffTriggered_ = false;
  portMUX_TYPE outputMux_ = portMUX_INITIALIZER_UNLOCKED;
  int16_t soilOverrides_[ZONE_COUNT] = {-1, -1, -1, -1};
  int8_t tankOverride_ = -1;
  bool physicalTankLow_ = true;
  bool effectiveTankLow_ = true;
  SensorState savedLiveSensors_[ZONE_COUNT]{};
  ZoneState savedLiveZones_[ZONE_COUNT]{};
  int8_t debugOutputZone_ = -1;
  uint32_t debugOutputStartedAt_ = 0;
  uint32_t debugOutputDurationMs_ = 0;
  char debugOutputRequestId_[65]{};
  DebugActionEvent debugActions_[8]{};
  uint8_t debugActionRead_ = 0;
  uint8_t debugActionCount_ = 0;
  uint32_t lastLoopDurationUs_ = 0;
  uint32_t maxLoopDurationUs_ = 0;
  uint32_t sampleDurationUs_ = 0;

  uint16_t readAveragedRaw(uint8_t pin, SensorState &sensor);
  void updateMainTankLevel(uint32_t now);
  void engageMainTankInterlock(uint32_t now);
  void sampleAllSensors(uint32_t now);
  void evaluateZoneAfterSample(size_t zone, uint32_t now);
  void updateTimedStates(uint32_t now);
  void scheduleNextZone(uint32_t now);
  void startQueuedZone(size_t zone, uint32_t now);
  void setRelay(size_t zone, bool on);
  void initializeRelaySafetyTimer();
  bool armRelaySafetyTimer(size_t zone, uint32_t durationMs);
  void cancelRelaySafetyTimer();
  void handleRelaySafetyCutoff();
  static void relaySafetyTimerCallback(void *argument);
  static void debugLeaseTimerCallback(void *argument);
  static void debugTankTimerCallback(void *argument);
  void cancelAllActivity(uint32_t now);
  void finishDebugPulse(const char *status, const char *reason, uint32_t now);
  void addDebugAction(const char *requestId, const char *command,
                      const char *status, uint32_t now, uint8_t zoneId = 0,
                      const char *message = nullptr);
  void setPhase(size_t zone, ZonePhase phase, uint32_t now,
                FaultCode fault = FaultCode::None);
  void markFault(size_t zone, FaultCode fault, uint32_t now);
  void invalidateRecovery(size_t zone);
  void resetSensorValidation(size_t zone);
  ZonePhase idlePhase(size_t zone) const;
  void resetRuntimeState(uint32_t now);
};

}  // namespace watering
