/// @file velux_web.cpp
/// @brief Strona konfiguracji VELUX w panelu WWW SUPLA.

#include "velux_web.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

/// "vpow3" -> prefiks "vpow", indeks 3. Zwraca false, gdy klucz nie pasuje
/// albo indeks jest spoza zakresu.
bool parseIndexedKey(const char *key, const char *prefix, uint8_t &index, uint8_t max_index) {
  const size_t prefix_len = strlen(prefix);
  if (strncmp(key, prefix, prefix_len) != 0) {
    return false;
  }
  const char *rest = key + prefix_len;
  if (*rest == '\0') {
    return false;
  }
  char *end = nullptr;
  const long value = strtol(rest, &end, 10);
  if (end == rest || *end != '\0' || value < 0 || value >= max_index) {
    return false;
  }
  index = static_cast<uint8_t>(value);
  return true;
}

bool parseIndexedKey(const char *key, const char *prefix, uint8_t &index) {
  return parseIndexedKey(key, prefix, index, velux::MAX_SLOTS);
}

/// "slot2" -> 2. Zwraca false dla "(brak)"/śmieci.
bool parseSlotValue(const char *value, uint8_t &slot) {
  const char *rest = nullptr;
  if (strncmp(value, "slot", 4) == 0) {
    rest = value + 4;
  } else {
    return false;
  }
  char *end = nullptr;
  const long parsed = strtol(rest, &end, 10);
  if (end == rest || *end != '\0' || parsed < 0 || parsed >= velux::MAX_SLOTS) {
    return false;
  }
  slot = static_cast<uint8_t>(parsed);
  return true;
}

const char *twoWayActionLabel(velux::VeluxRadio::TwoWayAction action) {
  switch (action) {
    case velux::VeluxRadio::TwoWayAction::OPEN:
      return "Otwórz";
    case velux::VeluxRadio::TwoWayAction::CLOSE:
      return "Zamknij";
    case velux::VeluxRadio::TwoWayAction::STOP:
      return "Stop";
    case velux::VeluxRadio::TwoWayAction::VENT:
      return "Wentylacja (okno)";
    case velux::VeluxRadio::TwoWayAction::POLL:
      return "Odczytaj status";
    default:
      return "";
  }
}

}  // namespace

VeluxHtml::VeluxHtml(velux::VeluxRadio *radio) : Supla::HtmlElement(Supla::HTML_SECTION_FORM), radio_(radio) {}

