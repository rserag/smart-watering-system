#include "watering_types.h"

#include <cstdlib>
#include <cstring>

namespace watering {

namespace {

ValidationResult invalid(const char *message) {
  ValidationResult result{};
  result.valid = false;
  strlcpy(result.message, message, sizeof(result.message));
  return result;
}

}  // namespace

SystemConfig makeDefaultConfig() {
  SystemConfig config{};
  config.schemaVersion = CONFIG_SCHEMA_VERSION;
  config.revision = 1;
  config.automaticWateringEnabled = true;
  config.sampleIntervalMs = 5000;
  config.telemetryIntervalMs = 5000;
  config.maxConcurrentZones = 1;

  for (size_t index = 0; index < ZONE_COUNT; ++index) {
    ZoneConfig &zone = config.zones[index];
    zone.enabled = index <= 2;
    zone.dryRaw = 2600;
    zone.wetRaw = 1250;
    zone.startWateringPercent = 30;
    zone.stopWateringPercent = 63;
    zone.dryConfirmationSamples = 3;
    zone.wetConfirmationSamples = 2;
    zone.pulseOnMs = 8000;
    zone.soakMs = 90000;
    zone.maxWateringOnMsPerCycle = 60000;
    zone.cooldownMs = 1800000;
  }

  return config;
}

ValidationResult validateConfig(const SystemConfig &config) {
  if (config.schemaVersion != CONFIG_SCHEMA_VERSION) {
    return invalid("unsupported schemaVersion");
  }
  if (config.revision == 0) {
    return invalid("revision must be greater than zero");
  }
  if (config.sampleIntervalMs < MIN_SAMPLE_INTERVAL_MS ||
      config.sampleIntervalMs > MAX_SAMPLE_INTERVAL_MS) {
    return invalid("sampleIntervalMs is outside the safe range");
  }
  if (config.telemetryIntervalMs < MIN_TELEMETRY_INTERVAL_MS ||
      config.telemetryIntervalMs > MAX_TELEMETRY_INTERVAL_MS) {
    return invalid("telemetryIntervalMs is outside the safe range");
  }
  if (config.maxConcurrentZones != 1) {
    return invalid("this device permits exactly one active zone");
  }

  for (size_t index = 0; index < ZONE_COUNT; ++index) {
    const ZoneConfig &zone = config.zones[index];
    if (zone.dryRaw < 0 || zone.dryRaw > 4095 || zone.wetRaw < 0 ||
        zone.wetRaw > 4095) {
      return invalid("a calibration value is outside the ADC range");
    }
    if (abs(zone.dryRaw - zone.wetRaw) < MIN_CALIBRATION_SPAN) {
      return invalid("a calibration span is too small");
    }
    if (zone.startWateringPercent > 100 ||
        zone.stopWateringPercent > 100 ||
        zone.startWateringPercent >= zone.stopWateringPercent) {
      return invalid("watering percentage thresholds are invalid");
    }
    if (zone.dryConfirmationSamples == 0 ||
        zone.dryConfirmationSamples > 12 ||
        zone.wetConfirmationSamples == 0 ||
        zone.wetConfirmationSamples > 12) {
      return invalid("confirmation sample count is outside the safe range");
    }
    if (zone.pulseOnMs < MIN_PULSE_MS || zone.pulseOnMs > MAX_PULSE_MS) {
      return invalid("pulseOnMs is outside the safe range");
    }
    if (zone.soakMs < MIN_SOAK_MS || zone.soakMs > MAX_SOAK_MS) {
      return invalid("soakMs is outside the safe range");
    }
    if (zone.maxWateringOnMsPerCycle < zone.pulseOnMs ||
        zone.maxWateringOnMsPerCycle > MAX_TOTAL_WATERING_MS) {
      return invalid("maxWateringOnMsPerCycle is outside the safe range");
    }
    if (zone.cooldownMs > MAX_COOLDOWN_MS) {
      return invalid("cooldownMs is outside the safe range");
    }
  }

  ValidationResult result{};
  result.valid = true;
  strlcpy(result.message, "valid", sizeof(result.message));
  return result;
}

int calculateMoisturePercent(int raw, int dryRaw, int wetRaw) {
  if (dryRaw == wetRaw) {
    return 0;
  }

  const float percent =
      100.0f * static_cast<float>(raw - dryRaw) /
      static_cast<float>(wetRaw - dryRaw);
  return constrain(static_cast<int>(percent + 0.5f), 0, 100);
}

const char *zonePhaseName(ZonePhase phase) {
  switch (phase) {
    case ZonePhase::Disabled:
      return "disabled";
    case ZonePhase::Stabilizing:
      return "stabilizing";
    case ZonePhase::Monitoring:
      return "monitoring";
    case ZonePhase::DryConfirming:
      return "dry_confirming";
    case ZonePhase::Queued:
      return "queued";
    case ZonePhase::Watering:
      return "watering";
    case ZonePhase::ManualWatering:
      return "manual_watering";
    case ZonePhase::Soaking:
      return "soaking";
    case ZonePhase::Cooldown:
      return "cooldown";
    case ZonePhase::Fault:
      return "fault";
  }
  return "unknown";
}

const char *faultCodeName(FaultCode fault) {
  switch (fault) {
    case FaultCode::None:
      return "none";
    case FaultCode::SensorInvalid:
      return "sensor_invalid";
    case FaultCode::MaximumWateringReached:
      return "maximum_watering_reached";
    case FaultCode::EmergencyStop:
      return "emergency_stop";
  }
  return "unknown";
}

}  // namespace watering
