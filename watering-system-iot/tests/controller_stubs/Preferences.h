#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace controller_test {
inline std::map<std::string, std::vector<uint8_t>> storedPreferences;
inline bool preferencesOpenFails = false;
inline bool preferencesWriteFails = false;
}

class Preferences {
 public:
  bool begin(const char *, bool) { return !controller_test::preferencesOpenFails; }
  void end() {}
  bool isKey(const char *key) { return controller_test::storedPreferences.count(key) != 0; }
  size_t getBytesLength(const char *key) {
    const auto entry = controller_test::storedPreferences.find(key);
    return entry == controller_test::storedPreferences.end() ? 0 : entry->second.size();
  }
  size_t getBytes(const char *key, void *destination, size_t size) {
    const auto entry = controller_test::storedPreferences.find(key);
    if (entry == controller_test::storedPreferences.end()) return 0;
    const size_t count = std::min(size, entry->second.size());
    std::memcpy(destination, entry->second.data(), count);
    return count;
  }
  size_t putBytes(const char *key, const void *source, size_t size) {
    if (controller_test::preferencesWriteFails) return 0;
    const auto *bytes = static_cast<const uint8_t *>(source);
    controller_test::storedPreferences[key] = std::vector<uint8_t>(bytes, bytes + size);
    return size;
  }
  bool getBool(const char *key, bool fallback) {
    const auto entry = controller_test::storedPreferences.find(key);
    return entry == controller_test::storedPreferences.end() ? fallback : entry->second.front() != 0;
  }
  size_t putBool(const char *key, bool value) {
    const uint8_t byte = value;
    return putBytes(key, &byte, sizeof(byte));
  }
};
