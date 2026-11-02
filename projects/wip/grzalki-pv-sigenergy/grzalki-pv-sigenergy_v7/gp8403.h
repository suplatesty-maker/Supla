#pragma once
// v7: DFRobot DFR0971 / GP8403 protocol, NOT the 15-bit GP8413.
// Verified against DFRobot_GP8XXX 1.1.0: setDACOutRange, setDACOutVoltage,
// sendData and DFRobot_GP8403's default resolution.
#include <stdint.h>

namespace Gp8403 {
constexpr uint8_t ADDRESS = 0x58;       // Match the module's address switches.
constexpr uint8_t RANGE_REG = 0x01;
constexpr uint8_t RANGE_10V = 0x11;
constexpr uint8_t CHANNEL0_REG = 0x02;  // Four zero bytes here also clear OUT1.
constexpr uint16_t MAX_CODE = 0x0FFF;   // 12 bits: 0..4095.
constexpr uint8_t DATA_SHIFT = 4;

constexpr uint16_t encodeCode(uint16_t code) {
  return uint16_t((code > MAX_CODE ? MAX_CODE : code) << DATA_SHIFT);
}
constexpr uint16_t wordForFraction(float fraction) {
  // Zero, negative and NaN requests map to zero; saturate before conversion.
  // Truncate like the earlier controller; do not round a request upwards.
  return encodeCode(!(fraction > 0) ? 0 :
                    (fraction >= 1 ? MAX_CODE : uint16_t(fraction * MAX_CODE)));
}
// Names must not collide with Arduino.h macros lowByte()/highByte().
constexpr uint8_t low8(uint16_t word) { return uint8_t(word); }
constexpr uint8_t high8(uint16_t word) { return uint8_t(word >> 8); }
}  // namespace Gp8403

static_assert(Gp8403::wordForFraction(0) == 0x0000, "OFF");
static_assert(Gp8403::wordForFraction(-1) == 0x0000, "Clamp negative");
static_assert(Gp8403::wordForFraction(0.25f) == 0x3FF0, "Quarter scale");
static_assert(Gp8403::wordForFraction(0.5f) == 0x7FF0, "Half scale");
static_assert(Gp8403::wordForFraction(0.75f) == 0xBFF0, "Three-quarter scale");
static_assert(Gp8403::wordForFraction(1) == 0xFFF0, "Full scale is 12 bits");
static_assert(Gp8403::wordForFraction(2) == 0xFFF0, "Clamp overload");
static_assert(Gp8403::encodeCode(65535) == 0xFFF0, "Clamp raw input");
static_assert(Gp8403::low8(Gp8403::wordForFraction(1)) == 0xF0,
              "First I2C data byte");
static_assert(Gp8403::high8(Gp8403::wordForFraction(1)) == 0xFF,
              "Second I2C data byte");
