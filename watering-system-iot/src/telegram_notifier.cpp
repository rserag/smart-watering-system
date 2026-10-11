#include "telegram_notifier.h"

#include <algorithm>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <time.h>

#include "firmware_info.h"
#include "secrets.h"
#include "watering_types.h"

#ifndef TELEGRAM_OPERATOR_USER_ID
#define TELEGRAM_OPERATOR_USER_ID ""
#endif

namespace watering {

namespace {

// Go Daddy Root Certificate Authority - G2. api.telegram.org currently chains
// to this root. Keeping a CA here avoids the unsafe setInsecure() fallback.
constexpr char TELEGRAM_ROOT_CA[] PROGMEM = R"CERT(
-----BEGIN CERTIFICATE-----
MIIDxTCCAq2gAwIBAgIBADANBgkqhkiG9w0BAQsFADCBgzELMAkGA1UEBhMCVVMx
EDAOBgNVBAgTB0FyaXpvbmExEzARBgNVBAcTClNjb3R0c2RhbGUxGjAYBgNVBAoT
EUdvRGFkZHkuY29tLCBJbmMuMTEwLwYDVQQDEyhHbyBEYWRkeSBSb290IENlcnRp
ZmljYXRlIEF1dGhvcml0eSAtIEcyMB4XDTA5MDkwMTAwMDAwMFoXDTM3MTIzMTIz
NTk1OVowgYMxCzAJBgNVBAYTAlVTMRAwDgYDVQQIEwdBcml6b25hMRMwEQYDVQQH
EwpTY290dHNkYWxlMRowGAYDVQQKExFHb0RhZGR5LmNvbSwgSW5jLjExMC8GA1UE
AxMoR28gRGFkZHkgUm9vdCBDZXJ0aWZpY2F0ZSBBdXRob3JpdHkgLSBHMjCCASIw
DQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBAL9xYgjx+lk09xvJGKP3gElY6SKD
E6bFIEMBO4Tx5oVJnyfq9oQbTqC023CYxzIBsQU+B07u9PpPL1kwIuerGVZr4oAH
/PMWdYA5UXvl+TW2dE6pjYIT5LY/qQOD+qK+ihVqf94Lw7YZFAXK6sOoBJQ7Rnwy
DfMAZiLIjWltNowRGLfTshxgtDj6AozO091GB94KPutdfMh8+7ArU6SSYmlRJQVh
GkSBjCypQ5Yj36w6gZoOKcUcqeldHraenjAKOc7xiID7S13MMuyFYkMlNAJWJwGR
tDtwKj9useiciAF9n9T521NtYJ2/LOdYq7hfRvzOxBsDPAnrSTFcaUaz4EcCAwEA
AaNCMEAwDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMCAQYwHQYDVR0OBBYE
FDqahQcQZyi27/a9BUFuIMGU2g/eMA0GCSqGSIb3DQEBCwUAA4IBAQCZ21151fmX
WWcDYfF+OwYxdS2hII5PZYe096acvNjpL9DbWu7PdIxztDhC2gV7+AJ1uP2lsdeu
9tfeE8tTEH6KRtGX+rcuKxGrkLAngPnon1rpN5+r5N9ss4UXnT3ZJE95kTXWXwTr
gIOrmgIttRD02JDHBHNA7XIloKmf7J6raBKZV8aPEjoJpL1E/QYVN8Gb5DKj7Tjo
2GTzLH4U/ALqn83/B2gX2yKQOC16jdFU8WnjXzPKej17CuPKf1855eJ1usV2GDPO
LPAvTK33sefOT6jEm0pUBsV/fdUID+Ic/n4XuKxe9tQWskMJDE32p2u0mYRlynqI
4uJEvlz36hz1
-----END CERTIFICATE-----
)CERT";

constexpr time_t MIN_VALID_EPOCH = 1700000000;
constexpr uint32_t AUDIT_MAGIC = 0x54474155;  // "TGAU"
constexpr char AUDIT_NAMESPACE[] = "telegram";
constexpr char AUDIT_KEY[] = "audit";
constexpr char UPDATE_KEY[] = "update_id";

const char *recoveryFaultDescription(FaultCode fault) {
  switch (fault) {
    case FaultCode::MaximumWateringReached: return "Watering limit reached";
    case FaultCode::SensorInvalid: return "Sensor fault";
    case FaultCode::EmergencyStop: return "Emergency stop";
    case FaultCode::None: return "Fault cleared";
  }
  return "Fault";
}

}  // namespace

TelegramNotifier::TelegramNotifier(WateringController &controller,
                                   ConfigStore &configStore)
    : controller_(controller), configStore_(configStore) {}

void TelegramNotifier::begin(bool debugEnabled, bool hourlySilent) {
  debugEnabled_ = debugEnabled;
  hourlySilent_ = hourlySilent;
  nextDebugAt_ = 0;
  auditMutex_ = xSemaphoreCreateMutex();
  criticalQueue_ = xQueueCreate(CRITICAL_QUEUE_LENGTH, sizeof(NotificationJob));
  standardQueue_ = xQueueCreate(STANDARD_QUEUE_LENGTH, sizeof(NotificationJob));
  settingsCommands_ = xQueueCreate(1, sizeof(SettingsCommand));
  settingsReplies_ = xQueueCreate(1, sizeof(SettingsReply));
  recoveryCommands_ = xQueueCreate(1, sizeof(RecoveryCommand));
  recoveryReplies_ = xQueueCreate(1, sizeof(RecoveryReply));
  if (auditMutex_ == nullptr || criticalQueue_ == nullptr ||
      standardQueue_ == nullptr || settingsCommands_ == nullptr ||
      settingsReplies_ == nullptr || recoveryCommands_ == nullptr ||
      recoveryReplies_ == nullptr) {
    strlcpy(lastFailureStage_, "queue_allocation", sizeof(lastFailureStage_));
    Serial.println("Telegram notifier queue allocation failed");
    return;
  }
  loadAudit();
  Preferences preferences;
  if (preferences.begin(AUDIT_NAMESPACE, true)) {
    nextUpdateId_ = preferences.getLong64(UPDATE_KEY, 0);
    preferences.end();
  }
  if (configured()) {
    workerRunning_ =
        xTaskCreate(workerEntry, "telegram", WORKER_STACK_BYTES, this, 1, &workerTask_) ==
        pdPASS;
    if (!workerRunning_) {
      strlcpy(lastFailureStage_, "worker_allocation", sizeof(lastFailureStage_));
      Serial.println("Telegram worker allocation failed; direct delivery unavailable");
    }
  } else {
    Serial.println("Telegram direct delivery is not configured");
  }
  Serial.printf("Telegram debug: %s\n", debugEnabled_ ? "enabled" : "disabled");
}

