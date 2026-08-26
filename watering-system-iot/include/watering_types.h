#pragma once

#include <Arduino.h>

namespace watering {

constexpr size_t ZONE_COUNT = 4;
constexpr uint16_t CONFIG_SCHEMA_VERSION = 1;

constexpr uint16_t ADC_MIN_VALID = 50;
constexpr uint16_t ADC_MAX_VALID = 4050;
constexpr uint16_t MIN_CALIBRATION_SPAN = 300;

constexpr uint32_t MIN_SAMPLE_INTERVAL_MS = 1000;
constexpr uint32_t MAX_SAMPLE_INTERVAL_MS = 60000;
constexpr uint32_t MIN_TELEMETRY_INTERVAL_MS = 1000;
constexpr uint32_t MAX_TELEMETRY_INTERVAL_MS = 300000;
constexpr uint32_t MIN_PULSE_MS = 1000;
constexpr uint32_t MAX_PULSE_MS = 60000;
constexpr uint32_t MIN_SOAK_MS = 10000;
constexpr uint32_t MAX_SOAK_MS = 1800000;
constexpr uint32_t MAX_TOTAL_WATERING_MS = 180000;
constexpr uint32_t MAX_COOLDOWN_MS = 86400000;

enum class ZonePhase : uint8_t {
  Disabled,
  Stabilizing,
  Monitoring,
  DryConfirming,
  Queued,
  Watering,
  ManualWatering,
  Soaking,
  Cooldown,
  Fault,
};

enum class FaultCode : uint8_t {
  None,
  SensorInvalid,
  MaximumWateringReached,
  EmergencyStop,
};

struct ZoneConfig {
  bool enabled;
  int16_t dryRaw;
  int16_t wetRaw;
  uint8_t startWateringPercent;
  uint8_t stopWateringPercent;
  uint8_t dryConfirmationSamples;
  uint8_t wetConfirmationSamples;
  uint32_t pulseOnMs;
  uint32_t soakMs;
  uint32_t maxWateringOnMsPerCycle;
  uint32_t cooldownMs;
};

struct SystemConfig {
  uint16_t schemaVersion;
  uint32_t revision;
  bool automaticWateringEnabled;
  uint32_t sampleIntervalMs;
  uint32_t telemetryIntervalMs;
  uint8_t maxConcurrentZones;
  ZoneConfig zones[ZONE_COUNT];
};

struct SensorState {
  uint16_t latestRaw;
  uint16_t filteredRaw;
  uint8_t moisturePercent;
  uint8_t validSampleCount;
  uint8_t invalidSampleCount;
  uint32_t lastReadingAt;
  bool valid;
};

struct ZoneState {
  ZonePhase phase;
  FaultCode fault;
  bool relayOn;
  bool manualRequest;
  uint8_t consecutiveDryReadings;
  uint8_t consecutiveWetReadings;
  uint32_t phaseStartedAt;
  uint32_t wateringOnMsThisCycle;
  uint32_t activePulseMs;
  uint32_t manualDurationMs;
};

struct ValidationResult {
  bool valid;
  char message[128];
};

SystemConfig makeDefaultConfig();
ValidationResult validateConfig(const SystemConfig &config);
int calculateMoisturePercent(int raw, int dryRaw, int wetRaw);
const char *zonePhaseName(ZonePhase phase);
const char *faultCodeName(FaultCode fault);

}  // namespace watering
