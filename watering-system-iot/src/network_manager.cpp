#include "network_manager.h"

#include <WiFi.h>
#include <esp_system.h>

#include "secrets.h"

namespace watering {

namespace {

constexpr char FIRMWARE_VERSION[] = "0.4.0";

bool hasZoneConfigFields(JsonObjectConst zone) {
  return zone["id"].is<uint8_t>() && zone["enabled"].is<bool>() &&
         zone["dryRaw"].is<int>() && zone["wetRaw"].is<int>() &&
         zone["startWateringPercent"].is<uint8_t>() &&
         zone["stopWateringPercent"].is<uint8_t>() &&
         zone["dryConfirmationSamples"].is<uint8_t>() &&
         zone["wetConfirmationSamples"].is<uint8_t>() &&
         zone["pulseOnMs"].is<uint32_t>() && zone["soakMs"].is<uint32_t>() &&
         zone["maxWateringOnMsPerCycle"].is<uint32_t>() &&
         zone["cooldownMs"].is<uint32_t>();
}

}  // namespace

NetworkManager::NetworkManager(WateringController &controller,
                               ConfigStore &configStore, SystemConfig &config)
    : controller_(controller), configStore_(configStore), config_(config) {}

void NetworkManager::begin() {
  const uint64_t chipId = ESP.getEfuseMac();
  char bootId[24];
  snprintf(bootId, sizeof(bootId), "%08lx-%04x",
           static_cast<unsigned long>(chipId & 0xFFFFFFFF),
           static_cast<unsigned>(esp_random() & 0xFFFF));
  bootId_ = bootId;

  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.onEvent(
      [](arduino_event_id_t, arduino_event_info_t info) {
        const uint8_t reason = info.wifi_sta_disconnected.reason;
        Serial.printf("Wi-Fi station disconnected: %s (reason %u)\n",
                      WiFi.disconnectReasonName(
                          static_cast<wifi_err_reason_t>(reason)),
                      static_cast<unsigned>(reason));
      },
      ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.mode(WIFI_STA);

  webSocket_.onEvent(
      [this](WStype_t type, uint8_t *payload, size_t length) {
        handleWebSocketEvent(type, payload, length);
      });
  webSocket_.enableHeartbeat(15000, 3000, 2);
  webSocket_.setReconnectInterval(5000);

  startWifiAttempt(millis());
}

void NetworkManager::loop(uint32_t now) {
  handleWifi(now);
  handleWebSocketStartup();

  if (webSocketStarted_ && WiFi.status() == WL_CONNECTED &&
      (webSocketConnected_ || controller_.activeZone() < 0)) {
    webSocket_.loop();
  }

  const bool stateChanged = controller_.consumeStateChanged();
  if (webSocketConnected_ &&
      (stateChanged || now - lastTelemetryAt_ >= config_.telemetryIntervalMs)) {
    sendTelemetry(now);
  }
}

bool NetworkManager::wifiConnected() const {
  return WiFi.status() == WL_CONNECTED;
}

bool NetworkManager::backendConnected() const {
  return webSocketConnected_;
}

void NetworkManager::startWifiAttempt(uint32_t now) {
  lastWifiAttemptAt_ = now;
  Serial.println("Starting Wi-Fi connection attempt");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void NetworkManager::handleWifi(uint32_t now) {
  const bool connected = WiFi.status() == WL_CONNECTED;

  if (connected && !wifiWasConnected_) {
    wifiWasConnected_ = true;
    wifiRetryMs_ = WIFI_RETRY_MIN_MS;
    Serial.print("Wi-Fi connected. IP address: ");
    Serial.println(WiFi.localIP());

    return;
  }

  if (!connected && wifiWasConnected_) {
    wifiWasConnected_ = false;
    webSocketConnected_ = false;
    if (webSocketStarted_) {
      webSocket_.disconnect();
      webSocketStarted_ = false;
    }
    Serial.println("Wi-Fi disconnected; local watering remains active");
    wifiRetryMs_ = WIFI_RETRY_MIN_MS;
    startWifiAttempt(now);
  }

  if (!connected &&
      now - lastWifiAttemptAt_ >= WIFI_CONNECT_TIMEOUT_MS + wifiRetryMs_) {
    Serial.printf("Wi-Fi still not connected (status %d); retrying\n",
                  static_cast<int>(WiFi.status()));
    startWifiAttempt(now);
    wifiRetryMs_ = min(wifiRetryMs_ * 2, WIFI_RETRY_MAX_MS);
  }
}

void NetworkManager::handleWebSocketStartup() {
  if (WiFi.status() != WL_CONNECTED || !WEBSOCKET_ENABLED ||
      webSocketStarted_ || controller_.activeZone() >= 0) {
    return;
  }

  if (!webSocketConfigured()) {
    if (!webSocketConfigurationWarningPrinted_) {
      Serial.println(
          "WebSocket enabled but host or token is missing; "
          "connection refused for safety");
      webSocketConfigurationWarningPrinted_ = true;
    }
    return;
  }

  startWebSocket();
}

bool NetworkManager::webSocketConfigured() const {
  return WEBSOCKET_ENABLED && WEBSOCKET_HOST[0] != '\0' &&
         DEVICE_TOKEN[0] != '\0';
}

void NetworkManager::startWebSocket() {
  authorizationHeader_ = "Bearer ";
  authorizationHeader_ += DEVICE_TOKEN;
  // No CA or fingerprint is supplied, so the TLS client accepts an untrusted
  // server certificate. Traffic is encrypted, but the server is not verified.
  webSocket_.beginSSL(WEBSOCKET_HOST, WEBSOCKET_PORT, WEBSOCKET_PATH);
  // beginSSL resets request headers, so authorization must be set after it.
  webSocket_.setAuthorization(authorizationHeader_.c_str());
  webSocketStarted_ = true;
  Serial.println("WebSocket TLS client started without certificate verification");
}

void NetworkManager::handleWebSocketEvent(WStype_t type, uint8_t *payload,
                                           size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      webSocketConnected_ = true;
      Serial.println("WebSocket connected");
      sendHello();
      sendTelemetry(millis());
      break;
    case WStype_DISCONNECTED:
      if (webSocketConnected_) {
        Serial.println("WebSocket disconnected; local watering remains active");
      }
      webSocketConnected_ = false;
      break;
    case WStype_TEXT:
      handleIncomingText(payload, length);
      break;
    case WStype_ERROR:
      Serial.println("WebSocket transport error");
      break;
    default:
      break;
  }
}

void NetworkManager::handleIncomingText(const uint8_t *payload, size_t length) {
  if (length == 0 || length > MAX_MESSAGE_BYTES) {
    Serial.println("Rejected WebSocket message with invalid size");
    return;
  }

  JsonDocument document;
  const DeserializationError parseError =
      deserializeJson(document, payload, length);
  if (parseError) {
    Serial.println("Rejected malformed WebSocket JSON");
    return;
  }

  const JsonObjectConst root = document.as<JsonObjectConst>();
  const char *type = root["type"] | "";
  const uint16_t schemaVersion = root["schemaVersion"] | 0;
  if (!root["deviceId"].is<const char *>()) {
    Serial.println("Rejected WebSocket message without deviceId");
    return;
  }
  const char *targetDevice = root["deviceId"].as<const char *>();

  if (schemaVersion != CONFIG_SCHEMA_VERSION ||
      strcmp(targetDevice, DEVICE_ID) != 0) {
    Serial.println("Rejected WebSocket message for another schema or device");
    return;
  }

  if (strcmp(type, "config.set") == 0) {
    handleConfigSet(root);
  } else if (strcmp(type, "zone.water") == 0 ||
             strcmp(type, "zone.stop") == 0 ||
             strcmp(type, "system.stopAll") == 0 ||
             strcmp(type, "fault.clear") == 0 ||
             strcmp(type, "telemetry.request") == 0 ||
             strcmp(type, "config.get") == 0) {
    handleCommand(root, type);
  } else if (strcmp(type, "device.ready") == 0) {
    Serial.println("Server acknowledged device session");
  } else {
    Serial.println("Rejected unsupported WebSocket message type");
  }
}

bool NetworkManager::decodeConfigSnapshot(JsonObjectConst root,
                                          SystemConfig &candidate,
                                          String &error) const {
  if (!root["revision"].is<uint32_t>() ||
      !root["config"].is<JsonObjectConst>()) {
    error = "missing revision or config object";
    return false;
  }

  const JsonObjectConst incoming = root["config"].as<JsonObjectConst>();
  if (!incoming["sampleIntervalMs"].is<uint32_t>() ||
      !incoming["telemetryIntervalMs"].is<uint32_t>() ||
      !incoming["automaticWateringEnabled"].is<bool>() ||
      !incoming["maxConcurrentZones"].is<uint8_t>() ||
      !incoming["zones"].is<JsonArrayConst>()) {
    error = "incomplete system configuration";
    return false;
  }

  candidate = SystemConfig{};
  candidate.schemaVersion = CONFIG_SCHEMA_VERSION;
  candidate.revision = root["revision"].as<uint32_t>();
  candidate.automaticWateringEnabled =
      incoming["automaticWateringEnabled"].as<bool>();
  candidate.sampleIntervalMs = incoming["sampleIntervalMs"].as<uint32_t>();
  candidate.telemetryIntervalMs =
      incoming["telemetryIntervalMs"].as<uint32_t>();
  candidate.maxConcurrentZones =
      incoming["maxConcurrentZones"].as<uint8_t>();

  bool seen[ZONE_COUNT]{};
  const JsonArrayConst zones = incoming["zones"].as<JsonArrayConst>();
  if (zones.size() != ZONE_COUNT) {
    error = "configuration must contain exactly four zones";
    return false;
  }

  for (JsonObjectConst incomingZone : zones) {
    if (!hasZoneConfigFields(incomingZone)) {
      error = "a zone configuration is incomplete";
      return false;
    }

    const uint8_t id = incomingZone["id"].as<uint8_t>();
    if (id == 0 || id > ZONE_COUNT || seen[id - 1]) {
      error = "zone IDs must be unique values from 1 to 4";
      return false;
    }
    seen[id - 1] = true;

    ZoneConfig &zone = candidate.zones[id - 1];
    zone.enabled = incomingZone["enabled"].as<bool>();
    zone.dryRaw = incomingZone["dryRaw"].as<int>();
    zone.wetRaw = incomingZone["wetRaw"].as<int>();
    zone.startWateringPercent =
        incomingZone["startWateringPercent"].as<uint8_t>();
    zone.stopWateringPercent =
        incomingZone["stopWateringPercent"].as<uint8_t>();
    zone.dryConfirmationSamples =
        incomingZone["dryConfirmationSamples"].as<uint8_t>();
    zone.wetConfirmationSamples =
        incomingZone["wetConfirmationSamples"].as<uint8_t>();
    zone.pulseOnMs = incomingZone["pulseOnMs"].as<uint32_t>();
    zone.soakMs = incomingZone["soakMs"].as<uint32_t>();
    zone.maxWateringOnMsPerCycle =
        incomingZone["maxWateringOnMsPerCycle"].as<uint32_t>();
    zone.cooldownMs = incomingZone["cooldownMs"].as<uint32_t>();
  }

  const ValidationResult validation = validateConfig(candidate);
  if (!validation.valid) {
    error = validation.message;
    return false;
  }
  return true;
}

void NetworkManager::handleConfigSet(JsonObjectConst root) {
  const String requestId = root["requestId"] | "";
  const uint32_t revision = root["revision"] | 0;
  if (requestId.isEmpty()) {
    Serial.println("Rejected config.set without requestId");
    return;
  }

  if (revision <= config_.revision) {
    sendConfigAck(requestId, revision, "rejected",
                  "revision is not newer than the active configuration");
    return;
  }

  SystemConfig candidate{};
  String error;
  if (!decodeConfigSnapshot(root, candidate, error)) {
    sendConfigAck(requestId, revision, "rejected", error.c_str());
    return;
  }

  controller_.prepareForConfigUpdate(millis());
  if (!configStore_.save(candidate)) {
    sendConfigAck(requestId, revision, "rejected",
                  "failed to persist configuration");
    return;
  }

  config_ = candidate;
  controller_.applyConfig(config_, millis());
  sendConfigAck(requestId, revision, "applied");
}

void NetworkManager::handleCommand(JsonObjectConst root, const char *type) {
  const String requestId = root["requestId"] | "";
  if (requestId.isEmpty()) {
    Serial.println("Rejected command without requestId");
    return;
  }
  if (isDuplicateRequest(requestId)) {
    sendCommandAck(requestId, type, "duplicate");
    return;
  }

  const char *error = nullptr;
  bool success = false;

  if (strcmp(type, "system.stopAll") == 0) {
    controller_.emergencyStop(millis());
    success = true;
  } else if (strcmp(type, "telemetry.request") == 0) {
    sendTelemetry(millis());
    success = true;
  } else if (strcmp(type, "config.get") == 0) {
    sendConfigSnapshot(requestId);
    success = true;
  } else {
    const uint8_t zoneId = root["zoneId"] | 0;
    if (zoneId == 0 || zoneId > ZONE_COUNT) {
      error = "zoneId must be from 1 to 4";
    } else if (strcmp(type, "zone.water") == 0) {
      const time_t currentTime = time(nullptr);
      const uint32_t expiresAt = root["expiresAtEpoch"] | 0;
      if (!root["expiresAtEpoch"].is<uint32_t>() ||
          expiresAt <= static_cast<uint32_t>(currentTime) ||
          expiresAt - static_cast<uint32_t>(currentTime) >
              COMMAND_MAX_FUTURE_SECONDS) {
        error = "expiresAtEpoch must be within the next five minutes";
      } else {
        const uint32_t durationMs = root["durationMs"] | 0;
        success = controller_.requestManualWater(
            zoneId - 1, durationMs, millis(), error);
      }
    } else if (strcmp(type, "zone.stop") == 0) {
      success = controller_.stopZone(zoneId - 1, millis(), error);
    } else if (strcmp(type, "fault.clear") == 0) {
      success = controller_.clearFault(zoneId - 1, millis(), error);
    }
  }

  rememberRequest(requestId);
  sendCommandAck(requestId, type, success ? "accepted" : "rejected", error);
}

bool NetworkManager::isDuplicateRequest(const String &requestId) const {
  for (const String &recent : recentRequestIds_) {
    if (!recent.isEmpty() && recent == requestId) {
      return true;
    }
  }
  return false;
}

void NetworkManager::rememberRequest(const String &requestId) {
  recentRequestIds_[nextRequestSlot_] = requestId;
  nextRequestSlot_ = (nextRequestSlot_ + 1) % RECENT_REQUEST_COUNT;
}

void NetworkManager::sendHello() {
  JsonDocument document;
  document["type"] = "device.hello";
  document["schemaVersion"] = CONFIG_SCHEMA_VERSION;
  document["deviceId"] = DEVICE_ID;
  document["firmwareVersion"] = FIRMWARE_VERSION;
  document["bootId"] = bootId_;
  document["configRevision"] = config_.revision;
  document["automaticWateringEnabled"] =
      config_.automaticWateringEnabled;
  document["uptimeMs"] = millis();
  sendJson(document);
}

void NetworkManager::sendTelemetry(uint32_t now) {
  if (!webSocketConnected_) {
    return;
  }

  JsonDocument document;
  document["type"] = "telemetry";
  document["schemaVersion"] = CONFIG_SCHEMA_VERSION;
  document["deviceId"] = DEVICE_ID;
  document["bootId"] = bootId_;
  document["sequence"] = ++sequence_;
  document["uptimeMs"] = now;
  document["configRevision"] = config_.revision;
  document["wifiRssi"] = WiFi.RSSI();
  document["mainTankLow"] = controller_.mainTankLow();

  JsonArray zones = document["zones"].to<JsonArray>();
  for (size_t zone = 0; zone < ZONE_COUNT; ++zone) {
    const SensorState &sensor = controller_.sensorState(zone);
    const ZoneState &state = controller_.zoneState(zone);
    JsonObject item = zones.add<JsonObject>();
    item["id"] = zone + 1;
    item["raw"] = sensor.latestRaw;
    item["filteredRaw"] = sensor.filteredRaw;
    item["relativeMoisturePercent"] = sensor.moisturePercent;
    item["sensorValid"] = sensor.valid;
    item["phase"] = zonePhaseName(state.phase);
    item["relayOn"] = state.relayOn;
    item["wateringOnMsThisCycle"] = state.wateringOnMsThisCycle;
    if (state.fault == FaultCode::None) {
      item["fault"] = nullptr;
    } else {
      item["fault"] = faultCodeName(state.fault);
    }
  }

  if (sendJson(document)) {
    lastTelemetryAt_ = now;
  }
}

void NetworkManager::sendConfigSnapshot(const String &requestId) {
  JsonDocument document;
  document["type"] = "config.snapshot";
  document["schemaVersion"] = CONFIG_SCHEMA_VERSION;
  document["deviceId"] = DEVICE_ID;
  document["requestId"] = requestId;
  document["revision"] = config_.revision;

  JsonObject output = document["config"].to<JsonObject>();
  output["automaticWateringEnabled"] = config_.automaticWateringEnabled;
  output["sampleIntervalMs"] = config_.sampleIntervalMs;
  output["telemetryIntervalMs"] = config_.telemetryIntervalMs;
  output["maxConcurrentZones"] = config_.maxConcurrentZones;

  JsonArray zones = output["zones"].to<JsonArray>();
  for (size_t index = 0; index < ZONE_COUNT; ++index) {
    const ZoneConfig &zone = config_.zones[index];
    JsonObject item = zones.add<JsonObject>();
    item["id"] = index + 1;
    item["enabled"] = zone.enabled;
    item["dryRaw"] = zone.dryRaw;
    item["wetRaw"] = zone.wetRaw;
    item["startWateringPercent"] = zone.startWateringPercent;
    item["stopWateringPercent"] = zone.stopWateringPercent;
    item["dryConfirmationSamples"] = zone.dryConfirmationSamples;
    item["wetConfirmationSamples"] = zone.wetConfirmationSamples;
    item["pulseOnMs"] = zone.pulseOnMs;
    item["soakMs"] = zone.soakMs;
    item["maxWateringOnMsPerCycle"] =
        zone.maxWateringOnMsPerCycle;
    item["cooldownMs"] = zone.cooldownMs;
  }

  sendJson(document);
}

void NetworkManager::sendConfigAck(const String &requestId, uint32_t revision,
                                   const char *status, const char *error) {
  JsonDocument document;
  document["type"] = "config.ack";
  document["schemaVersion"] = CONFIG_SCHEMA_VERSION;
  document["deviceId"] = DEVICE_ID;
  document["requestId"] = requestId;
  document["revision"] = revision;
  document["status"] = status;
  if (error != nullptr) {
    document["message"] = error;
  }
  sendJson(document);
}

void NetworkManager::sendCommandAck(const String &requestId,
                                    const char *command, const char *status,
                                    const char *error) {
  JsonDocument document;
  document["type"] = "command.ack";
  document["schemaVersion"] = CONFIG_SCHEMA_VERSION;
  document["deviceId"] = DEVICE_ID;
  document["requestId"] = requestId;
  document["command"] = command;
  document["status"] = status;
  if (error != nullptr) {
    document["message"] = error;
  }
  sendJson(document);
}

bool NetworkManager::sendJson(JsonDocument &document) {
  if (!webSocketConnected_) {
    return false;
  }

  String payload;
  payload.reserve(2048);
  serializeJson(document, payload);
  return webSocket_.sendTXT(payload);
}

}  // namespace watering
