/// @file velux_radio.cpp
/// @brief Implementacja warstwy radia io-homecontrol 1W (SX1276) dla SUPLA.
///
/// To jedyny plik projektu, który widzi home_io_control (hioc) oraz SPI.
/// Nie dołącza nagłówków SuplaDevice - po stronie SUPLA obowiązuje interfejs
/// z velux_radio.h (szczegóły w komentarzu w tym nagłówku).

#include "velux_radio.h"

#include <Arduino.h>
#include <Preferences.h>
#include <SPI.h>
#include <esp_system.h>

#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include "hioc/device_registry.h"
#include "hioc/esphome/core/log.h"
#include "hioc/exchange_engine.h"
#include "hioc/hub_exchange.h"
#include "hioc/key_extraction_responder.h"
#include "hioc/oneway_controller.h"
#include "hioc/oneway_transmitter.h"
#include "hioc/pairing_responder.h"
#include "hioc/proto_codecs.h"
#include "hioc/proto_commands.h"
#include "hioc/proto_constants.h"
#include "hioc/proto_device_model.h"
#include "hioc/proto_frame.h"
#include "hioc/proto_sizes.h"
#include "hioc/proto_timing.h"
#include "hioc/radio_interface.h"
#include "hioc/radio_sx1276.h"
#include "hioc/tuning_config.h"

namespace hio = esphome::home_io_control;

namespace {

constexpr const char *const TAG = "veluxio";

/// 8 MHz to sprawdzona wartość dla SX127x na ESP32 (z zapasem do 10 MHz).
constexpr uint32_t SPI_FREQ_HZ = 8000000;

/// Osobna przestrzeń NVS: przeżywa "reset do ustawień fabrycznych" SUPLA
/// (ten czyści tylko konfigurację Supla), więc tożsamości 1W, klucz instalacji
/// 2W i przypisania urządzeń nie znikają po resecie z panelu.
constexpr const char *const NVS_NAMESPACE = "veluxid";

constexpr uint8_t STORE_MAGIC = 0xA5;
constexpr const char *const NVS_KEY_HUB = "hub";

/// Okno deduplikacji ramek z pilota (pilot powtarza komendę 4x co ~40 ms).
constexpr uint32_t RX_DEDUP_WINDOW_MS = 500;

/// Sentinel "brak komendy w kolejce".
constexpr uint8_t PENDING_SLOT_NONE = 0xFF;

/// Kanały, po kolei, na których leci roll-call (CH2 najpierw - kanał TX
/// protokołu, więc największa szansa na odpowiedź w pierwszej próbie).
constexpr uint32_t SCAN_CHANNELS[3] = {hio::FREQ_CH2, hio::FREQ_CH1, hio::FREQ_CH3};

/// Dopytywanie statusu po komendzie (0x04 z ack-a bywa nieaktualne).
constexpr uint32_t POLL_AFTER_COMMAND_MS = 4000;
/// Okresowe odświeżanie statusu produktów (0x03 -> 0x04).
constexpr uint32_t POLL_PERIOD_MS = 120000;

struct StoredIdentity {
  uint8_t magic;
  uint8_t node_id[hio::NODE_ID_SIZE];
  uint8_t key[hio::AES_KEY_SIZE];
};

struct StoredSlot {
  uint8_t magic;
  uint8_t low_power;
  uint8_t profile;
  // 2W: urządzenie przypisane do slotu (has_device == 0 -> slot czysto 1W).
  uint8_t has_device;
  uint8_t node_id[hio::NODE_ID_SIZE];
  uint8_t type;
  uint8_t subtype;
  uint8_t inverted;
};

/// Klucz instalacji odzyskany z pilota (rola urządzenia w parowaniu 2W).
struct StoredHub {
  uint8_t magic;
  uint8_t node_id[hio::NODE_ID_SIZE];
  uint8_t key[hio::AES_KEY_SIZE];
};

Preferences &nvs() {
  static Preferences prefs;
  static bool opened = false;
  if (!opened) {
    opened = prefs.begin(NVS_NAMESPACE, false);
  }
  return prefs;
}

void slotStoreKey(const char *prefix, uint8_t index, char (&out)[12]) {
  snprintf(out, sizeof(out), "%s%u", prefix, static_cast<unsigned>(index));
}

bool loadIdentity(uint8_t index, StoredIdentity &out) {
  char key[12];
  slotStoreKey("id", index, key);
  Preferences &prefs = nvs();
  if (!prefs.isKey(key) || prefs.getBytesLength(key) != sizeof(StoredIdentity)) {
    return false;
  }
  if (prefs.getBytes(key, &out, sizeof(StoredIdentity)) != sizeof(StoredIdentity)) {
    return false;
  }
  return out.magic == STORE_MAGIC;
}

void saveIdentity(uint8_t index, const StoredIdentity &in) {
  char key[12];
  slotStoreKey("id", index, key);
  nvs().putBytes(key, &in, sizeof(StoredIdentity));
}

/// Wczytanie konfiguracji slotu. Toleruje starszy, krótszy zapis (bez pól
/// 2W): brakujące bajty zostają wyzerowane, więc nowe pola mają sensowne
/// wartości domyślne ("brak urządzenia").
bool loadSlotConfig(uint8_t index, StoredSlot &out) {
  char key[12];
  slotStoreKey("cfg", index, key);
  Preferences &prefs = nvs();
  memset(&out, 0, sizeof(out));
  if (!prefs.isKey(key)) {
    return false;
  }
  size_t available = prefs.getBytesLength(key);
  if (available == 0) {
    return false;
  }
  if (available > sizeof(out)) {
    available = sizeof(out);
  }
  if (prefs.getBytes(key, &out, available) != available) {
    return false;
  }
  return out.magic == STORE_MAGIC;
}

void saveSlotConfig(uint8_t index, const StoredSlot &in) {
  char key[12];
  slotStoreKey("cfg", index, key);
  nvs().putBytes(key, &in, sizeof(StoredSlot));
}

bool loadHub(StoredHub &out) {
  Preferences &prefs = nvs();
  memset(&out, 0, sizeof(out));
  if (!prefs.isKey(NVS_KEY_HUB)) {
    return false;
  }
  size_t available = prefs.getBytesLength(NVS_KEY_HUB);
  if (available > sizeof(out)) {
    available = sizeof(out);
  }
  if (prefs.getBytes(NVS_KEY_HUB, &out, available) != available) {
    return false;
  }
  return out.magic == STORE_MAGIC;
}

void saveHub(const StoredHub &in) { nvs().putBytes(NVS_KEY_HUB, &in, sizeof(StoredHub)); }

void clearHub() { nvs().remove(NVS_KEY_HUB); }

/// Wypełnia bufor losowym adresem węzła (3 B) - nigdy nie może to być
/// 00 00 00 ani FF FF FF (te wzorce są traktowane jako "brak adresu").
void randomNodeId(uint8_t out[hio::NODE_ID_SIZE]) {
  do {
    esp_fill_random(out, hio::NODE_ID_SIZE);
  } while (!hio::stored_node_id_is_valid(out));
}

/// Access do SPI wymagany przez sterownik radia hioc.
/// NSS (CS) jest sterowany ręcznie - SX1276 nie ma sprzętowego CS w tym
/// sterowniku, a SPI.begin() dostaje -1 jako pin SS, żeby rdzeń ESP32
/// nie przejął naszego CS-a.
class Esp32SpiAccess : public hio::SpiAccess {
 public:
  explicit Esp32SpiAccess(const velux::Pins &pins)
      : cs_(pins.nss), settings_(SPI_FREQ_HZ, MSBFIRST, SPI_MODE0) {
    pinMode(cs_, OUTPUT);
    digitalWrite(cs_, HIGH);
    SPI.begin(pins.sck, pins.miso, pins.mosi, -1);
  }

  void spi_enable() override {
    SPI.beginTransaction(settings_);
    digitalWrite(cs_, LOW);
  }

  void spi_disable() override {
    digitalWrite(cs_, HIGH);
    SPI.endTransaction();
  }

  uint8_t spi_transfer(uint8_t data) override { return SPI.transfer(data); }
  void spi_write(uint8_t data) override { SPI.transfer(data); }
  uint8_t spi_read() override { return SPI.transfer(0); }

 private:
  uint8_t cs_;
  SPISettings settings_;
};

}  // namespace

// ============================================================================
// Impl - cała logika po stronie hioc
// ============================================================================

struct velux::VeluxRadio::Impl {
  velux::Pins pins{};
  uint8_t tx_power = 14;
  bool ready = false;

  uint8_t slot_count = 0;
  const velux::SlotConfig *slots = nullptr;

