// SomfyRtsTx.h — wersja finalna (10.09.2026)
// Nadajnik ramek Somfy RTS oparty 1:1 na implementacji z ESPSomfy-RTS
// (rstrouse/ESPSomfy-RTS, pliki Somfy.cpp/Somfy.h, wydanie v2.4.6).
// Zrodlo protokolu: https://pushstack.wordpress.com/somfy-rts-protocol/
#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <ELECHOUSE_CC1101_SRC_DRV.h>

// Dlugosc symbolu w mikrosekundach — stala z ESPSomfy-RTS (#define SYMBOL 640)
static constexpr uint16_t RTS_SYMBOL = 640;

enum class SomfyCommand : uint8_t {
  My   = 0x1,   // stop / pozycja ulubiona
  Up   = 0x2,
  MyUp = 0x3,
  Down = 0x4,
  MyDown = 0x5,
  UpDown = 0x6,
  Prog = 0x8,   // parowanie — uzywac ostroznie!
  SunFlag = 0x9,
  Flag = 0xA
};

class SomfyRtsTx {
  public:
    // gdoPin — GDO0 CC1101, wspolny dla TX (jak w ESPSomfy-RTS)
    SomfyRtsTx(uint8_t sckPin, uint8_t misoPin, uint8_t mosiPin,
               uint8_t csnPin, uint8_t gdoPin);

    // Inicjalizacja CC1101 z parametrami identycznymi jak w ESPSomfy-RTS.
    bool begin(float frequencyMHz = 433.42f, float deviationKHz = 47.60f,
               float rxBandwidthKHz = 99.97f, int8_t txPower = 10);

    // Wysyla komende do rolety o danym adresie (24-bit).
    // Rolling code przechowywany w NVS pod kluczem shadeKey.
    // Zwraca uzyty (nowy) rolling code.
    uint16_t sendCommand(const char *shadeKey, uint32_t remoteAddress,
                         SomfyCommand cmd, uint8_t repeats = 4);

    // Reczny odczyt/ustawienie rolling code (przy przenoszeniu adresu
    // z dzialajacego instalowania ESPSomfy-RTS lub pilota).
    uint16_t getRollingCode(const char *shadeKey);
    void setRollingCode(const char *shadeKey, uint16_t code);

  private:
    uint8_t _sckPin, _misoPin, _mosiPin, _csnPin, _gdoPin;
    Preferences _pref;

    uint16_t nextRollingCode(const char *shadeKey);
    void encodeFrame(byte *frame, uint32_t remoteAddress,
                     uint16_t rollingCode, SomfyCommand cmd);
    // sync: 2 dla pierwszej ramki 56-bit, 7 dla powtorzen
    void transmitFrame(byte *frame, uint8_t sync);
    void beginTransmit();
    void endTransmit();
};