void TelegramNotifier::loop(uint32_t now, bool wifiConnected) {
  processSettingsCommands();
  processRecoveryCommands(now);
  enqueueWateringFaults();
  if (configured() && wifiConnected && !timeSyncStarted_) {
    configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
    timeSyncStarted_ = true;
  }

  bool tankLow = false;
  if (controller_.consumeTankStateChanged(tankLow)) {
    enqueueTankState(tankLow);
  }

  PumpStartEvent pumpEvent{};
  if (controller_.consumePumpStarted(pumpEvent) && debugEnabled_) {
    enqueuePumpStarted(pumpEvent);
  }

  if (debugEnabled_ && due(now, nextDebugAt_)) {
    enqueueDebugReport(now, "hourly_debug");
    nextDebugAt_ = now + DEBUG_INTERVAL_MS;
  }
}

void TelegramNotifier::setBootId(const String &bootId) {
  bootId_ = bootId;
  for (auto &confirmation : confirmations_) confirmation = {};
}

void TelegramNotifier::setDebugEnabled(bool enabled, uint32_t now) {
  debugEnabled_ = enabled;
  nextDebugAt_ = enabled ? now : 0;
  Serial.printf("Telegram debug: %s\n", enabled ? "enabled" : "disabled");
}

bool TelegramNotifier::requestDebugReport(const String &requestId,
                                          uint32_t now) {
  return enqueueDebugReport(now, "manual_debug", requestId.c_str());
}

bool TelegramNotifier::debugEnabled() const { return debugEnabled_; }

bool TelegramNotifier::hourlySilent() const { return hourlySilent_; }

bool TelegramNotifier::setHourlySilent(bool enabled) {
  if (hourlySilent_ != enabled && !configStore_.saveTelegramHourlySilent(enabled)) {
    return false;
  }
  hourlySilent_ = enabled;
  settingsChanged_ = true;
  return true;
}

bool TelegramNotifier::consumeSettingsChanged() {
  const bool changed = settingsChanged_;
  settingsChanged_ = false;
  return changed;
}

void TelegramNotifier::processSettingsCommands() {
  SettingsCommand command{};
  if (settingsCommands_ == nullptr ||
      xQueueReceive(settingsCommands_, &command, 0) != pdTRUE) {
    return;
  }
  bool success = true;
  if (command.action == TelegramSettingsAction::SilentOn ||
      command.action == TelegramSettingsAction::SilentOff) {
    success = setHourlySilent(command.action == TelegramSettingsAction::SilentOn);
  }
  const SettingsReply reply{command, success, hourlySilent_, debugEnabled_};
  // Only one command is in flight; the worker drains this before polling again.
  xQueueSend(settingsReplies_, &reply, 0);
}

void TelegramNotifier::enqueueWateringFaults() {
  if (controller_.debugMode() == DebugMode::Simulation) return;
  if (!configured()) return;
  const uint32_t now = millis();
  for (size_t zone = 0; zone < ZONE_COUNT; ++zone) {
    const ZoneState &state = controller_.liveZoneState(zone);
    if (state.fault != FaultCode::MaximumWateringReached ||
        alertedFaultGeneration_[zone] == state.recoveryGeneration ||
        !due(now, nextFaultAlertAt_[zone])) continue;
    RecoveryButton button{};
    strlcpy(button.text, "Review recovery", sizeof(button.text));
    if (!telegramRecoveryCallback(button.data, sizeof(button.data), 'v',
                                   bootId_.c_str(), zone + 1,
                                   state.recoveryGeneration)) continue;
    char text[MESSAGE_BYTES];
    snprintf(text, sizeof(text),
             "Channel %u: Watering limit reached\nController: %s\n"
             "Pump stopped before the target moisture was confirmed.\n"
             "Check the water supply, tubing, and sensor. Review recovery to "
             "clear the fault and keep this channel paused.\n"
             "Use /faults to review current faults and paused channels.",
             static_cast<unsigned>(zone + 1), DEVICE_ID);
    nextFaultAlertAt_[zone] = now + 5000;
    if (enqueue(criticalQueue_, "maximum_watering_fault", text, nullptr, &button)) {
      alertedFaultGeneration_[zone] = state.recoveryGeneration;
    }
  }
}

