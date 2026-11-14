#pragma once
#include <cstdint>
typedef uint8_t DeviceAddress[8];
#define DEVICE_DISCONNECTED_C (-127.0f)
class OneWire;
class DallasTemperature {
 public:
  explicit DallasTemperature(OneWire*) {}
  void begin() {}
  int getDeviceCount() { return 1; }
  bool getAddress(uint8_t* a, int) {
    for (int i = 0; i < 8; ++i) a[i] = uint8_t(i + 1);
    return true;
  }
  bool isParasitePowerMode() { return false; }
  void setResolution(const uint8_t*, int) {}
  void setWaitForConversion(bool) {}
  void requestTemperatures() {}
  float getTempC(const uint8_t*);
};
