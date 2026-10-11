#pragma once

#include "display_test_state.h"
#include "config_store.h"
#include "watering_controller.h"

namespace watering {
class TelegramNotifier {
 public:
  TelegramNotifier(WateringController &, ConfigStore &) {}
  void begin(bool, bool) {}
  void loop(uint32_t now, bool) {
    display_test::telegramReceivedAt = now;
    controller_test::now += display_test::telegramDelayMs;
  }
};
}
