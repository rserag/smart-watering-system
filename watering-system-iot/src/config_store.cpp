#include "config_store.h"

#include <Preferences.h>

namespace watering {

namespace {

constexpr uint32_t STORAGE_MAGIC = 0x57415452;  // "WATR"
constexpr char STORAGE_NAMESPACE[] = "watering";
constexpr char STORAGE_KEY[] = "config";
constexpr char TELEGRAM_DEBUG_KEY[] = "tg_debug";

struct StoredConfig {
  uint32_t magic;
  uint16_t recordSize;
  SystemConfig config;
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