  Esp32SpiAccess *spi = nullptr;
  esphome::InternalGPIOPin *rst = nullptr;
  esphome::InternalGPIOPin *dio0 = nullptr;
  hio::RadioSX1276 *radio = nullptr;
  hio::TuningConfig tuning{};
  hio::OneWayTransmitter *tx = nullptr;

  StoredSlot slot_cfg[velux::MAX_SLOTS]{};
  std::string identity_ids[velux::MAX_SLOTS];

  char last_event[128] = {0};
  uint32_t last_event_ms = 0;
  char last_remote[16] = {0};
  uint32_t last_remote_ms = 0;

  volatile bool need_rebuild = false;

  // Deduplikacja powtarzanych ramek pilota (4 kopie na jedno naciśnięcie).
  uint8_t rx_src[hio::NODE_ID_SIZE]{};
  uint8_t rx_dst[hio::NODE_ID_SIZE]{};
  uint8_t rx_cmd = 0;
  uint8_t rx_main0 = 0;
  uint8_t rx_main1 = 0;
  bool rx_has_intent = false;
  uint32_t rx_seen_ms = 0;

  // --------------------------------------------------------------------
  // 2W: io-homecontrol dwukierunkowy
  // --------------------------------------------------------------------

  /// Klucz instalacji (odzyskany z pilota przez ekstrakcję). ExchangeEngine
  /// trzyma wskaźniki na te tablice, więc podmiana zawartości wystarcza.
  uint8_t hub_node_id[hio::NODE_ID_SIZE]{};
  uint8_t hub_key[hio::AES_KEY_SIZE]{};
  bool hub_key_valid = false;

  /// To samo co `radio`, ale jako typ bazowy - tyle wymaga ExchangeEngine.
  hio::RadioDriver *driver = nullptr;
  hio::ExchangeEngine *engine = nullptr;
  hio::DeviceRegistry registry{};
  hio::KeyExtractionResponder *key_resp = nullptr;

  /// Czy trzymamy kanał CH2 na czas wymiany (ekstrakcja klucza).
  bool holding_ch2 = false;

  /// Mini-scheduler (replace-on-same-name) na potrzeby respondera i dopytań.
  struct Timer {
    char name[20] = {0};
    uint32_t deadline_ms = 0;
    std::function<void()> fn;
    bool active = false;
  };
  Timer timers[16]{};  // 2 nazwy respondera + 1 dopytanie na slot (do 12)

  /// Jedna komenda z panelu WWW (web = inny task niż pętla główna).
  struct PendingOp {
    uint8_t kind = 0;
    uint8_t slot = PENDING_SLOT_NONE;
    uint8_t action = 0;
    uint8_t param = 0;
  };
  PendingOp pending_op{};
  volatile bool pending_op_valid = false;

  bool scan_busy = false;
  uint32_t scan_last_ms = 0;
  velux::VeluxRadio::ScannedDevice scan_devs[velux::VeluxRadio::MAX_SCAN_DEVICES]{};
  uint8_t scan_dev_count = 0;

  velux::VeluxRadio::TwoWayStatus status[velux::MAX_SLOTS]{};
  uint32_t status_ms[velux::MAX_SLOTS]{};
  uint32_t last_poll_ms[velux::MAX_SLOTS]{};
  /// Minimalny odstęp między wymianami statusu: każda wymiana blokuje pętlę
  /// (i obsługę SUPLA) na 1-3 s, więc przy wielu kanałach nie mogą iść jedna
  /// po drugiej bez przerwy.
  static constexpr uint32_t STATUS_POLL_MIN_GAP_MS = 3000;
  uint32_t last_status_poll_ms = 0;
  velux::VeluxRadio::StatusListener status_listener;
  velux::VeluxRadio::KeyExtractedListener key_listener;

  char last_2w_event[160] = {0};
  uint32_t last_2w_event_ms = 0;

  /// Ostatnia ramka 2W usłyszana z powietrza (dowolna, niekoniecznie do nas) -
  /// to jest test "czy w ogóle słyszymy instalację" przy uruchamianiu.
  char last_2w_heard[80] = {0};
  uint32_t last_2w_heard_ms = 0;

  void noteTwoWayHeard(const hio::IoFrame &frame) {
    snprintf(this->last_2w_heard, sizeof(this->last_2w_heard), "%s (0x%02X) od %s", hio::command_name(frame.cmd),
             frame.cmd, hio::node_id_to_string(frame.src).c_str());
    this->last_2w_heard_ms = millis();
    ESP_LOGD(TAG, "RX 2W: %s", this->last_2w_heard);
  }

  /// Kiedy uzbrojono ekstrakcję klucza (do licznika w panelu WWW).
  uint32_t key_extraction_arm_ms = 0;

  /// Kolejka komend z panelu WWW: pola ustawiane przed flagą, flaga na końcu.
  void queue(uint8_t kind, uint8_t slot = PENDING_SLOT_NONE, uint8_t action = 0, uint8_t param = 0) {
    if (this->pending_op_valid) {
      ESP_LOGW(TAG, "kolejka zajęta - nadpisuję poprzednią komendę");
    }
    this->pending_op.kind = kind;
    this->pending_op.slot = slot;
    this->pending_op.action = action;
    this->pending_op.param = param;
    this->pending_op_valid = true;
  }

  void storeTwoWayEvent(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(this->last_2w_event, sizeof(this->last_2w_event), format, args);
    va_end(args);
    this->last_2w_event_ms = millis();
  }

  /// ESPHome'owy set_timeout(): ten sam `name` zastępuje poprzedni termin.
  void scheduleNamed(const char *name, uint32_t delay_ms, std::function<void()> fn) {
    for (Timer &t : this->timers) {
      if (t.active && strcmp(t.name, name) == 0) {
        t.deadline_ms = millis() + delay_ms;
        t.fn = std::move(fn);
        return;
      }
    }
    for (Timer &t : this->timers) {
      if (!t.active) {
        snprintf(t.name, sizeof(t.name), "%s", name);
        t.deadline_ms = millis() + delay_ms;
        t.fn = std::move(fn);
        t.active = true;
        return;
      }
    }
    ESP_LOGW(TAG, "brak wolnego timera dla %s", name);
  }

  void runTimers() {
    const uint32_t now = millis();
    for (Timer &t : this->timers) {
      if (!t.active || static_cast<int32_t>(now - t.deadline_ms) < 0) {
        continue;
      }
      t.active = false;
      std::function<void()> fn = std::move(t.fn);
      t.fn = nullptr;
      if (fn) {
        fn();
      }
    }
  }

  velux::VeluxRadio::HubCredentials credentials() const {
    velux::VeluxRadio::HubCredentials out;
    out.valid = this->hub_key_valid;
    memcpy(out.node_id, this->hub_node_id, hio::NODE_ID_SIZE);
    memcpy(out.system_key, this->hub_key, hio::AES_KEY_SIZE);
    return out;
  }

  void saveCredentials() {
    StoredHub stored{};
    stored.magic = STORE_MAGIC;
    memcpy(stored.node_id, this->hub_node_id, hio::NODE_ID_SIZE);
    memcpy(stored.key, this->hub_key, hio::AES_KEY_SIZE);
    saveHub(stored);
  }

  void clearCredentials() {
    memset(this->hub_node_id, 0, hio::NODE_ID_SIZE);
    memset(this->hub_key, 0, hio::AES_KEY_SIZE);
    this->hub_key_valid = false;
    clearHub();
    this->storeTwoWayEvent("klucz instalacji usunięty");
    ESP_LOGW(TAG, "klucz instalacji usunięty z NVS");
  }

  /// Po każdej ramce ekstrakcji: jeśli responder odebrał właśnie klucz,
  /// przejmujemy go (node_id + system_key) i zapisujemy w NVS.
  void adoptCredentialsIfReady() {
    if (this->key_resp == nullptr || this->hub_key_valid) {
      return;
    }
    const hio::pairing_responder::ResponderContext &ctx = this->key_resp->key_extraction_ctx_;
    if (ctx.state != hio::pairing_responder::ResponderState::EXTRACTED &&
        ctx.state != hio::pairing_responder::ResponderState::SENT_NODE_VERIFY_RESP) {
      return;
    }
    memcpy(this->hub_node_id, ctx.hub_node_id, hio::NODE_ID_SIZE);
    memcpy(this->hub_key, ctx.recovered_key, hio::AES_KEY_SIZE);
    this->hub_key_valid = true;
    this->saveCredentials();
    ESP_LOGI(TAG, "EKSTRAKCJA OK: node_id=%02X%02X%02X, klucz instalacji zapisany w NVS", this->hub_node_id[0],
             this->hub_node_id[1], this->hub_node_id[2]);
    this->storeTwoWayEvent("klucz instalacji odebrany (node_id %02X%02X%02X)", this->hub_node_id[0],
                           this->hub_node_id[1], this->hub_node_id[2]);
    if (this->key_listener) {
      this->key_listener(this->credentials());
    }
  }