void VeluxHtml::send(Supla::WebSender *sender) {
  if (radio_ == nullptr || sender == nullptr) {
    return;
  }

  char line[192];

  // --- Stan radia i ostatnie zdarzenie ---
  sender->tag("div").attr("class", "box").body([&]() {
    sender->tag("h3").body("VELUX io-homecontrol (1W + 2W)");

    snprintf(line,
             sizeof(line),
             "Radio: %s%s",
             radio_->ready() ? "SX1276 OK" : "BRAK - sprawdź SPI, piny NSS/RST/DIO0 i zasilanie 3V3",
             radio_->ready() ? "" : " (komendy nie działają)");
    sender->formField([&]() { sender->send(line); });

    char remote[16];
    radio_->lastRemoteText(remote, sizeof(remote));
    snprintf(line,
             sizeof(line),
             "Ostatnie zdarzenie: %s  (ostatni pilot 1W: %s)",
             radio_->lastEvent(),
             remote);
    sender->formField([&]() { sender->send(line); });

    sender->formField([&]() {
      sender->send(
          "Nauka: na pilocie KLI przytrzymaj Gear ~1 s, poczekaj na sekwencję gotowości produktu i wtedy "
          "wybierz \"Nauka\" i zapisz. Klasy 0x30 ustaw wg logu (linie 0x2E po Gear).");
    });
  });

  // --- 2W: klucz instalacji, skan, przypisania ---
  sender->tag("div").attr("class", "box").body([&]() {
    sender->tag("h3").body("2W: klucz instalacji i urządzenia");

    const velux::VeluxRadio::HubCredentials creds = radio_->hubCredentials();
    if (creds.valid) {
      char key_hex[33];
      for (uint8_t b = 0; b < 16; b++) {
        snprintf(key_hex + b * 2, 3, "%02X", creds.system_key[b]);
      }
      snprintf(line, sizeof(line), "Klucz instalacji: node_id %02X%02X%02X, system_key %s", creds.node_id[0],
               creds.node_id[1], creds.node_id[2], key_hex);
    } else {
      snprintf(line, sizeof(line), "Klucz instalacji: BRAK - najpierw ekstrakcja z pilota (KLR 200)");
    }
    sender->formField([&]() { sender->send(line); });

    if (radio_->keyExtractionArmed()) {
      snprintf(line, sizeof(line), "Ekstrakcja: UZBROJONA, stan: %s, zostało ~%u s", radio_->keyExtractionStage(),
               static_cast<unsigned>(radio_->keyExtractionSecondsLeft()));
    } else {
      snprintf(line, sizeof(line), "Ekstrakcja: wyłączona (stan: %s)", radio_->keyExtractionStage());
    }
    sender->formField([&]() { sender->send(line); });

    snprintf(line, sizeof(line), "Ostatnie zdarzenie 2W: %s", radio_->lastTwoWayEvent());
    sender->formField([&]() { sender->send(line); });

    const uint32_t heard_age = radio_->lastHeardTwoWayAgeMs();
    if (heard_age == 0) {
      snprintf(line, sizeof(line), "Ostatnia ramka 2W z powietrza: %s", radio_->lastHeardTwoWay());
    } else {
      snprintf(line, sizeof(line), "Ostatnia ramka 2W z powietrza: %s (%u s temu)", radio_->lastHeardTwoWay(),
               static_cast<unsigned>(heard_age / 1000));
    }
    sender->formField([&]() { sender->send(line); });

    sender->formField([&]() {
      sender->send(
          "Ekstrakcja: uzbrój, a potem na pilocie KLR 200 uruchom dodawanie produktu (tryb parowania) i "
          "poczekaj - klucz pojawi się powyżej i zostanie zapisany w NVS.");
    });

    sender->labeledField("vext", "Klucz instalacji / ekstrakcja", [&]() {
      sender->selectTag("vext", "vext").body([&]() {
        sender->selectOption("", "(brak)", true);
        sender->selectOption("arm", "Uzbrój ekstrakcję klucza (10 min) - potem kreator parowania na pilocie");
        sender->selectOption("disarm", "Wyłącz ekstrakcję");
        sender->selectOption("clear", "Usuń zapisany klucz instalacji");
      });
    });

    sender->labeledField("vscan", "Skan urządzeń (roll-call 0x2A)", [&]() {
      sender->selectTag("vscan", "vscan").body([&]() {
        sender->selectOption("", "(brak)", true);
        sender->selectOption("scan", "Skanuj instalację - wypisz urządzenia (kilka sekund)");
      });
    });

    if (radio_->scanBusy()) {
      sender->formField([&]() { sender->send("Skan w toku..."); });
    } else if (radio_->scanAgeMs() > 0) {
      snprintf(line, sizeof(line), "Ostatni skan: %u s temu, znaleziono %u urządzeń",
               static_cast<unsigned>(radio_->scanAgeMs() / 1000), static_cast<unsigned>(radio_->scanCount()));
      sender->formField([&]() { sender->send(line); });

      for (uint8_t i = 0; i < radio_->scanCount(); i++) {
        const velux::VeluxRadio::ScannedDevice *dev = radio_->scanDevice(i);
        if (dev == nullptr) {
          continue;
        }
        snprintf(line, sizeof(line), "%u) %02X%02X%02X  %s  %s  %d dBm", static_cast<unsigned>(i) + 1, dev->node_id[0],
                 dev->node_id[1], dev->node_id[2], velux::VeluxRadio::deviceTypeName(dev->type),
                 dev->low_power ? "bateria/solar" : "sieciowe", dev->rssi_dbm);
        sender->formField([&]() { sender->send(line); });

        char key[16];
        snprintf(key, sizeof(key), "vas%u", static_cast<unsigned>(i));
        sender->labeledField(key, "Przypisz do kanału", [&]() {
          sender->selectTag(key, key).body([&]() {
            sender->selectOption("", "(nie przypisuj)", true);
            for (uint8_t s = 0; s < radio_->slotCount(); s++) {
              char value[16];
              char label[64];
              snprintf(value, sizeof(value), "slot%u", static_cast<unsigned>(s));
              const velux::SlotConfig *slot_config = radio_->slot(s);
              snprintf(label, sizeof(label), "%u. %s", static_cast<unsigned>(s) + 1,
                       slot_config != nullptr ? slot_config->name : "?");
              sender->selectOption(value, label);
            }
          });
        });
      }
    }
  });

  // --- Po jednym okienku na produkt ---
  for (uint8_t i = 0; i < radio_->slotCount(); i++) {
    const velux::SlotConfig *config = radio_->slot(i);
    if (config == nullptr) {
      continue;
    }
    const velux::EnrollProfile profile = radio_->enrollProfile(i);
    char identity[16];
    radio_->identityText(i, identity, sizeof(identity));

    snprintf(line, sizeof(line), "%u. %s", static_cast<unsigned>(i) + 1, config->name);
    sender->tag("div").attr("class", "box").body([&]() {
      sender->tag("h3").body(line);

      snprintf(line, sizeof(line), "Tożsamość 1W (adres kontrolera): %s", identity);
      sender->formField([&]() { sender->send(line); });

      char device_text[64];
      radio_->slotDeviceText(i, device_text, sizeof(device_text));
      snprintf(line, sizeof(line), "Urządzenie 2W: %s", device_text);
      sender->formField([&]() { sender->send(line); });

      const velux::VeluxRadio::TwoWayStatus status = radio_->slotStatus(i);
      if (status.known) {
        snprintf(line, sizeof(line), "Status 2W: pozycja %u%% %s (%u s temu)", status.position,
                 status.stopped ? "stoi" : "w ruchu", static_cast<unsigned>(status.age_ms / 1000));
      } else {
        snprintf(line, sizeof(line), "Status 2W: brak odczytu");
      }
      sender->formField([&]() { sender->send(line); });

      char key[16];
      snprintf(key, sizeof(key), "vpow%u", static_cast<unsigned>(i));
      sender->labeledField(key, "Odbiornik (preambuła)", [&]() {
        sender->selectTag(key, key).body([&]() {
          sender->selectOption(0, "sieciowy - krótka preambuła (KUX 110 / zasilacz)", !radio_->lowPower(i));
          sender->selectOption(1, "bateria / solar - długa preambuła", radio_->lowPower(i));
        });
      });

      snprintf(key, sizeof(key), "vprof%u", static_cast<unsigned>(i));
      sender->labeledField(key, "Klasy nauki (ramka 0x30)", [&]() {
        sender->selectTag(key, key).body([&]() {
          sender->selectOption(0,
                               "auto (roleta: roller_shutter+awning+dual, okno: window_opener+...)",
                               profile == velux::EnrollProfile::AUTO);
          sender->selectOption(1, "rolety zewnętrzne: roller_shutter, awning, dual_shutter",
                               profile == velux::EnrollProfile::EXTERIOR);
          sender->selectOption(2, "okno dachowe: window_opener, roller_shutter, dual_shutter",
                               profile == velux::EnrollProfile::WINDOW);
          sender->selectOption(3, "rolety wewnętrzne: blind, venetian_blind",
                               profile == velux::EnrollProfile::INTERIOR);
        });
      });

      snprintf(key, sizeof(key), "vact%u", static_cast<unsigned>(i));
      sender->labeledField(key, "Akcja 1W (zapis = wykonanie)", [&]() {
        sender->selectTag(key, key).body([&]() {
          sender->selectOption("", "(brak)", true);
          sender->selectOption("enroll", "Nauka (Enroll) - zamknięcie produktu = sukces");
          sender->selectOption("unenroll", "Wypisz ten kontroler z produktu (niepotwierdzone)");
          sender->selectOption("regen", "Nowa tożsamość 1W (później nauka od nowa)");
        });
      });

      snprintf(key, sizeof(key), "v2a%u", static_cast<unsigned>(i));
      sender->labeledField(key, "Test 2W (zapis = wykonanie)", [&]() {
        sender->selectTag(key, key).body([&]() {
          sender->selectOption("", "(brak)", true);
          sender->selectOption("open", twoWayActionLabel(velux::VeluxRadio::TwoWayAction::OPEN));
          sender->selectOption("close", twoWayActionLabel(velux::VeluxRadio::TwoWayAction::CLOSE));
          sender->selectOption("stop", twoWayActionLabel(velux::VeluxRadio::TwoWayAction::STOP));
          sender->selectOption("vent", twoWayActionLabel(velux::VeluxRadio::TwoWayAction::VENT));
          sender->selectOption("poll", twoWayActionLabel(velux::VeluxRadio::TwoWayAction::POLL));
          sender->selectOption("unassign", "Usuń przypisanie 2W z tego kanału");
        });
      });
    });
  }
}

