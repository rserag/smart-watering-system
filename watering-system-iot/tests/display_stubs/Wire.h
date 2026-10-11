#pragma once

#include <Arduino.h>

class TwoWire {
 public:
  void begin(uint8_t, uint8_t) {}
  void setClock(uint32_t) {}
  void setTimeOut(uint16_t) {}
  void beginTransmission(uint8_t) {}
  uint8_t endTransmission() { return 0; }
};
inline TwoWire Wire;