  // --- Skan instalacji (roll-call 0x2A -> 0x2B) -----------------------

  void noteScanReply(const hio::IoFrame &frame, const hio::ExchangeEngine::BroadcastReplyInfo &info) {
    for (uint8_t i = 0; i < this->scan_dev_count; i++) {
      if (memcmp(this->scan_devs[i].node_id, frame.src, hio::NODE_ID_SIZE) == 0) {
        return;  // to samo urządzenie odpowiedziało drugi raz
      }
    }
    if (this->scan_dev_count >= velux::VeluxRadio::MAX_SCAN_DEVICES) {
      return;
    }
    hio::IoDevice device{};
    std::string unused_id;
    const hio::DiscoveryResponseInfo decoded = hio::decode_discovery_response(frame, device, unused_id);
    velux::VeluxRadio::ScannedDevice &entry = this->scan_devs[this->scan_dev_count++];
    memcpy(entry.node_id, frame.src, hio::NODE_ID_SIZE);
    entry.type = static_cast<uint8_t>(device.type);
    entry.subtype = device.subtype;
    entry.inverted = device.inverted;
    entry.metadata_complete = decoded.metadata_complete;
    entry.low_power = decoded.has_extended && hio::discovery_power_save_mode(decoded.flags) == hio::POWER_SAVE_LOW_POWER;
    entry.rssi_dbm = info.rssi_dbm;
    ESP_LOGI(TAG, "skan: %s typ=%s(%u) podtyp=%u zasilanie=%s rssi=%d dBm",
             hio::node_id_to_string(frame.src).c_str(), hio::device_type_name(device.type), entry.type, entry.subtype,
             entry.low_power ? "bateria" : "sieć", info.rssi_dbm);
  }

  /// Skan idzie krokami (jedna próba na obieg loop()), bo jedno okno
  /// nasłuchu to ~2 s - blokowanie pętli na 6 prób zerwałoby łączność SUPLA.
  uint8_t scan_attempt = 0;    // 0 = nieaktywny, 1..6 = numer bieżącej próby
  uint32_t scan_next_ms = 0;

  static constexpr uint8_t SCAN_ATTEMPTS = 6;

  void startScan() {
    this->scan_dev_count = 0;
    this->scan_last_ms = millis();
    if (this->engine == nullptr || !this->hub_key_valid) {
      this->storeTwoWayEvent("skan niemożliwy: brak klucza instalacji");
      ESP_LOGW(TAG, "skan: najpierw ekstrakcja klucza z pilota (KLR 200)");
      return;
    }
    this->scan_attempt = 1;
    this->scan_next_ms = millis();
    this->scan_busy = true;
    ESP_LOGI(TAG, "skan: start (%u prób: 3 x urządzenia bateryjne, 3 x stale zasilane)", SCAN_ATTEMPTS);
  }

  /// Jedna próba roll-call: budowa ramki 0x2A + okno nasłuchu na 0x2B.
  void scanStep() {
    if (!this->scan_busy) {
      return;
    }
    const uint32_t now = millis();
    if (static_cast<int32_t>(now - this->scan_next_ms) < 0) {
      return;
    }
    if (this->scan_attempt > SCAN_ATTEMPTS) {
      this->scan_busy = false;
      this->scan_last_ms = millis();
      ESP_LOGI(TAG, "skan gotowy: %u urządzeń", this->scan_dev_count);
      this->storeTwoWayEvent("skan gotowy: %u urządzeń", this->scan_dev_count);
      return;
    }

    const uint8_t attempt = this->scan_attempt++;
    // Próby 1-3: urządzenia bateryjne (długa preambuła, nasłuch na wszystkich
    // kanałach - ich odpowiedzi potrafią wrócić na kanał zapytania).
    // Próby 4-6: stale zasilane (krótka preambuła, z pominięciem kanału pytania).
    const bool low_power = attempt <= 3;
    const uint32_t freq = SCAN_CHANNELS[(attempt - 1) % 3];
    const hio::ListenPolicy policy =
        low_power ? hio::ListenPolicy::ROTATE_ALL_CHANNELS : hio::ListenPolicy::ROTATE_SKIPPING_REQUEST;

    hio::IoFrame request;
    if (!hio::create_discovery_request(request, this->hub_node_id, hio::CMD_DISCOVER_SPE_REQ, hio::BROADCAST_DISCOVER,
                                       low_power, low_power, false, 0, this->hub_key)) {
      ESP_LOGW(TAG, "skan: próba %u - nie udało się zbudować ramki", attempt);
      this->scan_next_ms = millis() + 50;
      return;
    }
    ESP_LOGI(TAG, "skan: próba %u/%u (%s, %u Hz)", attempt, SCAN_ATTEMPTS, low_power ? "bateria" : "sieć",
             static_cast<unsigned>(freq));
    const uint8_t heard = this->engine->collect_broadcast_responses(
        request, freq, hio::CMD_DISCOVER_SPE_RESP, this->tuning.pairing_discovery_wait_ms,
        [this](const hio::IoFrame &frame, const hio::ExchangeEngine::BroadcastReplyInfo &info) {
          this->noteScanReply(frame, info);
        },
        policy);
    ESP_LOGI(TAG, "skan: próba %u - usłyszano %u, urządzeń łącznie %u", attempt, heard, this->scan_dev_count);
    this->scan_next_ms = millis() + 100;  // oddech dla SUPLA między próbami
  }

  // --- Przypisania slot <-> urządzenie -------------------------------

  void assignDevice(uint8_t slot, uint8_t scan_index) {
    if (scan_index >= this->scan_dev_count) {
      this->storeTwoWayEvent("slot %u: brak takiego wyniku skanu", slot + 1);
      return;
    }
    const velux::VeluxRadio::ScannedDevice &dev = this->scan_devs[scan_index];
    StoredSlot &cfg = this->slot_cfg[slot];
    cfg.has_device = 1;
    memcpy(cfg.node_id, dev.node_id, hio::NODE_ID_SIZE);
    cfg.type = dev.type;
    cfg.subtype = dev.subtype;
    cfg.inverted = dev.inverted ? 1 : 0;
    cfg.low_power = dev.low_power ? 1 : 0;
    saveSlotConfig(slot, cfg);
    this->need_rebuild = true;
    ESP_LOGI(TAG, "slot %u <- %s (%s, %s)", slot + 1, hio::node_id_to_string(dev.node_id).c_str(),
             hio::device_type_name(static_cast<hio::DeviceType>(dev.type)), dev.low_power ? "bateria" : "sieć");
    this->storeTwoWayEvent("slot %u: przypisano %s", slot + 1, hio::node_id_to_string(dev.node_id).c_str());
  }

  void unassignDevice(uint8_t slot) {
    StoredSlot &cfg = this->slot_cfg[slot];
    cfg.has_device = 0;
    memset(cfg.node_id, 0, hio::NODE_ID_SIZE);
    cfg.type = 0;
    cfg.subtype = 0;
    cfg.inverted = 0;
    saveSlotConfig(slot, cfg);
    this->status[slot] = velux::VeluxRadio::TwoWayStatus{};
    this->need_rebuild = true;
    this->storeTwoWayEvent("slot %u: usunięto przypisanie 2W", slot + 1);
  }

  // --- Status i komendy 2W -------------------------------------------

  void updateSlotStatus(uint8_t slot, float position_percent, bool stopped) {
    velux::VeluxRadio::TwoWayStatus &st = this->status[slot];
    int pos = static_cast<int>(position_percent + 0.5F);
    if (pos < 0) {
      pos = 0;
    }
    if (pos > 100) {
      pos = 100;
    }
    st.known = true;
    st.position = static_cast<uint8_t>(pos);
    st.stopped = stopped;
    this->status_ms[slot] = millis();
    ESP_LOGI(TAG, "slot %u: status pozycja=%d%% %s", slot + 1, pos, stopped ? "(stoi)" : "(w ruchu)");
    this->storeTwoWayEvent("slot %u: pozycja %d%%", slot + 1, pos);
    if (this->status_listener) {
      this->status_listener(slot, st);
    }
  }

