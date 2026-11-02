#pragma once
// v2: hand off the command inside the relay callback, not after iterate().
#include <supla/control/virtual_relay.h>
#include "mailbox.h"

class ServiceRelay : public Supla::Control::VirtualRelay {
 public:
  ServiceRelay() : VirtualRelay(SUPLA_BIT_FUNC_POWERSWITCH) {}

 protected:
  void setNewChannelValue(bool value) override {
    // VirtualRelay updates its state before calling this hook, including when
    // restoring EEPROM state and when a server countdown expires.
    Engine::mailbox.setService(isOn());
    Supla::Control::VirtualRelay::setNewChannelValue(value);
  }
};
