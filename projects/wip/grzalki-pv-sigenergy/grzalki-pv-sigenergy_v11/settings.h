#pragma once
// v11: configuration separated into safety and AUTO validity.
// Manual H2/H3 depends only on safetyValid, never on the Sigenergy map.
#include <supla/network/html/custom_parameter.h>
#include <supla/network/html/custom_text_parameter.h>
#include "control_logic.h"

// Loaded once BEFORE the controller task starts; immutable afterwards.
struct Settings {
  // 0 = plant map (30005/30006, FC04, unit 247, CT/on-grid checks)
  // 1 = own map (addr/input below; use 5004 + FC03 to replicate v6/v7)
  // 2 = auto: try plant, then 5004/FC03; latch the first plausible map
  int32_t map = 2;
  int32_t reg = 30005, word = 1, scale = 1000, dwell = 60;
  int32_t unit = 247, port = 502, input = 1;
  int32_t requireCt = 1, requireOnGrid = 1, wordAuto = 1;
  float ssrOn = 200, ssrOff = 80, h2On = 1700, h2Off = 1450;
  float h3On = 3200, h3Off = 2950, limit = 80, reset = 75;
  float fixed = 1500, maximum = 1500;
  bool safetyValid = false;  // temperature + power values; manual needs only this
  bool autoValid = false;    // stage thresholds + map; AUTO needs safety && this
  bool valid = false;        // safety && auto; IP is checked in the sketch
};
Settings cfg;

using Number = Supla::Html::CustomParameterTemplate<int32_t>;
using Decimal = Supla::Html::CustomParameterTemplate<float>;

Number mapField("sig_map",
    "Mapa Sigenergy: 0=plant 30005 FC04, 1=wlasna, 2=auto (plant -> 5004 FC03)",
    2, 0, 2);
Number regField("sig_grid_addr",
    "Wlasny rejestr mocy sieci (tylko sig_map=1)", 30005, 1, 65534);
Number inputField("sig_grid_input",
    "Wlasny typ: 1=Input FC04, 0=Holding FC03 (tylko sig_map=1)", 1, 0, 1);
Number wordField("word_order",
    "Kolejnosc slow: 1=starsze pierwsze, 0=zamienione", 1, 0, 1);
Number wordAutoField("word_auto",
    "Automatyczna korekta kolejnosci slow przy nierealnej wartosci", 1, 0, 1);
Number scaleField("grid_scale",
    "Mnoznik do W = wartosc * ta liczba / 1000", 1000, 1, 1000000);
Number requireCtField("require_ct",
    "Wymagaj statusu CT 30004=1 (0 wylacza kontrole)", 1, 0, 1);
Number requireOnGridField("require_ongrid",
    "Wymagaj pracy on-grid 30009=0 (0 wylacza kontrole)", 1, 0, 1);
Number dwellField("min_switch_s",
    "Minimalny odstep przelaczen H2/H3 w AUTO [s]", 60, 0, 3600);
Number unitField("mb_unit", "Modbus Unit ID (plant = 247)", 247, 1, 247);
Number portField("mb_port", "Modbus TCP port", 502, 1, 65535);
Decimal ssrOnField("ssr_on", "SSR start budzetu [W]", 200, 0, 10000, 1);
Decimal ssrOffField("ssr_off", "SSR stop budzetu [W], mniej niz start",
    80, 0, 10000, 1);
Decimal h2OnField("h2_on", "H2 start budzetu [W]", 1700, 0, 10000, 1);
Decimal h2OffField("h2_off", "H2 stop budzetu [W]", 1450, 0, 10000, 1);
Decimal h3OnField("h3_on", "H3 start budzetu [W]", 3200, 0, 10000, 1);
Decimal h3OffField("h3_off", "H3 stop budzetu [W]", 2950, 0, 10000, 1);
Decimal limitField("temp_limit", "Temperatura STOP [C], maks. 80",
    80, 30, 80, 1);
Decimal resetField("temp_reset", "Temperatura powrotu [C], ponizej STOP",
    75, 20, 79, 1);
Decimal fixedField("heater_power", "Moc jednej grzalki H2/H3 [W]",
    1500, 100, 10000, 1);
Decimal maximumField("ssr_max_power", "Moc grzalki SSR przy 10 V [W]",
    1500, 100, 10000, 1);
Supla::Html::CustomTextParameter ipField("inverter_ip",
    "IPv4 falownika (wymagane dla AUTO)", 15);

void loadSettings() {
  cfg.map = mapField.getParameterValue();
  cfg.reg = regField.getParameterValue();
  cfg.input = inputField.getParameterValue();
  cfg.word = wordField.getParameterValue();
  cfg.wordAuto = wordAutoField.getParameterValue();
  cfg.scale = scaleField.getParameterValue();
  cfg.requireCt = requireCtField.getParameterValue();
  cfg.requireOnGrid = requireOnGridField.getParameterValue();
  cfg.dwell = dwellField.getParameterValue();
  cfg.unit = unitField.getParameterValue();
  cfg.port = portField.getParameterValue();
  cfg.ssrOn = ssrOnField.getParameterValue();
  cfg.ssrOff = ssrOffField.getParameterValue();
  cfg.h2On = h2OnField.getParameterValue();
  cfg.h2Off = h2OffField.getParameterValue();
  cfg.h3On = h3OnField.getParameterValue();
  cfg.h3Off = h3OffField.getParameterValue();
  cfg.limit = limitField.getParameterValue();
  cfg.reset = resetField.getParameterValue();
  cfg.fixed = fixedField.getParameterValue();
  cfg.maximum = maximumField.getParameterValue();

  // Safety applies to every mode, including MANUAL. Invalid stage thresholds
  // must NOT block the two relay heaters, only AUTO. SSR power settings are
  // AUTO-only too: MANUAL never drives the SSR.
  cfg.safetyValid = Heating::validPair(cfg.limit, cfg.reset) &&
                    cfg.limit <= 80 && cfg.reset < cfg.limit;
  cfg.autoValid = cfg.fixed > 0 && cfg.maximum > 0 &&
                  Heating::validPair(cfg.ssrOn, cfg.ssrOff) &&
                  Heating::validPair(cfg.h2On, cfg.h2Off) &&
                  Heating::validPair(cfg.h3On, cfg.h3Off) &&
                  cfg.h3On > cfg.h2On && cfg.h3Off > cfg.h2Off &&
                  (cfg.map == 0 ||
                   cfg.map == 2 ||
                   (cfg.map == 1 && cfg.reg >= 1 && cfg.reg <= 65534 &&
                    (cfg.input == 0 || cfg.input == 1)));
  cfg.valid = cfg.safetyValid && cfg.autoValid;
}
