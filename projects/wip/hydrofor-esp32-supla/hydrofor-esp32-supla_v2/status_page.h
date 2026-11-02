#pragma once
// Live status on the device web page (not a SUPLA channel). Mainly needed for
// calibration: the sensor voltage at 0 bar goes into h_cal_zero_v.
#include <stdio.h>
#include <supla/network/html_element.h>
#include <supla/network/web_sender.h>
#include "engine.h"

class StatusPage : public Supla::HtmlElement {
 public:
  StatusPage() : Supla::HtmlElement(Supla::HTML_SECTION_DEVICE_INFO) {}
  void send(Supla::WebSender *sender) override {
    static const char *const STATES[] = {"start (2 min)", "czeka", "pompuje",
        "przerwa", "BLOKADA", "serwis", "AWARIA CZUJNIKA", "BLEDNE NASTAWY"};
    const Engine::Status s = Engine::mailbox.status();
    const auto &o = s.out;
    char text[200];
    if (!s.ready) {
      snprintf(text, sizeof(text), "Hydrofor: %s",
               s.taskFailed ? "BLAD ZADANIA STERUJACEGO" : "uruchamianie");
    } else {
      snprintf(text, sizeof(text),
               "Hydrofor: %.2f bar, %.0f l | czujnik %.3f V | %s | pompa %s | "
               "zawor: %s | ostatnia porcja %.0f l",
               double(s.pressure), double(o.tankL), double(s.sensorVolts),
               STATES[uint8_t(o.state) < 8 ? uint8_t(o.state) : 0],
               o.pump ? "ON" : "OFF", o.valveCity ? "woda miejska" : "studnia",
               double(o.lastBatchL));
    }
    sender->tag("p").body(text);
  }
};
