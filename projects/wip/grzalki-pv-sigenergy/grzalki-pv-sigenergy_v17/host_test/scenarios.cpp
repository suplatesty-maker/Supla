// Full-loop tests on the REAL v17 Engine: the global STOP switch.
#include <cstdio>
#include <cmath>
#include "controller.h"

uint32_t g_fakeMillis = 0;
float g_fakeTemp = 40.0f;
int g_h2Level = 0, g_h3Level = 0;
int g_dacLevel = -1;  // last millivolt-like level written to the DAC
TwoWire Wire;
SerialStub Serial;

float DallasTemperature::getTempC(const uint8_t*) { return g_fakeTemp; }
void gpio_set_level(gpio_num_t pin, int value) {
  if (pin == Controller::H2_PIN) g_h2Level = value;
  else if (pin == Controller::H3_PIN) g_h3Level = value;
}

static int failures = 0;
static void check(bool c, const char* w) {
  printf("  %s  %s\n", c ? "OK  " : "FAIL", w);
  if (!c) ++failures;
}

struct Rig {
  Controller::Engine engine;
  uint32_t lastPoll = 0;
  float pv = 2500;
  bool modbusAlive = true;

  void boot() {
    Controller::mailbox.setOperational(true);
    Controller::mailbox.setMaster(true);
    engine.begin();
  }
  void maybeGrid() {
    if (!modbusAlive || g_fakeMillis - lastPoll < 2000) return;
    lastPoll = g_fakeMillis;
    const auto s = Controller::mailbox.read();
    if (!s.status.ready || s.status.transitioning) return;
    const float load = s.status.ssrWatts + (s.status.h2 ? cfg.fixed : 0) +
                       (s.status.h3 ? cfg.fixed : 0);
    const float grid = load - pv;
    Controller::GridSample g;
    g.generation = s.command.generation;
    g.outputEpoch = s.status.outputEpoch;
    g.at = g_fakeMillis;
    g.valid = true;
    g.watts = grid;
    g.budget = Heating::available(grid, load);
    Controller::mailbox.grid(g);
  }
  void run(uint32_t ms) {
    const uint32_t end = g_fakeMillis + ms;
    while (g_fakeMillis < end) { g_fakeMillis += 50; engine.step(); maybeGrid(); }
  }
  Controller::Status st() const { return Controller::mailbox.read().status; }
  bool master() const { return Controller::mailbox.read().command.master; }
  void show(const char* tag) const {
    const auto s = st();
    printf("%-30s h2=%d h3=%d ssr=%4.0fW gpio25=%d gpio26=%d%s\n", tag, s.h2,
           s.h3, double(s.ssrWatts), g_h2Level, g_h3Level,
           master() ? "" : "  [STOP]");
  }
};

