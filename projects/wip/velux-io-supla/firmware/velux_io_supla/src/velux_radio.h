#pragma once

// ============================================================================
// velux_radio.h - warstwa radia io-homecontrol 1W (SX1276 / RFM95W 868 MHz)
// ============================================================================
// Czysty interfejs C++: ten nagłówek NIE dołącza niczego z home_io_control
// ani z SuplaDevice. Dzięki temu mogą go bezpiecznie włączać pliki po stronie
// SUPLA (SuplaDevice definiuje makro UNKNOWN_POSITION == -1, a hioc ma własną
// stałą o tej samej nazwie - połączenie obu w jednej jednostce kompilacji
// psuje hioc).
//
// Cała wiedza o protokole i radiu siedzi w velux_radio.cpp (jedyne miejsce,
// które widzi hioc, SPI i NVS).
//
// Model działania (1W = jeden kierunek, brak potwierdzeń):
//   - każdy slot ma własną tożsamość (adres 3 B + klucz 16 B) zapisaną w NVS,
//   - komenda leci jako seria 4 kopii (burst), bez żadnej odpowiedzi,
//   - odbiornik nasłuchuje (z przeskakiwaniem kanałów) i loguje ramki,
//     m.in. ramki 0x2E pilota KLI (z nich odczytuje się klasy nauki 0x30).
//
// Model działania (2W = io-homecontrol dwukierunkowy):
//   - najpierw ekstrakcja klucza instalacji z istniejącego pilota/huba:
//     nasze radio udaje nieznane urządzenie, pilot (KLR 200) paruje się z nim
//     i przekazuje swój node_id + system_key (rola "urządzenia"),
//   - potem "roll-call" (0x2A -> 0x2B) daje listę urządzeń instalacji
//     (adres, typ, klasa zasilania); przypisujemy je do slotów,
//   - komendy idą jako uwierzytelniona wymiana 2W (challenge 0x3C / 0x3D)
//     i wracają realnym statusem (0x04) - pozycja 0 = otwarte, 100 = zamknięte.
// ============================================================================

#include <stddef.h>
#include <stdint.h>

#include <functional>

namespace velux {

// Maksymalna liczba obsługiwanych produktów (kanałów Cover). Tablica
// VELUX_SLOTS w szkicu nie może być dłuższa niż ta wartość.
constexpr uint8_t MAX_SLOTS = 12;

// Rodzaj produktu - decyduje o klasie urządzenia, do której adresowane są
// komendy 1W (CMD_EXECUTE 0x00) i jaką funkcję kanał dostaje w SUPLA.
enum class ProductKind : uint8_t {
  ROLLER_SHUTTER = 0,  // roleta - klasa roller_shutter (0x02)
  WINDOW_OPENER = 1,   // okno dachowe - klasa window_opener (0x04)
  AWNING = 2,          // markiza - klasa awning (0x03)
};

// Profil klas docelowych ramki 0x30 "add controller" (gest nauki KLI).
// Prawdziwy pilot KLI wysyła 0x30 do każdej klasy, którą steruje. Klasy
// właściwe dla konkretnego pilota widać w logu jako linie 0x2E wysyłane
// przez pilot po przytrzymaniu Gear.
enum class EnrollProfile : uint8_t {
  AUTO = 0,      // wg rodzaju produktu (okno -> okno+roleta+dual, reszta -> zewnętrzne)
  EXTERIOR = 1,  // roller_shutter, awning, dual_shutter (KLI 310/313)
  WINDOW = 2,    // window_opener, roller_shutter, dual_shutter
  INTERIOR = 3,  // blind, venetian_blind (KLI 312)
};

// Piny radia SX1276 (RFM95W 868 MHz).
struct Pins {
  uint8_t sck;   // SPI SCK
  uint8_t miso;  // SPI MISO
  uint8_t mosi;  // SPI MOSI
  uint8_t nss;   // CS/NSS (sterowany ręcznie)
  uint8_t rst;   // RESET
  uint8_t dio0;  // DIO0 (PayloadReady - przerwanie)
};

// Konfiguracja jednego produktu. Ustawiana na stałe w szkicu (kanały SUPLA
// powstają raz, przy starcie).
struct SlotConfig {
  const char *name;          // nazwa produktu (panel WWW; SUPLA ma własną nazwę kanału)
  ProductKind kind;          // rodzaj produktu
  uint32_t opening_time_ms;  // orientacyjny czas pełnego otwarcia (animacja pozycji)
  uint32_t closing_time_ms;  // orientacyjny czas pełnego zamknięcia (animacja pozycji)
  bool default_low_power;    // odbiornik bateryjny/solarny (domyślna wartość z panelu)
  EnrollProfile default_profile;
};

class VeluxRadio {
 public:
  // Komendy wywoływane z serwera WWW (inny task niż pętla główna).
  // Trafiają do kolejki i są wykonywane w processPending() w loop().
  enum class Pending : uint8_t {
    NONE = 0,
    ENROLL,
    UNENROLL,
    REGENERATE,
    SCAN,        // roll-call 2W: wypisz urządzenia instalacji
    ARM_KEY,     // uzbrój responder ekstrakcji klucza (10 minut)
    DISARM_KEY,  // wyłącz responder
    CLEAR_KEY,   // skasuj zapisany klucz instalacji
    ASSIGN,      // przypisz urządzenie ze skanu do slotu
    UNASSIGN,    // usuń przypisanie urządzenia ze slotu
    TWOWAY,      // komenda 2W do urządzenia przypisanego do slotu
  };

