#pragma once
// Parametry zmieniane ze strony WWW (zapis w LittleFS).
// Ladowane raz, po SuplaDevice.begin(). Zmiana wymaga SAVE & RESTART.

#include <Arduino.h>
#include <math.h>
#include <supla/network/html/custom_parameter.h>
#include <supla/network/html/custom_text_parameter.h>

// Staly poczatek bloku rejestrow Sigenergy. Wszystkie offsety w parserze sa
// liczone wzgledem tego adresu, dlatego nie jest parametrem.
constexpr uint16_t SIG_BLOCK_START = 30004;

struct Settings {
  int32_t unit = 247;        // plant unit ID
  int32_t port = 502;        // Modbus TCP
  int32_t wordOrder = 1;     // 1 = starsze slowo pierwsze (S32)
  int32_t scale = 1000;      // W = surowa * scale / 1000
  int32_t blockCount = 54;   // 54 = 30004..30057, obejmuje moce faz CT
  int32_t requireStatus = 1; // wymagaj statusu CT = 1
  int32_t gridPhase = 1;     // faza CT z pompa: 1/2/3; 0 = suma (30005)

  int32_t saleAuto = 1;      // automat: brak PV + eksport = sprzedaz
  float saleOnW = 1000;      // eksport, od ktorego blokowac [W]
  float saleOffW = 300;      // eksport, ponizej ktorego odblokowac [W]
  float salePvMaxW = 100;    // PV ponizej tej mocy = brak produkcji [W]
  int32_t saleOnDelayS = 30;
  int32_t saleOffDelayS = 60;

  float pumpMinW = 10;       // ponizej tej mocy energia idzie do sieci [W]

  bool valid = false;
};
Settings cfg;

using Number = Supla::Html::CustomParameterTemplate<int32_t>;
using Decimal = Supla::Html::CustomParameterTemplate<float>;

// --- Polaczenie z Sigenergy -------------------------------------------------
Number unitField("mb_unit", "Sigenergy Modbus Unit ID (247)", 247, 1, 247);
Number portField("mb_port", "Sigenergy Modbus TCP port", 502, 1, 65535);
Number blockCountField(
    "sig_block_count", "Liczba rejestrow (54 = z mocami faz)", 54, 35, 120);
Number wordField("sig_word_order", "Kolejnosc slow: 1=starsze pierwsze", 1, 0,
                 1);
Number scaleField("sig_scale", "Skala: W = surowa * ta wartosc / 1000", 1000, 1,
                  1000000);
Number requireStatusField(
    "sig_require_status", "Wymagaj statusu przekladnika CT (1=tak)", 1, 0, 1);

// Faza, na ktorej wisi pompa. To jest zrodlo mocy sieci dla podzialu energii.
Number gridPhaseField(
    "sig_grid_phase",
    "Faza CT z pompa: 1/2/3, 0=suma (suma tylko dla 1-fazowej instalacji)",
    1, 0, 3);

// --- Blokada sprzedazy ------------------------------------------------------
Number saleAutoField("sale_auto",
                     "Automatyczna blokada sprzedazy (1=tak)", 1, 0, 1);
Decimal saleOnWField("sale_on_w", "Eksport od ktorego blokowac pompe [W]", 1000,
                     1, 50000, 0);
Decimal saleOffWField("sale_off_w", "Eksport ponizej ktorego odblokowac [W]",
                      300, 0, 49999, 0);
Decimal salePvMaxField("sale_pv_max_w",
                       "PV ponizej tej mocy = brak produkcji [W]", 100, 0, 50000,
                       0);
Number saleOnDelayField("sale_on_delay_s", "Opoznienie wejscia w sprzedaz [s]",
                        30, 0, 3600);
Number saleOffDelayField("sale_off_delay_s",
                         "Opoznienie wyjscia ze sprzedazy [s]", 60, 0, 3600);

// --- Pompa ------------------------------------------------------------------
Decimal pumpMinField("pump_min_w", "Minimalna moc pompy do przypisania zrodla [W]",
                     10, 0, 1000, 0);

// Wymagany IPv4 falownika.
Supla::Html::CustomTextParameter ipField(
    "inverter_ip", "IPv4 falownika Sigenergy (wymagane)", 15);

void loadSettings() {
  cfg.unit = unitField.getParameterValue();
  cfg.port = portField.getParameterValue();
  cfg.blockCount = blockCountField.getParameterValue();
  cfg.wordOrder = wordField.getParameterValue();
  cfg.scale = scaleField.getParameterValue();
  cfg.requireStatus = requireStatusField.getParameterValue();
  cfg.gridPhase = gridPhaseField.getParameterValue();
  cfg.saleAuto = saleAutoField.getParameterValue();
  cfg.saleOnW = saleOnWField.getParameterValue();
  cfg.saleOffW = saleOffWField.getParameterValue();
  cfg.salePvMaxW = salePvMaxField.getParameterValue();
  cfg.saleOnDelayS = saleOnDelayField.getParameterValue();
  cfg.saleOffDelayS = saleOffDelayField.getParameterValue();
  cfg.pumpMinW = pumpMinField.getParameterValue();

  cfg.valid = cfg.unit >= 1 && cfg.unit <= 247 &&
              cfg.port >= 1 && cfg.port <= 65535 &&
              cfg.blockCount >= 35 && cfg.blockCount <= 120 &&
              (cfg.wordOrder == 0 || cfg.wordOrder == 1) && cfg.scale >= 1 &&
              (cfg.requireStatus == 0 || cfg.requireStatus == 1) &&
              cfg.gridPhase >= 0 && cfg.gridPhase <= 3 &&
              // Odczyt mocy fazy wymaga bloku obejmujacego 30052..30057.
              (cfg.gridPhase == 0 || cfg.blockCount >= 54) &&
              (cfg.saleAuto == 0 || cfg.saleAuto == 1) &&
              isfinite(cfg.saleOnW) && isfinite(cfg.saleOffW) &&
              isfinite(cfg.salePvMaxW) && cfg.salePvMaxW >= 0 &&
              cfg.salePvMaxW <= cfg.saleOnW && cfg.saleOnW > cfg.saleOffW &&
              cfg.saleOnW >= 1 && cfg.saleOffW >= 0 &&
              cfg.saleOnDelayS >= 0 && cfg.saleOnDelayS <= 3600 &&
              cfg.saleOffDelayS >= 0 && cfg.saleOffDelayS <= 3600 &&
              isfinite(cfg.pumpMinW) && cfg.pumpMinW >= 0 &&
              cfg.pumpMinW <= 1000;
}