  /// Ramka 0x04 (odpowiedź na 0x00/0x03) albo 0x71 (status nieusolicytowany).
  /// `trust_position` = false dla ack-a komendy: bywa nieaktualny (hioc).
  void applyStatusFrame(uint8_t slot, const hio::IoFrame &frame, bool trust_position) {
    uint8_t target_offset = 0;
    uint8_t current_offset = 0;
    if (frame.cmd == hio::CMD_PRIVATE_RESP && frame.data_len >= 6) {
      if (!trust_position) {
        return;
      }
      target_offset = 2;
      current_offset = 4;
    } else if (frame.cmd == hio::CMD_STATUS_UPDATE && frame.data_len >= 11) {
      target_offset = 5;
      current_offset = 7;
    } else {
      return;
    }
    const uint16_t target_raw = static_cast<uint16_t>((frame.data[target_offset] << 8) | frame.data[target_offset + 1]);
    const uint16_t current_raw =
        static_cast<uint16_t>((frame.data[current_offset] << 8) | frame.data[current_offset + 1]);
    const bool stopped = (frame.data[0] & hio::STATUS_STOPPED) != 0;
    float target = 0;
    float position = 0;
    hio::decode_position_report(target_raw, current_raw, stopped, target, position);
    if (position >= 0) {
      this->updateSlotStatus(slot, position, stopped);
    }
  }

  static const char *actionName(velux::VeluxRadio::TwoWayAction action) {
    switch (action) {
      case velux::VeluxRadio::TwoWayAction::OPEN:
        return "OTWÓRZ";
      case velux::VeluxRadio::TwoWayAction::CLOSE:
        return "ZAMKNIJ";
      case velux::VeluxRadio::TwoWayAction::STOP:
        return "STOP";
      case velux::VeluxRadio::TwoWayAction::POSITION:
        return "POZYCJA";
      case velux::VeluxRadio::TwoWayAction::VENT:
        return "WENTYLACJA";
      case velux::VeluxRadio::TwoWayAction::POLL:
        return "STATUS";
      default:
        return "-";
    }
  }

  /// Wykonuje uwierzytelnioną wymianę 2W i (dla 0x03) dekoduje pozycję.
  bool runTwoWay(uint8_t slot, velux::VeluxRadio::TwoWayAction action, uint8_t position) {
    StoredSlot &cfg = this->slot_cfg[slot];
    if (this->engine == nullptr || !this->hub_key_valid) {
      this->storeTwoWayEvent("slot %u: brak klucza instalacji", slot + 1);
      return false;
    }
    if (cfg.has_device == 0) {
      this->storeTwoWayEvent("slot %u: brak urządzenia 2W (przypisz ze skanu)", slot + 1);
      return false;
    }

    const bool low_power = cfg.low_power != 0;
    hio::IoFrame request;
    bool built = false;
    switch (action) {
      case velux::VeluxRadio::TwoWayAction::OPEN:
        built = hio::create_execute_position(request, this->hub_node_id, cfg.node_id, low_power, 0, false);
        break;
      case velux::VeluxRadio::TwoWayAction::CLOSE:
        built = hio::create_execute_position(request, this->hub_node_id, cfg.node_id, low_power, 100, false);
        break;
      case velux::VeluxRadio::TwoWayAction::POSITION:
        built = hio::create_execute_position(request, this->hub_node_id, cfg.node_id, low_power,
                                             position > 100 ? 100 : position, false);
        break;
      case velux::VeluxRadio::TwoWayAction::STOP:
        built = hio::create_execute_command(request, this->hub_node_id, cfg.node_id, low_power,
                                            hio::CoverCommand::STOP, false);
        break;
      case velux::VeluxRadio::TwoWayAction::VENT:
        built = hio::create_execute_command(request, this->hub_node_id, cfg.node_id, low_power,
                                            hio::CoverCommand::VENT, false);
        break;
      case velux::VeluxRadio::TwoWayAction::POLL:
        built = hio::create_get_status(request, this->hub_node_id, cfg.node_id, low_power);
        break;
      default:
        return false;
    }
    if (!built) {
      this->storeTwoWayEvent("slot %u: nie udało się zbudować ramki", slot + 1);
      return false;
    }

    hio::IoFrame response;
    const hio::ExchangeOutcome outcome =
        this->engine->send_and_receive(request, response, hio::FREQ_CH2, hio::EXCHANGE_RETRY_COUNT);

    bool accepted = false;
    if (outcome == hio::ExchangeOutcome::SUCCESS_WITH_RESPONSE) {
      accepted = true;
      this->applyStatusFrame(slot, response, action == velux::VeluxRadio::TwoWayAction::POLL);
    } else if (outcome == hio::ExchangeOutcome::SUCCESS_UNCONFIRMED) {
      // Urządzenie przyjęło komendę (przeszło challenge), ale milczy -
      // typowe dla VELUX; status uzupełni dopytanie.
      accepted = true;
      ESP_LOGI(TAG, "slot %u: %s przyjęte bez odpowiedzi", slot + 1, actionName(action));
    }

    this->last_poll_ms[slot] = millis();
    this->last_status_poll_ms = this->last_poll_ms[slot];
    if (accepted) {
      ESP_LOGI(TAG, "slot %u: %s wysłane", slot + 1, actionName(action));
      this->storeTwoWayEvent("slot %u: %s wysłane", slot + 1, actionName(action));
    } else {
      ESP_LOGW(TAG, "slot %u: %s - brak odpowiedzi urządzenia", slot + 1, actionName(action));
      this->storeTwoWayEvent("slot %u: %s - brak odpowiedzi", slot + 1, actionName(action));
    }

    // Po komendzie dopytywanie o realny status (0x04 z ack-a bywa stary).
    if (action != velux::VeluxRadio::TwoWayAction::POLL) {
      char timer_name[20];
      snprintf(timer_name, sizeof(timer_name), "velux-poll%u", slot);
      this->scheduleNamed(timer_name, accepted ? POLL_AFTER_COMMAND_MS : 15000,
                          [this, slot]() { this->runTwoWay(slot, velux::VeluxRadio::TwoWayAction::POLL, 0); });
    }
    return accepted;
  }

  /// Ramki 2W odebrane poza wymianą: ekstrakcja klucza i nieusolicytowany
  /// status (0x71) wymagający uwierzytelnienia po naszej stronie.
  void handleTwoWayRx(const hio::IoFrame &frame, const hio::RadioRxPacket &packet) {
    this->noteTwoWayHeard(frame);
    if (this->key_resp != nullptr &&
        this->key_resp->key_extraction_ctx_.state != hio::pairing_responder::ResponderState::DISARMED) {
      if (this->key_resp->try_handle_frame(frame)) {
        ESP_LOGD(TAG, "ekstrakcja: ramka 0x%02X obsłużona (stan: %s)", frame.cmd,
                 hio::pairing_responder::responder_stage_name(this->key_resp->key_extraction_ctx_.state));
        this->adoptCredentialsIfReady();
        return;
      }
    }
    if (this->engine == nullptr || !this->hub_key_valid) {
      return;
    }
    if (frame.cmd == hio::CMD_STATUS_UPDATE && memcmp(frame.dst, this->hub_node_id, hio::NODE_ID_SIZE) == 0) {
      if (!this->engine->authenticate_request(frame, packet.freq_hz)) {
        ESP_LOGW(TAG, "RX 2W: status bez poprawnego uwierzytelnienia");
        return;
      }
      const std::string src = hio::node_id_to_string(frame.src);
      for (uint8_t i = 0; i < this->slot_count; i++) {
        if (this->slot_cfg[i].has_device != 0 && memcmp(this->slot_cfg[i].node_id, frame.src, hio::NODE_ID_SIZE) == 0) {
          this->applyStatusFrame(i, frame, true);
          break;
        }
      }
      hio::IoFrame ack;
      if (hio::create_status_update_resp(ack, this->hub_node_id, frame.src)) {
        const uint16_t preamble = this->radio->response_preamble();
        for (const uint32_t freq : SCAN_CHANNELS) {
          this->transmit(ack, freq, preamble);
        }
      }
      ESP_LOGI(TAG, "RX 2W: status od %s (potwierdzony)", src.c_str());
      this->storeTwoWayEvent("status od %s", src.c_str());
    }
  }

