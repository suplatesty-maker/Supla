// SomfyRollerShutter.h — wersja 2 (12.09.2026)
// Adapter kanalu Supla RollerShutter -> nadajnik Somfy RTS (SomfyRtsTx).
//
// V2: adres RTS czytany LENIWIE (w momencie wysylki) z magazynu konfiguracji
// Supla (klucz "<shadeKey>_addr", ustawiany w panelu WWW przez
// Supla::Html::CustomParameter). Fallback: SHADES[i].remoteAddress z kodu.
// Dzieki temu zmiana/dodanie rolety NIE wymaga rekompilacji — wpisujesz
// adres w panelu WWW i restartujesz urzadzenie.
//
// Zweryfikowane w zrodlach supla-device (roller_shutter.h/.cpp,
// roller_shutter_interface.h, storage/config.h, custom_parameter.h):
//
//  1. Piny przekazujemy jako PRAWIDLOWE, nieuzywane GPIO (25/26, fizycznie
//     niepodlaczone). onTimer() bazowej klasy robi wczesny return dla pinow
//     nieustawionych — dla -1 logika ruchu by byla martwa. Metody relay*
//     nadpisujemy, wiec GPIO nigdy nie jest realnie sterowane (wszystkie
//     sloty moga wspoldzielic te same "fałszywe" piny).
//
//  2. Sledzimy _upActive/_downActive — klasa bazowa w startClosing() wola
//     relayUpOff() "dla pewnosci" przed relayDownOn(); bez tego kazda
//     komenda Down nadawalaby najpierw "My".
//
//  3. openingTimeMs/closingTimeMs sa protected w RollerShutterInterface —
//     przypisanie w konstruktorze OK. To wartosci startowe; po kalibracji
//     nadpisze je konfiguracja z Supla Cloud / panelu WWW
//     (RollerShutterParameters -> setOpenCloseTime).
#pragma once

#include <SuplaDevice.h>
#include <supla/control/roller_shutter.h>
#include <supla/storage/config.h>
#include <supla/storage/storage.h>
#include "SomfyRtsTx.h"
#include "ShadeConfig.h"

// Nieuzywane GPIO — fizycznie NIE podlaczaj do niczego. Wolne przy
// okablowaniu CC1101 (SCK 18, MISO 19, MOSI 23, CSN 5, GDO0 13).
#ifndef PIN_RS_FAKE_UP
#define PIN_RS_FAKE_UP   25
#endif
#ifndef PIN_RS_FAKE_DOWN
#define PIN_RS_FAKE_DOWN 26
#endif

class SomfyRollerShutter : public Supla::Control::RollerShutter {
  public:
    SomfyRollerShutter(SomfyRtsTx *tx, const ShadeConfig &cfg)
      : Supla::Control::RollerShutter(PIN_RS_FAKE_UP, PIN_RS_FAKE_DOWN, true, false),
        _tx(tx), _cfg(cfg), _upActive(false), _downActive(false) {
      snprintf(_addrTag, sizeof(_addrTag), "%s_addr", cfg.shadeKey);
      // Pola protected z RollerShutterInterface — wartosci startowe.
      openingTimeMs = _cfg.upTimeMs;
      closingTimeMs = _cfg.downTimeMs;
    }

    // Aktualny adres RTS: panel WWW ma priorytet, potem fallback z kodu.
    uint32_t getRemoteAddress() const {
      auto cfg = Supla::Storage::ConfigInstance();
      int32_t val = 0;
      if (cfg && cfg->getInt32(_addrTag, &val) && val > 0) {
        return static_cast<uint32_t>(val);
      }
      return _cfg.remoteAddress;
    }

    // --- Metody przekaznikowe: JEDYNE miejsce wysylania komend RTS ---

    void relayUpOn() override {
      if (!_upActive) {
        _upActive = true;
        sendRts(SomfyCommand::Up);
      }
    }

    void relayDownOn() override {
      if (!_downActive) {
        _downActive = true;
        sendRts(SomfyCommand::Down);
      }
    }

    void relayUpOff() override {
      if (_upActive) {
        _upActive = false;
        sendRts(SomfyCommand::My);
      }
    }

    void relayDownOff() override {
      if (_downActive) {
        _downActive = false;
        sendRts(SomfyCommand::My);
      }
    }

  private:
    SomfyRtsTx *_tx;
    ShadeConfig _cfg;
    bool _upActive;
    bool _downActive;
    char _addrTag[16];  // "<shadeKey>_addr", np. "shade3_addr"

    void sendRts(SomfyCommand cmd) {
      uint32_t address = getRemoteAddress();
      if (address == 0) {
        Serial.printf("[%s] Slot nieaktywny (adres 0) — komenda zignorowana\n",
                      _cfg.shadeKey);
        return;
      }
      uint16_t code = _tx->sendCommand(_cfg.shadeKey, address, cmd, _cfg.repeats);
      Serial.printf("[%s] RTS addr=%06lX cmd=%d rcode=%u\n",
                    _cfg.shadeKey, (unsigned long)address, (int)cmd, code);
    }
};