void TelegramNotifier::showRecoveryConfirmation(RecoveryReply &reply,
                                                uint32_t now) {
  const TelegramRecoveryRequest &request = reply.command.request;
  const size_t zone = request.zoneId - 1;
  if (controller_.automationHeld()) {
    strlcpy(reply.text, "Diagnostics holds automation. End the diagnostic session and explicitly resume automation before reviewing channel recovery.", sizeof(reply.text));
    return;
  }
  const ZoneState &state = controller_.liveZoneState(zone);
  const bool clear = request.action == TelegramRecoveryAction::ReviewClear;
  if (clear && state.fault != FaultCode::MaximumWateringReached) {
    strlcpy(reply.text, "This channel no longer has a watering-limit fault. Use /faults for its current state.", sizeof(reply.text));
    return;
  }
  if (!clear && (!state.paused || state.fault != FaultCode::None)) {
    strlcpy(reply.text, "This channel is not ready to resume. Use /faults to review its current state.", sizeof(reply.text));
    return;
  }
  if (!clear && !controller_.config().zones[zone].enabled) {
    strlcpy(reply.text, "This channel is disabled. Enable it in the dashboard before requesting Resume.", sizeof(reply.text));
    return;
  }
  if (!clear && controller_.mainTankLow()) {
    strlcpy(reply.text, "The tank is low. This channel remains paused; restore the tank level before requesting Resume.", sizeof(reply.text));
    return;
  }

  RecoveryConfirmation &confirmation = confirmations_[nextConfirmation_];
  nextConfirmation_ = (nextConfirmation_ + 1) % RECOVERY_CONFIRMATION_COUNT;
  confirmation = {};
  bool nonceExists = false;
  do {
    confirmation.nonce = esp_random();
    nonceExists = false;
    for (const auto &candidate : confirmations_) {
      if (&candidate != &confirmation && candidate.nonce == confirmation.nonce) nonceExists = true;
    }
  } while (confirmation.nonce == 0 || nonceExists);
  confirmation.generation = state.recoveryGeneration;
  confirmation.expiresAt = now + RECOVERY_CONFIRMATION_MS;
  confirmation.actorId = reply.command.actorId;
  confirmation.zoneId = request.zoneId;
  confirmation.action = clear ? TelegramRecoveryAction::ClearPaused
                              : TelegramRecoveryAction::Resume;
  snprintf(reply.text, sizeof(reply.text),
           clear
               ? "Clear the watering-limit fault for Channel %u?\nController: %s\n"
                 "Check water supply, tubing, and sensor first. Clearing resets "
                 "the watering allowance. This channel stays paused, including "
                 "manual watering, until you separately choose Resume.\n"
                 "Confirmation expires in 60 seconds."
               : "Resume Channel %u?\nController: %s\n"
                 "Fresh sensor checks run before watering can start. Automatic "
                 "watering may then resume if enabled and the soil reads dry.\n"
                 "Confirmation expires in 60 seconds.",
           static_cast<unsigned>(request.zoneId), DEVICE_ID);
  strlcpy(reply.buttons[0].text, clear ? "Clear and keep paused" : "Resume watering",
          sizeof(reply.buttons[0].text));
  strlcpy(reply.buttons[1].text, "Cancel", sizeof(reply.buttons[1].text));
  if (telegramRecoveryCallback(reply.buttons[0].data, sizeof(reply.buttons[0].data),
                                clear ? 'c' : 's', bootId_.c_str(), request.zoneId,
                                confirmation.generation, confirmation.nonce) &&
      telegramRecoveryCallback(reply.buttons[1].data, sizeof(reply.buttons[1].data),
                                'x', bootId_.c_str(), request.zoneId,
                                confirmation.generation, confirmation.nonce)) {
    reply.buttonCount = 2;
  } else {
    confirmation.consumed = true;
    strlcpy(reply.text, "Could not create recovery confirmation. Please try /faults again.", sizeof(reply.text));
  }
}