  /// Odświeżanie statusu: jedno urządzenie na obieg pętli, nie częściej niż
  /// POLL_PERIOD_MS. Wymiana blokuje pętlę na ~0,5-2 s, więc przy wielu
  /// kanałach wymiany rozkładamy w czasie (STATUS_POLL_MIN_GAP_MS), żeby
  /// SUPLA nie traciła łączności.
  void pollStatuses() {
    if (this->engine == nullptr || !this->hub_key_valid || this->scan_busy || this->pending_op_valid) {
      return;
    }
    if (this->key_resp != nullptr &&
        this->key_resp->key_extraction_ctx_.state != hio::pairing_responder::ResponderState::DISARMED) {
      return;  // nie przeszkadzamy ekstrakcji klucza
    }
    const uint32_t now = millis();
    if (this->last_status_poll_ms != 0 && now - this->last_status_poll_ms < STATUS_POLL_MIN_GAP_MS) {
      return;  // najpierw oddajemy czas pętli głównej (SUPLA)
    }
    for (uint8_t i = 0; i < this->slot_count; i++) {
      if (this->slot_cfg[i].has_device == 0) {
        continue;
      }
      // Pierwszy odczyt ~15 s po starcie (żeby nie blokować bootowania),
      // potem co POLL_PERIOD_MS.
      const bool due = this->last_poll_ms[i] == 0 ? (now >= 15000) : (now - this->last_poll_ms[i] >= POLL_PERIOD_MS);
      if (!due) {
        continue;
      }
      this->last_status_poll_ms = now;
      this->runTwoWay(i, velux::VeluxRadio::TwoWayAction::POLL, 0);
      return;  // jedno na obieg
    }
  }

  void storeEvent(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(this->last_event, sizeof(this->last_event), format, args);
    va_end(args);
    this->last_event_ms = millis();
  }

  hio::DeviceType deviceTypeOf(uint8_t index) const {
    switch (this->slots[index].kind) {
      case velux::ProductKind::WINDOW_OPENER:
        return hio::DeviceType::WINDOW_OPENER;
      case velux::ProductKind::AWNING:
        return hio::DeviceType::AWNING;
      case velux::ProductKind::ROLLER_SHUTTER:
      default:
        return hio::DeviceType::ROLLER_SHUTTER;
    }
  }

  /// Klasy docelowe dla ramki 0x30 (gest nauki KLI).
  std::array<hio::DeviceType, 3> enrollClasses(uint8_t index) const {
    const auto unknown = hio::DeviceType::UNKNOWN;
    switch (this->slot_cfg[index].profile) {
      case static_cast<uint8_t>(velux::EnrollProfile::EXTERIOR):
        return {hio::DeviceType::ROLLER_SHUTTER, hio::DeviceType::AWNING, hio::DeviceType::DUAL_SHUTTER};
      case static_cast<uint8_t>(velux::EnrollProfile::WINDOW):
        return {hio::DeviceType::WINDOW_OPENER, hio::DeviceType::ROLLER_SHUTTER, hio::DeviceType::DUAL_SHUTTER};
      case static_cast<uint8_t>(velux::EnrollProfile::INTERIOR):
        return {hio::DeviceType::BLIND, hio::DeviceType::VENETIAN_BLIND, unknown};
      case static_cast<uint8_t>(velux::EnrollProfile::AUTO):
      default:
        if (this->slots[index].kind == velux::ProductKind::WINDOW_OPENER) {
          return {hio::DeviceType::WINDOW_OPENER, hio::DeviceType::ROLLER_SHUTTER, hio::DeviceType::DUAL_SHUTTER};
        }
        return {hio::DeviceType::ROLLER_SHUTTER, hio::DeviceType::AWNING, hio::DeviceType::DUAL_SHUTTER};
    }
  }

  hio::OneWayControllerIdentity makeIdentity(uint8_t index, const StoredIdentity &stored) const {
    hio::OneWayControllerIdentity identity;
    identity.id = this->identity_ids[index];
    memcpy(identity.node_id, stored.node_id, hio::NODE_ID_SIZE);
    memcpy(identity.system_key, stored.key, hio::AES_KEY_SIZE);
    identity.manufacturer = hio::MANUFACTURER_VELUX;
    identity.io_device_type = this->deviceTypeOf(index);
    identity.initial_sequence = 0;
    identity.node_id_derived = false;
    identity.enrollment_with_mac = false;
    identity.execute_acei = 0;
    // Prawdziwy pilot KLI nadaje komendy na adres "wszystkie urządzenia"
    // (00 00 3F), nie na adres klasowy.
    identity.execute_broadcast_all = true;
    identity.enrollment_classes = this->enrollClasses(index);
    identity.power_class_override = this->slot_cfg[index].low_power ? hio::OneWayPowerClass::LOW_POWER
                                                                    : hio::OneWayPowerClass::ALWAYS_ALIVE;
    return identity;
  }

  /// Wysyłka jednej kopii: serializacja + wysyłka z LBT (jak w hioc).
  bool transmit(const hio::IoFrame &frame, uint32_t freq, uint16_t preamble) {
    if (this->radio == nullptr) {
      return false;
    }
    uint8_t buffer[hio::FRAME_MAX_WIRE_SIZE];
    const uint8_t length = hio::serialize(frame, buffer, sizeof(buffer));
    if (length == 0) {
      ESP_LOGW(TAG, "serializacja nie powiodła się (cmd 0x%02X)", frame.cmd);
      return false;
    }
    hio::RadioTxConfig tx_config;
    tx_config.freq_hz = freq;
    tx_config.preamble_len = preamble;
    tx_config.wake = hio::tx_wake_for(hio::is_start(frame), preamble);

    hio::TxResult result;
    for (uint8_t lbt = 0;; lbt++) {
      if (lbt < this->tuning.lbt_max_retries) {
        tx_config.cca_threshold_dbm = this->tuning.lbt_rssi_threshold_dbm;
      } else {
        tx_config.cca_threshold_dbm.reset();
      }
      result = this->radio->send_packet(buffer, length, tx_config);
      if (result.status != hio::TxResult::Status::CHANNEL_BUSY) {
        break;
      }
      ESP_LOGD(TAG, "LBT: kanał zajęty (%d dBm), próba %u/%u", result.cca_level_dbm, lbt + 1,
               this->tuning.lbt_max_retries);
      delay(hio::LBT_RETRY_DELAY_MS);
    }
    if (!result.sent()) {
      ESP_LOGW(TAG, "nadawanie nie powiodło się (cmd 0x%02X)", frame.cmd);
      return false;
    }
    return true;
  }

  /// (Re)budowa nadajnika: tożsamości wszystkich slotów w jednym obiekcie.
  /// Wywoływana po starcie i po każdej zmianie ustawień slotu, bo tożsamość
  /// niesie klasę urządzenia i klasę mocy (preambułę).
  void rebuild() {
    delete this->tx;
    this->tx = new hio::OneWayTransmitter(
        [this](const hio::IoFrame &frame, uint32_t freq, uint16_t preamble) {
          return this->transmit(frame, freq, preamble);
        },
        &this->tuning);
    for (uint8_t i = 0; i < this->slot_count; i++) {
      StoredIdentity stored{};
      if (!loadIdentity(i, stored)) {
        continue;
      }
      this->tx->add_identity(this->makeIdentity(i, stored));
    }
    this->tx->setup();
    this->tx->set_command_report_callback([this](const hio::OneWayCommandReport &report) {
      ESP_LOGD(TAG, "raport 1W: %s intent=%s seq=%u nadano=%d", report.controller_id.c_str(), report.intent.c_str(),
               report.sequence, report.transmitted);
    });
  }

  /// Logowanie usłyszanej ramki 1W (pilot KLI, czujnik, inny kontroler).
  void handleRx(const hio::IoFrame &frame) {
    const hio::OneWayFrameInfo info = hio::decode_1w_frame(frame);
    const uint32_t now = millis();

    // Deduplikacja: pilot powtarza tę samą komendę 4x.
    const bool duplicate = now - this->rx_seen_ms < RX_DEDUP_WINDOW_MS && this->rx_cmd == frame.cmd &&
                           this->rx_has_intent == info.has_intent && this->rx_main0 == info.main0 &&
                           this->rx_main1 == info.main1 && memcmp(this->rx_src, frame.src, hio::NODE_ID_SIZE) == 0 &&
                           memcmp(this->rx_dst, frame.dst, hio::NODE_ID_SIZE) == 0;
    memcpy(this->rx_src, frame.src, hio::NODE_ID_SIZE);
    memcpy(this->rx_dst, frame.dst, hio::NODE_ID_SIZE);
    this->rx_cmd = frame.cmd;
    this->rx_main0 = info.main0;
    this->rx_main1 = info.main1;
    this->rx_has_intent = info.has_intent;
    this->rx_seen_ms = now;
    if (duplicate) {
      return;
    }

    const std::string src = hio::node_id_to_string(frame.src);
    snprintf(this->last_remote, sizeof(this->last_remote), "%s", src.c_str());
    this->last_remote_ms = now;

    const char *target = hio::oneway_target_label(info);
    if (info.has_intent) {
      ESP_LOGI(TAG, "RX 1W %s -> %s: %s(0x%02X) %s", src.c_str(), target, hio::command_name(info.cmd), info.cmd,
               info.intent);
      this->storeEvent("RX %s -> %s: %s", src.c_str(), target, info.intent);
    } else {
      ESP_LOGI(TAG, "RX 1W %s -> %s: %s(0x%02X) len=%u", src.c_str(), target, hio::command_name(info.cmd), info.cmd,
               info.data_len);
      this->storeEvent("RX %s -> %s: %s", src.c_str(), target, hio::command_name(info.cmd));
    }

    // Ramki 0x2E (DISCOVER_ALT_REQ) to sweep klas wysyłany przez pilot po
    // przytrzymaniu Gear - po nich ustawia się profil nauki (0x30).
    if (frame.cmd == hio::CMD_DISCOVER_ALT_REQ) {
      ESP_LOGI(TAG, "  ^ klasa nauki pilota: %s (ustaw profil nauki w panelu WWW)", target);
      this->storeEvent("Gear: klasa nauki %s - ustaw profil nauki", target);
    }
  }
};

