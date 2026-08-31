#pragma once

#include "watering_types.h"

namespace watering {

class ConfigStore {
 public:
  bool load(SystemConfig &config);
  bool save(const SystemConfig &config);
  bool loadTelegramDebugEnabled(bool &enabled);
  bool saveTelegramDebugEnabled(bool enabled);

 private:
  static uint32_t checksum(const uint8_t *data, size_t length);
};

}  // namespace watering