  // Komendy 2W do pojedynczego produktu.
  enum class TwoWayAction : uint8_t {
    NONE = 0,
    OPEN,      // pozycja 0 (otwarte)
    CLOSE,     // pozycja 100 (zamknięte)
    STOP,
    POSITION,  // pozycja z position_0_100
    VENT,      // pozycja wentylacyjna (okna dachowe)
    POLL,      // tylko odczyt statusu (0x03)
  };

  // Klucz instalacji odzyskany z pilota/huba (rola urządzenia w parowaniu).
  struct HubCredentials {
    bool valid = false;
    uint8_t node_id[3]{};
    uint8_t system_key[16]{};
  };

  // Jedno urządzenie znalezione w skanie roll-call.
  struct ScannedDevice {
    uint8_t node_id[3]{};
    uint8_t type = 0;  // hio DeviceType (2 = roller_shutter, 4 = window_opener, ...)
    uint8_t subtype = 0;
    bool inverted = false;
    bool low_power = false;  // odbiornik bateryjny/solarny: długa preambuła
    bool metadata_complete = false;
    int16_t rssi_dbm = 0;
  };

  // Ostatni znany status produktu (z ramki 0x04 albo 0x71).
  struct TwoWayStatus {
    bool known = false;
    uint8_t position = 0;  // 0 = otwarte, 100 = zamknięte
    bool stopped = true;
    uint32_t age_ms = 0;  // ile ms temu odebrano status (0 = wiek nieznany)
  };

  static constexpr uint8_t MAX_SCAN_DEVICES = 16;

  // Nazwa typu urządzenia (np. "window_opener") - bez ciągnięcia hioc do UI.
  static const char *deviceTypeName(uint8_t type);

  VeluxRadio() = default;
  ~VeluxRadio();

  VeluxRadio(const VeluxRadio &) = delete;
  VeluxRadio &operator=(const VeluxRadio &) = delete;

  // Inicjalizacja: SPI + SX1276 + wczytanie tożsamości slotów. Wywołać raz,
  // w setup(). slots[] musi żyć przez cały czas działania programu.
  // Zwraca false, gdy radio nie odpowiada (zły SPI/piny/zasilanie).
  bool begin(const Pins &pins, uint8_t tx_power_dbm, const SlotConfig *slots, uint8_t slot_count);

  // Nasłuch (nieblokujący) - wywoływać w każdym obiegu loop().
  void poll();

  // Wykonanie komend zakolejkowanych z panelu WWW - wywoływać w loop().
  // Uwaga: wysyłka blokuje pętlę na ~0,2-1,2 s (burst 4 kopii).
  void processPending();

  bool ready() const;
  const char *chipName() const;

  uint8_t slotCount() const;
  const SlotConfig *slot(uint8_t index) const;