// ============================================================================
// VeluxRadio
// ============================================================================

velux::VeluxRadio::~VeluxRadio() {
  if (this->impl_ != nullptr) {
    delete this->impl_->key_resp;
    delete this->impl_->engine;
    delete this->impl_->tx;
    delete this->impl_->radio;
    delete this->impl_->dio0;
    delete this->impl_->rst;
    delete this->impl_->spi;
    delete this->impl_;
    this->impl_ = nullptr;
  }
}

bool velux::VeluxRadio::begin(const Pins &pins, uint8_t tx_power_dbm, const SlotConfig *slots, uint8_t slot_count) {
  if (this->impl_ != nullptr) {
    return this->impl_->ready;
  }
  if (slots == nullptr || slot_count == 0 || slot_count > MAX_SLOTS) {
    ESP_LOGE(TAG, "błędna konfiguracja slotów (%u)", static_cast<unsigned>(slot_count));
    return false;
  }

  auto *impl = new Impl();
  impl->pins = pins;
  impl->tx_power = tx_power_dbm;
  impl->slots = slots;
  impl->slot_count = slot_count;
  this->impl_ = impl;

  // Tożsamości (adres + klucz) - raz wygenerowane, trwałe w NVS.
  for (uint8_t i = 0; i < slot_count; i++) {
    impl->identity_ids[i] = std::string("slot") + std::to_string(static_cast<unsigned>(i));
    StoredIdentity stored{};
    if (!loadIdentity(i, stored)) {
      memset(&stored, 0, sizeof(stored));
      stored.magic = STORE_MAGIC;
      randomNodeId(stored.node_id);
      esp_fill_random(stored.key, hio::AES_KEY_SIZE);
      saveIdentity(i, stored);
      ESP_LOGI(TAG, "slot %u: nowa tożsamość 1W %02X%02X%02X", static_cast<unsigned>(i) + 1, stored.node_id[0],
               stored.node_id[1], stored.node_id[2]);
    }
    StoredSlot slot_cfg{};
    if (!loadSlotConfig(i, slot_cfg)) {
      memset(&slot_cfg, 0, sizeof(slot_cfg));
      slot_cfg.magic = STORE_MAGIC;
      slot_cfg.low_power = slots[i].default_low_power ? 1 : 0;
      slot_cfg.profile = static_cast<uint8_t>(slots[i].default_profile);
      saveSlotConfig(i, slot_cfg);
    }
    impl->slot_cfg[i] = slot_cfg;
  }

  // Sprzęt: SPI + SX1276.
  impl->spi = new Esp32SpiAccess(pins);
  impl->rst = new esphome::InternalGPIOPin(pins.rst, OUTPUT);
  impl->dio0 = new esphome::InternalGPIOPin(pins.dio0, INPUT);
  impl->radio = new hio::RadioSX1276(impl->spi, impl->rst, impl->dio0, nullptr, tx_power_dbm,
                                     hio::SX1276_PA_SELECT_PA_BOOST);

  // Tuning: domyślne wartości hioc + preambuła zależna od sterownika.
  impl->tuning.normal_start_preamble = impl->radio->default_start_preamble();

  if (!impl->radio->init()) {
    ESP_LOGE(TAG, "SX1276 nie odpowiedział (wersja != 0x12) - sprawdź SPI i piny");
    impl->storeEvent("brak radia SX1276 - sprawdź SPI, piny NSS/RST/DIO0 i zasilanie 3V3");
    return false;
  }
  impl->radio->set_mode_standby();
  impl->radio->apply_tuning(impl->tuning);
  impl->radio->set_mode_rx();

  // Klucz instalacji 2W (jeśli był już odzyskany z pilota wcześniej).
  StoredHub stored_hub{};
  if (loadHub(stored_hub)) {
    memcpy(impl->hub_node_id, stored_hub.node_id, hio::NODE_ID_SIZE);
    memcpy(impl->hub_key, stored_hub.key, hio::AES_KEY_SIZE);
    impl->hub_key_valid = true;
    ESP_LOGI(TAG, "klucz instalacji 2W wczytany z NVS (node_id %02X%02X%02X)", impl->hub_node_id[0],
             impl->hub_node_id[1], impl->hub_node_id[2]);
  } else {
    ESP_LOGI(TAG, "brak klucza instalacji 2W - najpierw ekstrakcja z pilota (KLR 200)");
  }

  // Silnik wymian 2W i responder ekstrakcji klucza. Oba dostają wskaźniki do
  // naszych tablic (node_id/klucz), więc późniejsza podmiana ich zawartości
  // jest dla nich widoczna bez przebudowy obiektów.
  impl->driver = impl->radio;
  impl->engine = new hio::ExchangeEngine(&impl->driver, impl->hub_node_id, impl->hub_key, &impl->tuning);
  impl->key_resp = new hio::KeyExtractionResponder(
      impl->hub_node_id, &impl->driver, &impl->tuning, impl->registry,
      [impl](const hio::IoFrame &frame, uint32_t freq, uint16_t preamble) {
        return impl->transmit(frame, freq, preamble);
      },
      [impl](const char *name, uint32_t delay_ms, std::function<void()> fn) {
        impl->scheduleNamed(name, delay_ms, std::move(fn));
      });

  impl->rebuild();
  impl->ready = true;

  Serial.printf("[velux1w] SX1276 OK, %u kanał(y), TX %u dBm\n", static_cast<unsigned>(slot_count),
                static_cast<unsigned>(tx_power_dbm));
  impl->storeEvent("radio gotowe (%u kanałów)", static_cast<unsigned>(slot_count));
  return true;
}

void velux::VeluxRadio::poll() {
  if (this->impl_ == nullptr || !this->impl_->ready) {
    return;
  }
  this->impl_->runTimers();

  hio::RadioRxPacket packet;
  if (this->impl_->radio->check_for_packet(packet)) {
    hio::IoFrame frame;
    if (!hio::parse(packet.data, packet.len, frame)) {
      ESP_LOGD(TAG, "RX: nieparsowalna ramka (%u B)", packet.len);
    } else if ((frame.ctrl0 & hio::CTRL0_PROTOCOL_1W) != 0) {
      this->impl_->handleRx(frame);
    } else {
      this->impl_->handleTwoWayRx(frame, packet);
    }
  }

  // Ekstrakcja klucza: po pierwszej wymianie ramek trzymamy CH2 (tak jak
  // hub w hioc), żeby nie zgubić kolejnego kroku kreatora parowania.
  // Poza ekstrakcją skaczemy po kanałach - inaczej nie usłyszymy ruchu 2W
  // (urządzenia nadają na trzech kanałach). Podczas skanu kanał wybiera skan.
  if (this->impl_->scan_busy) {
    this->impl_->holding_ch2 = false;
  } else if (this->impl_->key_resp != nullptr && this->impl_->key_resp->awaiting_reply()) {
    if (!this->impl_->holding_ch2) {
      this->impl_->radio->change_frequency(hio::FREQ_CH2);
      this->impl_->radio->set_mode_rx();
      this->impl_->holding_ch2 = true;
      ESP_LOGI(TAG, "ekstrakcja: trzymam CH2 do końca wymiany");
    }
  } else {
    this->impl_->holding_ch2 = false;
    if (this->impl_->engine != nullptr) {
      this->impl_->engine->maybe_hop();
    }
  }

  this->impl_->scanStep();
  this->impl_->pollStatuses();
}

