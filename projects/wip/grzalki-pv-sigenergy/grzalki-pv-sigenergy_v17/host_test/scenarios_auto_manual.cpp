// v17: AUTO thresholds, MANUAL H2/H3 and the master switch interplay.
// Runs the REAL Controller::Engine::step(); only Arduino/FreeRTOS/1-Wire and
// Supla HTML are stubbed. GPIO and DAC decisions are the shipping code.
#include <cstdio>
#include "controller.h"

uint32_t g_fakeMillis = 0;
float g_fakeTemp = 40.0f;
int g_h2Level = 0, g_h3Level = 0;
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
  float pv = 0;
  void boot() {
    Controller::mailbox.setOperational(true);
    Controller::mailbox.setMaster(true);
    engine.begin();
  }
  void maybeGrid() {
    if (g_fakeMillis - lastPoll < 2000) return;
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
};

static void show(const char* tag, const Controller::Status& s) {
  printf("%-32s h2=%d h3=%d ssr=%4.0fW gpio25=%d gpio26=%d\n", tag, s.h2,
         s.h3, double(s.ssrWatts), g_h2Level, g_h3Level);
}

static void base() {
  cfg.safetyValid = cfg.ssrValid = cfg.autoValid = cfg.valid = true;
  cfg.limit = 80; cfg.reset = 75; cfg.fixed = 1500; cfg.maximum = 1500;
  cfg.h2On = 1700; cfg.h2Off = 1450; cfg.h3On = 3200; cfg.h3Off = 2950;
  cfg.ssrOn = 200; cfg.ssrOff = 80; cfg.dwell = 0;
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);

  printf("\n=== AUTO: kolejnosc progow (SSR -> H2 -> H3) ===\n");
  base();
  Rig r;
  r.pv = 1600;
  r.boot();
  r.run(20000);
  auto s = r.st();
  show("PV 1600 W", s);
  check(!s.h2 && s.ssrWatts > 1400, "1600 W: tylko SSR, H2 jeszcze nie");
  r.pv = 2100;
  r.run(10000);
  s = r.st();
  show("PV 2100 W", s);
  check(s.h2 && !s.h3, "2100 W: dolacza H2");
  r.pv = 3500;
  r.run(15000);
  s = r.st();
  show("PV 3500 W", s);
  check(s.h2 && s.h3, "3500 W: dolacza H3");
  r.pv = 3000;
  r.run(15000);
  s = r.st();
  show("PV 3000 W (histereza)", s);
  check(s.h2 && s.h3, "3000 W: H3 zostaje (prog h3_off = 2950)");
  r.pv = 1500;
  r.run(15000);
  s = r.st();
  show("PV 1500 W (histereza)", s);
  // H2 keeps running on purpose: 1500 W of PV exactly covers its 1500 W, the
  // budget stays at h2_off and nothing is drawn from the grid.
  check(s.h2 && !s.h3 && s.ssrWatts == 0, "1500 W: H2 trzyma (budzet > h2_off)");
  r.pv = 1200;
  r.run(15000);
  s = r.st();
  show("PV 1200 W", s);
  check(!s.h2 && !s.h3 && s.ssrWatts > 1100,
        "1200 W: H2 OFF (budzet < h2_off), SSR grzeje reszte");

  printf("\n=== MANUAL: tylko H2/H3, bez DAC ===\n");
  r.pv = 2200;
  r.run(10000);
  s = r.st();
  show("PV 2200 W", s);
  check(s.h2 && s.ssrWatts > 0, "H2 wrocilo, regulator bierze reszte nadwyzki");

  Controller::mailbox.setMode(true, g_fakeMillis, 0);
  r.run(3000);
  s = r.st();
  show("MANUAL bez suwakow", s);
  check(!s.h2 && !s.h3 && s.ssrWatts == 0, "MANUAL sam z siebie nic nie grzeje");
  uint32_t rev = 0;
  const uint32_t gen = Controller::mailbox.read().command.generation;
  Controller::mailbox.demand(0, true, g_fakeMillis, 0, gen, rev);
  r.run(3000);
  s = r.st();
  show("MANUAL + suwak H2", s);
  check(s.h2 && s.ssrWatts == 0, "H2 grzeje, DAC nadal 0 V");
  Controller::mailbox.setMode(false, 0, 0);
  r.run(10000);
  s = r.st();
  show("powrot AUTO", s);
  check(s.h2 && s.ssrWatts > 0, "AUTO przejmuje sterowanie");

  printf("\n=== Dwell 60 s: AUTO czeka, STOP dziala natychmiast ===\n");
  g_fakeMillis = 0;
  g_h2Level = g_h3Level = 0;
  base();
  cfg.dwell = 60;
  Rig r2;
  r2.pv = 2200;
  r2.boot();
  r2.run(20000);
  s = r2.st();
  show("AUTO dwell=60 s", s);
  check(s.h2, "H2 wlaczone");
  r2.pv = 800;
  r2.run(6000);
  s = r2.st();
  show("PV 800 W po 6 s", s);
  check(s.h2, "AUTO trzyma H2 przez dwell (do 60 s) - tak ma byc");
  Controller::mailbox.setMaster(false);
  r2.run(200);
  s = r2.st();
  show("STOP +0.2 s", s);
  check(!s.h2 && s.ssrWatts == 0 && g_h2Level == 0,
        "STOP gasi H2 od razu, bez czekania na dwell");

  printf("\n%s (%d bledow)\n", failures ? "BLAD" : "WSZYSTKO OK", failures);
  return failures ? 1 : 0;
}
