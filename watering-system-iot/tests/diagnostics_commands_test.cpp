#include <cassert>
#include <iostream>
#include <string>
#include "diagnostics_commands.h"
using namespace watering;
namespace {
constexpr char boot[] = "boot-a";
constexpr char lease[] = "12345678-1234-1234-1234-123456789abc";
constexpr uint32_t epoch = 1800000000;
JsonDocument pulse() {
  JsonDocument document;
  document["type"] = "debug.output.pulse";
  document["schemaVersion"] = 1;
  document["deviceId"] = "test";
  document["requestId"] = "pulse-id";
  document["expectedBootId"] = boot;
  document["expiresAtEpoch"] = epoch + 30;
  document["sessionId"] = lease;
  document["zoneId"] = 1;
  document["durationMs"] = 100;
  return document;
}
bool valid(const JsonDocument &document, uint32_t now = epoch) {
  const char *error = nullptr;
  return validateDiagnosticsCommand(document.as<JsonObjectConst>(), boot, now, error);
}
void testTypedAndFixedDeadline() {
  auto document = pulse(); assert(valid(document));
  document["durationMs"] = true; assert(!valid(document));
  document["durationMs"] = "100"; assert(!valid(document));
  document["durationMs"] = 99; assert(!valid(document));
  document["durationMs"] = 60001; assert(!valid(document));
  document["durationMs"] = 60000; assert(valid(document));
  document["zoneId"] = 0; assert(!valid(document));
  document["zoneId"] = 5; assert(!valid(document));
  document["zoneId"] = 1.5; assert(!valid(document));
  document["zoneId"] = 1; document["expectedBootId"] = "old-boot"; assert(!valid(document));
  document["expectedBootId"] = boot; document["expiresAtEpoch"] = epoch; assert(!valid(document));
  document["expiresAtEpoch"] = epoch + 31; assert(!valid(document));
  document["expiresAtEpoch"] = epoch + 30; assert(valid(document));
  assert(!valid(document, epoch + 30));
  assert(!valid(document, 0));
  document["sessionId"] = "not-a-uuid"; assert(!valid(document));
  document["sessionId"] = lease; document["unexpectedPin"] = 23; assert(!valid(document));
  document.remove("unexpectedPin"); document["durationMs"] = nullptr; assert(!valid(document));
}
void testInputAndStopValidation() {
  auto document = pulse();
  document["type"] = "debug.input.set";
  document.remove("durationMs");
  document["input"] = "soil"; document["raw"] = 4095; assert(valid(document));
  document["raw"] = 4096; assert(!valid(document));
  document["raw"] = -1; assert(!valid(document));
  document["raw"] = false; assert(!valid(document));
  document["raw"] = 2000; document["input"] = "tank"; document["low"] = true;
  assert(!valid(document)); // Soil-only parameters cannot leak into tank input.
  document.remove("raw"); document.remove("zoneId"); assert(valid(document));
  document["low"] = 1; assert(!valid(document));
  document["type"] = "debug.output.stop";
  document.remove("low"); document.remove("input"); document.remove("sessionId"); assert(valid(document));
  document["zoneId"] = 4; assert(valid(document));
  document["zoneId"] = true; assert(!valid(document));
}
void testDuplicateLedgerNeverEvictsUnexpiredPulse() {
  DiagnosticsRequestLedger ledger;
  assert(ledger.hasCapacity(epoch));
  ledger.remember("pulse-id", "debug.output.pulse", epoch + 30, epoch, true, nullptr);
  const auto *original = ledger.find("pulse-id", epoch);
  assert(original && original->accepted);
  unsigned starts = 1;
  for (unsigned repeat = 0; repeat < 20; ++repeat)
    if (ledger.find("pulse-id", epoch) == nullptr) ++starts;
  assert(starts == 1);
  for (unsigned index = 1; index < 64; ++index) {
    const std::string id = "burst-" + std::to_string(index);
    ledger.remember(id.c_str(), "debug.sample", epoch + 30, epoch, true, nullptr);
  }
  assert(!ledger.hasCapacity(epoch));
  assert(ledger.find("pulse-id", epoch));
  assert(ledger.hasCapacity(epoch + 30));
  auto expired = pulse(); assert(!valid(expired, epoch + 30)); // Expiry prevents executing an evicted old ID.
}
}
int main() {
  testTypedAndFixedDeadline();
  testInputAndStopValidation();
  testDuplicateLedgerNeverEvictsUnexpiredPulse();
  std::cout << "Diagnostics command validation and replay tests passed\n";
}
