#include "network_manager.h"

#include <WiFi.h>
#include <esp_system.h>
#include <time.h>

#include "firmware_info.h"
#include "secrets.h"
#include "backend_trust.h"

namespace watering {

namespace {

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
                               ConfigStore &configStore, SystemConfig &config,
                               TelegramNotifier &telegram, DisplayManager &display)
    : controller_(controller),
      configStore_(configStore),
      config_(config),
      telegram_(telegram), display_(display) {}

void NetworkManager::begin() {
  const uint64_t chipId = ESP.getEfuseMac();
  char bootId[24];
  snprintf(bootId, sizeof(bootId), "%08lx-%08lx",
           static_cast<unsigned long>(chipId & 0xFFFFFFFF),
           static_cast<unsigned long>(esp_random()));
  bootId_ = bootId;
  telegram_.setBootId(bootId_);

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
  processPendingTelegramDebug();

  if (webSocketStarted_ && WiFi.status() == WL_CONNECTED &&
      (webSocketConnected_ || controller_.activeZone() < 0)) {
    webSocket_.loop();
  }

  now = millis();
  const bool stateChanged = controller_.consumeStateChanged();
  if (!controller_.debugSessionActive()) display_.cancelTest(now, "session ended");
  const bool telegramSettingsChanged = telegram_.consumeSettingsChanged();
  if (webSocketConnected_ &&
      (stateChanged || telegramSettingsChanged ||
       now - lastTelemetryAt_ >= config_.telemetryIntervalMs)) {
    sendTelemetry(now);
  }
  if (webSocketConnected_ && (stateChanged || now - lastDebugSnapshotAt_ >=
      (controller_.debugSessionActive() ? 1000u : config_.telemetryIntervalMs))) {
    sendDebugSnapshot(now);
  }
  if (webSocketConnected_) {
    DebugActionEvent action{};
    for (uint8_t index = 0; index < 8 && controller_.consumeDebugAction(action); ++index)
      sendDebugAction(action);
    if (display_.consumeTestEvent(action)) sendDebugAction(action);
    sendTelegramDeliveryReport();
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
    // Expiring dashboard recovery commands require a clock even when Telegram
    // credentials are absent. Synchronization runs asynchronously.
    configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");

    return;
  }

  if (!connected && wifiWasConnected_) {
    controller_.endDebugSession(now, "Wi-Fi disconnected");
    display_.cancelTest(now, "Wi-Fi disconnected");
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
  if (time(nullptr) < 1700000000) return;

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
#ifdef WEBSOCKET_CA_CERT
  webSocket_.beginSslWithCA(WEBSOCKET_HOST, WEBSOCKET_PORT, WEBSOCKET_PATH, WEBSOCKET_CA_CERT);
#else
  webSocket_.beginSslWithCA(WEBSOCKET_HOST, WEBSOCKET_PORT, WEBSOCKET_PATH, BACKEND_ROOT_CA);
#endif
  // beginSSL resets request headers, so authorization must be set after it.
  webSocket_.setAuthorization(authorizationHeader_.c_str());
  webSocketStarted_ = true;
  Serial.println("WebSocket TLS client started with certificate verification");
}

void NetworkManager::handleWebSocketEvent(WStype_t type, uint8_t *payload,
                                           size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      webSocketConnected_ = true;
      Serial.println("WebSocket connected");
      telegram_.replayDeliveryReports();
      sendHello();
      sendTelemetry(millis());
      sendDebugSnapshot(millis());
      break;
    case WStype_DISCONNECTED:
      controller_.endDebugSession(millis(), "backend disconnected");
      display_.cancelTest(millis(), "backend disconnected");
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

  if (!root["schemaVersion"].is<uint16_t>() || schemaVersion != CONFIG_SCHEMA_VERSION ||
      strcmp(targetDevice, DEVICE_ID) != 0) {
    Serial.println("Rejected WebSocket message for another schema or device");
    return;
  }

  if (strncmp(type, "debug.", 6) == 0) {
    handleDebugCommand(root, type);
  } else if (strcmp(type, "config.set") == 0) {
    handleConfigSet(root);
  } else if (strcmp(type, "telegram.delivery.ack") == 0) {
    const char *eventId = root["eventId"] | "";
    const uint32_t updateSequence = root["updateSequence"] | 0;
    if (eventId[0] != '\0' && updateSequence > 0) {
      telegram_.acknowledgeDelivery(eventId, updateSequence);
    }
  } else if (strcmp(type, "zone.water") == 0 ||
             strcmp(type, "zone.stop") == 0 ||
             strcmp(type, "system.stopAll") == 0 ||
             strcmp(type, "fault.clear") == 0 ||
             strcmp(type, "fault.clearPaused") == 0 ||
             strcmp(type, "zone.resume") == 0 ||
             strcmp(type, "telegram.debug.set") == 0 ||
             strcmp(type, "telegram.hourlySilent.set") == 0 ||
             strcmp(type, "telegram.debug.send") == 0 ||
             strcmp(type, "telemetry.request") == 0 ||
             strcmp(type, "config.get") == 0) {
    handleCommand(root, type);
  } else if (strcmp(type, "device.ready") == 0) {
    Serial.println("Server acknowledged device session");
  } else {
    Serial.println("Rejected unsupported WebSocket message type");
  }
}


void NetworkManager::appendDebugSession(JsonObject session, uint32_t now) {
  const DebugMode mode = controller_.debugMode();
  session["mode"] = mode == DebugMode::Hardware ? "hardware" :
                    mode == DebugMode::Simulation ? "simulation" : "observe";
  if (controller_.debugSessionActive()) {
    session["sessionId"] = controller_.debugSessionId();
    session["owner"] = controller_.debugOwner();
  } else {
    session["sessionId"] = nullptr;
    session["owner"] = nullptr;
  }
  session["leaseRemainingMs"] = controller_.debugLeaseRemainingMs(now);
  session["automationHeld"] = controller_.automationHeld();
}

void NetworkManager::sendDebugSnapshot(uint32_t now, const char *requestId) {
  JsonDocument document;
  document["type"] = "debug.snapshot";
  document["schemaVersion"] = CONFIG_SCHEMA_VERSION;
  document["deviceId"] = DEVICE_ID;
  document["bootId"] = bootId_;
  document["sequence"] = ++debugSequence_;
  document["uptimeMs"] = now;
  document["configRevision"] = config_.revision;
  if (requestId != nullptr) document["requestId"] = requestId;
  appendDebugSession(document["session"].to<JsonObject>(), now);
  JsonObject hardware = document["hardware"].to<JsonObject>();
  hardware["board"] = "esp32dev";
  hardware["relayActiveHigh"] = true;
  hardware["tankPin"] = controller_.tankPin();
  JsonArray relays = hardware["relayPins"].to<JsonArray>();
  JsonArray adcPins = hardware["sensorPins"].to<JsonArray>();
  for (size_t zone = 0; zone < ZONE_COUNT; ++zone) {
    relays.add(controller_.relayPin(zone));
    adcPins.add(controller_.sensorPin(zone));
  }
  JsonObject oled = hardware["oled"].to<JsonObject>();
  oled["sda"] = display_.sdaPin(); oled["scl"] = display_.sclPin();
  oled["address"] = display_.address(); oled["available"] = display_.available();
  JsonObject tank = document["tank"].to<JsonObject>();
  tank["physicalLow"] = controller_.physicalTankLow();
  tank["effectiveLow"] = controller_.effectiveTankLow();
  tank["low"] = controller_.mainTankLow();
  tank["pendingLow"] = controller_.pendingTankLow();
  tank["debounceRemainingMs"] = controller_.tankDebounceRemainingMs(now);
  if (controller_.tankOverride() < 0) tank["overrideLow"] = nullptr;
  else tank["overrideLow"] = controller_.tankOverride() != 0;
  JsonObject timings = document["timings"].to<JsonObject>();
  timings["loopDurationUs"] = controller_.lastLoopDurationUs();
  timings["maxLoopDurationUs"] = controller_.maxLoopDurationUs();
  timings["sampleDurationUs"] = controller_.sampleDurationUs();
  JsonArray zones = document["zones"].to<JsonArray>();
  for (size_t zone = 0; zone < ZONE_COUNT; ++zone) {
    const SensorState &sensor = controller_.sensorState(zone);
    const ZoneState &state = controller_.zoneState(zone);
    const ZoneConfig &config = controller_.config().zones[zone];
    JsonObject item = zones.add<JsonObject>();
    item["id"] = zone + 1;
    item["physicalRaw"] = sensor.physicalRaw;
    item["raw"] = sensor.latestRaw;
    JsonArray samples = item["samples"].to<JsonArray>();
    for (uint8_t index = 0; index < 16; ++index) samples.add(sensor.samples[index]);
    item["minRaw"] = sensor.minRaw; item["maxRaw"] = sensor.maxRaw;
    item["filteredRaw"] = sensor.filteredRaw;
    item["relativeMoisturePercent"] = sensor.moisturePercent;
    item["unclampedPercent"] = config.wetRaw == config.dryRaw ? 0.0f :
        100.0f * (int(sensor.filteredRaw) - config.dryRaw) / (config.wetRaw - config.dryRaw);
    item["sensorValid"] = sensor.valid;
    item["validSamples"] = sensor.validSampleCount;
    item["invalidSamples"] = sensor.invalidSampleCount;
    item["dryCount"] = state.consecutiveDryReadings;
    item["wetCount"] = state.consecutiveWetReadings;
    item["sampleAgeMs"] = now - sensor.lastReadingAt;
    item["phase"] = zonePhaseName(state.phase);
    if (state.fault == FaultCode::None) item["fault"] = nullptr;
    else item["fault"] = faultCodeName(state.fault);
    item["enabled"] = config.enabled;
    item["paused"] = state.paused;
    item["relayOn"] = controller_.physicalRelayOn(zone);
    item["virtualRelayOn"] = controller_.virtualRelayOn(zone);
    item["decisionReason"] = controller_.decisionReason(zone);
    item["phaseElapsedMs"] = now - state.phaseStartedAt;
    item["pulseRemainingMs"] = controller_.pulseRemainingMs(zone, now);
    item["wateringOnMsThisCycle"] = state.wateringOnMsThisCycle;
    if (controller_.soilOverride(zone) < 0) item["overrideRaw"] = nullptr;
    else item["overrideRaw"] = controller_.soilOverride(zone);
    item["dryRaw"] = config.dryRaw; item["wetRaw"] = config.wetRaw;
    item["startWateringPercent"] = config.startWateringPercent;
    item["stopWateringPercent"] = config.stopWateringPercent;
    item["pulseMs"] = config.pulseOnMs; item["soakMs"] = config.soakMs;
    item["maxOnMs"] = config.maxWateringOnMsPerCycle;
  }
  if (sendJson(document)) lastDebugSnapshotAt_ = now;
}

void NetworkManager::sendDebugAction(const DebugActionEvent &event,
                                    const uint8_t *addresses, size_t count) {
  JsonDocument document;
  document["type"] = "debug.action";
  document["schemaVersion"] = CONFIG_SCHEMA_VERSION;
  document["deviceId"] = DEVICE_ID;
  document["bootId"] = bootId_;
  if (event.sessionId[0] == '\0') document["sessionId"] = nullptr;
  else document["sessionId"] = event.sessionId;
  document["requestId"] = event.requestId;
  document["command"] = event.command;
  document["status"] = event.status;
  document["uptimeMs"] = event.uptimeMs;
  if (event.zoneId != 0) document["zoneId"] = event.zoneId;
  if (event.message[0] != '\0') document["message"] = event.message;
  if (addresses != nullptr) {
    JsonArray found = document["result"]["addresses"].to<JsonArray>();
    for (size_t index = 0; index < count; ++index) found.add(addresses[index]);
  }
  sendJson(document);
}

void NetworkManager::handleDebugCommand(JsonObjectConst root, const char *type) {
  const char *requestId = root["requestId"] | "";
  const char *error = nullptr;
  const uint32_t epoch = static_cast<uint32_t>(time(nullptr));
  if (!validateDiagnosticsCommand(root, bootId_.c_str(), epoch, error)) {
    sendCommandAck(requestId, type, "rejected", error);
    return;
  }
  if (strcmp(type, "debug.snapshot.get") == 0) {
    sendDebugSnapshot(millis(), requestId);
    return;
  }
  if (const auto *previous = debugRequests_.find(requestId, epoch)) {
    if (strcmp(previous->command, type) != 0)
      sendCommandAck(requestId, type, "rejected", "requestId belongs to another command");
    else sendCommandAck(requestId, type, previous->accepted ? "accepted" : "rejected",
                        previous->error[0] == '\0' ? nullptr : previous->error);
    return;
  }
  if (!debugRequests_.hasCapacity(epoch)) {
    sendCommandAck(requestId, type, "rejected", "diagnostic command capacity reached; wait for deadlines");
    return;
  }
  const uint32_t now = millis();
  if (controller_.debugSessionActive() && controller_.debugLeaseRemainingMs(now) == 0) {
    controller_.endDebugSession(now, "session lease expired");
    display_.cancelTest(now, "session lease expired");
  }
  const bool start = strcmp(type, "debug.session.start") == 0;
  const bool resume = strcmp(type, "debug.automation.resume") == 0;
  const bool stop = strcmp(type, "debug.output.stop") == 0;
  const char *sessionId = root["sessionId"] | "";
  bool success = false;
  uint8_t addresses[126]{};
  size_t addressCount = 0;
  if (!start && !resume && !(stop && !controller_.debugSessionActive()) &&
      (!controller_.debugSessionActive() || strcmp(controller_.debugSessionId(), sessionId) != 0)) {
    error = "active matching diagnostics session required";
  } else if (start) {
    const char *mode = root["mode"].as<const char *>();
    success = controller_.startDebugSession(strcmp(mode, "hardware") == 0 ? DebugMode::Hardware : DebugMode::Simulation,
                                            sessionId, root["owner"].as<const char *>(), now, error);
  } else if (resume) {
    success = controller_.resumeAutomation(now, error);
  } else if (strcmp(type, "debug.session.renew") == 0) {
    success = controller_.renewDebugSession(sessionId, now, error);
  } else if (strcmp(type, "debug.session.end") == 0) {
    controller_.endDebugSession(now);
    display_.cancelTest(now, "session ended");
    success = true;
  } else if (strcmp(type, "debug.output.pulse") == 0) {
    success = controller_.debugPulse(root["zoneId"].as<uint8_t>() - 1,
                                     root["durationMs"].as<uint32_t>(), requestId, now, error);
  } else if (stop) {
    success = controller_.debugStop(diagnosticsHasField(root, "zoneId") ? root["zoneId"].as<int>() - 1 : -1, now, error);
    if (!diagnosticsHasField(root, "zoneId")) display_.cancelTest(now, "stopped by operator");
  } else if (strcmp(type, "debug.input.set") == 0) {
    success = strcmp(root["input"].as<const char *>(), "soil") == 0
                  ? controller_.setDebugSoil(root["zoneId"].as<uint8_t>() - 1, root["raw"].as<int>(), now, error)
                  : controller_.setDebugTank(root["low"].as<bool>(), now, error);
  } else if (strcmp(type, "debug.input.clear") == 0) {
    success = controller_.clearDebugInput(root["input"].as<const char *>(),
        diagnosticsHasField(root, "zoneId") ? root["zoneId"].as<int>() - 1 : -1, now, error);
  } else if (strcmp(type, "debug.sample") == 0) {
    controller_.debugSample(now);
    success = true;
  } else if (strcmp(type, "debug.oled.test") == 0) {
    success = display_.startTest(root["pattern"].as<const char *>(), root["durationMs"].as<uint32_t>(),
                                 requestId, sessionId, now, error);
  } else if (strcmp(type, "debug.i2c.scan") == 0) {
    addressCount = display_.scanI2c(addresses, sizeof(addresses));
    success = true;
  }
  debugRequests_.remember(requestId, type, root["expiresAtEpoch"].as<uint32_t>(), epoch, success, error);
  sendCommandAck(requestId, type, success ? "accepted" : "rejected", error);
  if (success && strcmp(type, "debug.output.pulse") != 0) {
    DebugActionEvent event{};
    strlcpy(event.sessionId, sessionId, sizeof(event.sessionId));
    strlcpy(event.requestId, requestId, sizeof(event.requestId));
    strlcpy(event.command, type, sizeof(event.command));
    strlcpy(event.status, strcmp(type, "debug.oled.test") == 0 ? "started" : "completed", sizeof(event.status));
    event.uptimeMs = millis();
    if (diagnosticsHasField(root, "zoneId")) event.zoneId = root["zoneId"].as<uint8_t>();
    sendDebugAction(event, strcmp(type, "debug.i2c.scan") == 0 ? addresses : nullptr, addressCount);
  }
  sendDebugSnapshot(millis());
}

void NetworkManager::processPendingTelegramDebug() {
  if (pendingTelegramDebugRequestId_.isEmpty()) {
    return;
  }

  const String requestId = pendingTelegramDebugRequestId_;
  pendingTelegramDebugRequestId_ = "";

  // A manual request arrives inside the secure WebSocket callback. Let that
  // callback and its JSON document unwind before the Telegram worker allocates
  // a second TLS connection. Starting both paths together can exhaust the
  // ESP32's transient memory and restart the controller.
  sendCommandAck(requestId, "telegram.debug.send", "accepted");
  if (!telegram_.requestDebugReport(requestId, millis())) {
    sendCommandAck(requestId, "telegram.debug.send", "rejected",
                   "failed to queue Telegram debug report");
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
  if (controller_.automationHeld()) {
    sendConfigAck(requestId, revision, "rejected", "configuration is locked while diagnostics holds automation");
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
  if (const RecentCommand *previous = recentCommand(requestId)) {
    if (previous->command != type) {
      sendCommandAck(requestId, type, "rejected", "requestId belongs to another command");
      return;
    }
    sendCommandAck(requestId, type, previous->accepted ? "accepted" : "rejected",
                   previous->error.isEmpty() ? nullptr : previous->error.c_str());
    return;
  }

  const char *error = nullptr;
  bool success = false;
  if (controller_.automationHeld() && strcmp(type, "system.stopAll") != 0 &&
      strcmp(type, "telemetry.request") != 0 && strcmp(type, "config.get") != 0 &&
      strcmp(type, "zone.stop") != 0) {
    sendCommandAck(requestId, type, "rejected", "ordinary controls are locked while diagnostics holds automation");
    return;
  }

  if (strcmp(type, "system.stopAll") == 0) {
    controller_.emergencyStop(millis());
    success = true;
  } else if (strcmp(type, "telemetry.request") == 0) {
    sendTelemetry(millis());
    success = true;
  } else if (strcmp(type, "config.get") == 0) {
    sendConfigSnapshot(requestId);
    success = true;
  } else if (strcmp(type, "telegram.debug.set") == 0) {
    if (!root["enabled"].is<bool>()) {
      error = "enabled must be a boolean";
    } else {
      const bool enabled = root["enabled"].as<bool>();
      success = configStore_.saveTelegramDebugEnabled(enabled);
      if (success) {
        telegram_.setDebugEnabled(enabled, millis());
      } else {
        error = "failed to persist Telegram debug setting";
      }
    }
  } else if (strcmp(type, "telegram.hourlySilent.set") == 0) {
    if (!root["enabled"].is<bool>()) {
      error = "enabled must be a boolean";
    } else {
      success = telegram_.setHourlySilent(root["enabled"].as<bool>());
      if (!success) {
        error = "failed to persist Telegram hourly silent setting";
      }
    }
  } else if (strcmp(type, "telegram.debug.send") == 0) {
    const time_t currentTime = time(nullptr);
    const uint32_t expiresAt = root["expiresAtEpoch"] | 0;
    if (!telegram_.configured()) {
      error = "Telegram is not configured";
    } else if (!telegram_.workerRunning()) {
      error = "Telegram sender is not running";
    } else if (currentTime < 1700000000) {
      error = "device time is not synchronized";
    } else if (!root["expiresAtEpoch"].is<uint32_t>() ||
               expiresAt <= static_cast<uint32_t>(currentTime) ||
               expiresAt - static_cast<uint32_t>(currentTime) >
                   COMMAND_MAX_FUTURE_SECONDS) {
      error = "expiresAtEpoch must be within the next five minutes";
    } else if (!pendingTelegramDebugRequestId_.isEmpty()) {
      error = "another Telegram debug report is being queued";
    } else {
      pendingTelegramDebugRequestId_ = requestId;
      success = true;
    }
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
      if (controller_.automationHeld()) success = controller_.debugStop(zoneId - 1, millis(), error);
      else success = controller_.stopZone(zoneId - 1, millis(), error);
    } else if (strcmp(type, "fault.clear") == 0) {
      error = "use fault.clearPaused with the current recovery confirmation";
    } else if (strcmp(type, "fault.clearPaused") == 0 ||
               strcmp(type, "zone.resume") == 0) {
      const time_t currentTime = time(nullptr);
      const uint32_t expiresAt = root["expiresAtEpoch"] | 0;
      if (!root["expectedBootId"].is<const char *>() ||
          bootId_ != root["expectedBootId"].as<const char *>()) {
        error = "controller restarted; review the current channel state";
      } else if (currentTime < 1700000000 ||
                 !root["expiresAtEpoch"].is<uint32_t>() ||
                 expiresAt <= static_cast<uint32_t>(currentTime) ||
                 expiresAt - static_cast<uint32_t>(currentTime) > 60) {
        error = "recovery confirmation expired; review the current channel state";
      } else if (!root["expectedRecoveryGeneration"].is<uint32_t>()) {
        error = "expectedRecoveryGeneration is required";
      } else {
        const uint32_t generation = root["expectedRecoveryGeneration"].as<uint32_t>();
        success = strcmp(type, "fault.clearPaused") == 0
                      ? controller_.clearFaultAndPause(zoneId - 1, generation, millis(), error)
                      : controller_.resumeZone(zoneId - 1, generation, millis(), error);
      }
    }
  }

  rememberRequest(requestId, type, success, error);
  if (success && strcmp(type, "telegram.debug.send") == 0) {
    return;
  }
  sendCommandAck(requestId, type, success ? "accepted" : "rejected", error);
  if (success && (strcmp(type, "telegram.debug.set") == 0 ||
                  strcmp(type, "fault.clearPaused") == 0 ||
                  strcmp(type, "zone.resume") == 0)) {
    sendTelemetry(millis());
  }
}

const NetworkManager::RecentCommand *NetworkManager::recentCommand(const String &requestId) const {
  for (const RecentCommand &recent : recentCommands_) {
    if (!recent.requestId.isEmpty() && recent.requestId == requestId) {
      return &recent;
    }
  }
  return nullptr;
}

void NetworkManager::rememberRequest(const String &requestId, const char *command,
                                     bool accepted, const char *error) {
  RecentCommand &recent = recentCommands_[nextRequestSlot_];
  recent.requestId = requestId;
  recent.command = command;
  recent.accepted = accepted;
  recent.error = error == nullptr ? "" : error;
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
  document["directTelegram"] = true;
  document["telegramDebugEnabled"] = telegram_.debugEnabled();
  document["telegramHourlySilent"] = telegram_.hourlySilent();
  document["telegramConfigured"] = telegram_.configured();
  document["telegramWorkerRunning"] = telegram_.workerRunning();
  document["telegramTimeReady"] = telegram_.timeReady();
  document["uptimeMs"] = millis();
  document["diagnosticsVersion"] = 1;
  appendDebugSession(document["debug"].to<JsonObject>(), millis());
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
  document["diagnosticsVersion"] = 1;
  document["simulation"] = controller_.debugMode() == DebugMode::Simulation;
  document["wateringSource"] = controller_.debugMode() == DebugMode::Hardware ? "diagnostic" : "automatic";
  appendDebugSession(document["debug"].to<JsonObject>(), now);
  document["mainTankLow"] = controller_.debugMode() == DebugMode::Simulation ? controller_.physicalTankLow() : controller_.mainTankLow();
  document["directTelegram"] = true;
  document["telegramDebugEnabled"] = telegram_.debugEnabled();
  document["telegramHourlySilent"] = telegram_.hourlySilent();
  document["telegramConfigured"] = telegram_.configured();
  document["telegramPendingMessages"] = telegram_.pendingCount();
  document["telegramLastSendSucceeded"] = telegram_.lastSendSucceeded();
  document["telegramWorkerRunning"] = telegram_.workerRunning();
  document["telegramTimeReady"] = telegram_.timeReady();
  if (telegram_.lastFailureStage()[0] == '\0') {
    document["telegramLastFailureStage"] = nullptr;
  } else {
    document["telegramLastFailureStage"] = telegram_.lastFailureStage();
  }

  JsonArray zones = document["zones"].to<JsonArray>();
  for (size_t zone = 0; zone < ZONE_COUNT; ++zone) {
    const SensorState &sensor = controller_.liveSensorState(zone);
    const ZoneState &state = controller_.liveZoneState(zone);
    JsonObject item = zones.add<JsonObject>();
    item["id"] = zone + 1;
    item["raw"] = sensor.latestRaw;
    item["filteredRaw"] = sensor.filteredRaw;
    item["relativeMoisturePercent"] = sensor.moisturePercent;
    item["sensorValid"] = sensor.valid;
    item["phase"] = zonePhaseName(state.phase);
    item["paused"] = state.paused;
    item["recoveryGeneration"] = state.recoveryGeneration;
    item["relayOn"] = controller_.physicalRelayOn(zone);
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

void NetworkManager::sendTelegramDeliveryReport() {
  TelegramDeliveryReport report{};
  if (!telegram_.nextDeliveryReport(report)) {
    return;
  }

  JsonDocument document;
  document["type"] = "telegram.delivery";
  document["schemaVersion"] = CONFIG_SCHEMA_VERSION;
  document["deviceId"] = DEVICE_ID;
  document["eventId"] = report.eventId;
  document["updateSequence"] = report.updateSequence;
  document["kind"] = report.kind;
  document["status"] = report.status;
  document["attempt"] = report.attempt;
  document["uptimeMs"] = report.uptimeMs;
  document["pendingCount"] = telegram_.pendingCount();
  if (report.requestId[0] != '\0') {
    document["requestId"] = report.requestId;
  }
  if (report.errorStage[0] != '\0') {
    document["errorStage"] = report.errorStage;
  }
  if (report.httpStatus != 0) {
    document["httpStatus"] = report.httpStatus;
  }
  if (report.telegramErrorCode != 0) {
    document["telegramErrorCode"] = report.telegramErrorCode;
  }
  if (report.telegramMessageId != 0) {
    document["telegramMessageId"] = report.telegramMessageId;
  }
  if (!sendJson(document)) {
    telegram_.markDeliveryReportForRetry(report.eventId,
                                         report.updateSequence);
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
