#include <Arduino.h>

#include "config_store.h"
#include "network_manager.h"
#include "watering_controller.h"
#include "watering_types.h"

namespace {

constexpr uint8_t RELAY_PINS[watering::ZONE_COUNT] = {27, 26, 25, 33};
constexpr uint8_t SENSOR_PINS[watering::ZONE_COUNT] = {34, 35, 36, 39};

watering::SystemConfig systemConfig;
watering::ConfigStore configStore;
watering::WateringController controller(RELAY_PINS, SENSOR_PINS);
watering::NetworkManager network(controller, configStore, systemConfig);

}  // namespace

void setup() {
  Serial.begin(115200);

  systemConfig = watering::makeDefaultConfig();
  controller.begin(systemConfig);

  watering::SystemConfig savedConfig;
  if (configStore.load(savedConfig)) {
    systemConfig = savedConfig;
    controller.applyConfig(systemConfig, millis());
    Serial.printf("Loaded saved configuration revision %lu\n",
                  static_cast<unsigned long>(systemConfig.revision));
  } else {
    Serial.println("Using safe default watering configuration");
  }

  Serial.printf("Automatic watering: %s\n",
                systemConfig.automaticWateringEnabled ? "enabled" : "disabled");
  for (size_t zone = 0; zone < watering::ZONE_COUNT; ++zone) {
    Serial.printf("Zone %u: %s\n", static_cast<unsigned>(zone + 1),
                  systemConfig.zones[zone].enabled ? "enabled" : "disabled");
  }

  network.begin();
}

void loop() {
  const uint32_t now = millis();
  controller.loop(now);
  network.loop(now);
  delay(5);  // Yield to the Wi-Fi stack without blocking control timing.
}
