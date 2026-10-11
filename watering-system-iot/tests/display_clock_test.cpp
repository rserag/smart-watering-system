#include <cassert>
#include <cstring>
#include <iostream>
#include <limits>

// Exercise the actual main-loop service order with only transport/hardware
// substituted. The real display and controller own the test and session clocks.
#ifndef WATERING_MAIN_SOURCE
#define WATERING_MAIN_SOURCE "../src/main.cpp"
#endif
#include WATERING_MAIN_SOURCE

namespace {
void expectPhysicalOutputsOff() {
  for (uint8_t pin : RELAY_PINS) assert(controller_test::digital[pin] == LOW);
  assert(controller_test::relayHighWrites == 0);
}

void testNetworkStartsOled(uint32_t initialTime) {
  controller_test::now = initialTime;
  display_test::networkDelayMs = 5;
  display_test::renderDelayMs = 250;  // Longer than the requested visible interval.
  display_test::ackDelayMs = 20;
  display_test::telegramDelayMs = 20;
  display_test::startOnNextNetworkLoop = true;
  const unsigned rendersBefore = display_test::renderCount;
  ::loop();

  const uint32_t expectedStart = initialTime + 5 + 250;
  assert(display.testStartedAt() == expectedStart);
  assert(display_test::startedEventUptime == expectedStart);
  // A stale pre-network timestamp used here would underflow both the newly
  // created session lease and the OLED duration, immediately ending the test.
  watering::DebugActionEvent event{};
  assert(!display.consumeTestEvent(event));
  assert(display_test::telegramReceivedAt == expectedStart + 20);
  assert(display_test::renderCount == rendersBefore + 1);
  assert(controller.debugSessionActive());
  assert(controller.debugLeaseRemainingMs(millis()) > 0);
  expectPhysicalOutputsOff();

  controller_test::now = expectedStart + 99;
  display.update(controller, true, true, millis());
  assert(!display.consumeTestEvent(event));
  controller_test::now = expectedStart + 100;
  display.update(controller, true, true, millis());
  assert(display.consumeTestEvent(event));
  assert(std::strcmp(event.status, "completed") == 0);
  assert(std::strcmp(event.requestId, display_test::requestId) == 0);
  assert(std::strcmp(event.sessionId, display_test::sessionId) == 0);
  assert(event.uptimeMs - display_test::startedEventUptime == 100);
  assert(!display.consumeTestEvent(event));
  expectPhysicalOutputsOff();
  controller.endDebugSession(millis());
}

void testCancellation() {
  controller_test::now += 1000;
  const char *error = nullptr;
  assert(controller.startDebugSession(watering::DebugMode::Simulation,
                                      display_test::sessionId, display_test::owner,
                                      millis(), error));
  assert(display.startTest("off", 500, display_test::requestId,
                            display_test::sessionId, error));
  controller.endDebugSession(millis());
  display.update(controller, true, true, millis());
  watering::DebugActionEvent event{};
  assert(display.consumeTestEvent(event));
  assert(std::strcmp(event.status, "cancelled") == 0);
  assert(!display.consumeTestEvent(event));
  assert(controller.automationHeld());
  expectPhysicalOutputsOff();
}
}

int main() {
  controller_test::now = 1000;
  controller_test::digital[MAIN_TANK_LEVEL_PIN] = HIGH;
  for (uint8_t pin : SENSOR_PINS) controller_test::analog[pin] = 1250;
  setup();
  testNetworkStartsOled(1000);
  // Unsigned elapsed-time arithmetic must also survive millis rollover.
  testNetworkStartsOled(std::numeric_limits<uint32_t>::max() - 150);
  testNetworkStartsOled(std::numeric_limits<uint32_t>::max() - 350);
  testCancellation();
  std::cout << "Display/main-loop clock regressions passed\n";
}