bool VeluxHtml::handleResponse(const char *key, const char *value) {
  if (radio_ == nullptr || key == nullptr || value == nullptr) {
    return false;
  }

  uint8_t index = 0;
  if (parseIndexedKey(key, "vpow", index)) {
    radio_->setLowPower(index, atoi(value) != 0);
    return true;
  }
  if (parseIndexedKey(key, "vprof", index)) {
    radio_->setEnrollProfile(index, static_cast<velux::EnrollProfile>(atoi(value)));
    return true;
  }
  if (parseIndexedKey(key, "vact", index)) {
    if (strcmp(value, "enroll") == 0) {
      radio_->queuePending(index, velux::VeluxRadio::Pending::ENROLL);
    } else if (strcmp(value, "unenroll") == 0) {
      radio_->queuePending(index, velux::VeluxRadio::Pending::UNENROLL);
    } else if (strcmp(value, "regen") == 0) {
      radio_->queuePending(index, velux::VeluxRadio::Pending::REGENERATE);
    }
    return true;
  }

  // --- 2W ---
  if (strcmp(key, "vext") == 0) {
    if (strcmp(value, "arm") == 0) {
      radio_->setKeyExtractionArmed(true);
    } else if (strcmp(value, "disarm") == 0) {
      radio_->setKeyExtractionArmed(false);
    } else if (strcmp(value, "clear") == 0) {
      radio_->clearHubCredentials();
    }
    return true;
  }
  if (strcmp(key, "vscan") == 0) {
    if (strcmp(value, "scan") == 0) {
      radio_->queueScan();
    }
    return true;
  }
  uint8_t scan_index = 0;
  if (parseIndexedKey(key, "vas", scan_index, velux::VeluxRadio::MAX_SCAN_DEVICES)) {
    uint8_t slot = 0;
    if (parseSlotValue(value, slot)) {
      radio_->queueAssign(slot, scan_index);
    }
    return true;
  }
  if (parseIndexedKey(key, "v2a", index)) {
    if (strcmp(value, "open") == 0) {
      radio_->queueTwoWay(index, velux::VeluxRadio::TwoWayAction::OPEN);
    } else if (strcmp(value, "close") == 0) {
      radio_->queueTwoWay(index, velux::VeluxRadio::TwoWayAction::CLOSE);
    } else if (strcmp(value, "stop") == 0) {
      radio_->queueTwoWay(index, velux::VeluxRadio::TwoWayAction::STOP);
    } else if (strcmp(value, "vent") == 0) {
      radio_->queueTwoWay(index, velux::VeluxRadio::TwoWayAction::VENT);
    } else if (strcmp(value, "poll") == 0) {
      radio_->queueTwoWay(index, velux::VeluxRadio::TwoWayAction::POLL);
    } else if (strcmp(value, "unassign") == 0) {
      radio_->queueUnassign(index);
    }
    return true;
  }
  return false;
}
