#pragma once
// Web (config mode / LAN) parameters. Loaded once in setup() before the control
// task starts; changes take effect after SAVE & RESTART.
// Keys: max 15 characters (SUPLA_CONFIG_MAX_KEY_SIZE = 16).
#include <supla/network/html/custom_parameter.h>
#include "hydro_logic.h"

struct Config {
  Hydro::Settings hydro;
  Hydro::Calibration cal;
  bool valid = false;
};
Config cfg;

namespace Web {
using Number = Supla::Html::CustomParameterTemplate<int32_t>;
using Decimal = Supla::Html::CustomParameterTemplate<float>;
const Hydro::Settings D{};
const Hydro::Calibration C{};

Decimal pMax("h_p_max", "Cisnienie maksymalne = pelne zbiorniki [bar]", D.pMax, 1.0f, 8.0f, 2);
Decimal dayDrop("h_day_drop", "Dzien: start pompy po spadku ponizej maks. [bar]", D.dayDrop, 0.05f, 3.0f, 2);
Decimal nightDrop("h_night_drop", "Noc: start pompy po spadku ponizej maks. [bar]", D.nightDrop, 0.05f, 3.0f, 2);
Decimal pCritical("h_p_crit", "Prog krytyczny -> woda miejska [bar]", D.pCritical, 0.3f, 7.0f, 2);
Decimal pEmergency("h_p_emerg", "Cisnienie awaryjne STOP [bar]", D.pEmergency, 1.5f, 10.0f, 2);
Decimal fullMargin("h_full_margin", "Margines 'pelny' ponizej maks. [bar]", D.fullMargin, 0.02f, 1.0f, 2);
Decimal precharge("h_precharge", "Cisnienie powietrza w pustych zbiornikach [bar]", D.precharge, 0.2f, 6.0f, 2);
Decimal capacity("h_capacity", "Pojemnosc wszystkich zbiornikow [l]", D.capacity, 10.0f, 5000.0f, 0);
Decimal dayBatch("h_day_batch", "Dzien: limit wody na porcje [l], 0 = bez limitu", D.dayBatchL, 0.0f, 1000.0f, 0);
Decimal nightBatch("h_night_batch", "Noc: limit wody na porcje [l], 0 = do suchobiegu", D.nightBatchL, 0.0f, 1000.0f, 0);
Decimal minYield("h_min_yield", "Porcja ponizej tylu litrow = nieudana [l]", D.minYieldL, 0.0f, 100.0f, 0);
Decimal dryRise("h_dry_rise", "Suchobieg: min. przyrost cisnienia w oknie [bar]", D.dryRise, 0.005f, 0.5f, 3);
Number dayWindow("h_day_win", "Dzien: okno detekcji suchobiegu [s]", D.dayWindowS, 5, 300);
Number nightWindow("h_night_win", "Noc: okno detekcji suchobiegu [s]", D.nightWindowS, 5, 300);
Number restMin("h_rest_min", "Przerwa po porcji na regeneracje studni [min]", D.restMin, 1, 600);
Number maxRun("h_max_run", "Maks. czas jednej porcji [min]", D.maxRunMin, 1, 60);
Number nightStart("h_night_from", "Noc od godziny (0-23)", D.nightStart, 0, 23);
Number nightEnd("h_night_to", "Noc do godziny (0-23), np. 5 = pelne o 5:00", D.nightEnd, 0, 23);
Number lockCount("h_lock_count", "Blokada po tylu nieudanych porcjach z rzedu", D.lockCount, 1, 20);
Number lockRetry("h_lock_retry", "Ponowna proba po blokadzie [h], 0 = tylko recznie", D.lockRetryH, 0, 48);
Decimal zeroV("h_cal_zero_v", "Kalibracja: napiecie czujnika przy 0 bar [V]", C.zeroV, 0.0f, 2.0f, 3);
Decimal fullV("h_cal_full_v", "Kalibracja: napiecie czujnika przy pelnej skali [V]", C.fullV, 2.0f, 5.0f, 3);
Decimal fullBar("h_cal_full_bar", "Kalibracja: pelna skala czujnika [bar]", C.fullBar, 1.0f, 60.0f, 3);
Decimal divider("h_cal_div", "Dzielnik: Vadc / Vczujnika (8.2k/(4.7k+8.2k))", C.divider, 0.1f, 1.0f, 4);
}  // namespace Web

void loadSettings() {
  auto &h = cfg.hydro;
  h.pMax = Web::pMax.getParameterValue();
  h.dayDrop = Web::dayDrop.getParameterValue();
  h.nightDrop = Web::nightDrop.getParameterValue();
  h.pCritical = Web::pCritical.getParameterValue();
  h.pEmergency = Web::pEmergency.getParameterValue();
  h.fullMargin = Web::fullMargin.getParameterValue();
  h.precharge = Web::precharge.getParameterValue();
  h.capacity = Web::capacity.getParameterValue();
  h.dayBatchL = Web::dayBatch.getParameterValue();
  h.nightBatchL = Web::nightBatch.getParameterValue();
  h.minYieldL = Web::minYield.getParameterValue();
  h.dryRise = Web::dryRise.getParameterValue();
  h.dayWindowS = Web::dayWindow.getParameterValue();
  h.nightWindowS = Web::nightWindow.getParameterValue();
  h.restMin = Web::restMin.getParameterValue();
  h.maxRunMin = Web::maxRun.getParameterValue();
  h.nightStart = Web::nightStart.getParameterValue();
  h.nightEnd = Web::nightEnd.getParameterValue();
  h.lockCount = Web::lockCount.getParameterValue();
  h.lockRetryH = Web::lockRetry.getParameterValue();
  auto &c = cfg.cal;
  c.zeroV = Web::zeroV.getParameterValue();
  c.fullV = Web::fullV.getParameterValue();
  c.fullBar = Web::fullBar.getParameterValue();
  c.divider = Web::divider.getParameterValue();
  cfg.valid = Hydro::valid(h) && Hydro::validCalibration(c);
}
