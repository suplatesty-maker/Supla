#pragma once

// ============================================================================
// velux_cover.h - kanał SUPLA (roleta / okno dachowe) sterowany przez 1W
// ============================================================================
// Jedna instancja = jeden produkt VELUX = jeden slot radia (velux_radio.h).
//
// Protokół 1W nie ma ŻADNEJ odpowiedzi (brak potwierdzenia i statusu), więc
// pozycja widziana w SUPLA jest tylko szacowana:
//   - po wysłanej komendzie pozycja "jedzie" przez czas otwierania/zamykania,
//   - STOP zamraża ją w miejscu, w którym (w przybliżeniu) jest produkt,
//   - po restarcie wracamy do ostatniej zapisanej pozycji SUPLA.
// Po użyciu fizycznego pilota KLI szacunek się rozjeżdża - koryguje go
// najbliższa komenda Otwórz/Zamknij.
// ============================================================================

#include <supla/control/roller_shutter_interface.h>

#include "velux_radio.h"

class VeluxCover : public Supla::Control::RollerShutterInterface {
 public:
  VeluxCover(velux::VeluxRadio *radio, uint8_t slot, const velux::SlotConfig &config);

  void onLoadState() override;
  void iterateAlways() override;

  // Wartości z serwera/aplikacji SUPLA. Nadpisane, żeby ominąć mechanizm
  // kalibracji RollerShutterInterface (ustawianie czasów z pola DurationMS
  // kasowałoby naszą szacowaną pozycję przy każdej komendzie).
  int32_t handleNewValueFromServer(TSD_SuplaChannelNewValue *newValue) override;

  // Konfiguracja kanału z serwera (czasy otwierania/zamykania, typ funkcji).
  Supla::ApplyConfigResult applyChannelConfig(TSD_ChannelConfig *result, bool local = false) override;

  // STOP użytkownika (aplikacja/panel): wysyła komendę i zatrzymuje animację.
  void stop() override;

  // Realny status z 2W (pozycja + ruch) - wołane z listenera radia w loop().
  void applyTwoWayStatus(const velux::VeluxRadio::TwoWayStatus &status);

 private:
  void applyTarget_();
  void sendStop_();
  void startAnimation_(uint8_t position);
  void updateAnimation_();

  velux::VeluxRadio *radio_ = nullptr;
  uint8_t slot_ = 0;
  const char *product_name_ = nullptr;

  bool pending_stop_ = false;   // STOP do wysłania w najbliższym iterateAlways()
  bool moving_ = false;         // trwa animacja pozycji
  uint32_t move_started_ms_ = 0;
  uint32_t move_duration_ms_ = 0;
  int16_t move_from_ = 0;       // jednostki 0,01 (tak jak currentPosition)
  int16_t move_to_ = 0;
};
