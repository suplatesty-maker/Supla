/*
  ============================================================================
  VELUX io-homecontrol (1W) -> SUPLA
  ============================================================================
  Sprzęt: ESP32 DevKit V1 (30 pin) + moduł SX1276/RFM95W 868 MHz (3,3 V).

  Podłączenie (SPI VSPI):
    SX1276  ->  ESP32 DevKit V1        uwagi
    VCC/3V3 ->  3V3                    moduł tylko 3,3 V
    GND     ->  GND
    SCK     ->  GPIO18
    MISO    ->  GPIO19
    MOSI    ->  GPIO23
    NSS/CS  ->  GPIO5                  sterowany ręcznie (SPI CS = -1)
    RESET   ->  GPIO14
    DIO0    ->  GPIO26                 przerwanie PayloadReady
    (DIO1..DIO5, ANT - nie podłączać; antena 868 MHz obowiązkowo)

  LED stanu: GPIO2 (wbudowana dioda; gdy świeci odwrotnie - patrz VELUX_LED_INVERT).
  Przycisk konfiguracji: GPIO0 (BOOT) - przytrzymanie ~5 s uruchamia tryb
  konfiguracji (AP 192.168.4.1), krótkie kliknięcie w trybie konfiguracji
  wychodzi z niego i restartuje urządzenie.

  Zakres: sterowanie roletami i oknami dachowymi VELUX przez io-homecontrol.
  1W (jednokierunkowo, bez potwierdzeń) działa od razu po nauce gestem KLI.
  2W (dwukierunkowo, z realnym statusem) wymaga najpierw ekstrakcji klucza
  instalacji z istniejącego pilota/huba (KLR 200) - patrz README.md.

  Moc nadawania 14 dBm = ~25 mW: maksimum dozwolone w paśmie 868,95 MHz
  (CH2), na którym nadaje 1W (ETSI EN 300 220). Nie zmieniać bez wiedzy.

  Środowisko: esp32:esp32 3.3.3, SuplaDevice 26.8. Kompilacja:
    arduino-cli compile --jobs 4 \
      --fqbn "esp32:esp32:esp32:PartitionScheme=min_spiffs" .

  UWAGA: home_io_control (katalog src/hioc) jest bez zmian, na licencji MIT
  (Lars Berning) - patrz src/hioc/LICENSE-home_io_control.txt.
  ============================================================================
*/

// ---------------------------------------------------------------------------
// KONFIGURACJA KOMPILACYJNA
// ---------------------------------------------------------------------------

// Piny radia SX1276.
#define VELUX_PIN_SCK 18
#define VELUX_PIN_MISO 19
#define VELUX_PIN_MOSI 23
#define VELUX_PIN_NSS 5
#define VELUX_PIN_RST 14
#define VELUX_PIN_DIO0 26

// LED stanu i przycisk konfiguracji (ESP32 DevKit V1).
#define VELUX_PIN_LED 2
#define VELUX_LED_INVERT 0  // 1, gdy dioda na płytce świeci odwrotnie
#define VELUX_PIN_BTN_CFG 0

// Moc nadawania w dBm (14 = ~25 mW, limit ETSI dla 868,95 MHz).
#define VELUX_TX_POWER 14

// ---------------------------------------------------------------------------
// PRODUKTY (KANAŁY SUPLA)
// ---------------------------------------------------------------------------
// Jeden wpis = jeden kanał SUPLA = jeden produkt (własny adres 1W w NVS
// i własne przypisanie urządzenia 2W).
// Maksymalnie 12 wpisów (velux::MAX_SLOTS).
//
//   {"nazwa", rodzaj, czas otwarcia [ms], czas zamknięcia [ms],
//    odbiornik bateryjny/solarny (true/false), profil nauki}
//
// Czasy przejazdu sterują tylko animacją pozycji w aplikacji - w 2W realną
// pozycję podaje samo urządzenie, a w 1W jest szacowana. Można je też zmienić
// z aplikacji SUPLA (konfiguracja kanału).
//
// Profil nauki (klasy ramki 0x30): AUTO wystarcza na start; właściwe klasy
// widać w logu - po przytrzymaniu Gear na pilocie KLI lecą linie 0x2E.
#include "src/velux_radio.h"

static const velux::SlotConfig VELUX_SLOTS[] = {
    // 4 rolety
    {"Roleta 1", velux::ProductKind::ROLLER_SHUTTER, 30000, 30000, false, velux::EnrollProfile::AUTO},
    {"Roleta 2", velux::ProductKind::ROLLER_SHUTTER, 30000, 30000, false, velux::EnrollProfile::AUTO},
    {"Roleta 3", velux::ProductKind::ROLLER_SHUTTER, 30000, 30000, false, velux::EnrollProfile::AUTO},
    {"Roleta 4", velux::ProductKind::ROLLER_SHUTTER, 30000, 30000, false, velux::EnrollProfile::AUTO},
    // 5 okien dachowych
    {"Okno dachowe 1", velux::ProductKind::WINDOW_OPENER, 30000, 30000, false, velux::EnrollProfile::AUTO},
    {"Okno dachowe 2", velux::ProductKind::WINDOW_OPENER, 30000, 30000, false, velux::EnrollProfile::AUTO},
    {"Okno dachowe 3", velux::ProductKind::WINDOW_OPENER, 30000, 30000, false, velux::EnrollProfile::AUTO},
    {"Okno dachowe 4", velux::ProductKind::WINDOW_OPENER, 30000, 30000, false, velux::EnrollProfile::AUTO},
    {"Okno dachowe 5", velux::ProductKind::WINDOW_OPENER, 30000, 30000, false, velux::EnrollProfile::AUTO},
};

