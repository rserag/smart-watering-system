#include "watering_controller.h"

#include <algorithm>

#include "config_store.h"

namespace watering {

WateringController::WateringController(
    const uint8_t (&relayPins)[ZONE_COUNT],
    const uint8_t (&sensorPins)[ZONE_COUNT], uint8_t mainTankLevelPin)
    : mainTankLevelPin_(mainTankLevelPin) {
  memcpy(relayPins_, relayPins, sizeof(relayPins_));
  memcpy(sensorPins_, sensorPins, sizeof(sensorPins_));
}

void WateringController::begin(const SystemConfig &config, ConfigStore *store) {
  config_ = config;
  configStore_ = store;

  // Load the OFF value before changing the pin mode to avoid a startup pulse.
  for (size_t zone = 0; zone < ZONE_COUNT; ++zone) {
    digitalWrite(relayPins_[zone], LOW);
    pinMode(relayPins_[zone], OUTPUT);

    pinMode(sensorPins_[zone], INPUT);
    analogSetPinAttenuation(sensorPins_[zone], ADC_11db);
  }

  // Secure the relay outputs before any storage operation can block.
  pausedZones_ = 0;
  if (configStore_ != nullptr && !configStore_->loadPausedZones(pausedZones_)) {
    // Unknown persisted state must never silently resume a cleared channel.
    pausedZones_ = (1u << ZONE_COUNT) - 1u;
    Serial.println("Pause storage unavailable: keeping all channels paused");
  }
  automationHeld_ = false;
  if (configStore_ != nullptr && !configStore_->loadAutomationHeld(automationHeld_)) {
    automationHeld_ = true;
    automationHoldPersisted_ = false;
    Serial.println("Diagnostics hold storage unavailable: automation held");
  }

  // An external 5 kOhm pull-up holds the input HIGH while the float switch is
  // open. Do not enable the ESP32's internal pull-up here.
  pinMode(mainTankLevelPin_, INPUT);
  mainTankLow_ = digitalRead(mainTankLevelPin_) == LOW;
  physicalTankLow_ = mainTankLow_;
  effectiveTankLow_ = mainTankLow_;
  pendingMainTankLow_ = mainTankLow_;
  tankStateChanged_ = mainTankLow_;
  tankLevelTransitionStartedAt_ = millis();
  Serial.printf("Main tank GPIO %u: %s\n", mainTankLevelPin_,
                mainTankLow_ ? "LOW WATER (LOW/closed)"
                             : "ready (HIGH/open)");

  initializeRelaySafetyTimer();
  esp_timer_create_args_t leaseArgs{};
  leaseArgs.callback = debugLeaseTimerCallback;
  leaseArgs.arg = this;
  leaseArgs.dispatch_method = ESP_TIMER_TASK;
  leaseArgs.name = "debug_lease";
  if (esp_timer_create(&leaseArgs, &debugLeaseTimer_) != ESP_OK) {
    debugLeaseTimer_ = nullptr;
  }
  leaseArgs.callback = debugTankTimerCallback;
  leaseArgs.name = "debug_tank";
  if (esp_timer_create(&leaseArgs, &debugTankTimer_) != ESP_OK) debugTankTimer_ = nullptr;
  resetRuntimeState(millis());
}

void WateringController::loop(uint32_t now) {
  const uint32_t started = micros();
  if (debugSessionActive() && debugLeaseRemainingMs(now) == 0) {
    endDebugSession(now, "session lease expired");
  }
  portENTER_CRITICAL(&outputMux_);
  const bool tankCutoff = debugTankCutoffTriggered_;
  debugTankCutoffTriggered_ = false;
  portEXIT_CRITICAL(&outputMux_);
  if (tankCutoff && debugOutputZone_ >= 0)
    finishDebugPulse("cancelled", "independent physical tank cutoff", now);
  handleRelaySafetyCutoff();
  updateMainTankLevel(now);

  if (!hasSampled_ || now - lastSampleAt_ >= config_.sampleIntervalMs) {
    sampleAllSensors(now);
  }

  if (debugOutputZone_ >= 0 &&
      now - debugOutputStartedAt_ >= debugOutputDurationMs_) {
    finishDebugPulse("completed", "pulse completed", now);
  }
  if (!automationHeld_ || debugMode_ == DebugMode::Simulation) {
    updateTimedStates(now);
    scheduleNextZone(now);
  }
  lastLoopDurationUs_ = micros() - started;
  maxLoopDurationUs_ = std::max(maxLoopDurationUs_, lastLoopDurationUs_);
}

void WateringController::applyConfig(const SystemConfig &config, uint32_t now, bool bootLoad) {
  if (automationHeld_ && !(bootLoad && !hasSampled_ && !debugSessionActive())) return;
  for (size_t zone = 0; zone < ZONE_COUNT; ++zone) {
    setRelay(zone, false);
  }
  config_ = config;
  resetRuntimeState(now);
  Serial.printf("Applied configuration revision %lu\n",
                static_cast<unsigned long>(config_.revision));
}

void WateringController::prepareForConfigUpdate(uint32_t now) {
  if (automationHeld_) return;
  for (size_t zone = 0; zone < ZONE_COUNT; ++zone) {
    setRelay(zone, false);
    zones_[zone].manualRequest = false;
    zones_[zone].wateringOnMsThisCycle = 0;
    invalidateRecovery(zone);
    resetSensorValidation(zone);
    setPhase(zone, idlePhase(zone), now);
  }
}

bool WateringController::requestManualWater(size_t zone, uint32_t durationMs,
                                             uint32_t now,
                                             const char *&error) {
  if (automationHeld_) {
    error = "automation is held for diagnostics; end the session and explicitly resume";
    return false;
  }
  if (zone >= ZONE_COUNT) {
    error = "invalid zone";
    return false;
  }
  if (!config_.zones[zone].enabled) {
    error = "zone is disabled";
    return false;
  }
  if (zones_[zone].paused) {
    error = "zone is paused; resume watering first";
    return false;
  }
  if (mainTankLow_) {
    error = "main tank water is low";
    return false;
  }
  if (!sensors_[zone].valid || zones_[zone].phase == ZonePhase::Fault) {
    error = "zone has no valid sensor state";
    return false;
  }
  if (sensors_[zone].validSampleCount < REQUIRED_STARTUP_SAMPLES ||
      zones_[zone].phase == ZonePhase::Stabilizing) {
    error = "zone is checking fresh sensor readings";
    return false;
  }
  if (durationMs < MIN_PULSE_MS || durationMs > MAX_PULSE_MS ||
      durationMs > config_.zones[zone].maxWateringOnMsPerCycle) {
    error = "duration is outside the safe range";
    return false;
  }

  if (zones_[zone].relayOn) {
    error = "zone is already watering";
    return false;
  }

  zones_[zone].manualRequest = true;
  zones_[zone].manualDurationMs = durationMs;
  zones_[zone].wateringOnMsThisCycle = 0;
  zones_[zone].consecutiveDryReadings = 0;
  zones_[zone].consecutiveWetReadings = 0;
  setPhase(zone, ZonePhase::Queued, now);
  error = nullptr;
  return true;
}

bool WateringController::stopZone(size_t zone, uint32_t now,
                                  const char *&error) {
  if (zone >= ZONE_COUNT) {
    error = "invalid zone";
    return false;
  }

  setRelay(zone, false);
  if (zones_[zone].phase == ZonePhase::Fault) {
    error = nullptr;
    return true;
  }
  zones_[zone].manualRequest = false;
  zones_[zone].wateringOnMsThisCycle = 0;
  setPhase(zone, idlePhase(zone), now);
  error = nullptr;
  return true;
}

void WateringController::emergencyStop(uint32_t now) {
  if (debugSessionActive()) endDebugSession(now, "emergency stop");
  for (size_t zone = 0; zone < ZONE_COUNT; ++zone) {
    markFault(zone, FaultCode::EmergencyStop, now);
  }
}

bool WateringController::clearFault(size_t zone, uint32_t now,
                                    const char *&error) {
  (void)now;
  if (zone >= ZONE_COUNT) {
    error = "invalid zone";
    return false;
  }
  // Older clients must not bypass occurrence checks or durable pause storage.
  error = "use confirmed clear-and-pause recovery";
  return false;
}

bool WateringController::clearFaultAndPause(size_t zone,
                                           uint32_t expectedGeneration,
                                           uint32_t now, const char *&error) {
  if (automationHeld_) {
    error = "recovery is blocked while diagnostics holds automation";
    return false;
  }
  if (zone >= ZONE_COUNT) {
    error = "invalid zone";
    return false;
  }
  ZoneState &state = zones_[zone];
  if (state.recoveryGeneration != expectedGeneration ||
      state.recoveryGeneration == UINT32_MAX) {
    error = "recovery is stale; review the current channel state";
    return false;
  }
  if (state.phase != ZonePhase::Fault ||
      state.fault != FaultCode::MaximumWateringReached) {
    error = "only a watering-limit fault can be cleared and paused";
    return false;
  }

  setRelay(zone, false);
  const uint8_t mask = pausedZones_ | (1u << zone);
  if (configStore_ == nullptr || !configStore_->savePausedZones(mask)) {
    error = "could not save channel pause; fault remains active";
    return false;
  }
  pausedZones_ = mask;
  state.paused = true;
  state.manualRequest = false;
  state.manualDurationMs = 0;
  state.activePulseMs = 0;
  state.wateringOnMsThisCycle = 0;
  resetSensorValidation(zone);
  invalidateRecovery(zone);
  setPhase(zone, idlePhase(zone), now);
  error = nullptr;
  return true;
}

bool WateringController::resumeZone(size_t zone, uint32_t expectedGeneration,
                                  uint32_t now, const char *&error) {
  if (automationHeld_) {
    error = "recovery is blocked while diagnostics holds automation";
    return false;
  }
  if (zone >= ZONE_COUNT) {
    error = "invalid zone";
    return false;
  }
  ZoneState &state = zones_[zone];
  if (state.recoveryGeneration != expectedGeneration ||
      state.recoveryGeneration == UINT32_MAX) {
    error = "recovery is stale; review the current channel state";
    return false;
  }
  if (!state.paused) {
    error = "zone is not paused";
    return false;
  }
  if (state.fault != FaultCode::None || state.phase == ZonePhase::Fault) {
    error = "zone still has an active fault";
    return false;
  }
  if (!config_.zones[zone].enabled) {
    error = "zone is disabled; enable it before resuming";
    return false;
  }
  if (mainTankLow_) {
    error = "main tank water is low; refill it before resuming";
    return false;
  }

  setRelay(zone, false);
  const uint8_t mask = pausedZones_ & ~(1u << zone);
  if (configStore_ == nullptr || !configStore_->savePausedZones(mask)) {
    error = "could not save resume; channel remains paused";
    return false;
  }
  pausedZones_ = mask;
  state.paused = false;
  state.manualRequest = false;
  state.manualDurationMs = 0;
  state.activePulseMs = 0;
  state.wateringOnMsThisCycle = 0;
  resetSensorValidation(zone);
  invalidateRecovery(zone);
  setPhase(zone, ZonePhase::Stabilizing, now);
  error = nullptr;
  return true;
}


bool WateringController::startDebugSession(DebugMode mode, const char *sessionId,
                                           const char *owner, uint32_t now,
                                           const char *&error) {
  if (debugSessionActive() && debugLeaseRemainingMs(now) == 0)
    endDebugSession(now, "session lease expired");
  if (debugSessionActive()) { error = "another diagnostics session is active"; return false; }
  if (mode == DebugMode::Observe || sessionId == nullptr || strlen(sessionId) != 36 ||
      owner == nullptr || strcmp(owner, "sahakyan.raphayel@gmail.com") != 0) {
    error = "invalid diagnostic mode, session or owner"; return false;
  }
  // Hold is saved before ownership is granted; reboot cannot restart watering.
  cancelAllActivity(now);
  automationHeld_ = true;
  automationHoldPersisted_ = configStore_ != nullptr && configStore_->saveAutomationHeld(true);
  if (!automationHoldPersisted_) {
    error = "could not persist automation hold; outputs remain off"; return false;
  }
  if (debugLeaseTimer_ == nullptr || (mode == DebugMode::Hardware && debugTankTimer_ == nullptr)) {
    error = "independent diagnostics safety timer unavailable"; return false;
  }
  debugLeaseExpired_ = false;
  if (esp_timer_start_once(debugLeaseTimer_, uint64_t(DEBUG_LEASE_MS) * 1000) != ESP_OK) {
    error = "could not arm independent lease timer"; return false;
  }
  strlcpy(debugSessionId_, sessionId, sizeof(debugSessionId_));
  strlcpy(debugOwner_, owner, sizeof(debugOwner_));
  debugLeaseRenewedAt_ = now;
  debugMode_ = mode;
  debugTankCutoffTriggered_ = false;
  if (mode == DebugMode::Hardware && esp_timer_start_periodic(debugTankTimer_, 10000) != ESP_OK) {
    endDebugSession(now, "physical tank timer failed");
    error = "could not arm physical tank cutoff"; return false;
  }
  for (size_t zone = 0; zone < ZONE_COUNT; ++zone) soilOverrides_[zone] = -1;
  tankOverride_ = -1;
  if (mode == DebugMode::Simulation) {
    memcpy(savedLiveSensors_, sensors_, sizeof(sensors_));
    memcpy(savedLiveZones_, zones_, sizeof(zones_));
    for (size_t zone = 0; zone < ZONE_COUNT; ++zone) {
      sensors_[zone] = SensorState{};
      zones_[zone] = ZoneState{};
      zones_[zone].phase = config_.zones[zone].enabled ? ZonePhase::Stabilizing
                                                    : ZonePhase::Disabled;
      zones_[zone].phaseStartedAt = now;
    }
    hasSampled_ = false;
  }
  stateChanged_ = true;
  error = nullptr;
  return true;
}

bool WateringController::renewDebugSession(const char *sessionId, uint32_t now,
                                           const char *&error) {
  if (!debugSessionActive() || debugLeaseRemainingMs(now) == 0 ||
      strcmp(debugSessionId_, sessionId) != 0) {
    if (debugSessionActive() && debugLeaseRemainingMs(now) == 0)
      endDebugSession(now, "session lease expired");
    error = "session is missing, expired or owned by another operator"; return false;
  }
  esp_timer_stop(debugLeaseTimer_);
  if (esp_timer_start_once(debugLeaseTimer_, uint64_t(DEBUG_LEASE_MS) * 1000) != ESP_OK) {
    endDebugSession(now, "lease timer failed");
    error = "could not renew independent lease timer"; return false;
  }
  debugLeaseRenewedAt_ = now;
  stateChanged_ = true;
  error = nullptr;
  return true;
}

void WateringController::cancelAllActivity(uint32_t now) {
  if (debugOutputZone_ >= 0) finishDebugPulse("cancelled", "activity cancelled", now);
  cancelRelaySafetyTimer();
  activeZone_ = -1;
  pumpStarted_ = false;
  for (size_t zone = 0; zone < ZONE_COUNT; ++zone) {
    digitalWrite(relayPins_[zone], LOW);
    zones_[zone].relayOn = false;
    zones_[zone].manualRequest = false;
    zones_[zone].manualDurationMs = 0;
    zones_[zone].activePulseMs = 0;
    zones_[zone].wateringOnMsThisCycle = 0;
    zones_[zone].consecutiveDryReadings = 0;
    zones_[zone].consecutiveWetReadings = 0;
    if (zones_[zone].phase != ZonePhase::Fault) setPhase(zone, idlePhase(zone), now);
  }
  stateChanged_ = true;
}

void WateringController::endDebugSession(uint32_t now, const char *reason) {
  if (!debugSessionActive()) return;
  if (debugOutputZone_ >= 0) finishDebugPulse("cancelled", reason, now);
  cancelAllActivity(now);
  esp_timer_stop(debugLeaseTimer_);
  if (debugTankTimer_ != nullptr) esp_timer_stop(debugTankTimer_);
  if (debugMode_ == DebugMode::Simulation) {
    memcpy(sensors_, savedLiveSensors_, sizeof(sensors_));
    memcpy(zones_, savedLiveZones_, sizeof(zones_));
  }
  debugMode_ = DebugMode::Observe;
  debugSessionId_[0] = '\0';
  debugOwner_[0] = '\0';
  debugLeaseExpired_ = false;
  debugTankCutoffTriggered_ = false;
  tankOverride_ = -1;
  physicalTankLow_ = digitalRead(mainTankLevelPin_) == LOW;
  effectiveTankLow_ = physicalTankLow_;
  mainTankLow_ = physicalTankLow_;
  pendingMainTankLow_ = physicalTankLow_;
  tankLevelTransitionStartedAt_ = now;
  for (size_t zone = 0; zone < ZONE_COUNT; ++zone) {
    soilOverrides_[zone] = -1;
    resetSensorValidation(zone);
  }
  hasSampled_ = false;
  cancelAllActivity(now);
}

bool WateringController::resumeAutomation(uint32_t now, const char *&error) {
  if (debugSessionActive()) { error = "end the diagnostics session before resuming"; return false; }
  cancelAllActivity(now);
  if (configStore_ == nullptr || !configStore_->saveAutomationHeld(false)) {
    automationHeld_ = true;
    error = "could not persist automation resume"; return false;
  }
  automationHeld_ = false;
  automationHoldPersisted_ = true;
  for (size_t zone = 0; zone < ZONE_COUNT; ++zone) {
    resetSensorValidation(zone);
    if (zones_[zone].phase != ZonePhase::Fault) setPhase(zone, idlePhase(zone), now);
  }
  hasSampled_ = false;
  stateChanged_ = true;
  error = nullptr;
  return true;
}

bool WateringController::debugPulse(size_t zone, uint32_t durationMs,
                                    const char *requestId, uint32_t now,
                                    const char *&error) {
  if (!debugSessionActive() || debugLeaseRemainingMs(now) == 0) {
    error = "active diagnostics session required"; return false;
  }
  if (zone >= ZONE_COUNT || durationMs < 100 || durationMs > 60000 ||
      requestId == nullptr || strlen(requestId) == 0 || strlen(requestId) > 64) {
    error = "invalid diagnostic pulse"; return false;
  }
  if (activeZone_ >= 0 || debugOutputZone_ >= 0) {
    error = "another relay is active"; return false;
  }
  physicalTankLow_ = digitalRead(mainTankLevelPin_) == LOW;
  if (debugMode_ == DebugMode::Hardware && (physicalTankLow_ || mainTankLow_)) {
    error = "physical tank is low or stabilizing"; return false;
  }
  if (debugMode_ == DebugMode::Simulation && mainTankLow_) {
    error = "simulated tank interlock is active"; return false;
  }
  if (debugMode_ == DebugMode::Hardware && !armRelaySafetyTimer(zone, durationMs)) {
    error = "independent pulse cutoff unavailable"; return false;
  }
  debugOutputZone_ = static_cast<int8_t>(zone);
  debugOutputStartedAt_ = now;
  debugOutputDurationMs_ = durationMs;
  strlcpy(debugOutputRequestId_, requestId, sizeof(debugOutputRequestId_));
  zones_[zone].relayOn = true;
  activeZone_ = static_cast<int8_t>(zone);
  // A fired lease callback can never be followed by a HIGH write.
  bool outputAllowed = false;
  portENTER_CRITICAL(&outputMux_);
  if (debugMode_ == DebugMode::Hardware && !mainTankLow_ &&
      digitalRead(mainTankLevelPin_) != LOW) debugTankCutoffTriggered_ = false;
  outputAllowed = !debugLeaseExpired_ && !relaySafetyCutoffTriggered_ &&
                  !debugTankCutoffTriggered_ &&
                  (debugMode_ != DebugMode::Hardware || digitalRead(mainTankLevelPin_) != LOW);
  if (outputAllowed)
    digitalWrite(relayPins_[zone], debugMode_ == DebugMode::Hardware ? HIGH : LOW);
  portEXIT_CRITICAL(&outputMux_);
  if (!outputAllowed) {
    finishDebugPulse("cancelled", "session lease expired", now);
    error = "session lease expired"; return false;
  }
  addDebugAction(requestId, "debug.output.pulse", "started", now, zone + 1);
  stateChanged_ = true;
  error = nullptr;
  return true;
}

void WateringController::finishDebugPulse(const char *status, const char *reason,
                                         uint32_t now) {
  if (debugOutputZone_ < 0) return;
  const size_t zone = static_cast<size_t>(debugOutputZone_);
  digitalWrite(relayPins_[zone], LOW);
  zones_[zone].relayOn = false;
  if (activeZone_ == debugOutputZone_) activeZone_ = -1;
  if (debugMode_ != DebugMode::Simulation) cancelRelaySafetyTimer();
  addDebugAction(debugOutputRequestId_, "debug.output.pulse", status, now, zone + 1, reason);
  debugOutputZone_ = -1;
  debugOutputRequestId_[0] = '\0';
  stateChanged_ = true;
}

bool WateringController::debugStop(int zone, uint32_t now, const char *&error, const char *reason) {
  if (debugOutputZone_ >= 0 && (zone < 0 || debugOutputZone_ == zone))
    finishDebugPulse("cancelled", reason, now);
  for (size_t index = 0; index < ZONE_COUNT; ++index) {
    if (zone >= 0 && zone != static_cast<int>(index)) continue;
    setRelay(index, false);
    zones_[index].manualRequest = false;
    if (zones_[index].phase != ZonePhase::Fault) setPhase(index, idlePhase(index), now);
  }
  // Stop outside a session must also inhibit automatic requeueing.
  if (!automationHeld_) {
    cancelAllActivity(now);
    automationHeld_ = true;
    automationHoldPersisted_ = false;
  }
  if (!automationHoldPersisted_) {
    automationHoldPersisted_ = configStore_ != nullptr && configStore_->saveAutomationHeld(true);
    if (!automationHoldPersisted_) {
      error = "outputs stopped but durable automation hold could not be saved";
      return false;
    }
  }
  stateChanged_ = true;
  error = nullptr;
  return true;
}

bool WateringController::setDebugSoil(size_t zone, int raw, uint32_t now,
                                     const char *&error) {
  if (debugMode_ != DebugMode::Simulation || debugLeaseRemainingMs(now) == 0 ||
      zone >= ZONE_COUNT || raw < 0 || raw > 4095) {
    error = "soil injection requires simulation and an ADC value from 0 to 4095"; return false;
  }
  soilOverrides_[zone] = raw;
  stateChanged_ = true;
  error = nullptr;
  return true;
}

bool WateringController::setDebugTank(bool low, uint32_t now, const char *&error) {
  if (debugMode_ != DebugMode::Simulation || debugLeaseRemainingMs(now) == 0) {
    error = "tank injection requires simulation"; return false;
  }
  tankOverride_ = low ? 1 : 0;
  updateMainTankLevel(now);
  stateChanged_ = true;
  error = nullptr;
  return true;
}

bool WateringController::clearDebugInput(const char *input, int zone,
                                        uint32_t now, const char *&error) {
  if (debugMode_ != DebugMode::Simulation || debugLeaseRemainingMs(now) == 0 ||
      zone >= static_cast<int>(ZONE_COUNT) || zone < -1) {
    error = "input overrides require simulation"; return false;
  }
  const bool all = strcmp(input, "all") == 0;
  if (!all && strcmp(input, "soil") != 0 && strcmp(input, "tank") != 0) {
    error = "unknown input"; return false;
  }
  if (all || strcmp(input, "soil") == 0)
    for (size_t index = 0; index < ZONE_COUNT; ++index)
      if (zone < 0 || static_cast<int>(index) == zone) soilOverrides_[index] = -1;
  if (all || strcmp(input, "tank") == 0) {
    tankOverride_ = -1;
    updateMainTankLevel(now);
  }
  stateChanged_ = true;
  error = nullptr;
  return true;
}

void WateringController::debugSample(uint32_t now) { sampleAllSensors(now); }

void WateringController::addDebugAction(const char *requestId, const char *command,
                                       const char *status, uint32_t now,
                                       uint8_t zoneId, const char *message) {
  if (debugActionCount_ == 8) {
    debugActionRead_ = (debugActionRead_ + 1) % 8;
    --debugActionCount_;
  }
  DebugActionEvent &event = debugActions_[(debugActionRead_ + debugActionCount_) % 8];
  event = DebugActionEvent{};
  strlcpy(event.sessionId, debugSessionId_, sizeof(event.sessionId));
  strlcpy(event.requestId, requestId, sizeof(event.requestId));
  strlcpy(event.command, command, sizeof(event.command));
  strlcpy(event.status, status, sizeof(event.status));
  if (message != nullptr) strlcpy(event.message, message, sizeof(event.message));
  event.uptimeMs = now;
  event.zoneId = zoneId;
  ++debugActionCount_;
}

bool WateringController::consumeDebugAction(DebugActionEvent &event) {
  if (debugActionCount_ == 0) return false;
  event = debugActions_[debugActionRead_];
  debugActionRead_ = (debugActionRead_ + 1) % 8;
  --debugActionCount_;
  return true;
}

void WateringController::debugLeaseTimerCallback(void *argument) {
  auto *controller = static_cast<WateringController *>(argument);
  portENTER_CRITICAL(&controller->outputMux_);
  controller->debugLeaseExpired_ = true;
  for (size_t zone = 0; zone < ZONE_COUNT; ++zone)
    digitalWrite(controller->relayPins_[zone], LOW);
  portEXIT_CRITICAL(&controller->outputMux_);
}

void WateringController::debugTankTimerCallback(void *argument) {
  auto *controller = static_cast<WateringController *>(argument);
  if (digitalRead(controller->mainTankLevelPin_) != LOW) {
    return;
  }
  portENTER_CRITICAL(&controller->outputMux_);
  controller->debugTankCutoffTriggered_ = true;
  for (size_t zone = 0; zone < ZONE_COUNT; ++zone)
    digitalWrite(controller->relayPins_[zone], LOW);
  portEXIT_CRITICAL(&controller->outputMux_);
}

DebugMode WateringController::debugMode() const { return debugMode_; }
bool WateringController::debugSessionActive() const { return debugMode_ != DebugMode::Observe; }
bool WateringController::automationHeld() const { return automationHeld_; }
const char *WateringController::debugSessionId() const { return debugSessionId_; }
const char *WateringController::debugOwner() const { return debugOwner_; }
uint32_t WateringController::debugLeaseRemainingMs(uint32_t now) const {
  if (!debugSessionActive() || debugLeaseExpired_ || now - debugLeaseRenewedAt_ >= DEBUG_LEASE_MS) return 0;
  return DEBUG_LEASE_MS - (now - debugLeaseRenewedAt_);
}
bool WateringController::physicalTankLow() const { return physicalTankLow_; }
bool WateringController::effectiveTankLow() const { return effectiveTankLow_; }
bool WateringController::pendingTankLow() const { return pendingMainTankLow_; }
uint32_t WateringController::tankDebounceRemainingMs(uint32_t now) const {
  if (pendingMainTankLow_ == mainTankLow_) return 0;
  const uint32_t duration = pendingMainTankLow_ ? TANK_LOW_DEBOUNCE_MS : TANK_RESTORED_DEBOUNCE_MS;
  const uint32_t elapsed = now - tankLevelTransitionStartedAt_;
  return elapsed >= duration ? 0 : duration - elapsed;
}
int WateringController::tankOverride() const { return tankOverride_; }
int WateringController::soilOverride(size_t zone) const { return soilOverrides_[zone]; }
bool WateringController::physicalRelayOn(size_t zone) const {
  return debugMode_ != DebugMode::Simulation && !debugLeaseExpired_ &&
         !debugTankCutoffTriggered_ &&
         !(relaySafetyCutoffTriggered_ && relaySafetyCutoffZone_ == static_cast<int8_t>(zone)) &&
         zones_[zone].relayOn;
}
bool WateringController::virtualRelayOn(size_t zone) const {
  return debugMode_ == DebugMode::Simulation && zones_[zone].relayOn;
}
uint32_t WateringController::pulseRemainingMs(size_t zone, uint32_t now) const {
  if (debugOutputZone_ == static_cast<int8_t>(zone)) {
    const uint32_t elapsed = now - debugOutputStartedAt_;
    return elapsed >= debugOutputDurationMs_ ? 0 : debugOutputDurationMs_ - elapsed;
  }
  if (!zones_[zone].relayOn) return 0;
  const uint32_t elapsed = now - zones_[zone].phaseStartedAt;
  return elapsed >= zones_[zone].activePulseMs ? 0 : zones_[zone].activePulseMs - elapsed;
}
const char *WateringController::decisionReason(size_t zone) const {
  if (debugOutputZone_ == static_cast<int8_t>(zone)) return "diagnostic pulse";
  if (automationHeld_ && debugMode_ != DebugMode::Simulation) return "automation held for diagnostics";
  if (zones_[zone].fault != FaultCode::None) return faultCodeName(zones_[zone].fault);
  if (zones_[zone].paused) return "channel paused";
  if (!config_.zones[zone].enabled) return "channel disabled";
  if (mainTankLow_) return "tank interlock";
  if (!sensors_[zone].valid) return "invalid sensor sample";
  if (sensors_[zone].validSampleCount < REQUIRED_STARTUP_SAMPLES) return "checking fresh sensor samples";
  if (!config_.automaticWateringEnabled && !zones_[zone].manualRequest) return "automatic watering disabled";
  return zonePhaseName(zones_[zone].phase);
}
uint8_t WateringController::relayPin(size_t zone) const { return relayPins_[zone]; }
uint8_t WateringController::sensorPin(size_t zone) const { return sensorPins_[zone]; }
uint8_t WateringController::tankPin() const { return mainTankLevelPin_; }
uint32_t WateringController::lastLoopDurationUs() const { return lastLoopDurationUs_; }
uint32_t WateringController::maxLoopDurationUs() const { return maxLoopDurationUs_; }
uint32_t WateringController::sampleDurationUs() const { return sampleDurationUs_; }

const SensorState &WateringController::sensorState(size_t zone) const {
  return sensors_[zone];
}

const ZoneState &WateringController::zoneState(size_t zone) const {
  return zones_[zone];
}

const SensorState &WateringController::liveSensorState(size_t zone) const {
  return debugMode_ == DebugMode::Simulation ? savedLiveSensors_[zone] : sensors_[zone];
}

const ZoneState &WateringController::liveZoneState(size_t zone) const {
  return debugMode_ == DebugMode::Simulation ? savedLiveZones_[zone] : zones_[zone];
}

const SystemConfig &WateringController::config() const { return config_; }

bool WateringController::consumeStateChanged() {
  const bool changed = stateChanged_;
  stateChanged_ = false;
  return changed;
}

bool WateringController::consumeTankStateChanged(bool &isLow) {
  if (!tankStateChanged_) {
    return false;
  }
  tankStateChanged_ = false;
  if (debugMode_ == DebugMode::Simulation) return false;
  isLow = mainTankLow_;
  return true;
}

bool WateringController::consumePumpStarted(PumpStartEvent &event) {
  if (!pumpStarted_) {
    return false;
  }
  pumpStarted_ = false;
  if (debugMode_ == DebugMode::Simulation) return false;
  event = pumpStartEvent_;
  return true;
}

int8_t WateringController::activeZone() const { return activeZone_; }

bool WateringController::mainTankLow() const { return mainTankLow_; }

uint16_t WateringController::readAveragedRaw(uint8_t pin, SensorState &sensor) {
  // The first conversion after changing ADC channels can contain residue.
  analogRead(pin);
  delayMicroseconds(100);

  uint32_t total = 0;
  sensor.minRaw = 4095;
  sensor.maxRaw = 0;
  for (uint8_t sample = 0; sample < SAMPLES_PER_READING; ++sample) {
    sensor.samples[sample] = analogRead(pin);
    total += sensor.samples[sample];
    sensor.minRaw = std::min(sensor.minRaw, sensor.samples[sample]);
    sensor.maxRaw = std::max(sensor.maxRaw, sensor.samples[sample]);
    delayMicroseconds(50);
  }
  return total / SAMPLES_PER_READING;
}

void WateringController::updateMainTankLevel(uint32_t now) {
  physicalTankLow_ = digitalRead(mainTankLevelPin_) == LOW;
  // A real low-water observation cuts diagnostic hardware immediately. The
  // ordinary control signal still uses the established debounce below.
  if (debugMode_ == DebugMode::Hardware && physicalTankLow_ && debugOutputZone_ >= 0) {
    finishDebugPulse("cancelled", "physical tank is low", now);
  }
  const bool observedLow = debugMode_ == DebugMode::Simulation && tankOverride_ >= 0
                               ? tankOverride_ != 0 : physicalTankLow_;
  effectiveTankLow_ = observedLow;
  if (observedLow != pendingMainTankLow_) {
    pendingMainTankLow_ = observedLow;
    tankLevelTransitionStartedAt_ = now;
    return;
  }

  const uint32_t debounceMs = observedLow ? TANK_LOW_DEBOUNCE_MS
                                          : TANK_RESTORED_DEBOUNCE_MS;
  if (observedLow == mainTankLow_ ||
      now - tankLevelTransitionStartedAt_ < debounceMs) {
    return;
  }

  mainTankLow_ = observedLow;
  stateChanged_ = true;
  tankStateChanged_ = true;
  if (mainTankLow_) {
    Serial.println("Main tank LOW: stopping and blocking all watering");
    engageMainTankInterlock(now);
  } else {
    Serial.println("Main tank level restored: watering interlock cleared");
  }
}

void WateringController::engageMainTankInterlock(uint32_t now) {
  if (debugOutputZone_ >= 0) finishDebugPulse("cancelled", "tank interlock", now);
  for (size_t zone = 0; zone < ZONE_COUNT; ++zone) {
    setRelay(zone, false);
    zones_[zone].manualRequest = false;
    zones_[zone].wateringOnMsThisCycle = 0;
    zones_[zone].consecutiveDryReadings = 0;
    zones_[zone].consecutiveWetReadings = 0;
    if (zones_[zone].phase != ZonePhase::Fault) {
      setPhase(zone, idlePhase(zone), now);
    }
  }
}

void WateringController::sampleAllSensors(uint32_t now) {
  const uint32_t started = micros();
  lastSampleAt_ = now;
  hasSampled_ = true;

  const bool tankPinHigh = digitalRead(mainTankLevelPin_) == HIGH;
  Serial.printf("Main tank GPIO %u: pin=%s, state=%s\n", mainTankLevelPin_,
                tankPinHigh ? "HIGH/open" : "LOW/closed",
                mainTankLow_ ? "LOW WATER (watering blocked)" : "ready");
  Serial.println("Moisture sensor readings:");
  for (size_t zone = 0; zone < ZONE_COUNT; ++zone) {
    SensorState &sensor = sensors_[zone];
    sensor.physicalRaw = readAveragedRaw(sensorPins_[zone], sensor);
    const uint16_t raw = debugMode_ == DebugMode::Simulation && soilOverrides_[zone] >= 0
                             ? soilOverrides_[zone] : sensor.physicalRaw;
    sensor.latestRaw = raw;
    sensor.lastReadingAt = now;

    const bool valid = raw >= ADC_MIN_VALID && raw <= ADC_MAX_VALID;
    if (!valid) {
      sensor.valid = false;
      sensor.validSampleCount = 0;
      if (sensor.invalidSampleCount < UINT8_MAX) {
        ++sensor.invalidSampleCount;
      }
    } else {
      if (sensor.validSampleCount == 0) {
        sensor.filteredRaw = raw;
      } else {
        sensor.filteredRaw =
            (static_cast<uint32_t>(sensor.filteredRaw) * 3 + raw) / 4;
      }
      if (sensor.validSampleCount < UINT8_MAX) {
        ++sensor.validSampleCount;
      }
      sensor.invalidSampleCount = 0;
      sensor.valid = true;
      sensor.moisturePercent = static_cast<uint8_t>(calculateMoisturePercent(
          sensor.filteredRaw, config_.zones[zone].dryRaw,
          config_.zones[zone].wetRaw));
    }

    Serial.printf(
        "  Zone %u GPIO %u: raw=%u filtered=%u relative moisture=%u%% "
        "valid=%s\n",
        static_cast<unsigned>(zone + 1), sensorPins_[zone], raw,
        sensor.filteredRaw, sensor.moisturePercent, valid ? "yes" : "no");

    evaluateZoneAfterSample(zone, now);
  }

  stateChanged_ = true;
  sampleDurationUs_ = micros() - started;
}

void WateringController::evaluateZoneAfterSample(size_t zone, uint32_t now) {
  if (automationHeld_ && debugMode_ != DebugMode::Simulation) return;
  SensorState &sensor = sensors_[zone];
  ZoneState &state = zones_[zone];
  const ZoneConfig &zoneConfig = config_.zones[zone];

  // Faults remain latched, even when later readings are invalid or the channel
  // is disabled. A paused channel samples for display but cannot queue water.
  if (state.phase == ZonePhase::Fault) {
    return;
  }
  if (!zoneConfig.enabled) {
    setRelay(zone, false);
    setPhase(zone, ZonePhase::Disabled, now);
    return;
  }
  if (state.paused) {
    setRelay(zone, false);
    setPhase(zone, ZonePhase::Paused, now);
    return;
  }

  if (!sensor.valid) {
    state.consecutiveDryReadings = 0;
    state.consecutiveWetReadings = 0;
    if (sensor.invalidSampleCount >= INVALID_SAMPLES_BEFORE_FAULT) {
      markFault(zone, FaultCode::SensorInvalid, now);
    }
    return;
  }

  if (sensor.validSampleCount < REQUIRED_STARTUP_SAMPLES) {
    // An isolated invalid sample may occur inside a pulse, soak or cooldown.
    // Preserve those timers and the current cycle's watering allowance while
    // fresh readings rebuild. Only idle detection returns to stabilization.
    if (state.phase == ZonePhase::Monitoring ||
        state.phase == ZonePhase::DryConfirming) {
      setPhase(zone, ZonePhase::Stabilizing, now);
    }
    return;
  }

  if (state.phase == ZonePhase::Stabilizing) {
    if (sensor.validSampleCount >= REQUIRED_STARTUP_SAMPLES) {
      setPhase(zone, ZonePhase::Monitoring, now);
    }
    return;
  }

  if (sensor.moisturePercent >= zoneConfig.stopWateringPercent) {
    if (state.consecutiveWetReadings < UINT8_MAX) {
      ++state.consecutiveWetReadings;
    }
  } else {
    state.consecutiveWetReadings = 0;
  }

  if (state.phase == ZonePhase::Monitoring ||
      state.phase == ZonePhase::DryConfirming) {
    if (!config_.automaticWateringEnabled || mainTankLow_) {
      state.consecutiveDryReadings = 0;
      if (state.phase == ZonePhase::DryConfirming) {
        setPhase(zone, ZonePhase::Monitoring, now);
      }
      return;
    }

    if (sensor.moisturePercent <= zoneConfig.startWateringPercent) {
      if (state.consecutiveDryReadings < UINT8_MAX) {
        ++state.consecutiveDryReadings;
      }
      if (state.consecutiveDryReadings >=
          zoneConfig.dryConfirmationSamples) {
        state.wateringOnMsThisCycle = 0;
        state.manualRequest = false;
        setPhase(zone, ZonePhase::Queued, now);
      } else {
        setPhase(zone, ZonePhase::DryConfirming, now);
      }
    } else {
      state.consecutiveDryReadings = 0;
      if (state.phase == ZonePhase::DryConfirming) {
        setPhase(zone, ZonePhase::Monitoring, now);
      }
    }
  }
}

void WateringController::updateTimedStates(uint32_t now) {
  for (size_t zone = 0; zone < ZONE_COUNT; ++zone) {
    if (debugOutputZone_ == static_cast<int8_t>(zone)) continue;
    ZoneState &state = zones_[zone];
    const ZoneConfig &zoneConfig = config_.zones[zone];
    if (state.paused) {
      if (state.relayOn) {
        setRelay(zone, false);
      }
      continue;
    }
    const uint32_t elapsed = now - state.phaseStartedAt;

    if (state.phase == ZonePhase::Watering &&
        elapsed >= state.activePulseMs) {
      setRelay(zone, false);
      state.wateringOnMsThisCycle += state.activePulseMs;
      state.consecutiveWetReadings = 0;
      setPhase(zone, ZonePhase::Soaking, now);
      continue;
    }

    if (state.phase == ZonePhase::ManualWatering &&
        elapsed >= state.activePulseMs) {
      setRelay(zone, false);
      state.wateringOnMsThisCycle += state.activePulseMs;
      state.manualRequest = false;
      setPhase(zone, ZonePhase::Cooldown, now);
      continue;
    }

    if (state.phase == ZonePhase::Soaking && elapsed >= zoneConfig.soakMs) {
      if (state.consecutiveWetReadings >=
          zoneConfig.wetConfirmationSamples) {
        setPhase(zone, ZonePhase::Cooldown, now);
      } else if (state.wateringOnMsThisCycle >=
                 zoneConfig.maxWateringOnMsPerCycle) {
        markFault(zone, FaultCode::MaximumWateringReached, now);
      } else if (sensors_[zone].moisturePercent <
                 zoneConfig.stopWateringPercent) {
        setPhase(zone, ZonePhase::Queued, now);
      }
      continue;
    }

    if (state.phase == ZonePhase::Cooldown && elapsed >= zoneConfig.cooldownMs) {
      state.wateringOnMsThisCycle = 0;
      state.consecutiveDryReadings = 0;
      state.consecutiveWetReadings = 0;
      setPhase(zone, ZonePhase::Monitoring, now);
    }
  }
}

void WateringController::scheduleNextZone(uint32_t now) {
  if (activeZone_ >= 0 || mainTankLow_ ||
      (automationHeld_ && debugMode_ != DebugMode::Simulation)) {
    return;
  }

  int8_t selectedZone = -1;
  uint32_t longestWait = 0;
  for (size_t zone = 0; zone < ZONE_COUNT; ++zone) {
    if (!zones_[zone].paused && zones_[zone].phase == ZonePhase::Queued) {
      const uint32_t wait = now - zones_[zone].phaseStartedAt;
      if (selectedZone < 0 || wait > longestWait) {
        selectedZone = static_cast<int8_t>(zone);
        longestWait = wait;
      }
    }
  }

  if (selectedZone >= 0) {
    startQueuedZone(static_cast<size_t>(selectedZone), now);
  }
}

void WateringController::startQueuedZone(size_t zone, uint32_t now) {
  ZoneState &state = zones_[zone];
  const ZoneConfig &zoneConfig = config_.zones[zone];

  if (state.paused || state.fault != FaultCode::None) {
    setRelay(zone, false);
    state.manualRequest = false;
    if (state.fault == FaultCode::None) {
      setPhase(zone, idlePhase(zone), now);
    }
    return;
  }
  if (mainTankLow_) {
    state.manualRequest = false;
    setPhase(zone, idlePhase(zone), now);
    return;
  }

  if (!zoneConfig.enabled || !sensors_[zone].valid) {
    markFault(zone, FaultCode::SensorInvalid, now);
    return;
  }
  if (sensors_[zone].validSampleCount < REQUIRED_STARTUP_SAMPLES) {
    // Keep the existing queued cycle and its budget; resetting its phase here
    // would let later dry detection start a new allowance prematurely.
    return;
  }

  if (state.manualRequest) {
    state.activePulseMs = state.manualDurationMs;
    setRelay(zone, true);
    setPhase(zone, ZonePhase::ManualWatering, now);
    pumpStartEvent_ = PumpStartEvent{
        static_cast<uint8_t>(zone + 1), true, state.activePulseMs,
        sensors_[zone].moisturePercent};
    pumpStarted_ = true;
    return;
  }

  const uint32_t remaining =
      zoneConfig.maxWateringOnMsPerCycle - state.wateringOnMsThisCycle;
  if (remaining == 0) {
    markFault(zone, FaultCode::MaximumWateringReached, now);
    return;
  }

  state.activePulseMs = std::min(zoneConfig.pulseOnMs, remaining);
  setRelay(zone, true);
  setPhase(zone, ZonePhase::Watering, now);
  pumpStartEvent_ = PumpStartEvent{
      static_cast<uint8_t>(zone + 1), false, state.activePulseMs,
      sensors_[zone].moisturePercent};
  pumpStarted_ = true;
}

void WateringController::setRelay(size_t zone, bool on) {
  if (zone >= ZONE_COUNT) {
    return;
  }

  if (on && ((automationHeld_ && debugMode_ != DebugMode::Simulation) ||
             zones_[zone].paused || mainTankLow_ ||
             !config_.zones[zone].enabled ||
             zones_[zone].fault != FaultCode::None ||
             !sensors_[zone].valid ||
             sensors_[zone].validSampleCount < REQUIRED_STARTUP_SAMPLES)) {
    return;
  }
  if (on && activeZone_ >= 0 && activeZone_ != static_cast<int8_t>(zone)) {
    return;
  }

  if (on && debugMode_ != DebugMode::Simulation &&
      !armRelaySafetyTimer(zone, zones_[zone].activePulseMs)) return;
  digitalWrite(relayPins_[zone], on && debugMode_ != DebugMode::Simulation ? HIGH : LOW);
  zones_[zone].relayOn = on;
  if (on) {
    activeZone_ = static_cast<int8_t>(zone);
  } else if (activeZone_ == static_cast<int8_t>(zone)) {
    cancelRelaySafetyTimer();
    activeZone_ = -1;
  }
  stateChanged_ = true;
}

void WateringController::initializeRelaySafetyTimer() {
  if (relaySafetyTimer_ != nullptr) {
    return;
  }

  esp_timer_create_args_t timerArgs{};
  timerArgs.callback = relaySafetyTimerCallback;
  timerArgs.arg = this;
  timerArgs.dispatch_method = ESP_TIMER_TASK;
  timerArgs.name = "relay_cutoff";
  if (esp_timer_create(&timerArgs, &relaySafetyTimer_) != ESP_OK) {
    relaySafetyTimer_ = nullptr;
    Serial.println("Warning: failed to create independent relay cutoff timer");
  }
}

bool WateringController::armRelaySafetyTimer(size_t zone,
                                             uint32_t durationMs) {
  if (relaySafetyTimer_ == nullptr || durationMs == 0) {
    return false;
  }

  esp_timer_stop(relaySafetyTimer_);
  relaySafetyCutoffTriggered_ = false;
  relaySafetyCutoffZone_ = static_cast<int8_t>(zone);
  if (esp_timer_start_once(relaySafetyTimer_,
                          static_cast<uint64_t>(durationMs) * 1000ULL) != ESP_OK) {
    relaySafetyCutoffZone_ = -1;
    return false;
  }
  return true;
}

void WateringController::cancelRelaySafetyTimer() {
  if (relaySafetyTimer_ != nullptr) {
    esp_timer_stop(relaySafetyTimer_);
  }
  relaySafetyCutoffZone_ = -1;
  relaySafetyCutoffTriggered_ = false;
}

void WateringController::handleRelaySafetyCutoff() {
  if (!relaySafetyCutoffTriggered_) {
    return;
  }

  const int8_t cutoffZone = relaySafetyCutoffZone_;
  if (debugOutputZone_ == cutoffZone && cutoffZone >= 0) {
    finishDebugPulse("completed", "independent pulse cutoff", millis());
    return;
  }
  relaySafetyCutoffTriggered_ = false;
  relaySafetyCutoffZone_ = -1;
  if (cutoffZone >= 0 && cutoffZone < static_cast<int8_t>(ZONE_COUNT)) {
    zones_[cutoffZone].relayOn = false;
    if (activeZone_ == cutoffZone) {
      activeZone_ = -1;
    }
    stateChanged_ = true;
  }
}

void WateringController::relaySafetyTimerCallback(void *argument) {
  auto *controller = static_cast<WateringController *>(argument);
  const int8_t zone = controller->relaySafetyCutoffZone_;
  portENTER_CRITICAL(&controller->outputMux_);
  if (zone >= 0 && zone < static_cast<int8_t>(ZONE_COUNT)) {
    // This callback runs independently of network processing.
    digitalWrite(controller->relayPins_[zone], LOW);
    controller->relaySafetyCutoffTriggered_ = true;
  }
  portEXIT_CRITICAL(&controller->outputMux_);
}

void WateringController::setPhase(size_t zone, ZonePhase phase, uint32_t now,
                                   FaultCode fault) {
  ZoneState &state = zones_[zone];
  if (state.phase == phase && state.fault == fault) {
    return;
  }

  state.phase = phase;
  state.fault = fault;
  state.phaseStartedAt = now;
  stateChanged_ = true;
  Serial.printf("Zone %u state: %s", static_cast<unsigned>(zone + 1),
                zonePhaseName(phase));
  if (fault != FaultCode::None) {
    Serial.printf(" (%s)", faultCodeName(fault));
  }
  Serial.println();
}

void WateringController::markFault(size_t zone, FaultCode fault,
                                    uint32_t now) {
  setRelay(zone, false);
  zones_[zone].manualRequest = false;
  if (zones_[zone].phase != ZonePhase::Fault || zones_[zone].fault != fault) {
    invalidateRecovery(zone);
  }
  setPhase(zone, ZonePhase::Fault, now, fault);
}

void WateringController::invalidateRecovery(size_t zone) {
  // Saturation prevents an ancient confirmation becoming valid after wrap.
  if (zones_[zone].recoveryGeneration != UINT32_MAX) {
    ++zones_[zone].recoveryGeneration;
  }
  stateChanged_ = true;
}

void WateringController::resetSensorValidation(size_t zone) {
  sensors_[zone].valid = false;
  sensors_[zone].validSampleCount = 0;
  sensors_[zone].invalidSampleCount = 0;
  zones_[zone].consecutiveDryReadings = 0;
  zones_[zone].consecutiveWetReadings = 0;
}

ZonePhase WateringController::idlePhase(size_t zone) const {
  if (!config_.zones[zone].enabled) {
    return ZonePhase::Disabled;
  }
  if (zones_[zone].paused) {
    return ZonePhase::Paused;
  }
  return sensors_[zone].valid &&
                 sensors_[zone].validSampleCount >= REQUIRED_STARTUP_SAMPLES
             ? ZonePhase::Monitoring
             : ZonePhase::Stabilizing;
}

void WateringController::resetRuntimeState(uint32_t now) {
  activeZone_ = -1;
  hasSampled_ = false;
  lastSampleAt_ = now;

  for (size_t zone = 0; zone < ZONE_COUNT; ++zone) {
    digitalWrite(relayPins_[zone], LOW);
    const uint32_t generation = zones_[zone].recoveryGeneration;
    sensors_[zone] = SensorState{};
    zones_[zone] = ZoneState{};
    zones_[zone].recoveryGeneration = generation;
    invalidateRecovery(zone);
    zones_[zone].paused = (pausedZones_ & (1u << zone)) != 0;
    zones_[zone].phase = idlePhase(zone);
    zones_[zone].phaseStartedAt = now;
  }
  stateChanged_ = true;
}

}  // namespace watering
