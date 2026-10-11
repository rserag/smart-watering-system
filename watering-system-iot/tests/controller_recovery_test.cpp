#include <cassert>
#include <iostream>

#include "Preferences.h"
#include "config_store.h"
#include "watering_controller.h"

using namespace watering;

namespace {

constexpr uint8_t relayPins[ZONE_COUNT] = {1, 2, 3, 4};
constexpr uint8_t sensorPins[ZONE_COUNT] = {11, 12, 13, 14};
constexpr uint8_t tankPin = 20;

SystemConfig testConfig() {
  SystemConfig config = makeDefaultConfig();
  config.sampleIntervalMs = 1000;
  for (size_t zone = 0; zone < ZONE_COUNT; ++zone) {
    config.zones[zone].enabled = zone == 0;
    config.zones[zone].dryConfirmationSamples = 1;
    config.zones[zone].wetConfirmationSamples = 1;
    config.zones[zone].pulseOnMs = 1000;
    config.zones[zone].soakMs = 10000;
    config.zones[zone].maxWateringOnMsPerCycle = 1000;
    config.zones[zone].cooldownMs = 1000;
  }
  assert(validateConfig(config).valid);
  return config;
}

void resetEnvironment() {
  controller_test::now = 0;
  controller_test::relayHighWrites = 0;
  controller_test::preferencesOpenFails = false;
  controller_test::preferencesWriteFails = false;
  controller_test::storedPreferences.clear();
  for (uint8_t pin : sensorPins) controller_test::analog[pin] = 2600;
  controller_test::digital[tankPin] = HIGH;
}

void tick(WateringController &controller, unsigned count = 1) {
  for (unsigned index = 0; index < count; ++index) {
    controller_test::now += 1000;
    controller.loop(controller_test::now);
  }
}

void reachLimit(WateringController &controller) {
  for (unsigned index = 0; index < 30 &&
                           controller.zoneState(0).phase != ZonePhase::Fault;
       ++index) {
    tick(controller);
  }
  assert(controller.zoneState(0).fault == FaultCode::MaximumWateringReached);
  assert(!controller.zoneState(0).relayOn);
}

void clearPaused(WateringController &controller) {
  const char *error = nullptr;
  assert(controller.clearFaultAndPause(0, controller.zoneState(0).recoveryGeneration,
                                      controller_test::now, error));
  assert(error == nullptr);
  assert(controller.zoneState(0).paused);
  assert(controller.zoneState(0).phase == ZonePhase::Paused);
  assert(controller.zoneState(0).fault == FaultCode::None);
}

void testDurablePauseAndFreshResume() {
  resetEnvironment();
  ConfigStore store;
  SystemConfig config = testConfig();
  WateringController controller(relayPins, sensorPins, tankPin);
  controller.begin(config, &store);
  assert(controller.zoneState(0).recoveryGeneration != 0);
  reachLimit(controller);
  const char *error = nullptr;
  const uint32_t faultGeneration = controller.zoneState(0).recoveryGeneration;
  assert(!controller.clearFault(0, controller_test::now, error));
  assert(!controller.clearFaultAndPause(0, faultGeneration - 1,
                                       controller_test::now, error));
  clearPaused(controller);
  const uint32_t pausedGeneration = controller.zoneState(0).recoveryGeneration;
  assert(pausedGeneration > faultGeneration);
  assert(!controller.sensorState(0).valid);
  assert(controller.sensorState(0).validSampleCount == 0);
  assert(controller.zoneState(0).consecutiveDryReadings == 0);
  assert(controller.zoneState(0).consecutiveWetReadings == 0);
  assert(controller.zoneState(0).wateringOnMsThisCycle == 0);
  assert(!controller.clearFaultAndPause(0, faultGeneration,
                                       controller_test::now, error));
  const unsigned highWrites = controller_test::relayHighWrites;
  tick(controller, 40);
  assert(controller_test::relayHighWrites == highWrites);
  assert(!controller.requestManualWater(0, 1000, controller_test::now, error));
  assert(controller.stopZone(0, controller_test::now, error));
  assert(controller.zoneState(0).phase == ZonePhase::Paused);

  controller_test::digital[tankPin] = LOW;
  tick(controller, 2);
  assert(controller.mainTankLow());
  assert(!controller.resumeZone(0, pausedGeneration, controller_test::now, error));
  assert(controller.zoneState(0).paused);
  controller_test::digital[tankPin] = HIGH;
  tick(controller, 3);
  assert(!controller.mainTankLow());
  assert(controller.zoneState(0).phase == ZonePhase::Paused);

  controller.prepareForConfigUpdate(controller_test::now);
  assert(controller.zoneState(0).paused);
  assert(controller.zoneState(0).recoveryGeneration > pausedGeneration);
  assert(!controller.resumeZone(0, pausedGeneration, controller_test::now, error));
  config.zones[0].enabled = false;
  controller.applyConfig(config, controller_test::now);
  assert(controller.zoneState(0).phase == ZonePhase::Disabled);
  assert(controller.zoneState(0).paused);
  assert(!controller.resumeZone(0, controller.zoneState(0).recoveryGeneration,
                                controller_test::now, error));
  config.zones[0].enabled = true;
  config.automaticWateringEnabled = false;
  assert(store.save(config));
  controller.applyConfig(config, controller_test::now);
  assert(controller.zoneState(0).phase == ZonePhase::Paused);
  assert(controller_test::relayHighWrites == highWrites);

  WateringController restarted(relayPins, sensorPins, tankPin);
  restarted.begin(config, &store);
  tick(restarted, 4);
  assert(restarted.zoneState(0).paused);
  assert(restarted.zoneState(0).phase == ZonePhase::Paused);
  const uint32_t resumeGeneration = restarted.zoneState(0).recoveryGeneration;
  assert(restarted.resumeZone(0, resumeGeneration, controller_test::now, error));
  assert(restarted.zoneState(0).recoveryGeneration > resumeGeneration);
  assert(restarted.zoneState(0).phase == ZonePhase::Stabilizing);
  assert(!restarted.sensorState(0).valid);
  assert(!restarted.requestManualWater(0, 1000, controller_test::now, error));
  tick(restarted);
  assert(restarted.sensorState(0).validSampleCount == 1);
  assert(!restarted.requestManualWater(0, 1000, controller_test::now, error));
  tick(restarted);
  assert(!restarted.requestManualWater(0, 1000, controller_test::now, error));
  tick(restarted);
  assert(restarted.zoneState(0).phase == ZonePhase::Monitoring);
  assert(restarted.requestManualWater(0, 1000, controller_test::now, error));
  restarted.loop(controller_test::now);
  assert(restarted.zoneState(0).relayOn);
  assert(restarted.zoneState(0).phase == ZonePhase::ManualWatering);
  uint8_t mask = 255;
  assert(store.loadPausedZones(mask) && mask == 0);
}

void testPersistenceFailureAndChannelIsolation() {
  resetEnvironment();
  ConfigStore store;
  assert(store.savePausedZones(0b1010));
  WateringController controller(relayPins, sensorPins, tankPin);
  controller.begin(testConfig(), &store);
  reachLimit(controller);
  const uint32_t generation = controller.zoneState(0).recoveryGeneration;
  const char *error = nullptr;
  controller_test::preferencesWriteFails = true;
  assert(!controller.clearFaultAndPause(0, generation, controller_test::now, error));
  assert(error != nullptr);
  assert(controller.zoneState(0).fault == FaultCode::MaximumWateringReached);
  assert(controller.zoneState(0).recoveryGeneration == generation);
  assert(!controller.zoneState(0).relayOn);
  controller_test::preferencesWriteFails = false;
  clearPaused(controller);
  uint8_t mask = 0;
  assert(store.loadPausedZones(mask) && mask == 0b1011);
  const uint32_t pausedGeneration = controller.zoneState(0).recoveryGeneration;
  controller_test::preferencesWriteFails = true;
  assert(!controller.resumeZone(0, pausedGeneration, controller_test::now, error));
  assert(controller.zoneState(0).paused);
  assert(controller.zoneState(0).recoveryGeneration == pausedGeneration);
  tick(controller, 5);
  assert(controller.zoneState(0).phase == ZonePhase::Paused);
  controller_test::preferencesWriteFails = false;
  assert(controller.resumeZone(0, pausedGeneration, controller_test::now, error));
  assert(store.loadPausedZones(mask) && mask == 0b1010);
  const unsigned highWrites = controller_test::relayHighWrites;
  tick(controller, 3);
  assert(controller_test::relayHighWrites == highWrites);
  assert(controller.zoneState(0).phase == ZonePhase::Monitoring);
  tick(controller);
  assert(controller.zoneState(0).phase == ZonePhase::Watering);
}

void testPauseSurvivesEmergencyAndFailedReads() {
  resetEnvironment();
  ConfigStore store;
  assert(store.savePausedZones(1));
  WateringController controller(relayPins, sensorPins, tankPin);
  controller.begin(testConfig(), &store);
  const uint32_t generation = controller.zoneState(0).recoveryGeneration;
  controller.emergencyStop(controller_test::now);
  assert(controller.zoneState(0).paused);
  assert(controller.zoneState(0).fault == FaultCode::EmergencyStop);
  assert(controller.zoneState(0).recoveryGeneration > generation);
  const char *error = nullptr;
  assert(!controller.resumeZone(0, controller.zoneState(0).recoveryGeneration,
                                controller_test::now, error));
  assert(!controller.clearFaultAndPause(0, controller.zoneState(0).recoveryGeneration,
                                       controller_test::now, error));
  controller.applyConfig(testConfig(), controller_test::now);
  assert(controller.zoneState(0).paused);
  assert(controller.zoneState(0).phase == ZonePhase::Paused);

  controller_test::storedPreferences["paused_zones"][0] ^= 1;
  WateringController corrupt(relayPins, sensorPins, tankPin);
  corrupt.begin(testConfig(), &store);
  for (size_t zone = 0; zone < ZONE_COUNT; ++zone) {
    assert(corrupt.zoneState(zone).paused);
  }
  tick(corrupt, 10);
  assert(controller_test::relayHighWrites == 0);
  controller_test::preferencesOpenFails = true;
  WateringController inaccessible(relayPins, sensorPins, tankPin);
  inaccessible.begin(testConfig(), &store);
  assert(inaccessible.zoneState(0).paused);
  assert(!inaccessible.resumeZone(0, inaccessible.zoneState(0).recoveryGeneration,
                                  controller_test::now, error));
}

void testFaultOccurrenceRemainsLatched() {
  resetEnvironment();
  ConfigStore store;
  WateringController controller(relayPins, sensorPins, tankPin);
  controller.begin(testConfig(), &store);
  reachLimit(controller);
  const uint32_t generation = controller.zoneState(0).recoveryGeneration;
  controller_test::analog[sensorPins[0]] = 0;
  tick(controller, 4);
  assert(controller.zoneState(0).fault == FaultCode::MaximumWateringReached);
  assert(controller.zoneState(0).recoveryGeneration == generation);
  clearPaused(controller);
  const uint32_t pausedGeneration = controller.zoneState(0).recoveryGeneration;
  controller.emergencyStop(controller_test::now);
  assert(controller.zoneState(0).recoveryGeneration > pausedGeneration);
  const char *error = nullptr;
  assert(!controller.resumeZone(0, pausedGeneration, controller_test::now, error));
  assert(!controller.clearFaultAndPause(ZONE_COUNT, 0, controller_test::now, error));
  assert(!controller.resumeZone(ZONE_COUNT, 0, controller_test::now, error));
}

void testIntermittentReadingPreservesAutomaticPulseTimer() {
  resetEnvironment();
  ConfigStore store;
  SystemConfig config = testConfig();
  config.zones[0].pulseOnMs = 4000;
  config.zones[0].maxWateringOnMsPerCycle = 4000;
  WateringController controller(relayPins, sensorPins, tankPin);
  controller.begin(config, &store);
  tick(controller, 4);
  assert(controller.zoneState(0).phase == ZonePhase::Watering);
  const uint32_t startedAt = controller.zoneState(0).phaseStartedAt;
  controller_test::analog[sensorPins[0]] = 0;
  tick(controller);
  controller_test::analog[sensorPins[0]] = 2600;
  tick(controller);
  assert(controller.sensorState(0).validSampleCount == 1);
  assert(controller.zoneState(0).phase == ZonePhase::Watering);
  assert(controller.zoneState(0).phaseStartedAt == startedAt);
  assert(controller.zoneState(0).relayOn);
  tick(controller, 2);
  assert(controller.zoneState(0).phase == ZonePhase::Soaking);
  assert(!controller.zoneState(0).relayOn);
  assert(controller.zoneState(0).wateringOnMsThisCycle == 4000);
}

void testIntermittentReadingPreservesManualPulseTimer() {
  resetEnvironment();
  ConfigStore store;
  SystemConfig config = testConfig();
  config.automaticWateringEnabled = false;
  config.zones[0].maxWateringOnMsPerCycle = 4000;
  WateringController controller(relayPins, sensorPins, tankPin);
  controller.begin(config, &store);
  tick(controller, 3);
  const char *error = nullptr;
  assert(controller.requestManualWater(0, 4000, controller_test::now, error));
  controller.loop(controller_test::now);
  assert(controller.zoneState(0).phase == ZonePhase::ManualWatering);
  const uint32_t startedAt = controller.zoneState(0).phaseStartedAt;
  controller_test::analog[sensorPins[0]] = 0;
  tick(controller);
  controller_test::analog[sensorPins[0]] = 2600;
  tick(controller);
  assert(controller.zoneState(0).phase == ZonePhase::ManualWatering);
  assert(controller.zoneState(0).phaseStartedAt == startedAt);
  assert(controller.zoneState(0).relayOn);
  tick(controller, 2);
  assert(controller.zoneState(0).phase == ZonePhase::Cooldown);
  assert(!controller.zoneState(0).relayOn);
  assert(controller.zoneState(0).wateringOnMsThisCycle == 4000);
}

void testIntermittentReadingPreservesSoakTimerAndCycleBudget() {
  resetEnvironment();
  ConfigStore store;
  WateringController controller(relayPins, sensorPins, tankPin);
  controller.begin(testConfig(), &store);
  tick(controller, 5);
  assert(controller.zoneState(0).phase == ZonePhase::Soaking);
  const uint32_t startedAt = controller.zoneState(0).phaseStartedAt;
  controller_test::analog[sensorPins[0]] = 0;
  tick(controller);
  controller_test::analog[sensorPins[0]] = 2600;
  tick(controller);
  assert(controller.zoneState(0).phase == ZonePhase::Soaking);
  assert(controller.zoneState(0).phaseStartedAt == startedAt);
  tick(controller, 8);
  assert(controller.zoneState(0).fault == FaultCode::MaximumWateringReached);
  assert(controller.zoneState(0).wateringOnMsThisCycle == 1000);

  resetEnvironment();
  SystemConfig config = testConfig();
  config.zones[0].pulseOnMs = 4000;
  config.zones[0].maxWateringOnMsPerCycle = 8000;
  WateringController queued(relayPins, sensorPins, tankPin);
  queued.begin(config, &store);
  tick(queued, 16);
  assert(queued.zoneState(0).phase == ZonePhase::Soaking);
  controller_test::analog[sensorPins[0]] = 0;
  tick(queued);
  controller_test::analog[sensorPins[0]] = 2600;
  tick(queued);
  assert(queued.sensorState(0).validSampleCount == 1);
  assert(queued.zoneState(0).phase == ZonePhase::Queued);
  assert(queued.zoneState(0).wateringOnMsThisCycle == 4000);
  assert(!queued.zoneState(0).relayOn);
  tick(queued);
  assert(queued.zoneState(0).phase == ZonePhase::Queued);
  tick(queued);
  assert(queued.zoneState(0).phase == ZonePhase::Watering);
  assert(queued.zoneState(0).wateringOnMsThisCycle == 4000);
  tick(queued, 14);
  assert(queued.zoneState(0).fault == FaultCode::MaximumWateringReached);
  assert(queued.zoneState(0).wateringOnMsThisCycle == 8000);
}

}  // namespace

int main() {
  testDurablePauseAndFreshResume();
  testPersistenceFailureAndChannelIsolation();
  testPauseSurvivesEmergencyAndFailedReads();
  testFaultOccurrenceRemainsLatched();
  testIntermittentReadingPreservesAutomaticPulseTimer();
  testIntermittentReadingPreservesManualPulseTimer();
  testIntermittentReadingPreservesSoakTimerAndCycleBudget();
  std::cout << "Controller paused-recovery tests passed\n";
}
