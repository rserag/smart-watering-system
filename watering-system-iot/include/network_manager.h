#pragma once

#include <ArduinoJson.h>
#include <WebSocketsClient.h>

#include "config_store.h"
#include "telegram_notifier.h"
#include "watering_controller.h"

namespace watering {

class NetworkManager {
 public:
  NetworkManager(WateringController &controller, ConfigStore &configStore,
                 SystemConfig &config, TelegramNotifier &telegram);

  void begin();
  void loop(uint32_t now);
  bool wifiConnected() const;
  bool backendConnected() const;

 private:
  static constexpr size_t MAX_MESSAGE_BYTES = 4096;
  static constexpr size_t RECENT_REQUEST_COUNT = 8;
  static constexpr uint32_t WIFI_RETRY_MIN_MS = 1000;
  static constexpr uint32_t WIFI_RETRY_MAX_MS = 60000;
  static constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;
  static constexpr uint32_t COMMAND_MAX_FUTURE_SECONDS = 300;

  WateringController &controller_;
  ConfigStore &configStore_;
  SystemConfig &config_;
  TelegramNotifier &telegram_;
  WebSocketsClient webSocket_;

  bool wifiWasConnected_ = false;
  bool webSocketStarted_ = false;
  bool webSocketConnected_ = false;
  bool webSocketConfigurationWarningPrinted_ = false;
  uint32_t lastWifiAttemptAt_ = 0;
  uint32_t wifiRetryMs_ = WIFI_RETRY_MIN_MS;
  uint32_t lastTelemetryAt_ = 0;
  uint32_t sequence_ = 0;
  String bootId_;
  String authorizationHeader_;
  String pendingTelegramDebugRequestId_;
  String recentRequestIds_[RECENT_REQUEST_COUNT];
  size_t nextRequestSlot_ = 0;

  void startWifiAttempt(uint32_t now);
  void handleWifi(uint32_t now);
  void handleWebSocketStartup();
  bool webSocketConfigured() const;
  void startWebSocket();
  void handleWebSocketEvent(WStype_t type, uint8_t *payload, size_t length);
  void handleIncomingText(const uint8_t *payload, size_t length);
  void processPendingTelegramDebug();

  bool decodeConfigSnapshot(JsonObjectConst root, SystemConfig &candidate,
                            String &error) const;
  void handleConfigSet(JsonObjectConst root);
  void handleCommand(JsonObjectConst root, const char *type);

  bool isDuplicateRequest(const String &requestId) const;
  void rememberRequest(const String &requestId);
  void sendHello();
  void sendTelemetry(uint32_t now);
  void sendTelegramDeliveryReport();
  void sendConfigSnapshot(const String &requestId);
  void sendConfigAck(const String &requestId, uint32_t revision,
                     const char *status, const char *error = nullptr);
  void sendCommandAck(const String &requestId, const char *command,
                      const char *status, const char *error = nullptr);
  bool sendJson(JsonDocument &document);
};

}  // namespace watering