// ---------------------------------------------------------------------------
// KOD
// ---------------------------------------------------------------------------

#include <SuplaDevice.h>
#include <supla/control/button.h>
#include <supla/device/status_led.h>
#include <supla/network/esp_web_server.h>
#include <supla/network/esp_wifi.h>
#include <supla/network/html/device_info.h>
#include <supla/network/html/protocol_parameters.h>
#include <supla/network/html/status_led_parameters.h>
#include <supla/network/html/wifi_parameters.h>
#include <supla/storage/eeprom.h>
#include <supla/storage/littlefs_config.h>

#include "src/velux_cover.h"
#include "src/velux_web.h"

Supla::Eeprom eeprom;
Supla::LittleFsConfig configSupla;

Supla::ESPWifi wifi;
Supla::EspWebServer suplaServer;
Supla::Device::StatusLed statusLed(VELUX_PIN_LED, VELUX_LED_INVERT == 1);

// Radio io-homecontrol (1W + 2W) - wspólne dla wszystkich kanałów.
velux::VeluxRadio veluxRadio;
// Kanały SUPLA - potrzebne, żeby status 2W trafił do właściwego kanału.
static VeluxCover *veluxCovers[velux::MAX_SLOTS] = {};

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println();
  Serial.println("==============================================");
  Serial.println(" VELUX io-homecontrol (1W + 2W) -> SUPLA");
  Serial.println(" ESP32 DevKit V1 (30 pin) + SX1276 868 MHz");
  Serial.println("==============================================");

  new Supla::Html::DeviceInfo(&SuplaDevice);
  new Supla::Html::WifiParameters;
  new Supla::Html::ProtocolParameters;
  new Supla::Html::StatusLedParameters;

  // Radio: SPI + SX1276 + tożsamości 1W z NVS.
  const velux::Pins pins = {VELUX_PIN_SCK, VELUX_PIN_MISO, VELUX_PIN_MOSI,
                            VELUX_PIN_NSS, VELUX_PIN_RST, VELUX_PIN_DIO0};
  const uint8_t slotCount = sizeof(VELUX_SLOTS) / sizeof(VELUX_SLOTS[0]);
  const bool radioOk = veluxRadio.begin(pins, VELUX_TX_POWER, VELUX_SLOTS, slotCount);
  Serial.printf("Radio SX1276: %s (%u kanał(y))\n",
                radioOk ? "OK" : "BRAK - sprawdz polaczenia SPI i zasilanie",
                static_cast<unsigned>(slotCount));

  // Kanały SUPLA (roleta / okno dachowe).
  for (uint8_t i = 0; i < slotCount; i++) {
    veluxCovers[i] = new VeluxCover(&veluxRadio, i, VELUX_SLOTS[i]);
  }

  // Realny status 2W -> kanał SUPLA (pozycja z urządzenia zamiast szacunku).
  veluxRadio.setStatusListener([](uint8_t slot, const velux::VeluxRadio::TwoWayStatus &status) {
    if (slot < velux::MAX_SLOTS && veluxCovers[slot] != nullptr) {
      veluxCovers[slot]->applyTwoWayStatus(status);
    }
  });

  // Klucz instalacji odebrany z pilota - wypisz, żeby dało się go zapisać.
  veluxRadio.setKeyExtractedListener([](const velux::VeluxRadio::HubCredentials &creds) {
    Serial.printf("[velux2w] KLUCZ INSTALACJI: node_id %02X%02X%02X system_key ", creds.node_id[0], creds.node_id[1],
                  creds.node_id[2]);
    for (uint8_t i = 0; i < 16; i++) {
      Serial.printf("%02X", creds.system_key[i]);
    }
    Serial.println();
  });

  // Strona VELUX w panelu konfiguracji (nauka, klasy 0x30, tożsamość).
  new VeluxHtml(&veluxRadio);

  // Przytrzymanie ~5 s -> tryb konfiguracji (AP); klik w trybie
  // konfiguracji -> wyjście i restart.
  auto cfgButton = new Supla::Control::Button(VELUX_PIN_BTN_CFG, true, true);
  cfgButton->configureAsConfigButton(&SuplaDevice);

  SuplaDevice.setName("SUPLA-VELUX-1W");
  SuplaDevice.setSwVersion("velux-io-supla-v1");
  // Pusta konfiguracja -> od razu AP 192.168.4.1 (pierwsze uruchomienie).
  SuplaDevice.setInitialMode(Supla::InitialMode::StartInCfgMode);
  SuplaDevice.begin();
}

void loop() {
  SuplaDevice.iterate();

  // Nasłuch 1W (nieblokujący) - loguje m.in. ramki 0x2E pilota KLI.
  veluxRadio.poll();

  // Akcje z panelu WWW (nauka, wypisanie, nowa tożsamość) wykonują się tutaj,
  // w pętli głównej - wysyłka blokuje na ~1-2 s.
  veluxRadio.processPending();
}
