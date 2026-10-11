#pragma once

#include <ArduinoJson.h>
#include <cstdint>
#include <cstring>

namespace watering {

inline bool diagnosticsHasField(JsonObjectConst root, const char *key) {
  for (JsonPairConst entry : root) if (std::strcmp(entry.key().c_str(), key) == 0) return true;
  return false;
}

inline bool diagnosticsUuid(const char *value) {
  if (value == nullptr || std::strlen(value) != 36) return false;
  for (size_t index = 0; index < 36; ++index) {
    const char c = value[index];
    if (index == 8 || index == 13 || index == 18 || index == 23) {
      if (c != '-') return false;
    } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
  }
  return true;
}

inline bool validateDiagnosticsCommand(JsonObjectConst root, const char *bootId,
                                       uint32_t epoch, const char *&error) {
  if (!root["type"].is<const char *>() || !root["requestId"].is<const char *>() ||
      std::strlen(root["requestId"].as<const char *>()) == 0 ||
      std::strlen(root["requestId"].as<const char *>()) > 64 ||
      !root["expectedBootId"].is<const char *>() ||
      std::strcmp(root["expectedBootId"].as<const char *>(), bootId) != 0) {
    error = "invalid requestId or controller boot changed"; return false;
  }
  if (epoch < 1700000000 || !root["expiresAtEpoch"].is<uint32_t>()) {
    error = "synchronized time and command deadline required"; return false;
  }
  const uint32_t deadline = root["expiresAtEpoch"].as<uint32_t>();
  if (deadline <= epoch || deadline - epoch > 30) {
    error = "command deadline must be within the next 30 seconds"; return false;
  }
  const char *type = root["type"].as<const char *>();
  const char *fields = "";
  if (std::strcmp(type, "debug.session.start") == 0) {
    fields = "mode sessionId owner";
    if (!root["mode"].is<const char *>() ||
        (std::strcmp(root["mode"].as<const char *>(), "hardware") != 0 &&
         std::strcmp(root["mode"].as<const char *>(), "simulation") != 0) ||
        !root["owner"].is<const char *>() ||
        std::strcmp(root["owner"].as<const char *>(), "sahakyan.raphayel@gmail.com") != 0) {
      error = "invalid diagnostic mode or owner"; return false;
    }
  } else if (std::strcmp(type, "debug.session.renew") == 0 || std::strcmp(type, "debug.session.end") == 0) {
    fields = "sessionId";
  } else if (std::strcmp(type, "debug.output.pulse") == 0) {
    fields = "sessionId zoneId durationMs";
    if (!root["durationMs"].is<uint32_t>() || root["durationMs"].as<uint32_t>() < 100 ||
        root["durationMs"].as<uint32_t>() > 60000) { error = "pulse duration must be 100 to 60000ms"; return false; }
  } else if (std::strcmp(type, "debug.output.stop") == 0) {
    fields = "sessionId zoneId";
  } else if (std::strcmp(type, "debug.input.set") == 0) {
    if (!root["input"].is<const char *>()) { error = "input must be soil or tank"; return false; }
    const char *input = root["input"].as<const char *>();
    if (std::strcmp(input, "soil") == 0) {
      fields = "sessionId input zoneId raw";
      if (!root["raw"].is<int>() || root["raw"].as<int>() < 0 || root["raw"].as<int>() > 4095) {
        error = "raw must be an integer from 0 to 4095"; return false;
      }
    } else if (std::strcmp(input, "tank") == 0) {
      fields = "sessionId input low";
      if (!root["low"].is<bool>()) { error = "low must be boolean"; return false; }
    } else { error = "unknown input"; return false; }
  } else if (std::strcmp(type, "debug.input.clear") == 0) {
    fields = "sessionId input zoneId";
    if (!root["input"].is<const char *>()) { error = "input is required"; return false; }
    const char *input = root["input"].as<const char *>();
    if (std::strcmp(input, "soil") != 0 && std::strcmp(input, "tank") != 0 && std::strcmp(input, "all") != 0) {
      error = "unknown input"; return false;
    }
    if (std::strcmp(input, "soil") != 0 && diagnosticsHasField(root, "zoneId")) {
      error = "zoneId is only valid for soil input"; return false;
    }
  } else if (std::strcmp(type, "debug.oled.test") == 0) {
    fields = "sessionId pattern durationMs";
    if (!root["pattern"].is<const char *>() || !root["durationMs"].is<uint32_t>() ||
        root["durationMs"].as<uint32_t>() < 100 || root["durationMs"].as<uint32_t>() > 10000) {
      error = "OLED duration must be 100 to 10000ms"; return false;
    }
    const char *pattern = root["pattern"].as<const char *>();
    if (std::strcmp(pattern, "checkerboard") != 0 && std::strcmp(pattern, "text") != 0 && std::strcmp(pattern, "off") != 0) {
      error = "unknown OLED pattern"; return false;
    }
  } else if (std::strcmp(type, "debug.sample") == 0 || std::strcmp(type, "debug.i2c.scan") == 0) {
    fields = "sessionId";
  } else if (std::strcmp(type, "debug.snapshot.get") != 0 && std::strcmp(type, "debug.automation.resume") != 0) {
    error = "unsupported diagnostics command"; return false;
  }
  const bool needsSession = std::strcmp(type, "debug.snapshot.get") != 0 &&
                            std::strcmp(type, "debug.automation.resume") != 0 &&
                            std::strcmp(type, "debug.output.stop") != 0;
  if (needsSession && (!root["sessionId"].is<const char *>() || !diagnosticsUuid(root["sessionId"].as<const char *>()))) {
    error = "valid sessionId required"; return false;
  }
  if (diagnosticsHasField(root, "sessionId") &&
      (!root["sessionId"].is<const char *>() || !diagnosticsUuid(root["sessionId"].as<const char *>()))) {
    error = "invalid sessionId"; return false;
  }
  const bool needsZone = std::strcmp(type, "debug.output.pulse") == 0 ||
                         (std::strcmp(type, "debug.input.set") == 0 && std::strcmp(root["input"].as<const char *>(), "soil") == 0);
  if ((needsZone || diagnosticsHasField(root, "zoneId")) &&
      (!root["zoneId"].is<uint8_t>() || root["zoneId"].as<uint8_t>() < 1 || root["zoneId"].as<uint8_t>() > 4)) {
    error = "zoneId must be an integer from 1 to 4"; return false;
  }
  // Match whole tokens to reject unknown fields, including misplaced parameters.
  for (JsonPairConst entry : root) {
    const char *name = entry.key().c_str();
    if (std::strcmp(name, "type") == 0 || std::strcmp(name, "requestId") == 0 ||
        std::strcmp(name, "schemaVersion") == 0 || std::strcmp(name, "deviceId") == 0 ||
        std::strcmp(name, "expectedBootId") == 0 || std::strcmp(name, "expiresAtEpoch") == 0) continue;
    bool allowed = false;
    for (const char *token = fields; *token != '\0';) {
      const char *end = std::strchr(token, ' ');
      const size_t length = end == nullptr ? std::strlen(token) : size_t(end - token);
      if (std::strlen(name) == length && std::strncmp(name, token, length) == 0) allowed = true;
      if (end == nullptr) break;
      token = end + 1;
    }
    if (!allowed) { error = "unexpected diagnostics parameter"; return false; }
  }
  error = nullptr;
  return true;
}

// Entries remain until the original fixed deadline. A burst cannot evict an
// unexpired pulse ID and replay it; full capacity fails closed.
class DiagnosticsRequestLedger {
 public:
  struct Entry { char id[65]{}; char command[32]{}; char error[96]{}; uint32_t deadline = 0; bool accepted = false; };
  const Entry *find(const char *id, uint32_t epoch) const {
    for (const auto &entry : entries_)
      if (entry.deadline > epoch && std::strcmp(entry.id, id) == 0) return &entry;
    return nullptr;
  }
  bool hasCapacity(uint32_t epoch) const {
    for (const auto &entry : entries_) if (entry.deadline <= epoch) return true;
    return false;
  }
  void remember(const char *id, const char *command, uint32_t deadline, uint32_t epoch, bool accepted, const char *error) {
    for (auto &entry : entries_) {
      if (entry.deadline > epoch) continue;
      entry = Entry{};
      copy(entry.id, id, sizeof(entry.id)); copy(entry.command, command, sizeof(entry.command));
      if (error != nullptr) copy(entry.error, error, sizeof(entry.error));
      entry.deadline = deadline; entry.accepted = accepted;
      return;
    }
  }
 private:
  Entry entries_[64]{};
  static void copy(char *target, const char *source, size_t size) {
    const size_t count = std::strlen(source) < size - 1 ? std::strlen(source) : size - 1;
    std::memcpy(target, source, count); target[count] = '\0';
  }
};

}  // namespace watering