void velux::VeluxRadio::processPending() {
  if (this->impl_ == nullptr) {
    return;
  }
  // Ustawienia z panelu WWW zmieniają tożsamości - nadajnik przebudowujemy
  // tutaj, w pętli głównej (panel to inny task).
  if (this->impl_->need_rebuild) {
    this->impl_->need_rebuild = false;
    this->impl_->rebuild();
  }

  if (!this->impl_->pending_op_valid) {
    return;
  }
  const Impl::PendingOp op = this->impl_->pending_op;
  this->impl_->pending_op_valid = false;

  switch (static_cast<Pending>(op.kind)) {
    case Pending::ENROLL:
      this->enroll(op.slot);
      break;
    case Pending::UNENROLL:
      this->unenroll(op.slot);
      break;
    case Pending::REGENERATE:
      this->regenerateIdentity(op.slot);
      break;
    case Pending::SCAN:
      this->impl_->startScan();
      break;
    case Pending::ARM_KEY:
      if (this->impl_->key_resp != nullptr) {
        this->impl_->key_extraction_arm_ms = millis();
        this->impl_->key_resp->set_armed(true);
        this->impl_->storeTwoWayEvent("ekstrakcja klucza uzbrojona (10 min)");
      }
      break;
    case Pending::DISARM_KEY:
      if (this->impl_->key_resp != nullptr) {
        this->impl_->key_resp->set_armed(false);
        this->impl_->key_extraction_arm_ms = 0;
        this->impl_->storeTwoWayEvent("ekstrakcja klucza wyłączona");
      }
      break;
    case Pending::CLEAR_KEY:
      this->impl_->clearCredentials();
      break;
    case Pending::ASSIGN:
      this->impl_->assignDevice(op.slot, op.param);
      break;
    case Pending::UNASSIGN:
      this->impl_->unassignDevice(op.slot);
      break;
    case Pending::TWOWAY:
      this->impl_->runTwoWay(op.slot, static_cast<TwoWayAction>(op.action), op.param);
      break;
    case Pending::NONE:
    default:
      break;
  }
}

bool velux::VeluxRadio::ready() const { return this->impl_ != nullptr && this->impl_->ready; }

const char *velux::VeluxRadio::chipName() const {
  if (this->impl_ == nullptr || this->impl_->radio == nullptr) {
    return "brak";
  }
  return this->impl_->radio->chip_name();
}

uint8_t velux::VeluxRadio::slotCount() const { return this->impl_ == nullptr ? 0 : this->impl_->slot_count; }

const velux::SlotConfig *velux::VeluxRadio::slot(uint8_t index) const {
  if (this->impl_ == nullptr || index >= this->impl_->slot_count) {
    return nullptr;
  }
  return &this->impl_->slots[index];
}

bool velux::VeluxRadio::lowPower(uint8_t index) const {
  if (this->impl_ == nullptr || index >= this->impl_->slot_count) {
    return false;
  }
  return this->impl_->slot_cfg[index].low_power != 0;
}

void velux::VeluxRadio::setLowPower(uint8_t index, bool value) {
  if (this->impl_ == nullptr || index >= this->impl_->slot_count) {
    return;
  }
  if ((this->impl_->slot_cfg[index].low_power != 0) == value) {
    return;
  }
  this->impl_->slot_cfg[index].low_power = value ? 1 : 0;
  saveSlotConfig(index, this->impl_->slot_cfg[index]);
  // Przebudowa nadajnika tylko w pętli głównej (processPending): ta funkcja
  // jest wołana z serwera WWW, czyli z innego tasku niż wysyłka.
  this->impl_->need_rebuild = true;
  this->impl_->storeEvent("slot %u: zasilanie odbiornika = %s", static_cast<unsigned>(index) + 1,
                          value ? "bateria/solar" : "sieciowe");
}

velux::EnrollProfile velux::VeluxRadio::enrollProfile(uint8_t index) const {
  if (this->impl_ == nullptr || index >= this->impl_->slot_count) {
    return EnrollProfile::AUTO;
  }
  return static_cast<EnrollProfile>(this->impl_->slot_cfg[index].profile);
}

void velux::VeluxRadio::setEnrollProfile(uint8_t index, EnrollProfile profile) {
  if (this->impl_ == nullptr || index >= this->impl_->slot_count) {
    return;
  }
  if (this->impl_->slot_cfg[index].profile == static_cast<uint8_t>(profile)) {
    return;
  }
  this->impl_->slot_cfg[index].profile = static_cast<uint8_t>(profile);
  saveSlotConfig(index, this->impl_->slot_cfg[index]);
  // Jak przy setLowPower(): przebudowa nadajnika w pętli głównej.
  this->impl_->need_rebuild = true;
  this->impl_->storeEvent("slot %u: profil nauki = %u", static_cast<unsigned>(index) + 1,
                          static_cast<unsigned>(profile));
}

void velux::VeluxRadio::identityText(uint8_t index, char *out, size_t out_size) const {
  if (out == nullptr || out_size == 0) {
    return;
  }
  out[0] = '\0';
  if (this->impl_ == nullptr || index >= this->impl_->slot_count) {
    snprintf(out, out_size, "brak");
    return;
  }
  StoredIdentity stored{};
  if (!loadIdentity(index, stored)) {
    snprintf(out, out_size, "brak");
    return;
  }
  snprintf(out, out_size, "%02X%02X%02X", stored.node_id[0], stored.node_id[1], stored.node_id[2]);
}

void velux::VeluxRadio::regenerateIdentity(uint8_t index) {
  if (this->impl_ == nullptr || index >= this->impl_->slot_count) {
    return;
  }
  StoredIdentity stored{};
  memset(&stored, 0, sizeof(stored));
  stored.magic = STORE_MAGIC;
  randomNodeId(stored.node_id);
  esp_fill_random(stored.key, hio::AES_KEY_SIZE);
  saveIdentity(index, stored);
  this->impl_->rebuild();
  ESP_LOGI(TAG, "slot %u: nowa tożsamość %02X%02X%02X", static_cast<unsigned>(index) + 1, stored.node_id[0],
           stored.node_id[1], stored.node_id[2]);
  this->impl_->storeEvent("slot %u: nowa tożsamość %02X%02X%02X - wymagana ponowna nauka",
                          static_cast<unsigned>(index) + 1, stored.node_id[0], stored.node_id[1], stored.node_id[2]);
}

bool velux::VeluxRadio::sendPosition(uint8_t index, uint8_t position_0_100) {
  if (this->impl_ == nullptr || !this->impl_->ready || index >= this->impl_->slot_count) {
    if (this->impl_ != nullptr) {
      this->impl_->storeEvent("nie mogę wysłać: brak radia");
    }
    return false;
  }
  if (position_0_100 > 100) {
    position_0_100 = 100;
  }
  const bool ok = this->impl_->tx->send_position(this->impl_->identity_ids[index], position_0_100);
  if (ok) {
    ESP_LOGI(TAG, "slot %u: pozycja %u%% wysłana", static_cast<unsigned>(index) + 1, position_0_100);
    this->impl_->storeEvent("slot %u: pozycja %u%%", static_cast<unsigned>(index) + 1, position_0_100);
  }
  return ok;
}

bool velux::VeluxRadio::sendStop(uint8_t index) {
  if (this->impl_ == nullptr || !this->impl_->ready || index >= this->impl_->slot_count) {
    return false;
  }
  const bool ok = this->impl_->tx->send_command(this->impl_->identity_ids[index], hio::CoverCommand::STOP);
  if (ok) {
    ESP_LOGI(TAG, "slot %u: STOP wysłany", static_cast<unsigned>(index) + 1);
    this->impl_->storeEvent("slot %u: STOP", static_cast<unsigned>(index) + 1);
  }
  return ok;
}

bool velux::VeluxRadio::enroll(uint8_t index) {
  if (this->impl_ == nullptr || !this->impl_->ready || index >= this->impl_->slot_count) {
    if (this->impl_ != nullptr) {
      this->impl_->storeEvent("nauka niemożliwa: brak radia");
    }
    return false;
  }
  impl_->storeEvent("slot %u: nauka w toku (to trwa ~1-2 s)...", static_cast<unsigned>(index) + 1);
  const bool ok = this->impl_->tx->send_enrollment(this->impl_->identity_ids[index]);
  ESP_LOGI(TAG, "slot %u: nauka %s", static_cast<unsigned>(index) + 1, ok ? "wysłana" : "nie powiodła się");
  impl_->storeEvent("slot %u: nauka %s", static_cast<unsigned>(index) + 1,
                    ok ? "wysłana (zamykanie = sukces)" : "nie powiodła się");
  return ok;
}