void TelegramNotifier::processRecoveryCommands(uint32_t now) {
  RecoveryCommand command{};
  if (recoveryCommands_ == nullptr ||
      xQueueReceive(recoveryCommands_, &command, 0) != pdTRUE) return;
  RecoveryReply reply{};
  reply.command = command;
  const TelegramRecoveryRequest &request = command.request;
  strlcpy(reply.acknowledgement, "Recovery reviewed", sizeof(reply.acknowledgement));

  if (request.action == TelegramRecoveryAction::Invalid) {
    strlcpy(reply.text, "Use /faults, /clearfault 1 through 4, or /resume 1 through 4. Recovery buttons must come from this controller's current messages.", sizeof(reply.text));
  } else if (request.action == TelegramRecoveryAction::List) {
    size_t used = snprintf(reply.text, sizeof(reply.text), "Controller: %s\nLive faults and paused channels (diagnostics simulation excluded):\n", DEVICE_ID);
    uint8_t count = 0;
    for (size_t zone = 0; zone < ZONE_COUNT && used < sizeof(reply.text); ++zone) {
      const ZoneState &state = controller_.liveZoneState(zone);
      if (state.fault == FaultCode::None && !state.paused) continue;
      ++count;
      const int written = snprintf(reply.text + used, sizeof(reply.text) - used,
                                   "Channel %u: %s%s\n", static_cast<unsigned>(zone + 1),
                                   recoveryFaultDescription(state.fault),
                                   state.paused ? "; paused (manual and automatic watering blocked)" : "");
      if (written < 0) break;
      used += std::min(static_cast<size_t>(written), sizeof(reply.text) - used);
      if (state.fault == FaultCode::MaximumWateringReached ||
          (state.paused && state.fault == FaultCode::None)) {
        RecoveryButton &button = reply.buttons[reply.buttonCount];
        const bool clear = state.fault == FaultCode::MaximumWateringReached;
        snprintf(button.text, sizeof(button.text), clear ? "Review Channel %u recovery" : "Review Channel %u Resume",
                 static_cast<unsigned>(zone + 1));
        if (telegramRecoveryCallback(button.data, sizeof(button.data), clear ? 'v' : 'p',
                                      bootId_.c_str(), zone + 1, state.recoveryGeneration)) ++reply.buttonCount;
      }
    }
    if (count == 0) strlcpy(reply.text + used, "No current faults or paused channels.", sizeof(reply.text) - used);
  } else if (request.zoneId < 1 || request.zoneId > ZONE_COUNT ||
             (command.callbackId[0] != '\0' && strcmp(request.bootId, bootId_.c_str()) != 0)) {
    strlcpy(reply.text, "This recovery button belongs to an earlier controller session. Use /faults for a fresh review.", sizeof(reply.text));
  } else {
    const ZoneState &state = controller_.liveZoneState(request.zoneId - 1);
    const bool review = request.action == TelegramRecoveryAction::ReviewClear ||
                        request.action == TelegramRecoveryAction::ReviewResume;
    if (review) {
      if (command.callbackId[0] != '\0' && request.generation != state.recoveryGeneration) {
        strlcpy(reply.text, "The channel state changed since this message. Use /faults for a fresh review.", sizeof(reply.text));
      } else {
        showRecoveryConfirmation(reply, now);
      }
    } else {
      RecoveryConfirmation *confirmation = nullptr;
      TelegramRecoveryConfirmationCheck confirmationCheck = TelegramRecoveryConfirmationCheck::Invalid;
      for (auto &candidate : confirmations_) {
        const auto check = checkTelegramRecoveryConfirmation(request, candidate, bootId_.c_str(), command.actorId, now);
        if (check != TelegramRecoveryConfirmationCheck::Invalid) {
          confirmation = &candidate;
          confirmationCheck = check;
          break;
        }
      }
      reply.removeButtons = confirmation != nullptr;
      if (confirmation == nullptr || confirmationCheck == TelegramRecoveryConfirmationCheck::Expired) {
        strlcpy(reply.acknowledgement, "Confirmation expired or belongs to another operator", sizeof(reply.acknowledgement));
        strlcpy(reply.text, "This confirmation expired or belongs to another operator. The channel was not changed. Use /faults for a fresh review.", sizeof(reply.text));
      } else if (confirmationCheck == TelegramRecoveryConfirmationCheck::AlreadyConsumed) {
        strlcpy(reply.acknowledgement, "Already processed", sizeof(reply.acknowledgement));
        strlcpy(reply.text, confirmation->outcome, sizeof(reply.text));
      } else {
        // Consume before evaluating or mutating. Neither a replay nor another
        // tap can repeat an action, including one that failed to persist.
        confirmation->consumed = true;
        if (request.generation != state.recoveryGeneration) {
          strlcpy(reply.text, "The channel state changed after confirmation was opened. The channel was not changed. Use /faults for a fresh review.", sizeof(reply.text));
        } else if (request.action == TelegramRecoveryAction::Cancel) {
          strlcpy(reply.acknowledgement, "Recovery cancelled", sizeof(reply.acknowledgement));
          strlcpy(reply.text, "Recovery cancelled. The channel state is unchanged.", sizeof(reply.text));
        } else {
          const char *error = nullptr;
          const bool clear = request.action == TelegramRecoveryAction::ClearPaused;
          const bool success = clear
                                   ? controller_.clearFaultAndPause(request.zoneId - 1, request.generation, now, error)
                                   : controller_.resumeZone(request.zoneId - 1, request.generation, now, error);
          if (success) {
            strlcpy(reply.acknowledgement, clear ? "Fault cleared; channel remains paused" : "Resumed; checking sensor", sizeof(reply.acknowledgement));
            if (clear) {
              snprintf(reply.text, sizeof(reply.text),
                       "Channel %u: Fault cleared. Channel remains paused; manual and automatic watering are blocked.\n"
                       "Use /resume %u or /faults to separately review Resume.\nOperator: %lld",
                       static_cast<unsigned>(request.zoneId), static_cast<unsigned>(request.zoneId),
                       static_cast<long long>(command.actorId));
            } else {
              snprintf(reply.text, sizeof(reply.text),
                       "Channel %u: Resumed; checking sensor.\nThree fresh valid sensor samples are required before watering. "
                       "Automatic watering may start if enabled and soil reads dry.\n"
                       "Use /faults to review current faults and pauses.\nOperator: %lld",
                       static_cast<unsigned>(request.zoneId), static_cast<long long>(command.actorId));
            }
            // Durable delivery reports also provide the recovery result and
            // actor to the backend. This keeps audit/retry behavior shared.
            char requestId[101];
            snprintf(requestId, sizeof(requestId), "telegram:%lld:%lld:%u", static_cast<long long>(command.actorId),
                     static_cast<long long>(command.updateId), static_cast<unsigned>(request.zoneId));
            if (clear) {
              strlcpy(reply.buttons[0].text, "Review Resume", sizeof(reply.buttons[0].text));
              const ZoneState &pausedState = controller_.liveZoneState(request.zoneId - 1);
              if (telegramRecoveryCallback(reply.buttons[0].data, sizeof(reply.buttons[0].data), 'p',
                                            bootId_.c_str(), request.zoneId, pausedState.recoveryGeneration)) {
                reply.buttonCount = 1;
              }
            }
            strlcpy(confirmation->outcome, reply.text, sizeof(confirmation->outcome));
            if (enqueue(criticalQueue_, clear ? "fault_cleared_paused" : "channel_resumed", reply.text, requestId,
                        reply.buttonCount == 0 ? nullptr : &reply.buttons[0])) {
              // The queued, audited notification delivers the outcome.
              reply.text[0] = '\0';
            }
          } else {
            strlcpy(reply.acknowledgement, "Recovery could not complete", sizeof(reply.acknowledgement));
            snprintf(reply.text, sizeof(reply.text), "Channel %u: %s could not complete (%s). Use /faults to review its current state.",
                     static_cast<unsigned>(request.zoneId), clear ? "fault clearing" : "Resume", error == nullptr ? "controller rejected action" : error);
          }
        }
        if (confirmation->outcome[0] == '\0') strlcpy(confirmation->outcome, reply.text, sizeof(confirmation->outcome));
      }
    }
  }
  // There is exactly one command in flight. The worker waits for this main-task
  // snapshot and never reads controller state or mutates controller settings.
  xQueueSend(recoveryReplies_, &reply, 0);
}

void TelegramNotifier::sendRecoveryReply(const RecoveryReply &reply) {
  rememberUpdate(reply.command.updateId);
  if (reply.command.callbackId[0] != '\0') {
    JsonDocument request;
    request["callback_query_id"] = reply.command.callbackId;
    request["text"] = reply.acknowledgement;
    JsonDocument response;
    callTelegram("answerCallbackQuery", request, response);
  }
  if (reply.removeButtons && reply.command.messageId != 0) {
    JsonDocument request;
    request["chat_id"] = TELEGRAM_CHAT_ID;
    request["message_id"] = reply.command.messageId;
    request["reply_markup"]["inline_keyboard"].to<JsonArray>();
    JsonDocument response;
    callTelegram("editMessageReplyMarkup", request, response);
  }
  if (reply.text[0] != '\0') sendMessage(reply.text, false, false, reply.buttons, reply.buttonCount);
}

bool TelegramNotifier::configured() const {
  return TELEGRAM_ENABLED && TELEGRAM_BOT_TOKEN[0] != '\0' &&
         TELEGRAM_CHAT_ID[0] != '\0';
}