static void base() {
  cfg.safetyValid = cfg.ssrValid = cfg.autoValid = cfg.valid = true;
  cfg.limit = 80; cfg.reset = 75; cfg.fixed = 1500; cfg.maximum = 1500;
  cfg.h2On = 1700; cfg.h2Off = 1450; cfg.h3On = 3200; cfg.h3Off = 2950;
  cfg.ssrOn = 200; cfg.ssrOff = 80; cfg.dwell = 0;
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);

  // -------------------------------------------------- AUTO: STOP cuts all --
  printf("\n=== AUTO, PV 3400 W (H2+H3+SSR), STOP OFF ===\n");
  base();
  Rig r;
  r.pv = 3400;
  r.boot();
  r.run(25000);
  r.show("przed STOP");
  auto s = r.st();
  check(s.h2 && s.h3 && s.ssrWatts > 0, "wszystkie trzy grzalki grzeja");

  Controller::mailbox.setMaster(false);
  r.run(200);
  r.show("STOP +0.2 s");
  s = r.st();
  check(!s.h2 && !s.h3 && s.ssrWatts == 0, "STOP wylaczyl H2, H3 i DAC");
  check(g_h2Level == 0 && g_h3Level == 0, "GPIO25/26 maja poziom LOW");

  printf("\n=== STOP trzyma sie przez 2 min przy PV 3400 W ===\n");
  r.run(120000);
  r.show("STOP +120 s");
  s = r.st();
  check(!s.h2 && !s.h3 && s.ssrWatts == 0 && !r.master(), "nic sie nie wlaczylo");

  printf("\n=== STOP OFF -> ON: AUTO wraca samo ===\n");
  Controller::mailbox.setMaster(true);
  r.run(8000);
  r.show("STOP ON +8 s");
  s = r.st();
  check(s.h2 && s.h3 && s.ssrWatts > 0, "AUTO znowu grzeje bez dotykania trybu");

  // ------------------------------------------------------ MANUAL + STOP ----
  printf("\n=== MANUAL: H2 i H3 recznie ON, potem STOP ===\n");
  Controller::mailbox.setMode(true, g_fakeMillis, 0);
  r.run(2000);
  uint32_t rev = 0;
  uint32_t gen = Controller::mailbox.read().command.generation;
  Controller::mailbox.demand(0, true, g_fakeMillis, 0, gen, rev);
  Controller::mailbox.demand(1, true, g_fakeMillis, 0, gen, rev);
  r.run(3000);
  r.show("MANUAL H2+H3 ON");
  s = r.st();
  check(s.h2 && s.h3, "obie grzalki grzeja recznie");
  check(s.ssrWatts == 0, "SSR w MANUAL pozostaje 0 V");

  Controller::mailbox.setMaster(false);
  r.run(200);
  r.show("STOP w MANUAL");
  s = r.st();
  check(!s.h2 && !s.h3 && g_h2Level == 0 && g_h3Level == 0,
        "STOP dziala tez w MANUAL");
  check(Controller::mailbox.read().command.manual, "tryb MANUAL zostal");

  printf("\n=== STOP ON w MANUAL: wraca reczne grzanie ===\n");
  Controller::mailbox.setMaster(true);
  r.run(3000);
  r.show("STOP ON w MANUAL");
  s = r.st();
  check(s.h2 && s.h3, "suwaki H2/H3 nadal pamietaja ON");

  // ------------------------------------------- STOP a nowy suwak H2 w MANUAL
  printf("\n=== STOP OFF, potem proba wlaczenia H2 suwakiem ===\n");
  Controller::mailbox.setMaster(false);
  r.run(500);
  gen = Controller::mailbox.read().command.generation;
  const bool accepted = Controller::mailbox.demand(0, true, g_fakeMillis, 0, gen, rev);
  r.run(3000);
  r.show("po zadaniu H2 ON przy STOP");
  s = r.st();
  check(!s.h2 && g_h2Level == 0, "Engine nie wlaczyl H2 mimo zadania");
  printf("       (mailbox przyjal zadanie: %s)\n", accepted ? "tak" : "nie");
  Controller::mailbox.setMaster(true);
  r.run(4000);
  r.show("STOP ON");
  check(r.st().h2, "po STOP ON zadanie zostalo zrealizowane");

  Controller::mailbox.setMode(false, 0, 0);
  r.run(8000);

  // ---------------------------------------- STOP przetrwa awarie czujnika --
  printf("\n=== STOP OFF, awaria czujnika, powrot czujnika ===\n");
  Controller::mailbox.setMaster(false);
  r.run(500);
  g_fakeTemp = 85.0f;
  r.run(5000);
  r.show("awaria czujnika");
  g_fakeTemp = 40.0f;
  r.run(15000);
  r.show("czujnik wrocil");
  s = r.st();
  check(!r.master(), "STOP nie zostal skasowany przez awarie");
  check(!s.h2 && s.ssrWatts == 0, "nic nie grzeje");

  // -------------------------------------- STOP OFF przy braku Modbus ------
  printf("\n=== STOP OFF przy zerwanym Modbus, potem powrot danych ===\n");
  Controller::mailbox.setMaster(true);
  r.run(10000);
  check(r.st().h2, "grzanie wrocilo");
  r.modbusAlive = false;
  r.run(12000);
  r.show("brak Modbus");
  Controller::mailbox.setMaster(false);
  r.modbusAlive = true;
  r.run(15000);
  r.show("Modbus wrocil, STOP OFF");
  check(!r.st().h2 && r.st().ssrWatts == 0, "STOP nadal trzyma");

  // ---------------------------------------------- szybkie przelaczanie -----
  printf("\n=== 60 x STOP ON/OFF co 100 ms ===\n");
  bool ok = true;
  for (int i = 0; i < 60; ++i) {
    Controller::mailbox.setMaster(i % 2 == 0);
    r.run(100);
    const auto cur = r.st();
    if (i % 2 == 1 && (cur.h2 || cur.h3 || cur.ssrWatts != 0)) ok = false;
  }
  check(ok, "kazde STOP OFF natychmiast gasi wszystkie wyjscia");
  Controller::mailbox.setMaster(true);
  r.run(8000);
  r.show("po stress");
  check(r.st().h2, "po stress AUTO wraca");

  printf("\n=== ODTWORZENIE ZGLOSZENIA: PV 2200 W, H2 ON, wylaczam suwak ===\n");
  base();
  g_fakeMillis = 0;
  g_h2Level = g_h3Level = 0;
  Rig rep;
  rep.pv = 2200;
  rep.boot();
  rep.run(20000);
  rep.show("AUTO 20 s");
  auto rs = rep.st();
  check(rs.h2 && rs.ssrWatts > 0, "H2 i SSR grzeja z nadwyzki 2200 W");
  Controller::mailbox.setMaster(false);
  rep.run(200);
  rep.show("suwak OFF +0.2 s");
  rs = rep.st();
  check(!rs.h2 && rs.ssrWatts == 0 && g_h2Level == 0,
        "H2 GASNIE razem z suwakiem (w v16 zostawal ON)");
  printf("\n");

  printf("\n%s (%d bledow)\n", failures ? "BLAD" : "WSZYSTKO OK", failures);
  return failures ? 1 : 0;
}