bool velux::VeluxRadio::unenroll(uint8_t index) {
  if (this->impl_ == nullptr || !this->impl_->ready || index >= this->impl_->slot_count) {
    return false;
  }
  const bool ok = this->impl_->tx->send_unenrollment(this->impl_->identity_ids[index]);
  ESP_LOGI(TAG, "slot %u: wypisanie %s (niepotwierdzone przez urządzenie)", static_cast<unsigned>(index) + 1,
           ok ? "wysłane" : "nie powiodło się");
  this->impl_->storeEvent("slot %u: wypisanie %s", static_cast<unsigned>(index) + 1,
                          ok ? "wysłane (bez potwierdzenia)" : "nie powiodło się");
  return ok;
}

void velux::VeluxRadio::queuePending(uint8_t index, Pending command) {
  if (this->impl_ == nullptr || index >= this->impl_->slot_count || command == Pending::NONE) {
    return;
  }
  this->impl_->queue(static_cast<uint8_t>(command), index);
}

const char *velux::VeluxRadio::lastEvent() const {
  if (this->impl_ == nullptr) {
    return "brak";
  }
  return this->impl_->last_event;
}

void velux::VeluxRadio::lastRemoteText(char *out, size_t out_size) const {
  if (out == nullptr || out_size == 0) {
    return;
  }
  if (this->impl_ == nullptr || this->impl_->last_remote[0] == '\0') {
    snprintf(out, out_size, "brak");
    return;
  }
  snprintf(out, out_size, "%s", this->impl_->last_remote);
}

// ============================================================================
// 2W: klucz instalacji, skan, przypisania, komendy
// ============================================================================

const char *velux::VeluxRadio::deviceTypeName(uint8_t type) {
  return hio::device_type_name(static_cast<hio::DeviceType>(type));
}

velux::VeluxRadio::HubCredentials velux::VeluxRadio::hubCredentials() const {
  if (this->impl_ == nullptr) {
    return HubCredentials{};
  }
  return this->impl_->credentials();
}

bool velux::VeluxRadio::twoWayReady() const {
  return this->impl_ != nullptr && this->impl_->ready && this->impl_->hub_key_valid && this->impl_->engine != nullptr;
}

void velux::VeluxRadio::clearHubCredentials() {
  if (this->impl_ != nullptr) {
    this->impl_->queue(static_cast<uint8_t>(Pending::CLEAR_KEY));
  }
}

bool velux::VeluxRadio::keyExtractionArmed() const {
  return this->impl_ != nullptr && this->impl_->key_resp != nullptr &&
         this->impl_->key_resp->key_extraction_ctx_.state != hio::pairing_responder::ResponderState::DISARMED;
}

const char *velux::VeluxRadio::keyExtractionStage() const {
  if (this->impl_ == nullptr || this->impl_->key_resp == nullptr) {
    return "brak";
  }
  return hio::pairing_responder::responder_stage_name(this->impl_->key_resp->key_extraction_ctx_.state);
}

uint32_t velux::VeluxRadio::keyExtractionSecondsLeft() const {
  if (this->impl_ == nullptr || !this->keyExtractionArmed() || this->impl_->key_extraction_arm_ms == 0) {
    return 0;
  }
  // Okno uzbrojenia respondera to 10 minut (KEY_EXTRACTION_AUTO_OFF_MS).
  constexpr uint32_t WINDOW_MS = 10UL * 60UL * 1000UL;
  const uint32_t elapsed = millis() - this->impl_->key_extraction_arm_ms;
  return elapsed >= WINDOW_MS ? 0 : (WINDOW_MS - elapsed) / 1000;
}

void velux::VeluxRadio::setKeyExtractionArmed(bool armed) {
  if (this->impl_ == nullptr) {
    return;
  }
  this->impl_->queue(static_cast<uint8_t>(armed ? Pending::ARM_KEY : Pending::DISARM_KEY));
}

void velux::VeluxRadio::queueScan() {
  if (this->impl_ != nullptr) {
    this->impl_->queue(static_cast<uint8_t>(Pending::SCAN));
  }
}

bool velux::VeluxRadio::scanBusy() const { return this->impl_ != nullptr && this->impl_->scan_busy; }

uint32_t velux::VeluxRadio::scanAgeMs() const {
  if (this->impl_ == nullptr || this->impl_->scan_last_ms == 0) {
    return 0;
  }
  return millis() - this->impl_->scan_last_ms;
}

uint8_t velux::VeluxRadio::scanCount() const { return this->impl_ == nullptr ? 0 : this->impl_->scan_dev_count; }

const velux::VeluxRadio::ScannedDevice *velux::VeluxRadio::scanDevice(uint8_t index) const {
  if (this->impl_ == nullptr || index >= this->impl_->scan_dev_count) {
    return nullptr;
  }
  return &this->impl_->scan_devs[index];
}

void velux::VeluxRadio::queueAssign(uint8_t slot, uint8_t scan_index) {
  if (this->impl_ == nullptr || slot >= this->impl_->slot_count) {
    return;
  }
  this->impl_->queue(static_cast<uint8_t>(Pending::ASSIGN), slot, 0, scan_index);
}

void velux::VeluxRadio::queueUnassign(uint8_t slot) {
  if (this->impl_ == nullptr || slot >= this->impl_->slot_count) {
    return;
  }
  this->impl_->queue(static_cast<uint8_t>(Pending::UNASSIGN), slot);
}

bool velux::VeluxRadio::slotHasDevice(uint8_t slot) const {
  return this->impl_ != nullptr && slot < this->impl_->slot_count && this->impl_->slot_cfg[slot].has_device != 0;
}

bool velux::VeluxRadio::slotTwoWayReady(uint8_t slot) const { return this->slotHasDevice(slot) && this->twoWayReady(); }

bool velux::VeluxRadio::slotLowPower(uint8_t slot) const {
  return this->slotHasDevice(slot) && this->impl_->slot_cfg[slot].low_power != 0;
}

void velux::VeluxRadio::slotDeviceText(uint8_t slot, char *out, size_t out_size) const {
  if (out == nullptr || out_size == 0) {
    return;
  }
  out[0] = '\0';
  if (!this->slotHasDevice(slot)) {
    snprintf(out, out_size, "brak (1W)");
    return;
  }
  const StoredSlot &cfg = this->impl_->slot_cfg[slot];
  snprintf(out, out_size, "%02X%02X%02X %s%s", cfg.node_id[0], cfg.node_id[1], cfg.node_id[2],
           hio::device_type_name(static_cast<hio::DeviceType>(cfg.type)), cfg.low_power != 0 ? " (bateria)" : "");
}

void velux::VeluxRadio::queueTwoWay(uint8_t slot, TwoWayAction action, uint8_t position_0_100) {
  if (this->impl_ == nullptr || slot >= this->impl_->slot_count || action == TwoWayAction::NONE) {
    return;
  }
  this->impl_->queue(static_cast<uint8_t>(Pending::TWOWAY), slot, static_cast<uint8_t>(action), position_0_100);
}

velux::VeluxRadio::TwoWayStatus velux::VeluxRadio::slotStatus(uint8_t slot) const {
  if (this->impl_ == nullptr || slot >= this->impl_->slot_count) {
    return TwoWayStatus{};
  }
  TwoWayStatus status = this->impl_->status[slot];
  if (status.known) {
    status.age_ms = millis() - this->impl_->status_ms[slot];
  }
  return status;
}

const char *velux::VeluxRadio::lastTwoWayEvent() const {
  if (this->impl_ == nullptr || this->impl_->last_2w_event[0] == '\0') {
    return "brak";
  }
  return this->impl_->last_2w_event;
}

const char *velux::VeluxRadio::lastHeardTwoWay() const {
  if (this->impl_ == nullptr || this->impl_->last_2w_heard[0] == '\0') {
    return "brak (nie słyszałem jeszcze żadnej ramki 2W)";
  }
  return this->impl_->last_2w_heard;
}

uint32_t velux::VeluxRadio::lastHeardTwoWayAgeMs() const {
  if (this->impl_ == nullptr || this->impl_->last_2w_heard_ms == 0) {
    return 0;
  }
  return millis() - this->impl_->last_2w_heard_ms;
}

void velux::VeluxRadio::setStatusListener(StatusListener listener) {
  if (this->impl_ != nullptr) {
    this->impl_->status_listener = std::move(listener);
  }
}

void velux::VeluxRadio::setKeyExtractedListener(KeyExtractedListener listener) {
  if (this->impl_ != nullptr) {
    this->impl_->key_listener = std::move(listener);
  }
}
