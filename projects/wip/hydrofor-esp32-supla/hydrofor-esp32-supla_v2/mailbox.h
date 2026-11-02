#pragma once
// v2: SUPLA owns the service switch and clock; the control task gets snapshots.
#include <math.h>
#include <freertos/FreeRTOS.h>
#include "hydro_logic.h"

namespace Engine {
struct Commands {
  bool disabled = false, forceCity = false, clockOk = false;
  int hour = 12;
};
struct Status {
  bool ready = false, taskFailed = false, sensorOk = false, clockOk = false;
  float pressure = NAN, sensorVolts = NAN;
  Hydro::Output out;
  uint32_t sequence = 0;
};

class Mailbox {
 public:
  void setService(bool disabled) {
    portENTER_CRITICAL(&mux);
    cmd.disabled = disabled;
    portEXIT_CRITICAL(&mux);
  }
  void setClock(bool ready, int hour) {
    portENTER_CRITICAL(&mux);
    cmd.clockOk = ready;
    cmd.hour = hour;
    portEXIT_CRITICAL(&mux);
  }
  Commands commands() {
    portENTER_CRITICAL(&mux);
    const Commands c = cmd;
    portEXIT_CRITICAL(&mux);
    return c;
  }
  void publish(const Status &s) {
    portENTER_CRITICAL(&mux);
    st = s;
    portEXIT_CRITICAL(&mux);
  }
  Status status() {
    portENTER_CRITICAL(&mux);
    const Status s = st;
    portEXIT_CRITICAL(&mux);
    return s;
  }
  void taskFailed() {
    portENTER_CRITICAL(&mux);
    st.taskFailed = true;
    portEXIT_CRITICAL(&mux);
  }

 private:
  portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
  Commands cmd;
  Status st;
};
Mailbox mailbox;
}  // namespace Engine
