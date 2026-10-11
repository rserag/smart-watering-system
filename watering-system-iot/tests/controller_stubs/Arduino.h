#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

constexpr int LOW = 0;
constexpr int HIGH = 1;
constexpr int INPUT = 0;
constexpr int OUTPUT = 1;
constexpr int ADC_11db = 3;
#define F(value) value
using portMUX_TYPE = int;
constexpr int portMUX_INITIALIZER_UNLOCKED = 0;
inline void portENTER_CRITICAL(portMUX_TYPE *) {}
inline void portEXIT_CRITICAL(portMUX_TYPE *) {}

namespace controller_test {
inline uint32_t now = 0;
inline int digital[64]{};
inline int analog[64]{};
inline unsigned relayHighWrites = 0;
}

inline uint32_t millis() { return controller_test::now; }
inline uint32_t micros() { return controller_test::now * 1000; }
inline void pinMode(uint8_t, int) {}
inline void analogSetPinAttenuation(uint8_t, int) {}
inline void delayMicroseconds(unsigned) {}
inline void delay(uint32_t duration) { controller_test::now += duration; }
inline int digitalRead(uint8_t pin) { return controller_test::digital[pin]; }
inline int analogRead(uint8_t pin) { return controller_test::analog[pin]; }
inline void digitalWrite(uint8_t pin, int value) {
  controller_test::digital[pin] = value;
  if (value == HIGH) ++controller_test::relayHighWrites;
}
inline int constrain(int value, int minimum, int maximum) {
  return value < minimum ? minimum : value > maximum ? maximum : value;
}
inline size_t strlcpy(char *destination, const char *source, size_t size) {
  const size_t length = std::strlen(source);
  if (size > 0) {
    const size_t copied = length < size - 1 ? length : size - 1;
    std::memcpy(destination, source, copied);
    destination[copied] = '\0';
  }
  return length;
}
struct TestSerial {
  void begin(unsigned) {}
  template <typename... Args> void printf(const char *, Args...) {}
  void println(const char * = "") {}
};
inline TestSerial Serial;
