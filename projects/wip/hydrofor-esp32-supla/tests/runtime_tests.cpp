#include <cassert>
#include <cstdio>
#include <thread>
#include "../hydrofor-esp32-supla_v2/service_relay.h"
#include "../hydrofor-esp32-supla_v2/runtime_watchdog.h"
#include "../hydrofor-esp32-supla_v2/hydro_tests.h"

namespace FakeWdt {
struct Model {
  bool initialized = false, subscribed[2] = {}, fed[2] = {};
  bool failConfigure = false, failInit = false, failAdd = false;
  int currentTask = 0, initCalls = 0, addCalls = 0;
  uint32_t now = 0, lastReset = 0;
  esp_task_wdt_config_t config = {};
  bool expired() const { return now - lastReset >= config.timeout_ms; }
};
Model model;
}

esp_err_t esp_task_wdt_reconfigure(const esp_task_wdt_config_t *config) {
  auto &m = FakeWdt::model;
  if (m.failConfigure) return ESP_FAIL;
  if (!m.initialized) return ESP_ERR_INVALID_STATE;
  m.config = *config;
  return ESP_OK;
}
esp_err_t esp_task_wdt_init(const esp_task_wdt_config_t *config) {
  auto &m = FakeWdt::model;
  ++m.initCalls;
  if (m.failInit) return ESP_FAIL;
  m.initialized = true;
  m.config = *config;
  return ESP_OK;
}
esp_err_t esp_task_wdt_status(TaskHandle_t task) {
  assert(task == nullptr);
  const auto &m = FakeWdt::model;
  if (!m.initialized) return ESP_ERR_INVALID_STATE;
  return m.subscribed[m.currentTask] ? ESP_OK : ESP_ERR_NOT_FOUND;
}
esp_err_t esp_task_wdt_add(TaskHandle_t task) {
  assert(task == nullptr);
  auto &m = FakeWdt::model;
  ++m.addCalls;
  if (m.failAdd) return ESP_FAIL;
  m.subscribed[m.currentTask] = true;
  return ESP_OK;
}
esp_err_t esp_task_wdt_reset() {
  auto &m = FakeWdt::model;
  assert(m.subscribed[m.currentTask]);
  m.fed[m.currentTask] = true;
  for (int i = 0; i < 2; ++i) {
    if (m.subscribed[i] && !m.fed[i]) return ESP_OK;
  }
  m.lastReset = m.now;
  m.fed[0] = m.fed[1] = false;
  return ESP_OK;
}

void watchdogTests() {
  auto &m = FakeWdt::model;
  m = {};
  assert(RuntimeWatchdog::begin());
  assert(m.initCalls == 1 && m.subscribed[0]);
  assert(m.config.trigger_panic && m.config.timeout_ms == 30000);
  assert(RuntimeWatchdog::subscribeCurrentTask());
  assert(m.addCalls == 1);  // no duplicate subscription
  m.currentTask = 1;
  assert(RuntimeWatchdog::subscribeCurrentTask());
  // Both tasks make progress for two minutes, including reconnect-sized delays.
  for (m.now = 50; m.now <= 120000; m.now += 50) {
    m.currentTask = 1;
    RuntimeWatchdog::feed();
    if (m.now % 10000 == 0) {
      m.currentTask = 0;
      RuntimeWatchdog::feed();
    }
    assert(!m.expired());
  }
  // Regression: a stalled SUPLA loop cannot be hidden by healthy control.
  const uint32_t stoppedAt = m.lastReset;
  for (; m.now < stoppedAt + RuntimeWatchdog::TIMEOUT_MS; m.now += 50) {
    m.currentTask = 1;
    RuntimeWatchdog::feed();
  }
  assert(m.expired());

  // Conversely, a running main loop cannot hide a stopped control task.
  m = {};
  m.initialized = true;
  assert(RuntimeWatchdog::begin());
  assert(m.initCalls == 0);
  m.currentTask = 1;
  assert(RuntimeWatchdog::subscribeCurrentTask());
  for (m.now = 0; m.now <= RuntimeWatchdog::TIMEOUT_MS; m.now += 50) {
    m.currentTask = 0;
    RuntimeWatchdog::feed();
  }
  assert(m.expired());

  m = {};
  m.failConfigure = true;
  assert(!RuntimeWatchdog::begin() && m.initCalls == 0);
  m = {};
  m.failInit = true;
  assert(!RuntimeWatchdog::begin());
  m = {};
  m.failAdd = true;
  assert(!RuntimeWatchdog::begin());
  m = {};
  assert(RuntimeWatchdog::begin());
  m.currentTask = 1;
  m.failAdd = true;
  assert(!RuntimeWatchdog::subscribeCurrentTask());
}

