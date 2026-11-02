#pragma once

// Minimalna podmiana klas GPIO z ESPHome. Sterowniki radia używają tylko
// setup(), digital_read(), digital_write() i attach_interrupt().

#include <Arduino.h>

#include <cstdint>

namespace esphome {
namespace gpio {

enum InterruptType : uint8_t {
  INTERRUPT_RISING_EDGE = 1,
  INTERRUPT_FALLING_EDGE = 2,
  INTERRUPT_ANY_EDGE = 3,
};

}  // namespace gpio

class InternalGPIOPin {
 public:
  /// @param pin numer GPIO.
  /// @param mode tryb Arduino (INPUT, OUTPUT, INPUT_PULLUP...).
  InternalGPIOPin(uint8_t pin, uint8_t mode) : pin_(pin), mode_(mode) {}

  void setup() { pinMode(this->pin_, this->mode_); }
  bool digital_read() { return digitalRead(this->pin_) == HIGH; }
  void digital_write(bool value) { digitalWrite(this->pin_, value ? HIGH : LOW); }
  uint8_t get_pin() const { return this->pin_; }

  template<typename T> void attach_interrupt(void (*func)(T *), T *arg, gpio::InterruptType type) const {
    int mode = RISING;
    if (type == gpio::INTERRUPT_FALLING_EDGE) {
      mode = FALLING;
    } else if (type == gpio::INTERRUPT_ANY_EDGE) {
      mode = CHANGE;
    }
    attachInterruptArg(this->pin_, reinterpret_cast<void (*)(void *)>(func), arg, mode);
  }

 private:
  uint8_t pin_;
  uint8_t mode_;
};

}  // namespace esphome
