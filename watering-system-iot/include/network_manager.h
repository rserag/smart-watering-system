#pragma once

#include <ArduinoJson.h>
#include <WebSocketsClient.h>
#include <WiFi.h>

#include "config_store.h"
#include "diagnostics_commands.h"
#include "display_manager.h"
#include "telegram_notifier.h"
#include "watering_controller.h"

namespace watering {

class NetworkManager {
 public:
  NetworkManager(WateringController &controller, ConfigStore &configStore,
                 SystemConfig &config, TelegramNotifier &telegram, DisplayManager &display);

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
  static constexpr size_t MAX_OUTGOING_JSON_BYTES = 8192;
  static constexpr uint32_t SLOW_TRANSPORT_MS = 250;

  class TransportClient final : public WebSocketsClient {
   public:
    struct HeartbeatState {
      uint32_t intervalMs;
      uint32_t pongDeadlineMs;
      uint32_t lastPingMarkerAt;
      uint8_t misses;
      uint8_t missLimit;
      bool pongReceived;
    };
    HeartbeatState heartbeatState() const {
      // These scalar fields survive clientDisconnect() and require no access
      // to the TCP/TLS objects deleted before its event callback.
      return {_client.pingInterval, _client.pongTimeout, _client.lastPing,
              _client.pongTimeoutCount, _client.disconnectTimeoutCount,
              _client.pongReceived};
    }
    bool cachedSslResult(int &result) const {
#if defined(ESP32) && defined(HAS_SSL)
      if (_client.isSSL && _client.ssl != nullptr) {
        // Core 2.0.17 caches start_ssl_client()'s return, including a positive
        // socket descriptor on success. This is not the latest I/O error.
        // A one-byte scratch avoids retaining any formatted message.
        char ignored[1]{};
        result = _client.ssl->lastError(ignored, sizeof(ignored));
        return true;
      }
#endif
      result = 0;
      return false;
    }
    void abortTcpConnection() {
      // Public disconnect() sends another WebSocket frame. A short write may
      // have left the peer inside the previous frame, so use TCP cleanup only.
      WebSocketsClient::clientDisconnect(&_client, nullptr);
    }
  };

  WateringController &controller_;
  ConfigStore &configStore_;
  SystemConfig &config_;
  TelegramNotifier &telegram_;
  DisplayManager &display_;
  TransportClient webSocket_;

  bool wifiWasConnected_ = false;
  bool webSocketStarted_ = false;
  bool webSocketConnected_ = false;
  bool webSocketAbortPending_ = false;
  bool webSocketPongSeen_ = false;
  uint32_t lastWebSocketPongAt_ = 0;
  bool diagnosticsWifiSleepCaptured_ = false;
  bool diagnosticsWifiSleepApplied_ = false;
  wifi_ps_type_t previousDiagnosticsWifiSleep_ = WIFI_PS_MIN_MODEM;
  bool webSocketConfigurationWarningPrinted_ = false;
  uint32_t lastWifiAttemptAt_ = 0;
  uint32_t wifiRetryMs_ = WIFI_RETRY_MIN_MS;
  uint32_t lastTelemetryAt_ = 0;
  uint32_t sequence_ = 0;
  uint32_t debugSequence_ = 0;
  uint32_t lastDebugSnapshotAt_ = 0;
  DiagnosticsRequestLedger debugRequests_;
  String bootId_;
  String authorizationHeader_;
  String pendingTelegramDebugRequestId_;
  struct RecentCommand {
    String requestId;
    String command;
    String error;
    bool accepted = false;
  };
  RecentCommand recentCommands_[RECENT_REQUEST_COUNT];
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
  void handleDebugCommand(JsonObjectConst root, const char *type);
  void appendDebugSession(JsonObject session, uint32_t now);
  void sendDebugSnapshot(uint32_t now, const char *requestId = nullptr);
  void sendDebugAction(const DebugActionEvent &event, const uint8_t *addresses = nullptr, size_t count = 0);

  const RecentCommand *recentCommand(const String &requestId) const;
  void rememberRequest(const String &requestId, const char *command,
                       bool accepted, const char *error);
  void sendHello();
  void sendTelemetry(uint32_t now);
  void sendTelegramDeliveryReport();
  void sendConfigSnapshot(const String &requestId);
  void sendConfigAck(const String &requestId, uint32_t revision,
                     const char *status, const char *error = nullptr);
  void sendCommandAck(const String &requestId, const char *command,
                      const char *status, const char *error = nullptr);
  bool sendJson(JsonDocument &document);
  void abortFailedWebSocket();
  void updateDiagnosticsWifiSleep();
};

}  // namespace watering