void serviceTests() {
  ServiceRelay relay;
  Engine::mailbox.setClock(true, 23);
  // The new command must be visible even if subsequent SUPLA work blocks.
  relay.channelCallback = [](bool state) {
    const auto cmd = Engine::mailbox.commands();
    assert(cmd.disabled == state);
    assert(cmd.clockOk && cmd.hour == 23);
  };
  relay.turnOn();
  assert(Engine::mailbox.commands().disabled);
  relay.turnOff();
  assert(!Engine::mailbox.commands().disabled);

  Hydro::Controller controller(Hydro::Settings{}, 0);
  Hydro::Input in;
  in.sensorOk = true;
  in.clockOk = true;
  in.hour = 12;
  // Service at one minute, then repeated toggling during/after the boot delay.
  for (in.now = 0; in.now < 30 * Hydro::MINUTE_MS; in.now += 250) {
    const bool disabled = (in.now / Hydro::MINUTE_MS) % 2 != 0;
    if (disabled) relay.turnOn(); else relay.turnOff();
    in.disabled = Engine::mailbox.commands().disabled;
    in.pressure = 3.0f + float(in.now % 20000) / 100000.0f;
    const auto out = controller.step(in);
    assert(HydroTests::near(out.tankL,
                           Hydro::litersAt(in.pressure, 1.5f, 200), 0.001f));
    if (disabled) {
      assert(!out.pump && out.valveCity && out.state == Hydro::State::Disabled);
    }
  }

  // Cancelling an active batch should count it once, not each service tick.
  Hydro::Controller active(Hydro::Settings{}, 0);
  in.now = Hydro::BOOT_DELAY_MS;
  in.disabled = false;
  in.pressure = 3.0f;
  assert(active.step(in).pump);
  in.disabled = true;
  for (int i = 0; i < 1000; ++i) {
    in.now += 250;
    const auto out = active.step(in);
    assert(out.batches == 1 && !out.pump && out.valveCity);
  }
}

void mailboxTests() {
  Engine::Mailbox box;
  std::thread serviceWriter([&] {
    for (int i = 0; i < 100000; ++i) box.setService(i % 2);
  });
  std::thread clockWriter([&] {
    for (int i = 0; i < 100000; ++i) box.setClock(true, i % 24);
  });
  std::thread statusWriter([&] {
    for (uint32_t i = 1; i <= 100000; ++i) {
      Engine::Status s;
      s.ready = true;
      s.sequence = i;
      s.pressure = float(i);
      s.sensorVolts = float(i * 2);
      box.publish(s);
    }
  });
  for (int i = 0; i < 100000; ++i) {
    const auto s = box.status();
    if (s.ready) {
      assert(s.pressure == float(s.sequence));
      assert(s.sensorVolts == float(s.sequence * 2));
    }
    const auto cmd = box.commands();
    assert(cmd.hour >= 0 && cmd.hour < 24);
  }
  serviceWriter.join();
  clockWriter.join();
  statusWriter.join();
  const auto cmd = box.commands();
  assert(cmd.disabled && cmd.clockOk && cmd.hour == 99999 % 24);
}

int main() {
  watchdogTests();
  serviceTests();
  mailboxTests();
  std::puts("PASS: watchdog registration/failures/stalls, service handoff and "
            "30-minute cycling, concurrent snapshots, hydro static_asserts");
}
