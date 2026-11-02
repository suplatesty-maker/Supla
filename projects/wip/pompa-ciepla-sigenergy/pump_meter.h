#pragma once
// PZEM-004T v3.0 wrapper that exposes a validity flag, the last power and the
// last energy counter. It feeds a standard SUPLA one-phase electricity meter.

#include <Arduino.h>
#include <math.h>
#include <PZEM004Tv30.h>
#include <supla/sensor/one_phase_electricity_meter.h>

class PumpMeter : public Supla::Sensor::OnePhaseElectricityMeter {
 public:
  PumpMeter(HardwareSerial &serial, uint8_t rxPin, uint8_t txPin, uint8_t addr)
      : pzem(serial, rxPin, txPin, addr) {}

  void onInit() override {
    readValuesFromDevice();
    updateChannelValues();
  }

  void readValuesFromDevice() override {
    ++sequence_;
    const float current = pzem.current();
    if (isnan(current)) {
      valid_ = false;
      resetReadParameters();
      return;
    }

    const float voltage = pzem.voltage();
    const float active = pzem.power();
    const float energy = pzem.energy();
    const float frequency = pzem.frequency();
    const float pf = pzem.pf();
    if (isnan(voltage) || isnan(active) || isnan(energy) ||
        isnan(frequency) || isnan(pf)) {
      valid_ = false;
      resetReadParameters();
      return;
    }
    // A PZEM with no AC voltage still answers on the TTL side. Treat a
    // missing voltage/frequency as invalid so the split does not silently
    // count everything as zero.
    if (voltage < 80.0f || frequency < 40.0f || frequency > 70.0f) {
      valid_ = false;
      resetReadParameters();
      return;
    }

    const float apparent = voltage * current;
    const float reactive =
        apparent > active ? sqrtf(apparent * apparent - active * active) : 0.0f;

    setVoltage(0, voltage * 100);
    setCurrent(0, current * 1000);
    setPowerActive(0, active * 100000);
    setFwdActEnergy(0, energy * 100000);
    setFreq(frequency * 100);
    setPowerFactor(0, pf * 1000);
    setPowerApparent(0, apparent * 100000);
    setPowerReactive(0, reactive * 100000);

    valid_ = true;
    lastPowerW_ = active;
    lastEnergyKwh_ = energy;
    lastValidMs_ = millis();
  }

  bool valid() const { return valid_; }
  float lastPowerW() const { return lastPowerW_; }
  float lastEnergyKwh() const { return lastEnergyKwh_; }
  uint32_t lastValidMs() const { return lastValidMs_; }
  uint32_t sequence() const { return sequence_; }

  // SUPLA "reset counters" on this channel resets the PZEM hardware counter.
  // The split counters detect the negative delta and start a new baseline.
  void resetStorage() override { pzem.resetEnergy(); }

 private:
  PZEM004Tv30 pzem;
  bool valid_ = false;
  float lastPowerW_ = NAN;
  float lastEnergyKwh_ = NAN;
  uint32_t lastValidMs_ = 0;
  uint32_t sequence_ = 0;
};
