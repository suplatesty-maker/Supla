#pragma once
#include <supla/network/html/custom_parameter.h>
#include <supla/network/html/custom_text_parameter.h>
#include "control_logic.h"

// Keys compatible with v3. Values take effect only after save AND restart.
struct Settings {
  int32_t reg = 5004, word = 1, scale = 1000, dwell = 60;
  int32_t unit = 247, port = 502, input = 0;
  float ssrOn = 200, ssrOff = 80, h2On = 1700, h2Off = 1450;
  float h3On = 3200, h3Off = 2950, limit = 80, reset = 75;
  float fixed = 1500, maximum = 1500;
  bool valid = false;
};
Settings cfg;
using Number = Supla::Html::CustomParameterTemplate<int32_t>;
using Decimal = Supla::Html::CustomParameterTemplate<float>;
Number regField("reg_addr", "Adres rejestru mocy (zweryfikuj dokumentacje)", 5004, 0, 65534);
Number wordField("word_order", "Slowa: 1=wyzsze pierwsze, 0=zamienione", 1, 0, 1);
Number scaleField("grid_scale", "Mnoznik do W = ta liczba / 1000", 1000, 1, 1000000);
Number dwellField("min_switch_s", "Minimalny odstep przelaczen H2/H3 [s]", 60, 0, 3600);
Number unitField("mb_unit", "Modbus Unit ID", 247, 1, 247);
Number portField("mb_port", "Modbus TCP port", 502, 1, 65535);
Number inputField("mb_input", "Rejestry: 0=Holding FC03, 1=Input FC04", 0, 0, 1);
Decimal ssrOnField("ssr_on", "SSR start budzetu [W]", 200, 0, 10000, 1);
Decimal ssrOffField("ssr_off", "SSR stop budzetu [W], mniej niz start", 80, 0, 10000, 1);
Decimal h2OnField("h2_on", "H2 start budzetu [W]", 1700, 0, 10000, 1);
Decimal h2OffField("h2_off", "H2 stop budzetu [W]", 1450, 0, 10000, 1);
Decimal h3OnField("h3_on", "H3 start budzetu [W]", 3200, 0, 10000, 1);
Decimal h3OffField("h3_off", "H3 stop budzetu [W]", 2950, 0, 10000, 1);
Decimal limitField("temp_limit", "Temperatura STOP [C], maks. 80", 80, 30, 80, 1);
Decimal resetField("temp_reset", "Temperatura powrotu [C], ponizej STOP", 75, 20, 79, 1);
Decimal fixedField("heater_power", "Moc jednej grzalki H2/H3 [W]", 1500, 100, 10000, 1);
Decimal maximumField("ssr_max_power", "Moc grzalki SSR przy 10 V [W]", 1500, 100, 10000, 1);
Supla::Html::CustomTextParameter ipField("inverter_ip", "IPv4 falownika (wymagane)", 15);

void loadSettings() {
  cfg.reg = regField.getParameterValue();
  cfg.word = wordField.getParameterValue();
  cfg.scale = scaleField.getParameterValue();
  cfg.dwell = dwellField.getParameterValue();
  cfg.unit = unitField.getParameterValue();
  cfg.port = portField.getParameterValue();
  cfg.input = inputField.getParameterValue();
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
  cfg.valid = Heating::validPair(cfg.limit, cfg.reset) &&
              Heating::validPair(cfg.ssrOn, cfg.ssrOff) &&
              Heating::validPair(cfg.h2On, cfg.h2Off) &&
              Heating::validPair(cfg.h3On, cfg.h3Off) &&
              cfg.h3On > cfg.h2On && cfg.h3Off > cfg.h2Off;
}
