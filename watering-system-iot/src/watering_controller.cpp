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

  // An external 5 kOhm pull-up holds the input HIGH while the float switch is
  // open. Do not enable the ESP32's internal pull-up here.
  pinMode(mainTankLevelPin_, INPUT);
  mainTankLow_ = digitalRead(mainTankLevelPin_) == LOW;
  pendingMainTankLow_ = mainTankLow_;
  tankStateChanged_ = mainTankLow_;
  tankLevelTransitionStartedAt_ = millis();
  Serial.printf("Main tank GPIO %u: %s\n", mainTankLevelPin_,
                mainTankLow_ ? "LOW WATER (LOW/closed)"
                             : "ready (HIGH/open)");

  initializeRelaySafetyTimer();
  resetRuntimeState(millis());
}

void WateringController::loop(uint32_t now) {
  handleRelaySafetyCutoff();
  updateMainTankLevel(now);

  if (!hasSampled_ || now - lastSampleAt_ >= config_.sampleIntervalMs) {
    sampleAllSensors(now);
  }

  updateTimedStates(now);
  scheduleNextZone(now);
}

void WateringController::applyConfig(const SystemConfig &config, uint32_t now) {
  for (size_t zone = 0; zone < ZONE_COUNT; ++zone) {
    setRelay(zone, false);
  }
  config_ = config;
  resetRuntimeState(now);
  Serial.printf("Applied configuration revision %lu\n",
                static_cast<unsigned long>(config_.revision));
}

void WateringController::prepareForConfigUpdate(uint32_t now) {
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

const SensorState &WateringController::sensorState(size_t zone) const {
  return sensors_[zone];
}

const ZoneState &WateringController::zoneState(size_t zone) const {
  return zones_[zone];
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
  isLow = mainTankLow_;
  return true;
}

bool WateringController::consumePumpStarted(PumpStartEvent &event) {
  if (!pumpStarted_) {
    return false;
  }
  pumpStarted_ = false;
  event = pumpStartEvent_;
  return true;
}

int8_t WateringController::activeZone() const { return activeZone_; }

bool WateringController::mainTankLow() const { return mainTankLow_; }

uint16_t WateringController::readAveragedRaw(uint8_t pin) {
  // The first conversion after changing ADC channels can contain residue.
  analogRead(pin);
  delayMicroseconds(100);

  uint32_t total = 0;
  for (uint8_t sample = 0; sample < SAMPLES_PER_READING; ++sample) {
    total += analogRead(pin);
    delayMicroseconds(50);
  }
  return total / SAMPLES_PER_READING;
}

void WateringController::updateMainTankLevel(uint32_t now) {
  const bool observedLow = digitalRead(mainTankLevelPin_) == LOW;
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
  lastSampleAt_ = now;
  hasSampled_ = true;

  const bool tankPinHigh = digitalRead(mainTankLevelPin_) == HIGH;
  Serial.printf("Main tank GPIO %u: pin=%s, state=%s\n", mainTankLevelPin_,
                tankPinHigh ? "HIGH/open" : "LOW/closed",
                mainTankLow_ ? "LOW WATER (watering blocked)" : "ready");
  Serial.println("Moisture sensor readings:");
  for (size_t zone = 0; zone < ZONE_COUNT; ++zone) {
    SensorState &sensor = sensors_[zone];
    const uint16_t raw = readAveragedRaw(sensorPins_[zone]);
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
}

void WateringController::evaluateZoneAfterSample(size_t zone, uint32_t now) {
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
  if (activeZone_ >= 0 || mainTankLow_) {
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

  if (on && (zones_[zone].paused || mainTankLow_ ||
             !config_.zones[zone].enabled ||
             zones_[zone].fault != FaultCode::None ||
             !sensors_[zone].valid ||
             sensors_[zone].validSampleCount < REQUIRED_STARTUP_SAMPLES)) {
    return;
  }
  if (on && activeZone_ >= 0 && activeZone_ != static_cast<int8_t>(zone)) {
    return;
  }

  digitalWrite(relayPins_[zone], on ? HIGH : LOW);
  zones_[zone].relayOn = on;
  if (on) {
    activeZone_ = static_cast<int8_t>(zone);
    armRelaySafetyTimer(zone, zones_[zone].activePulseMs);
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

void WateringController::armRelaySafetyTimer(size_t zone,
                                             uint32_t durationMs) {
  if (relaySafetyTimer_ == nullptr || durationMs == 0) {
    return;
  }

  esp_timer_stop(relaySafetyTimer_);
  relaySafetyCutoffTriggered_ = false;
  relaySafetyCutoffZone_ = static_cast<int8_t>(zone);
  esp_timer_start_once(relaySafetyTimer_,
                       static_cast<uint64_t>(durationMs) * 1000ULL);
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
  if (zone >= 0 && zone < static_cast<int8_t>(ZONE_COUNT)) {
    // This callback runs independently of network processing.
    digitalWrite(controller->relayPins_[zone], LOW);
    controller->relaySafetyCutoffTriggered_ = true;
  }
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
