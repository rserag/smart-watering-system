#include <cassert>
#include <iostream>
#include <cstring>

#include "Preferences.h"
#include "config_store.h"
#include "watering_controller.h"

using namespace watering;
namespace {
constexpr uint8_t relays[ZONE_COUNT] = {1, 2, 3, 4};
constexpr uint8_t soils[ZONE_COUNT] = {11, 12, 13, 14};
constexpr uint8_t tank = 20;
constexpr char sessionId[] = "12345678-1234-1234-1234-123456789abc";
constexpr char otherSession[] = "87654321-1234-1234-1234-123456789abc";
constexpr char owner[] = "sahakyan.raphayel@gmail.com";

void reset() {
  controller_test::now = 0;
  controller_test::relayHighWrites = 0;
  controller_test::preferencesOpenFails = false;
  controller_test::preferencesWriteFails = false;
  controller_test::storedPreferences.clear();
  controller_test::timers.clear();
  controller_test::timerCreateFails = false;
  controller_test::timerStartFails = false;
  controller_test::timerFailName = nullptr;
  for (uint8_t pin : soils) controller_test::analog[pin] = 1250;
  controller_test::digital[tank] = HIGH;
}
SystemConfig config(bool enabled = false) {
  SystemConfig value = makeDefaultConfig();
  value.sampleIntervalMs = 1000;
  for (size_t zone = 0; zone < ZONE_COUNT; ++zone) {
    value.zones[zone].enabled = enabled && zone == 0;
    value.zones[zone].dryConfirmationSamples = 1;
    value.zones[zone].wetConfirmationSamples = 1;
    value.zones[zone].pulseOnMs = 1000;
    value.zones[zone].soakMs = 10000;
    value.zones[zone].maxWateringOnMsPerCycle = 1000;
  }
  return value;
}
void advance(uint32_t duration) {
  controller_test::now += duration;
  controller_test::fireTimers();
}
void tick(WateringController &controller, uint32_t duration = 1000) {
  advance(duration);
  controller.loop(controller_test::now);
}
void off() {
  for (uint8_t pin : relays) assert(controller_test::digital[pin] == LOW);
}
void start(WateringController &controller, DebugMode mode) {
  const char *error = nullptr;
  assert(controller.startDebugSession(mode, sessionId, owner, millis(), error));
  assert(error == nullptr);
  assert(controller.automationHeld());
}

void testHardwareBypassAndLifecycle() {
  reset();
  ConfigStore store;
  assert(store.savePausedZones(1));
  WateringController controller(relays, soils, tank);
  controller.begin(config(), &store);
  controller.emergencyStop(millis());
  const uint32_t generation = controller.zoneState(0).recoveryGeneration;
  start(controller, DebugMode::Hardware);
  const char *error = nullptr;
  assert(!controller.startDebugSession(DebugMode::Hardware, otherSession, owner, millis(), error));
  assert(!controller.requestManualWater(0, 1000, millis(), error));
  assert(!controller.resumeAutomation(millis(), error));
  assert(!controller.setDebugSoil(0, 2000, millis(), error));
  assert(controller.debugPulse(0, 100, "pulse-one", millis(), error));
  assert(controller_test::digital[relays[0]] == HIGH);
  assert(controller.zoneState(0).paused);
  assert(controller.zoneState(0).fault == FaultCode::EmergencyStop);
  assert(!controller.debugPulse(1, 100, "pulse-two", millis(), error));
  advance(100); // Independent callback executes without controller/network loop.
  off();
  assert(!controller.physicalRelayOn(0));
  controller.loop(millis());
  DebugActionEvent event{};
  assert(controller.consumeDebugAction(event));
  assert(std::strcmp(event.status, "started") == 0);
  assert(std::strcmp(event.requestId, "pulse-one") == 0);
  assert(controller.consumeDebugAction(event));
  assert(std::strcmp(event.status, "completed") == 0);
  assert(std::strcmp(event.requestId, "pulse-one") == 0);
  assert(!controller.consumeDebugAction(event));
  assert(controller.zoneState(0).recoveryGeneration == generation);
  controller.endDebugSession(millis());
  assert(controller.automationHeld());
  assert(controller.resumeAutomation(millis(), error));
  assert(controller.zoneState(0).paused);
  assert(controller.zoneState(0).fault == FaultCode::EmergencyStop);
  assert(!controller.sensorState(0).valid);
}

void testLeaseCutoffAndBootHold() {
  reset();
  ConfigStore store;
  WateringController controller(relays, soils, tank);
  controller.begin(config(), &store);
  start(controller, DebugMode::Hardware);
  const char *error = nullptr;
  assert(controller.debugPulse(0, 60000, "long-pulse", millis(), error));
  advance(30000); // Simulate network/TLS blocking the main loop.
  off();
  assert(controller.debugLeaseRemainingMs(millis()) == 0);
  assert(!controller.renewDebugSession(sessionId, millis(), error));
  assert(controller.debugMode() == DebugMode::Observe);
  assert(controller.automationHeld());
  WateringController reboot(relays, soils, tank);
  reboot.begin(config(true), &store);
  assert(reboot.automationHeld());
  for (uint8_t pin : soils) controller_test::analog[pin] = 2600;
  for (unsigned count = 0; count < 10; ++count) tick(reboot);
  off();
  assert(!reboot.requestManualWater(0, 1000, millis(), error));
  assert(reboot.resumeAutomation(millis(), error));
  assert(!reboot.sensorState(0).valid);
  assert(!reboot.automationHeld());
}

void testPhysicalTankTimerAfterRenewal() {
  reset();
  ConfigStore store;
  WateringController controller(relays, soils, tank);
  controller.begin(config(), &store);
  start(controller, DebugMode::Hardware);
  const char *error = nullptr;
  tick(controller, 5000);
  assert(controller.renewDebugSession(sessionId, millis(), error));
  assert(controller.debugPulse(0, 60000, "tank-pulse", millis(), error));
  controller_test::digital[tank] = LOW;
  advance(10); // Tank timer, independent of loop/debounce/network handling.
  off();
  controller.loop(millis());
  assert(!controller.physicalRelayOn(0));
  assert(!controller.debugPulse(1, 100, "low-pulse", millis(), error));
  tick(controller, 250);
  assert(controller.mainTankLow());
  controller_test::digital[tank] = HIGH;
  tick(controller, 10);
  assert(!controller.debugPulse(1, 100, "early-restore", millis(), error));
  tick(controller, 2000);
  assert(!controller.mainTankLow());
  assert(controller.debugPulse(1, 100, "restored-pulse", millis(), error));
  controller.endDebugSession(millis(), "test ended");
  off();
}

void testSimulationPipelineAndLiveIsolation() {
  reset();
  ConfigStore store;
  assert(store.savePausedZones(2));
  const SystemConfig original = config(true);
  WateringController controller(relays, soils, tank);
  controller.begin(original, &store);
  controller.emergencyStop(millis());
  const uint32_t generation = controller.zoneState(0).recoveryGeneration;
  start(controller, DebugMode::Simulation);
  const char *error = nullptr;
  assert(controller.setDebugSoil(0, 2600, millis(), error));
  for (unsigned count = 0; count < 4; ++count) tick(controller);
  assert(controller.virtualRelayOn(0));
  assert(!controller.physicalRelayOn(0));
  assert(controller_test::relayHighWrites == 0);
  off();
  const SensorState &sensor = controller.sensorState(0);
  assert(sensor.physicalRaw == 1250 && sensor.latestRaw == 2600);
  assert(sensor.filteredRaw == 2600 && sensor.moisturePercent == 0);
  assert(sensor.minRaw == 1250 && sensor.maxRaw == 1250);
  for (uint16_t sample : sensor.samples) assert(sample == 1250);
  assert(controller.liveSensorState(0).latestRaw != 2600);
  assert(controller.liveZoneState(0).fault == FaultCode::EmergencyStop);
  PumpStartEvent pump{};
  assert(!controller.consumePumpStarted(pump));
  assert(controller.setDebugSoil(0, 0, millis(), error));
  controller.debugSample(millis());
  controller.debugSample(millis());
  controller.debugSample(millis());
  assert(controller.zoneState(0).fault == FaultCode::SensorInvalid);
  assert(controller.sensorState(0).invalidSampleCount == 3);
  assert(controller.sensorState(0).filteredRaw == 2600); // Visible stale value, validity false.
  controller.endDebugSession(millis());
  assert(controller.zoneState(0).fault == FaultCode::EmergencyStop);
  assert(controller.zoneState(0).recoveryGeneration == generation);
  assert(controller.zoneState(1).paused);
  assert(!controller.sensorState(0).valid);
  assert(controller.soilOverride(0) == -1);
  assert(std::memcmp(&controller.config(), &original, sizeof(original)) == 0);
  assert(controller_test::relayHighWrites == 0);
  off();
}

void testInjectedTankDebounceAndVirtualPulse() {
  reset();
  controller_test::digital[tank] = LOW;
  ConfigStore store;
  WateringController controller(relays, soils, tank);
  controller.begin(config(), &store);
  start(controller, DebugMode::Simulation);
  const char *error = nullptr;
  assert(controller.setDebugTank(false, millis(), error));
  assert(controller.physicalTankLow());
  assert(!controller.effectiveTankLow());
  assert(controller.mainTankLow());
  assert(controller.tankDebounceRemainingMs(millis()) == 2000);
  tick(controller, 2000);
  assert(!controller.mainTankLow());
  assert(controller.debugPulse(3, 5000, "virtual", millis(), error));
  assert(controller.virtualRelayOn(3));
  off();
  assert(controller.clearDebugInput("tank", -1, millis(), error));
  assert(controller.effectiveTankLow());
  assert(controller.tankDebounceRemainingMs(millis()) == 250);
  tick(controller, 250);
  assert(controller.mainTankLow());
  assert(!controller.virtualRelayOn(3));
  assert(controller_test::relayHighWrites == 0);
  controller.endDebugSession(millis());
  assert(controller.mainTankLow());
}

void testTimerAndStorageFailures() {
  reset();
  ConfigStore store;
  WateringController controller(relays, soils, tank);
  controller.begin(config(), &store);
  start(controller, DebugMode::Hardware);
  const char *error = nullptr;
  controller_test::timerFailName = "relay_cutoff";
  assert(!controller.debugPulse(0, 1000, "failed-timer", millis(), error));
  off();
  assert(controller_test::relayHighWrites == 0);
  controller_test::timerFailName = "debug_lease";
  assert(!controller.renewDebugSession(sessionId, millis(), error));
  assert(controller.debugMode() == DebugMode::Observe);
  off();
  controller_test::timerFailName = nullptr;
  controller_test::preferencesWriteFails = true;
  assert(!controller.resumeAutomation(millis(), error));
  assert(controller.automationHeld());
  controller_test::preferencesWriteFails = false;
  assert(controller.resumeAutomation(millis(), error));
  controller_test::preferencesWriteFails = true;
  assert(!controller.debugStop(-1, millis(), error));
  assert(!controller.debugStop(-1, millis(), error));
  off();
  controller_test::preferencesWriteFails = false;
  assert(controller.debugStop(-1, millis(), error));
  WateringController reboot(relays, soils, tank);
  reboot.begin(config(), &store);
  assert(reboot.automationHeld());

  reset();
  controller_test::timerCreateFails = true;
  WateringController failed(relays, soils, tank);
  failed.begin(config(), &store);
  assert(!failed.startDebugSession(DebugMode::Hardware, sessionId, owner, millis(), error));
  assert(failed.automationHeld());
  off();
  reset();
  controller_test::storedPreferences["debug_hold"] = {9};
  WateringController corrupt(relays, soils, tank);
  corrupt.begin(config(true), &store);
  assert(corrupt.automationHeld());
  off();
}

void testConfigAndRecoveryConflicts() {
  reset();
  ConfigStore store;
  WateringController controller(relays, soils, tank);
  const SystemConfig original = config(true);
  controller.begin(original, &store);
  start(controller, DebugMode::Hardware);
  SystemConfig changed = original;
  changed.revision = 2;
  changed.zones[0].dryRaw = 3200;
  controller.prepareForConfigUpdate(millis());
  controller.applyConfig(changed, millis());
  assert(controller.config().revision == original.revision);
  assert(controller.config().zones[0].dryRaw == original.zones[0].dryRaw);
  const char *error = nullptr;
  assert(!controller.clearFaultAndPause(0, controller.zoneState(0).recoveryGeneration, millis(), error));
  assert(!controller.resumeZone(0, controller.zoneState(0).recoveryGeneration, millis(), error));
  controller.endDebugSession(millis());
  controller.applyConfig(changed, millis());
  assert(controller.config().revision == original.revision);
  assert(controller.resumeAutomation(millis(), error));
  controller.applyConfig(changed, millis());
  assert(controller.config().revision == 2);
}
}

int main() {
  testHardwareBypassAndLifecycle();
  testLeaseCutoffAndBootHold();
  testPhysicalTankTimerAfterRenewal();
  testSimulationPipelineAndLiveIsolation();
  testInjectedTankDebounceAndVirtualPulse();
  testTimerAndStorageFailures();
  testConfigAndRecoveryConflicts();
  std::cout << "Controller diagnostics safety tests passed\n";
}
