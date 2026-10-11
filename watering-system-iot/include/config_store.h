#pragma once

#include "watering_types.h"

namespace watering {

class ConfigStore {
 public:
  bool load(SystemConfig &config);
  bool save(const SystemConfig &config);
  // A missing pause record is a successful load of the initial, unpaused mask.
  // A storage error/corrupt record is a failure so the controller can fail safe.
  bool loadPausedZones(uint8_t &mask);
  bool savePausedZones(uint8_t mask);
  bool loadTelegramDebugEnabled(bool &enabled);
  bool saveTelegramDebugEnabled(bool enabled);
  bool loadTelegramHourlySilent(bool &enabled);
  bool saveTelegramHourlySilent(bool enabled);

 private:
  static uint32_t checksum(const uint8_t *data, size_t length);
};

}  // namespace watering
