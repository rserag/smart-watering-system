#pragma once

#include <cassert>

#include "display_test_state.h"
#include "display_manager.h"
#include "telegram_notifier.h"

namespace watering {
class NetworkManager {
 public:
  NetworkManager(WateringController &controller, ConfigStore &, SystemConfig &,
                 TelegramNotifier &, DisplayManager &display)
      : controller_(controller), display_(display) {}
  void begin() {}
  bool wifiConnected() const { return true; }
  bool backendConnected() const { return true; }
  void loop(uint32_t) {
    controller_test::now += display_test::networkDelayMs;
    if (!display_test::startOnNextNetworkLoop) return;
    display_test::startOnNextNetworkLoop = false;
    const char *error = nullptr;
    assert(controller_.startDebugSession(DebugMode::Simulation,
                                         display_test::sessionId, display_test::owner,
                                         millis(), error));
    assert(display_.startTest("checkerboard", 100, display_test::requestId,
                              display_test::sessionId, error));
    // Model the blocking ACK before the real handler emits its started event.
    controller_test::now += display_test::ackDelayMs;
    display_test::startedEventUptime = display_.testStartedAt();
  }

 private:
  WateringController &controller_;
  DisplayManager &display_;
};
}
