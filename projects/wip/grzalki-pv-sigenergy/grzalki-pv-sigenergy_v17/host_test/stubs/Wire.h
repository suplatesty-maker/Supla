#pragma once
#include <cstdint>
#include <cstddef>
class TwoWire {
 public:
  bool begin(int, int) { return true; }
  void setClock(uint32_t) {}
  void setTimeOut(uint32_t) {}
  void beginTransmission(uint8_t) {}
  size_t write(uint8_t) { return 1; }
  size_t write(const uint8_t*, size_t n) { return n; }
  uint8_t endTransmission() { return 0; }
};
extern TwoWire Wire;
