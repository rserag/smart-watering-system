#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include "config_store.h"
#include "telegram_settings.h"

#include "watering_controller.h"

namespace watering {

struct TelegramDeliveryReport {
  char eventId[48];
  char requestId[101];
  char kind[24];
  char status[24];
  char errorStage[24];
  uint32_t updateSequence;
  uint32_t uptimeMs;
  uint8_t attempt;
  int16_t httpStatus;
  int32_t telegramErrorCode;
  int64_t telegramMessageId;
};

class TelegramNotifier {
 public:
  TelegramNotifier(WateringController &controller, ConfigStore &configStore);

  void begin(bool debugEnabled, bool hourlySilent);
  void loop(uint32_t now, bool wifiConnected);
  void setBootId(const String &bootId);
  void setDebugEnabled(bool enabled, uint32_t now);
  bool requestDebugReport(const String &requestId, uint32_t now);

  bool debugEnabled() const;
  bool hourlySilent() const;
  bool setHourlySilent(bool enabled);
  bool consumeSettingsChanged();
  bool configured() const;
  bool workerRunning() const;
  bool timeReady() const;
  bool lastSendSucceeded() const;
  const char *lastFailureStage() const;
  uint8_t pendingCount() const;

  bool nextDeliveryReport(TelegramDeliveryReport &report);
  void markDeliveryReportForRetry(const char *eventId,
                                  uint32_t updateSequence);
  void acknowledgeDelivery(const char *eventId, uint32_t updateSequence);
  void replayDeliveryReports();

 private:
  static constexpr uint32_t DEBUG_INTERVAL_MS = 3600000;
  static constexpr size_t MESSAGE_BYTES = 768;
  static constexpr uint8_t CRITICAL_QUEUE_LENGTH = 4;
  static constexpr uint8_t STANDARD_QUEUE_LENGTH = 12;
  static constexpr uint8_t AUDIT_LENGTH = 16;

  struct NotificationJob {
    char eventId[48];
    char requestId[101];
    char kind[24];
    char text[MESSAGE_BYTES];
    bool silent;
    uint8_t attempts;
    uint32_t updateSequence;
    uint32_t nextAttemptAt;
  };

  struct SendResult {
    bool success;
    int16_t httpStatus;
    int32_t telegramErrorCode;
    int64_t telegramMessageId;
    char errorStage[24];
  };

  struct SettingsCommand {
    int64_t updateId;
    char callbackId[128];
    TelegramSettingsAction action;
  };

  struct SettingsReply {
    SettingsCommand command;
    bool success;
    bool silent;
    bool debugEnabled;
  };

  WateringController &controller_;
  ConfigStore &configStore_;
  QueueHandle_t settingsCommands_ = nullptr;
  QueueHandle_t settingsReplies_ = nullptr;
  QueueHandle_t criticalQueue_ = nullptr;
  QueueHandle_t standardQueue_ = nullptr;
  TaskHandle_t workerTask_ = nullptr;
  SemaphoreHandle_t auditMutex_ = nullptr;
  bool debugEnabled_ = false;
  bool hourlySilent_ = false;
  bool settingsChanged_ = false;
  int64_t nextUpdateId_ = 0;
  bool settingsPending_ = false;
  uint32_t nextPollAt_ = 0;
  bool timeSyncStarted_ = false;
  bool workerRunning_ = false;
  uint32_t nextDebugAt_ = 0;
  uint32_t eventSequence_ = 0;
  String bootId_;
  volatile bool lastSendSucceeded_ = false;
  char lastFailureStage_[24]{};
  TelegramDeliveryReport audit_[AUDIT_LENGTH]{};
  bool auditDirty_[AUDIT_LENGTH]{};
  uint8_t auditCount_ = 0;

  void enqueueTankState(bool isLow);
  void enqueuePumpStarted(const PumpStartEvent &event);
  bool enqueueDebugReport(uint32_t now, const char *kind,
                          const char *requestId = nullptr);
  bool enqueue(QueueHandle_t queue, const char *kind, const char *text,
               const char *requestId = nullptr);
  SendResult sendMessage(const char *text, bool silent = false,
                         bool settingsButtons = false);
  SendResult callTelegram(const char *method, JsonDocument &request,
                          JsonDocument &response);
  void pollSettings();
  void processSettingsCommands();
  void sendSettingsReply(const SettingsReply &reply);
  void rememberUpdate(int64_t updateId);
  bool telegramReady() const;
  String nextEventId();
  TelegramDeliveryReport makeReport(const NotificationJob &job,
                                    const char *status,
                                    const SendResult *result = nullptr) const;
  void upsertAudit(const TelegramDeliveryReport &report);
  void loadAudit();
  void persistAuditLocked();
  static bool due(uint32_t now, uint32_t deadline);
  static uint32_t retryDelayMs(uint8_t attempts);
  static void workerEntry(void *argument);
  void workerLoop();
};

}  // namespace watering
