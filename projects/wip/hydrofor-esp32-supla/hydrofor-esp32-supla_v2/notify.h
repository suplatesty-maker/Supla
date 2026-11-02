#pragma once
// SUPLA push notifications. Queued from the status sync and retried until the
// server accepts them, so an event during a WiFi outage is not lost.
#include <SuplaDevice.h>
#include <supla/device/notifications.h>

namespace Notify {
enum Event : uint8_t { LOCK, CRITICAL, RETURN, SENSOR, OVER, CONFIG, COUNT };
bool pending[COUNT] = {};
float eventBar[COUNT] = {};

inline void queue(Event e, float bar) {
  pending[e] = true;
  eventBar[e] = bar;
}

inline bool sendOne(uint8_t e, float b) {
  switch (e) {
    case LOCK:
      return Supla::Notification::SendF(-1, "Hydrofor: blokada pompy",
          "Brak wody w studni lub awaria pompy (%.2f bar). Zawor na wode miejska.", b);
    case CRITICAL:
      return Supla::Notification::SendF(-1, "Hydrofor: woda miejska",
          "Cisnienie spadlo do %.2f bar. Zawor przelaczony na wode miejska.", b);
    case RETURN:
      return Supla::Notification::SendF(-1, "Hydrofor: studnia",
          "Zbiorniki pelne (%.2f bar). Powrot na wode ze studni.", b);
    case SENSOR:
      return Supla::Notification::Send(-1, "Hydrofor: awaria czujnika",
          "Pompa zatrzymana, zawor na wode miejska. Sprawdz czujnik i przewody.");
    case OVER:
      return Supla::Notification::SendF(-1, "Hydrofor: nadcisnienie",
          "Cisnienie %.2f bar. Pompa zatrzymana. Sprawdz przekaznik pompy!", b);
    case CONFIG:
      return Supla::Notification::Send(-1, "Hydrofor: bledna konfiguracja",
          "Pompa wylaczona. Popraw nastawy na stronie WWW urzadzenia.");
  }
  return true;
}

// At most one message every 2 s.
inline void send() {
  static uint32_t lastSend = 0;
  if (SuplaDevice.getCurrentStatus() != STATUS_REGISTERED_AND_READY ||
      millis() - lastSend < 2000) return;
  for (uint8_t e = 0; e < COUNT; ++e) {
    if (!pending[e]) continue;
    lastSend = millis();
    if (sendOne(e, eventBar[e])) pending[e] = false;
    return;
  }
}
}  // namespace Notify
