#pragma once
// Minimal contract of SuplaDevice 26.8 VirtualRelay/Relay. This is not a SUPLA
// protocol implementation: hardware/Cloud and EEPROM tests are still required.
#include <functional>
constexpr int SUPLA_BIT_FUNC_POWERSWITCH = 1;
namespace Supla {
namespace Control {
class VirtualRelay {
 public:
  explicit VirtualRelay(int) {}
  virtual ~VirtualRelay() = default;
  void turnOn() { state = true; setNewChannelValue(true); }
  void turnOff() { state = false; setNewChannelValue(false); }
  bool isOn() { return state; }
  std::function<void(bool)> channelCallback;

 protected:
  virtual void setNewChannelValue(bool) {
    if (channelCallback) channelCallback(isOn());
  }

 private:
  bool state = false;
};
}  // namespace Control
}  // namespace Supla