bool TelegramNotifier::workerRunning() const { return workerRunning_; }

bool TelegramNotifier::timeReady() const {
  return time(nullptr) >= MIN_VALID_EPOCH;
}

bool TelegramNotifier::lastSendSucceeded() const {
  return lastSendSucceeded_;
}

const char *TelegramNotifier::lastFailureStage() const {
  return lastFailureStage_;
}

uint8_t TelegramNotifier::pendingCount() const {
  const UBaseType_t critical = criticalQueue_ == nullptr
                                   ? 0
                                   : uxQueueMessagesWaiting(criticalQueue_);
  const UBaseType_t standard = standardQueue_ == nullptr
                                   ? 0
                                   : uxQueueMessagesWaiting(standardQueue_);
  return static_cast<uint8_t>(
      std::min<UBaseType_t>(critical + standard, UINT8_MAX));
}

void TelegramNotifier::enqueueTankState(bool isLow) {
  char text[MESSAGE_BYTES];
  if (isLow) {
    snprintf(text, sizeof(text),
             "WARNING: Main tank water is low\nController: %s\nAll pumps "
             "were stopped locally.",
             DEVICE_ID);
  } else {
    snprintf(text, sizeof(text),
             "Main tank level restored\nController: %s\nThe low-water "
             "safety interlock has cleared.",
             DEVICE_ID);
  }
  enqueue(criticalQueue_, isLow ? "tank_low" : "tank_restored", text);
}

void TelegramNotifier::enqueuePumpStarted(const PumpStartEvent &event) {
  char text[MESSAGE_BYTES];
  snprintf(text, sizeof(text),
           "Pump started\nController: %s\nZone: %u\nMode: %s\nMoisture: "
           "%u%%\nPulse: %lu seconds\nTank: ready",
           DEVICE_ID, static_cast<unsigned>(event.zoneId),
           event.manual ? "manual" : "automatic",
           static_cast<unsigned>(event.moisturePercent),
           static_cast<unsigned long>((event.durationMs + 999) / 1000));
  enqueue(standardQueue_, "pump_started", text);
}

bool TelegramNotifier::enqueueDebugReport(uint32_t now, const char *kind,
                                          const char *requestId) {
  char text[MESSAGE_BYTES];
  const char *title = strcmp(kind, "manual_debug") == 0
                          ? "On-demand controller report"
                          : "Hourly controller report";
  const bool simulation = controller_.debugMode() == DebugMode::Simulation;
  const char *diagnostics = simulation ? "simulation; physical relays OFF; live readings before test" :
      controller_.debugMode() == DebugMode::Hardware ? "hardware testing; automation held" :
      controller_.automationHeld() ? "automation held; explicit resume required" : "observe";
  size_t used = snprintf(
      text, sizeof(text),
      "%s\nController: %s\nDiagnostics: %s\nUptime: %lu h\nWi-Fi: %d "
      "dBm\nTank: %s\nAutomatic watering: %s\nActive zone: %d\n",
      title, DEVICE_ID, diagnostics, static_cast<unsigned long>(now / 3600000UL), WiFi.RSSI(),
      (simulation ? controller_.physicalTankLow() : controller_.mainTankLow()) ? "LOW" : "ready",
      controller_.config().automaticWateringEnabled ? "enabled" : "disabled",
      simulation || controller_.activeZone() < 0 ? 0 : controller_.activeZone() + 1);

  for (size_t zone = 0; zone < ZONE_COUNT && used < sizeof(text); ++zone) {
    const SensorState &sensor = controller_.liveSensorState(zone);
    const ZoneState &state = controller_.liveZoneState(zone);
    const int written = snprintf(
        text + used, sizeof(text) - used, "Z%u: %u%%, %s%s%s%s\n",
        static_cast<unsigned>(zone + 1),
        static_cast<unsigned>(sensor.moisturePercent), zonePhaseName(state.phase),
        state.fault == FaultCode::None ? "" : ", fault=",
        state.fault == FaultCode::None ? "" : faultCodeName(state.fault),
        state.paused ? ", paused" : "");
    if (written < 0) {
      break;
    }
    used += std::min(static_cast<size_t>(written), sizeof(text) - used);
  }
  if (used < sizeof(text)) {
    snprintf(text + used, sizeof(text) - used, "Free heap: %u bytes\nFirmware: %s",
             ESP.getFreeHeap(), FIRMWARE_VERSION);
  }
  return enqueue(standardQueue_, kind, text, requestId);
}

bool TelegramNotifier::enqueue(QueueHandle_t queue, const char *kind,
                               const char *text, const char *requestId,
                               const RecoveryButton *button) {
  if (!configured() || !workerRunning_ || queue == nullptr) {
    return false;
  }
  NotificationJob job{};
  const String eventId = nextEventId();
  strlcpy(job.eventId, eventId.c_str(), sizeof(job.eventId));
  strlcpy(job.kind, kind, sizeof(job.kind));
  if (requestId != nullptr) {
    strlcpy(job.requestId, requestId, sizeof(job.requestId));
  }
  strlcpy(job.text, text, sizeof(job.text));
  if (button != nullptr) job.recoveryButton = *button;
  // Capture the preference on the main task so retries retain the same mode.
  // NotificationJob is private and never persisted, so its layout is safe to extend.
  job.silent = silentTelegramNotification(kind, hourlySilent_);
  job.updateSequence = 1;
  job.nextAttemptAt = millis();
  if (xQueueSend(queue, &job, 0) == pdTRUE) {
    upsertAudit(makeReport(job, "queued"));
    return true;
  }
  upsertAudit(makeReport(job, "dropped"));
  Serial.println("Telegram notification queue is full; message dropped");
  return false;
}