  // Ustawienia slotu (zapisywane w NVS).
  bool lowPower(uint8_t index) const;
  void setLowPower(uint8_t index, bool value);
  EnrollProfile enrollProfile(uint8_t index) const;
  void setEnrollProfile(uint8_t index, EnrollProfile profile);

  // Tożsamość: 6-cyfrowy hex adresu albo "brak".
  void identityText(uint8_t index, char *out, size_t out_size) const;
  // Nowy losowy adres i klucz - po tym trzeba enrolować od nowa.
  void regenerateIdentity(uint8_t index);

  // === Wysyłka (blokująca; tylko z loop()) ===
  // Pozycja 0 = otwarte, 100 = zamknięte (jak w SUPLA).
  bool sendPosition(uint8_t index, uint8_t position_0_100);
  bool sendStop(uint8_t index);
  // Nauka wg gestu KLI (0x39 + sweep 0x30 + STOP/DOWN).
  bool enroll(uint8_t index);
  // Wypisanie kontrolera (0x39) - niepotwierdzone na sprzęcie.
  bool unenroll(uint8_t index);

  // Kolejka dla wywołań z serwera WWW.
  void queuePending(uint8_t index, Pending command);

  // Ostatnie zdarzenie (TX/RX) - pokazywane w panelu WWW.
  const char *lastEvent() const;

  // Adres ramki ostatnio usłyszanej z pilota (np. "084374") albo "brak".
  void lastRemoteText(char *out, size_t out_size) const;

  // ==========================================================================
  // 2W: io-homecontrol dwukierunkowy (klucz instalacji + skan + komendy)
  // ==========================================================================

  // Podgląd stanu 2W.
  HubCredentials hubCredentials() const;
  bool twoWayReady() const;  // radio + klucz instalacji
  void clearHubCredentials();

  // Ekstrakcja klucza z istniejącego pilota (KLR 200): uzbrojenie respondera,
  // który odpowie na kreatora parowania i odbierze node_id + system_key.
  bool keyExtractionArmed() const;
  const char *keyExtractionStage() const;     // nazwa stanu (do panelu WWW)
  uint32_t keyExtractionSecondsLeft() const;  // 0 = nieuzbrojone/wygasło
  void setKeyExtractionArmed(bool armed);

  // Skan instalacji (roll-call): 2 przebiegi x 3 kanały, ~kilka sekund.
  void queueScan();
  bool scanBusy() const;
  uint32_t scanAgeMs() const;  // ile ms temu skończył się ostatni skan (0 = nigdy)
  uint8_t scanCount() const;
  const ScannedDevice *scanDevice(uint8_t index) const;

  // Przypisanie znalezionego urządzenia do slotu (zapisywane w NVS).
  void queueAssign(uint8_t slot, uint8_t scan_index);
  void queueUnassign(uint8_t slot);
  bool slotHasDevice(uint8_t slot) const;
  bool slotTwoWayReady(uint8_t slot) const;
  void slotDeviceText(uint8_t slot, char *out, size_t out_size) const;
  bool slotLowPower(uint8_t slot) const;

  // Komenda 2W (blokująca, wykonywana w processPending()).
  void queueTwoWay(uint8_t slot, TwoWayAction action, uint8_t position_0_100 = 0);
  TwoWayStatus slotStatus(uint8_t slot) const;
  const char *lastTwoWayEvent() const;

  // Ostatnia ramka 2W usłyszana z powietrza (test zasięgu/odbioru) i jej wiek.
  const char *lastHeardTwoWay() const;
  uint32_t lastHeardTwoWayAgeMs() const;

  // Powiadomienie po odebraniu statusu (wywoływane w loop()).
  using StatusListener = std::function<void(uint8_t slot, const TwoWayStatus &status)>;
  void setStatusListener(StatusListener listener);
  // Powiadomienie po odebraniu klucza instalacji (wywoływane w loop()).
  using KeyExtractedListener = std::function<void(const HubCredentials &credentials)>;
  void setKeyExtractedListener(KeyExtractedListener listener);

 private:
  struct Impl;
  Impl *impl_ = nullptr;
};

}  // namespace velux
