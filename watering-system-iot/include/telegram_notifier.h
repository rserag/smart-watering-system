#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <cstddef>
#include "config_store.h"
#include "telegram_settings.h"
#include "telegram_recovery.h"

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
  static constexpr uint8_t CRITICAL_QUEUE_LENGTH = 8;
  static constexpr uint8_t STANDARD_QUEUE_LENGTH = 12;
  static constexpr uint8_t AUDIT_LENGTH = 16;
  static constexpr uint32_t WORKER_STACK_BYTES = 8192;
  static constexpr uint32_t RECOVERY_CONFIRMATION_MS = 60000;
  static constexpr uint8_t RECOVERY_CONFIRMATION_COUNT = 8;

  struct RecoveryButton {
    char text[32];
    char data[65];
  };

  struct NotificationJob {
    char eventId[48];
    char requestId[101];
    char kind[24];
    char text[MESSAGE_BYTES];
    bool silent;
    uint8_t attempts;
    uint32_t updateSequence;
    uint32_t nextAttemptAt;
    RecoveryButton recoveryButton;
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

  struct RecoveryCommand {
    int64_t updateId;
    int64_t actorId;
    int64_t messageId;
    char callbackId[128];
    TelegramRecoveryRequest request;
  };

  struct RecoveryReply {
    RecoveryCommand command;
    char text[MESSAGE_BYTES];
    char acknowledgement[128];
    RecoveryButton buttons[ZONE_COUNT];
    uint8_t buttonCount;
    bool removeButtons;
  };

  // Preserve the existing NVS blob layout, but use the report array itself as
  // the persistence buffer. Copying this ~4 KB blob onto a task stack leaves
  // too little room for the nested NVS/flash calls.
  struct StoredAudit {
    uint32_t magic;
    uint16_t recordSize;
    uint8_t count;
    TelegramDeliveryReport reports[AUDIT_LENGTH];
  };
  static_assert(offsetof(StoredAudit, reports) == 8,
                "persisted Telegram audit header must remain compatible");
  static_assert(sizeof(StoredAudit) == 8 + sizeof(TelegramDeliveryReport) * AUDIT_LENGTH,
                "persisted Telegram audit size must remain compatible");

  // Owned exclusively by the main task. Consuming before a controller mutation
  // makes duplicate callbacks harmless even if Telegram redelivers an update.
  using RecoveryConfirmation = TelegramRecoveryConfirmation;

  WateringController &controller_;
  ConfigStore &configStore_;
  QueueHandle_t settingsCommands_ = nullptr;
  QueueHandle_t settingsReplies_ = nullptr;
  QueueHandle_t recoveryCommands_ = nullptr;
  QueueHandle_t recoveryReplies_ = nullptr;
  QueueHandle_t criticalQueue_ = nullptr;
  QueueHandle_t standardQueue_ = nullptr;
  TaskHandle_t workerTask_ = nullptr;
  SemaphoreHandle_t auditMutex_ = nullptr;
  bool debugEnabled_ = false;
  bool hourlySilent_ = false;
  bool settingsChanged_ = false;
  int64_t nextUpdateId_ = 0;
  bool settingsPending_ = false;
  bool recoveryPending_ = false;
  RecoveryConfirmation confirmations_[RECOVERY_CONFIRMATION_COUNT]{};
  uint8_t nextConfirmation_ = 0;
  uint32_t alertedFaultGeneration_[ZONE_COUNT]{};
  uint32_t nextFaultAlertAt_[ZONE_COUNT]{};
  uint32_t nextPollAt_ = 0;
  bool timeSyncStarted_ = false;
  bool workerRunning_ = false;
  uint32_t nextDebugAt_ = 0;
  uint32_t eventSequence_ = 0;
  String bootId_;
  volatile bool lastSendSucceeded_ = false;
  char lastFailureStage_[24]{};
  StoredAudit auditStorage_{};
  bool auditDirty_[AUDIT_LENGTH]{};
  uint8_t auditCount_ = 0;
  // These queue receive buffers belong only to the worker. Keeping them in the
  // notifier avoids retaining ~3 KB of locals while HTTPS or NVS calls run.
  NotificationJob workerJob_{};
  RecoveryReply workerRecoveryReply_{};
  SettingsReply workerSettingsReply_{};
  uint32_t workerMinimumStackFreeBytes_ = WORKER_STACK_BYTES;

  void enqueueTankState(bool isLow);
  void enqueuePumpStarted(const PumpStartEvent &event);
  bool enqueueDebugReport(uint32_t now, const char *kind,
                          const char *requestId = nullptr);
  bool enqueue(QueueHandle_t queue, const char *kind, const char *text,
               const char *requestId = nullptr,
               const RecoveryButton *button = nullptr);
  SendResult sendMessage(const char *text, bool silent = false,
                         bool settingsButtons = false,
                         const RecoveryButton *buttons = nullptr,
                         uint8_t buttonCount = 0);
  SendResult callTelegram(const char *method, JsonDocument &request,
                          JsonDocument &response);
  void pollSettings();
  void processSettingsCommands();
  void sendSettingsReply(const SettingsReply &reply);
  void processRecoveryCommands(uint32_t now);
  void sendRecoveryReply(const RecoveryReply &reply);
  void enqueueWateringFaults();
  void showRecoveryConfirmation(RecoveryReply &reply, uint32_t now);
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
  void recordWorkerStackWatermark();
};

}  // namespace watering