TelegramNotifier::SendResult TelegramNotifier::sendMessage(
    const char *text, bool silent, bool settingsButtons,
    const RecoveryButton *buttons, uint8_t buttonCount) {
  JsonDocument request;
  request["chat_id"] = TELEGRAM_CHAT_ID;
  request["text"] = text;
  request["disable_notification"] = silent;
  if (settingsButtons) {
    JsonArray row = request["reply_markup"]["inline_keyboard"].to<JsonArray>()
                        .add<JsonArray>();
    JsonObject on = row.add<JsonObject>();
    on["text"] = "Silent";
    on["callback_data"] = "hourly_silent:on";
    JsonObject off = row.add<JsonObject>();
    off["text"] = "With sound";
    off["callback_data"] = "hourly_silent:off";
  } else if (buttons != nullptr && buttonCount > 0) {
    JsonArray keyboard = request["reply_markup"]["inline_keyboard"].to<JsonArray>();
    for (uint8_t index = 0; index < buttonCount; ++index) {
      JsonObject button = keyboard.add<JsonArray>().add<JsonObject>();
      button["text"] = buttons[index].text;
      button["callback_data"] = buttons[index].data;
    }
  }
  JsonDocument response;
  return callTelegram("sendMessage", request, response);
}

TelegramNotifier::SendResult TelegramNotifier::callTelegram(
    const char *method, JsonDocument &request, JsonDocument &response) {
  SendResult result{};
  WiFiClientSecure client;
  client.setCACert(TELEGRAM_ROOT_CA);
  client.setTimeout(10);

  String url = "https://api.telegram.org/bot";
  url += TELEGRAM_BOT_TOKEN;
  url += '/';
  url += method;
  String body;
  serializeJson(request, body);

  HTTPClient http;
  http.setConnectTimeout(10000);
  http.setTimeout(10000);
  if (!http.begin(client, url)) {
    strlcpy(result.errorStage, "setup", sizeof(result.errorStage));
    return result;
  }
  http.addHeader("Content-Type", "application/json");
  const int status = http.POST(body);
  result.httpStatus = static_cast<int16_t>(status);
  if (status < 0) {
    result.telegramErrorCode = status;
    strlcpy(result.errorStage, "transport", sizeof(result.errorStage));
    http.end();
    return result;
  }

  const DeserializationError error = deserializeJson(response, http.getString());
  if (error) {
    strlcpy(result.errorStage, "response", sizeof(result.errorStage));
  } else if (status != HTTP_CODE_OK || !(response["ok"] | false)) {
    result.telegramErrorCode = response["error_code"] | status;
    strlcpy(result.errorStage, "telegram_api", sizeof(result.errorStage));
  } else {
    result.success = true;
    result.telegramMessageId = response["result"]["message_id"] | 0LL;
  }
  http.end();
  return result;
}

void TelegramNotifier::rememberUpdate(int64_t updateId) {
  nextUpdateId_ = updateId + 1;
  Preferences preferences;
  if (preferences.begin(AUDIT_NAMESPACE, false)) {
    preferences.putLong64(UPDATE_KEY, nextUpdateId_);
    preferences.end();
  }
}

void TelegramNotifier::pollSettings() {
  if (settingsPending_ || recoveryPending_ || !telegramReady() || !due(millis(), nextPollAt_)) {
    return;
  }
  nextPollAt_ = millis() + 5000;
  JsonDocument request;
  request["offset"] = nextUpdateId_;
  request["limit"] = 1;
  request["timeout"] = 0;
  JsonArray allowed = request["allowed_updates"].to<JsonArray>();
  allowed.add("message");
  allowed.add("callback_query");
  JsonDocument response;
  if (!callTelegram("getUpdates", request, response).success) {
    nextPollAt_ = millis() + 30000;
    return;
  }
  JsonObjectConst update = response["result"][0];
  if (update.isNull() || !update["update_id"].is<int64_t>()) {
    return;
  }
  const int64_t updateId = update["update_id"].as<int64_t>();
  JsonObjectConst callback = update["callback_query"];
  const bool isCallback = !callback.isNull();
  JsonObjectConst message = isCallback ? callback["message"].as<JsonObjectConst>()
                                       : update["message"].as<JsonObjectConst>();
  // The configured destination is also the control allowlist. Never reply to
  // or act on messages from other chats, forwarded messages, or other bots.
  const String chatId = message["chat"]["id"].as<String>();
  const bool forwarded = !message["forward_origin"].isNull() ||
                         !message["forward_from"].isNull() ||
                         !message["forward_from_chat"].isNull() ||
                         (message["is_automatic_forward"] | false);
  const JsonObjectConst sender = isCallback ? callback["from"].as<JsonObjectConst>()
                                           : message["from"].as<JsonObjectConst>();
  if (!authorizedTelegramSettings(
          chatId.c_str(), TELEGRAM_CHAT_ID, forwarded, sender["is_bot"].as<bool>())) {
    rememberUpdate(updateId);
    return;
  }
  String text = isCallback ? String(callback["data"] | "")
                           : String(message["text"] | "");
  text.trim();
  const TelegramRecoveryRequest recovery = telegramRecoveryRequest(text.c_str(), isCallback, ZONE_COUNT);
  if (recovery.action != TelegramRecoveryAction::Ignore) {
    const String actorId = sender["id"].as<String>();
    if (!sender["id"].is<int64_t>() || !authorizedTelegramRecovery(
            chatId.c_str(), TELEGRAM_CHAT_ID, actorId.c_str(),
            TELEGRAM_OPERATOR_USER_ID, forwarded, sender["is_bot"].as<bool>())) {
      // Recovery has a stricter operator allowlist than hourly preferences.
      // Do not send a response to unauthorized senders or acknowledge their action.
      rememberUpdate(updateId);
      return;
    }
    RecoveryCommand command{};
    command.updateId = updateId;
    command.actorId = sender["id"].as<int64_t>();
    command.messageId = message["message_id"] | 0LL;
    command.request = recovery;
    strlcpy(command.callbackId, callback["id"] | "", sizeof(command.callbackId));
    if (xQueueSend(recoveryCommands_, &command, 0) == pdTRUE) recoveryPending_ = true;
    return;
  }
  SettingsCommand command{};
  command.updateId = updateId;
  command.action = telegramSettingsAction(text.c_str(), isCallback);
  if (command.action == TelegramSettingsAction::Ignore) {
    rememberUpdate(updateId);
    return;
  }
  strlcpy(command.callbackId, callback["id"] | "", sizeof(command.callbackId));
  if (xQueueSend(settingsCommands_, &command, 0) == pdTRUE) {
    settingsPending_ = true;
  }
}

