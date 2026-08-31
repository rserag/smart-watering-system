#include "telegram_notifier.h"

#include <algorithm>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>

#include "firmware_info.h"
#include "secrets.h"
#include "watering_types.h"

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

struct StoredAudit {
  uint32_t magic;
  uint16_t recordSize;
  uint8_t count;
  TelegramDeliveryReport reports[16];
};

}  // namespace

TelegramNotifier::TelegramNotifier(WateringController &controller)
    : controller_(controller) {}

void TelegramNotifier::begin(bool debugEnabled) {
  debugEnabled_ = debugEnabled;
  nextDebugAt_ = 0;
  auditMutex_ = xSemaphoreCreateMutex();
  criticalQueue_ = xQueueCreate(CRITICAL_QUEUE_LENGTH, sizeof(NotificationJob));
  standardQueue_ = xQueueCreate(STANDARD_QUEUE_LENGTH, sizeof(NotificationJob));
  if (auditMutex_ == nullptr || criticalQueue_ == nullptr ||
      standardQueue_ == nullptr) {
    Serial.println("Telegram notifier queue allocation failed");
    return;
  }
  loadAudit();
  if (configured()) {
    workerRunning_ =
        xTaskCreate(workerEntry, "telegram", 8192, this, 1, &workerTask_) ==
        pdPASS;
  } else {
    Serial.println("Telegram direct delivery is not configured");
  }
  Serial.printf("Telegram debug: %s\n", debugEnabled_ ? "enabled" : "disabled");
}

void TelegramNotifier::loop(uint32_t now, bool wifiConnected) {
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

void TelegramNotifier::setBootId(const String &bootId) { bootId_ = bootId; }

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
  size_t used = snprintf(
      text, sizeof(text),
      "%s\nController: %s\nUptime: %lu h\nWi-Fi: %d "
      "dBm\nTank: %s\nAutomatic watering: %s\nActive zone: %d\n",
      title, DEVICE_ID, static_cast<unsigned long>(now / 3600000UL), WiFi.RSSI(),
      controller_.mainTankLow() ? "LOW" : "ready",
      controller_.config().automaticWateringEnabled ? "enabled" : "disabled",
      controller_.activeZone() < 0 ? 0 : controller_.activeZone() + 1);

  for (size_t zone = 0; zone < ZONE_COUNT && used < sizeof(text); ++zone) {
    const SensorState &sensor = controller_.sensorState(zone);
    const ZoneState &state = controller_.zoneState(zone);
    const int written = snprintf(
        text + used, sizeof(text) - used, "Z%u: %u%%, %s%s%s\n",
        static_cast<unsigned>(zone + 1),
        static_cast<unsigned>(sensor.moisturePercent), zonePhaseName(state.phase),
        state.fault == FaultCode::None ? "" : ", fault=",
        state.fault == FaultCode::None ? "" : faultCodeName(state.fault));
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
                               const char *text, const char *requestId) {
  if (!configured() || queue == nullptr) {
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

TelegramNotifier::SendResult TelegramNotifier::sendMessage(const char *text) {
  SendResult result{};
  WiFiClientSecure client;
  client.setCACert(TELEGRAM_ROOT_CA);
  client.setTimeout(10);

  String url = "https://api.telegram.org/bot";
  url += TELEGRAM_BOT_TOKEN;
  url += "/sendMessage";

  JsonDocument request;
  request["chat_id"] = TELEGRAM_CHAT_ID;
  request["text"] = text;
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

  JsonDocument response;
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
  StoredAudit stored{};
  const size_t length = preferences.getBytesLength(AUDIT_KEY);
  const size_t bytesRead =
      length == sizeof(stored)
          ? preferences.getBytes(AUDIT_KEY, &stored, sizeof(stored))
          : 0;
  preferences.end();
  if (bytesRead != sizeof(stored) || stored.magic != AUDIT_MAGIC ||
      stored.recordSize != sizeof(stored) || stored.count > AUDIT_LENGTH) {
    return;
  }
  auditCount_ = stored.count;
  memcpy(audit_, stored.reports,
         sizeof(TelegramDeliveryReport) * auditCount_);
  for (size_t index = 0; index < auditCount_; ++index) {
    auditDirty_[index] = true;
  }
}

void TelegramNotifier::persistAuditLocked() {
  StoredAudit stored{};
  stored.magic = AUDIT_MAGIC;
  stored.recordSize = sizeof(stored);
  stored.count = auditCount_;
  memcpy(stored.reports, audit_,
         sizeof(TelegramDeliveryReport) * auditCount_);
  Preferences preferences;
  if (!preferences.begin(AUDIT_NAMESPACE, false)) {
    return;
  }
  preferences.putBytes(AUDIT_KEY, &stored, sizeof(stored));
  preferences.end();
}

void TelegramNotifier::upsertAudit(const TelegramDeliveryReport &report) {
  if (auditMutex_ == nullptr ||
      xSemaphoreTake(auditMutex_, pdMS_TO_TICKS(2000)) != pdTRUE) {
    return;
  }
  size_t slot = auditCount_;
  for (size_t index = 0; index < auditCount_; ++index) {
    if (strcmp(audit_[index].eventId, report.eventId) == 0) {
      slot = index;
      break;
    }
  }
  if (slot == auditCount_) {
    if (auditCount_ < AUDIT_LENGTH) {
      ++auditCount_;
    } else {
      memmove(&audit_[0], &audit_[1],
              sizeof(TelegramDeliveryReport) * (AUDIT_LENGTH - 1));
      memmove(&auditDirty_[0], &auditDirty_[1],
              sizeof(bool) * (AUDIT_LENGTH - 1));
      slot = AUDIT_LENGTH - 1;
    }
  }
  audit_[slot] = report;
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
      report = audit_[index];
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
    if (strcmp(audit_[index].eventId, eventId) == 0 &&
        audit_[index].updateSequence == updateSequence) {
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
    if (strcmp(audit_[index].eventId, eventId) == 0 &&
        updateSequence >= audit_[index].updateSequence) {
      if (index + 1 < auditCount_) {
        memmove(&audit_[index], &audit_[index + 1],
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
  for (;;) {
    NotificationJob job{};
    QueueHandle_t source = criticalQueue_;
    if (xQueueReceive(criticalQueue_, &job, 0) != pdTRUE) {
      source = standardQueue_;
      if (xQueueReceive(standardQueue_, &job, pdMS_TO_TICKS(1000)) != pdTRUE) {
        continue;
      }
    }

    const uint32_t now = millis();
    if (!due(now, job.nextAttemptAt) || !telegramReady()) {
      xQueueSend(source, &job, 0);
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    ++job.updateSequence;
    if (job.attempts < UINT8_MAX) {
      ++job.attempts;
    }
    upsertAudit(makeReport(job, "sending"));
    const SendResult result = sendMessage(job.text);
    lastSendSucceeded_ = result.success;
    if (result.success) {
      lastFailureStage_[0] = '\0';
      ++job.updateSequence;
      upsertAudit(makeReport(job, "sent", &result));
      Serial.println("Telegram notification sent");
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
  }
}

}  // namespace watering
