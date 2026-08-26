#pragma once

#include "watering_types.h"

namespace watering {

class ConfigStore {
 public:
  bool load(SystemConfig &config);
  bool save(const SystemConfig &config);

 private:
  static uint32_t checksum(const uint8_t *data, size_t length);
};

}  // namespace watering