void TelegramNotifier::sendSettingsReply(const SettingsReply &reply) {
  // Save the cursor only after the main task has applied the command. Explicit
  // on/off actions are idempotent if power fails between saving these records.
  rememberUpdate(reply.command.updateId);
  if (reply.command.callbackId[0] != '\0') {
    JsonDocument request;
    request["callback_query_id"] = reply.command.callbackId;
    request["text"] = reply.success ? "Setting saved" : "Could not save setting";
    JsonDocument response;
    callTelegram("answerCallbackQuery", request, response);
  }
  char text[320];
  snprintf(text, sizeof(text),
           "%sController: %s\nHourly status updates: %s\nHourly notifications: %s\n"
           "Choose the sound setting below, or use /hourly_silent on or off.\n"
           "Use /faults to review faults and paused channels.",
           reply.success ? "" : "Could not save setting. Please try again.\n",
           DEVICE_ID, reply.debugEnabled ? "enabled" : "disabled",
           reply.silent ? "silent" : "with sound");
  sendMessage(text, true, true);
}

bool TelegramNotifier::telegramReady() const {
  return configured() && WiFi.status() == WL_CONNECTED &&
         timeReady();
}

String TelegramNotifier::nextEventId() {
  ++eventSequence_;
  String value = bootId_.isEmpty() ? String("boot") : bootId_;
  value += ':';
  value += eventSequence_;
  return value;
}

TelegramDeliveryReport TelegramNotifier::makeReport(
    const NotificationJob &job, const char *status,
    const SendResult *result) const {
  TelegramDeliveryReport report{};
  strlcpy(report.eventId, job.eventId, sizeof(report.eventId));
  strlcpy(report.requestId, job.requestId, sizeof(report.requestId));
  strlcpy(report.kind, job.kind, sizeof(report.kind));
  strlcpy(report.status, status, sizeof(report.status));
  report.updateSequence = job.updateSequence;
  report.uptimeMs = millis();
  report.attempt = job.attempts;
  if (result != nullptr) {
    strlcpy(report.errorStage, result->errorStage,
            sizeof(report.errorStage));
    report.httpStatus = result->httpStatus;
    report.telegramErrorCode = result->telegramErrorCode;
    report.telegramMessageId = result->telegramMessageId;
  }
  return report;
}

void TelegramNotifier::loadAudit() {
  Preferences preferences;
  if (!preferences.begin(AUDIT_NAMESPACE, true)) {
    return;
  }
  StoredAudit &stored = auditStorage_;
  static_assert(sizeof(StoredAudit) == 3976 && sizeof(TelegramDeliveryReport) == 248,
                "existing Telegram audit NVS records must stay readable");
  const size_t length = preferences.getBytesLength(AUDIT_KEY);
  const size_t bytesRead =
      length == sizeof(stored)
          ? preferences.getBytes(AUDIT_KEY, &stored, sizeof(stored))
          : 0;
  preferences.end();
  if (bytesRead != sizeof(stored) || stored.magic != AUDIT_MAGIC ||
      stored.recordSize != sizeof(stored) || stored.count > AUDIT_LENGTH) {
    memset(&stored, 0, sizeof(stored));
    return;
  }
  auditCount_ = stored.count;
  for (size_t index = 0; index < auditCount_; ++index) {
    auditDirty_[index] = true;
  }
}

void TelegramNotifier::persistAuditLocked() {
  StoredAudit &stored = auditStorage_;
  stored.magic = AUDIT_MAGIC;
  stored.recordSize = sizeof(stored);
  stored.count = auditCount_;
  memset(stored.reports + auditCount_, 0,
         sizeof(TelegramDeliveryReport) * (AUDIT_LENGTH - auditCount_));
  Preferences preferences;
  if (!preferences.begin(AUDIT_NAMESPACE, false)) {
    Serial.println("Telegram audit storage unavailable; report remains in memory");
    return;
  }
  if (preferences.putBytes(AUDIT_KEY, &stored, sizeof(stored)) != sizeof(stored)) {
    Serial.println("Telegram audit persistence failed; report remains in memory");
  }
  preferences.end();
}

void TelegramNotifier::upsertAudit(const TelegramDeliveryReport &report) {
  if (auditMutex_ == nullptr ||
      xSemaphoreTake(auditMutex_, pdMS_TO_TICKS(2000)) != pdTRUE) {
    return;
  }
  size_t slot = auditCount_;
  for (size_t index = 0; index < auditCount_; ++index) {
    if (strcmp(auditStorage_.reports[index].eventId, report.eventId) == 0) {
      slot = index;
      break;
    }
  }
  if (slot == auditCount_) {
    if (auditCount_ < AUDIT_LENGTH) {
      ++auditCount_;
    } else {
      memmove(&auditStorage_.reports[0], &auditStorage_.reports[1],
              sizeof(TelegramDeliveryReport) * (AUDIT_LENGTH - 1));
      memmove(&auditDirty_[0], &auditDirty_[1],
              sizeof(bool) * (AUDIT_LENGTH - 1));
      slot = AUDIT_LENGTH - 1;
    }
  }
  auditStorage_.reports[slot] = report;
  auditDirty_[slot] = true;
  persistAuditLocked();
  xSemaphoreGive(auditMutex_);
}

bool TelegramNotifier::nextDeliveryReport(TelegramDeliveryReport &report) {
  if (auditMutex_ == nullptr ||
      xSemaphoreTake(auditMutex_, pdMS_TO_TICKS(50)) != pdTRUE) {
    return false;
  }
  bool found = false;
  for (size_t index = 0; index < auditCount_; ++index) {
    if (auditDirty_[index]) {
      report = auditStorage_.reports[index];
      auditDirty_[index] = false;
      found = true;
      break;
    }
  }
  xSemaphoreGive(auditMutex_);
  return found;
}

