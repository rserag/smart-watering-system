#include "config_store.h"

#include <Preferences.h>

namespace watering {

namespace {

constexpr uint32_t STORAGE_MAGIC = 0x57415452;  // "WATR"
constexpr char STORAGE_NAMESPACE[] = "watering";
constexpr char STORAGE_KEY[] = "config";
constexpr char TELEGRAM_DEBUG_KEY[] = "tg_debug";
constexpr char TELEGRAM_HOURLY_SILENT_KEY[] = "tg_hr_silent";
constexpr char PAUSED_ZONES_KEY[] = "paused_zones";
constexpr char AUTOMATION_HELD_KEY[] = "debug_hold";
constexpr uint32_t PAUSED_ZONES_MAGIC = 0x50415553;  // "PAUS"
constexpr uint8_t VALID_PAUSED_ZONES_MASK = (1u << ZONE_COUNT) - 1u;

struct StoredConfig {
  uint32_t magic;
  uint16_t recordSize;
  SystemConfig config;
  uint32_t checksum;
};

struct StoredPausedZones {
  uint32_t magic;
  uint8_t mask;
  uint8_t reserved[3];
  uint32_t checksum;
};

}  // namespace

bool ConfigStore::load(SystemConfig &config) {
  Preferences preferences;
  if (!preferences.begin(STORAGE_NAMESPACE, true)) {
    return false;
  }

  StoredConfig stored{};
  const size_t storedLength = preferences.getBytesLength(STORAGE_KEY);
  const size_t bytesRead =
      storedLength == sizeof(stored)
          ? preferences.getBytes(STORAGE_KEY, &stored, sizeof(stored))
          : 0;
  preferences.end();

  if (bytesRead != sizeof(stored) || stored.magic != STORAGE_MAGIC ||
      stored.recordSize != sizeof(stored)) {
    return false;
  }

  const uint32_t expected = checksum(
      reinterpret_cast<const uint8_t *>(&stored.config), sizeof(stored.config));
  if (expected != stored.checksum) {
    return false;
  }

  const ValidationResult validation = validateConfig(stored.config);
  if (!validation.valid) {
    return false;
  }

  config = stored.config;
  return true;
}

bool ConfigStore::save(const SystemConfig &config) {
  if (!validateConfig(config).valid) {
    return false;
  }

  StoredConfig stored{};
  stored.magic = STORAGE_MAGIC;
  stored.recordSize = sizeof(stored);
  stored.config = config;
  stored.checksum = checksum(
      reinterpret_cast<const uint8_t *>(&stored.config), sizeof(stored.config));

  Preferences preferences;
  if (!preferences.begin(STORAGE_NAMESPACE, false)) {
    return false;
  }
  const size_t bytesWritten =
      preferences.putBytes(STORAGE_KEY, &stored, sizeof(stored));
  preferences.end();
  return bytesWritten == sizeof(stored);
}

bool ConfigStore::loadPausedZones(uint8_t &mask) {
  Preferences preferences;
  // Opening read/write creates the namespace on the first boot, allowing a
  // missing pause record to be distinguished from inaccessible storage.
  if (!preferences.begin(STORAGE_NAMESPACE, false)) {
    return false;
  }
  if (!preferences.isKey(PAUSED_ZONES_KEY)) {
    preferences.end();
    mask = 0;
    return true;
  }

  StoredPausedZones stored{};
  const size_t length = preferences.getBytesLength(PAUSED_ZONES_KEY);
  const size_t bytesRead = length == sizeof(stored)
                               ? preferences.getBytes(PAUSED_ZONES_KEY, &stored,
                                                      sizeof(stored))
                               : 0;
  preferences.end();
  if (bytesRead != sizeof(stored) || stored.magic != PAUSED_ZONES_MAGIC ||
      (stored.mask & ~VALID_PAUSED_ZONES_MASK) != 0 ||
      stored.checksum != checksum(reinterpret_cast<const uint8_t *>(&stored),
                                  offsetof(StoredPausedZones, checksum))) {
    return false;
  }
  mask = stored.mask;
  return true;
}

bool ConfigStore::savePausedZones(uint8_t mask) {
  if ((mask & ~VALID_PAUSED_ZONES_MASK) != 0) {
    return false;
  }
  StoredPausedZones stored{};
  stored.magic = PAUSED_ZONES_MAGIC;
  stored.mask = mask;
  stored.checksum = checksum(reinterpret_cast<const uint8_t *>(&stored),
                             offsetof(StoredPausedZones, checksum));
  Preferences preferences;
  if (!preferences.begin(STORAGE_NAMESPACE, false)) {
    return false;
  }
  const size_t bytesWritten =
      preferences.putBytes(PAUSED_ZONES_KEY, &stored, sizeof(stored));
  preferences.end();
  return bytesWritten == sizeof(stored);
}

bool ConfigStore::loadAutomationHeld(bool &held) {
  Preferences preferences;
  if (!preferences.begin(STORAGE_NAMESPACE, false)) return false;
  if (!preferences.isKey(AUTOMATION_HELD_KEY)) {
    held = false;
    preferences.end();
    return true;
  }
  uint8_t value = 0;
  const bool valid = preferences.getBytesLength(AUTOMATION_HELD_KEY) == 1 &&
                     preferences.getBytes(AUTOMATION_HELD_KEY, &value, 1) == 1 &&
                     value <= 1;
  preferences.end();
  if (valid) held = value != 0;
  return valid;
}

bool ConfigStore::saveAutomationHeld(bool held) {
  Preferences preferences;
  if (!preferences.begin(STORAGE_NAMESPACE, false)) return false;
  const uint8_t value = held ? 1 : 0;
  const bool success = preferences.putBytes(AUTOMATION_HELD_KEY, &value, 1) == 1;
  preferences.end();
  return success;
}

bool ConfigStore::loadTelegramDebugEnabled(bool &enabled) {
  Preferences preferences;
  if (!preferences.begin(STORAGE_NAMESPACE, true)) {
    return false;
  }
  const bool present = preferences.isKey(TELEGRAM_DEBUG_KEY);
  if (present) {
    enabled = preferences.getBool(TELEGRAM_DEBUG_KEY, false);
  }
  preferences.end();
  return present;
}

bool ConfigStore::saveTelegramDebugEnabled(bool enabled) {
  Preferences preferences;
  if (!preferences.begin(STORAGE_NAMESPACE, false)) {
    return false;
  }
  const size_t bytesWritten =
      preferences.putBool(TELEGRAM_DEBUG_KEY, enabled);
  preferences.end();
  return bytesWritten == sizeof(uint8_t);
}

bool ConfigStore::loadTelegramHourlySilent(bool &enabled) {
  Preferences preferences;
  if (!preferences.begin(STORAGE_NAMESPACE, true)) {
    return false;
  }
  const bool present = preferences.isKey(TELEGRAM_HOURLY_SILENT_KEY);
  if (present) {
    enabled = preferences.getBool(TELEGRAM_HOURLY_SILENT_KEY, false);
  }
  preferences.end();
  return present;
}

bool ConfigStore::saveTelegramHourlySilent(bool enabled) {
  Preferences preferences;
  if (!preferences.begin(STORAGE_NAMESPACE, false)) {
    return false;
  }
  const size_t bytesWritten = preferences.putBool(TELEGRAM_HOURLY_SILENT_KEY, enabled);
  preferences.end();
  return bytesWritten == sizeof(uint8_t);
}

uint32_t ConfigStore::checksum(const uint8_t *data, size_t length) {
  // FNV-1a is sufficient here to detect an incomplete or corrupted NVS record.
  uint32_t value = 2166136261u;
  for (size_t index = 0; index < length; ++index) {
    value ^= data[index];
    value *= 16777619u;
  }
  return value;
}

}  // namespace watering