void TelegramNotifier::markDeliveryReportForRetry(
    const char *eventId, uint32_t updateSequence) {
  if (auditMutex_ == nullptr ||
      xSemaphoreTake(auditMutex_, pdMS_TO_TICKS(50)) != pdTRUE) {
    return;
  }
  for (size_t index = 0; index < auditCount_; ++index) {
    if (strcmp(auditStorage_.reports[index].eventId, eventId) == 0 &&
        auditStorage_.reports[index].updateSequence == updateSequence) {
      auditDirty_[index] = true;
      break;
    }
  }
  xSemaphoreGive(auditMutex_);
}

void TelegramNotifier::acknowledgeDelivery(const char *eventId,
                                           uint32_t updateSequence) {
  if (auditMutex_ == nullptr ||
      xSemaphoreTake(auditMutex_, pdMS_TO_TICKS(2000)) != pdTRUE) {
    return;
  }
  for (size_t index = 0; index < auditCount_; ++index) {
    if (strcmp(auditStorage_.reports[index].eventId, eventId) == 0 &&
        updateSequence >= auditStorage_.reports[index].updateSequence) {
      if (index + 1 < auditCount_) {
        memmove(&auditStorage_.reports[index], &auditStorage_.reports[index + 1],
                sizeof(TelegramDeliveryReport) * (auditCount_ - index - 1));
        memmove(&auditDirty_[index], &auditDirty_[index + 1],
                sizeof(bool) * (auditCount_ - index - 1));
      }
      --auditCount_;
      persistAuditLocked();
      break;
    }
  }
  xSemaphoreGive(auditMutex_);
}

void TelegramNotifier::replayDeliveryReports() {
  if (auditMutex_ == nullptr ||
      xSemaphoreTake(auditMutex_, pdMS_TO_TICKS(50)) != pdTRUE) {
    return;
  }
  for (size_t index = 0; index < auditCount_; ++index) {
    auditDirty_[index] = true;
  }
  xSemaphoreGive(auditMutex_);
}

bool TelegramNotifier::due(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}

uint32_t TelegramNotifier::retryDelayMs(uint8_t attempts) {
  static constexpr uint32_t DELAYS[] = {15000, 60000, 300000, 900000, 3600000};
  constexpr size_t DELAY_COUNT = sizeof(DELAYS) / sizeof(DELAYS[0]);
  const size_t index = std::min<size_t>(attempts, DELAY_COUNT - 1);
  return DELAYS[index] + esp_random() % 5000;
}

void TelegramNotifier::workerEntry(void *argument) {
  static_cast<TelegramNotifier *>(argument)->workerLoop();
}

void TelegramNotifier::workerLoop() {
  RecoveryReply &recoveryReply = workerRecoveryReply_;
  SettingsReply &reply = workerSettingsReply_;
  NotificationJob &job = workerJob_;
  for (;;) {
    recordWorkerStackWatermark();
    if (xQueueReceive(recoveryReplies_, &recoveryReply, 0) == pdTRUE) {
      if (telegramReady()) {
        sendRecoveryReply(recoveryReply);
        recoveryPending_ = false;
      } else {
        xQueueSend(recoveryReplies_, &recoveryReply, 0);
      }
    }
    if (xQueueReceive(settingsReplies_, &reply, 0) == pdTRUE) {
      if (telegramReady()) {
        sendSettingsReply(reply);
        settingsPending_ = false;
      } else {
        xQueueSend(settingsReplies_, &reply, 0);
      }
    }
    QueueHandle_t source = criticalQueue_;
    if (xQueueReceive(criticalQueue_, &job, 0) != pdTRUE) {
      source = standardQueue_;
      if (xQueueReceive(standardQueue_, &job, pdMS_TO_TICKS(1000)) != pdTRUE) {
        pollSettings();
        continue;
      }
    }

    const uint32_t now = millis();
    if (!due(now, job.nextAttemptAt) || !telegramReady()) {
      xQueueSend(source, &job, 0);
      vTaskDelay(pdMS_TO_TICKS(500));
      pollSettings();
      continue;
    }

    ++job.updateSequence;
    if (job.attempts < UINT8_MAX) {
      ++job.attempts;
    }
    upsertAudit(makeReport(job, "sending"));
    recordWorkerStackWatermark();
    const SendResult result = sendMessage(job.text, job.silent, false,
                                         &job.recoveryButton,
                                         job.recoveryButton.data[0] == '\0' ? 0 : 1);
    lastSendSucceeded_ = result.success;
    recordWorkerStackWatermark();
    if (result.success) {
      lastFailureStage_[0] = '\0';
      ++job.updateSequence;
      upsertAudit(makeReport(job, "sent", &result));
      Serial.println("Telegram notification sent");
      pollSettings();
      continue;
    }

    strlcpy(lastFailureStage_, result.errorStage,
            sizeof(lastFailureStage_));
    job.nextAttemptAt = millis() + retryDelayMs(job.attempts);
    ++job.updateSequence;
    if (xQueueSend(source, &job, 0) != pdTRUE) {
      upsertAudit(makeReport(job, "dropped", &result));
      Serial.println("Telegram retry queue is full; message dropped");
    } else {
      upsertAudit(makeReport(job, "retry_scheduled", &result));
      Serial.println("Telegram notification failed; retry scheduled");
    }
    pollSettings();
  }
}

void TelegramNotifier::recordWorkerStackWatermark() {
  // ESP-IDF reports this in bytes, including the deepest call since startup.
  // The canary remains enabled; this gives commissioning a measured margin.
  const uint32_t freeBytes = uxTaskGetStackHighWaterMark(nullptr);
  if (freeBytes < workerMinimumStackFreeBytes_) {
    workerMinimumStackFreeBytes_ = freeBytes;
    Serial.printf("Telegram worker minimum free stack: %lu / %lu bytes; "
                  "heap free/min/largest: %lu/%lu/%lu bytes\n",
                  static_cast<unsigned long>(freeBytes),
                  static_cast<unsigned long>(WORKER_STACK_BYTES),
                  static_cast<unsigned long>(ESP.getFreeHeap()),
                  static_cast<unsigned long>(ESP.getMinFreeHeap()),
                  static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
  }
}

}  // namespace watering
