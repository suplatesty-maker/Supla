// SomfyRtsTx.cpp — wersja finalna (10.09.2026)
// Implementacja 1:1 na podstawie Transceiver::sendFrame,
// somfy_frame_t::encodeFrame i SomfyRemote::getNextRollingCode
// z rstrouse/ESPSomfy-RTS (Somfy.cpp, wydanie v2.4.6).
//
// esp_task_wdt_reset() w petli powtorzen — dokladnie jak w oryginale
// ESPSomfy-RTS (zabezpieczenie przed resetem watchdogiem przy dluzszych
// transmisjach, bo nadawanie blokuje loop na ~0.5-1 s).
#include "SomfyRtsTx.h"
#include <esp_task_wdt.h>

SomfyRtsTx::SomfyRtsTx(uint8_t sckPin, uint8_t misoPin, uint8_t mosiPin,
                       uint8_t csnPin, uint8_t gdoPin)
  : _sckPin(sckPin), _misoPin(misoPin), _mosiPin(mosiPin),
    _csnPin(csnPin), _gdoPin(gdoPin) {}

bool SomfyRtsTx::begin(float frequencyMHz, float deviationKHz,
                       float rxBandwidthKHz, int8_t txPower) {
  pinMode(_gdoPin, OUTPUT);
  digitalWrite(_gdoPin, LOW);

  // Konfiguracja identyczna z Transceiver::begin / transceiver_config_t::apply
  // w ESPSomfy-RTS
  ELECHOUSE_cc1101.setGDO0(_gdoPin);
  ELECHOUSE_cc1101.setSpiPin(_sckPin, _misoPin, _mosiPin, _csnPin);
  ELECHOUSE_cc1101.Init();
  ELECHOUSE_cc1101.setCCMode(0);           // tryb transmisji zewnetrznej
  ELECHOUSE_cc1101.setMHZ(frequencyMHz);   // 433.42 MHz dla RTS
  ELECHOUSE_cc1101.setRxBW(rxBandwidthKHz);
  ELECHOUSE_cc1101.setDeviation(deviationKHz);
  ELECHOUSE_cc1101.setPA(txPower);
  ELECHOUSE_cc1101.setModulation(2);       // ASK/OOK
  ELECHOUSE_cc1101.setManchester(1);       // Manchester ON
  ELECHOUSE_cc1101.setPktFormat(3);        // asynchroniczny tryb szeregowy
  ELECHOUSE_cc1101.setDcFilterOff(0);
  ELECHOUSE_cc1101.setCrc(0);
  ELECHOUSE_cc1101.setCRC_AF(0);
  ELECHOUSE_cc1101.setSyncMode(0);         // bez preambuly/sync-worda

  return true;
}

// Rolling code — identycznie jak SomfyRemote::getNextRollingCode():
// pref.begin("ShadeCodes"); code = getUShort(key, 0) + 1; putUShort(key, code);
// Inkrementacja i zapis PRZED nadaniem — jak w oryginale.
uint16_t SomfyRtsTx::nextRollingCode(const char *shadeKey) {
  _pref.begin("ShadeCodes");
  uint16_t code = _pref.getUShort(shadeKey, 0);
  code++;
  _pref.putUShort(shadeKey, code);
  _pref.end();
  return code;
}

uint16_t SomfyRtsTx::getRollingCode(const char *shadeKey) {
  _pref.begin("ShadeCodes", true);
  uint16_t code = _pref.getUShort(shadeKey, 0);
  _pref.end();
  return code;
}

void SomfyRtsTx::setRollingCode(const char *shadeKey, uint16_t code) {
  _pref.begin("ShadeCodes");
  _pref.putUShort(shadeKey, code);
  _pref.end();
}

// Identyczne z somfy_frame_t::encodeFrame() dla protokolu RTS (56-bit).
void SomfyRtsTx::encodeFrame(byte *frame, uint32_t remoteAddress,
                             uint16_t rollingCode, SomfyCommand cmd) {
  const byte btn = static_cast<byte>(cmd);
  // encKey: klucze 160-175, powiazany z rolling code (jak w oryginale)
  const byte encKey = 0xA0 | static_cast<uint8_t>(rollingCode & 0x000F);

  frame[0] = encKey;                     // klucz szyfrujacy
  frame[1] = (btn & 0x0F) << 4;          // komenda; LSB = miejsce na checksume
  frame[2] = rollingCode >> 8;           // rolling code big-endian
  frame[3] = rollingCode;
  frame[4] = remoteAddress >> 16;        // adres 24-bit big-endian
  frame[5] = remoteAddress >> 8;
  frame[6] = remoteAddress;
  frame[7] = 132;                        // bajty 7-9 nie sa wysylane w 56-bit,
  frame[8] = 0;                          // ale oryginal je wypelnia — zachowujemy 1:1
  frame[9] = 29;

  // Checksum: XOR gornych i dolnych nibbli bajtow 0-6
  byte checksum = 0;
  for (byte i = 0; i < 7; i++) {
    checksum = checksum ^ frame[i] ^ (frame[i] >> 4);
  }
  checksum &= 0b1111;
  frame[1] |= checksum;

  // Obfuskacja: XOR lancuchowy bajtow 1-6
  for (byte i = 1; i < 7; i++) {
    frame[i] ^= frame[i - 1];
  }
}

void SomfyRtsTx::beginTransmit() {
  // Identyczne z Transceiver::beginTransmit()
  pinMode(_gdoPin, OUTPUT);
  digitalWrite(_gdoPin, 0);
  ELECHOUSE_cc1101.SetTx();
}

void SomfyRtsTx::endTransmit() {
  // Identyczne z Transceiver::endTransmit()
  ELECHOUSE_cc1101.setSidle();
}

// Identyczne z Transceiver::sendFrame() — wake-up, hardware sync,
// software sync, payload Manchester MSB-first, cisza miedzy ramkami.
void SomfyRtsTx::transmitFrame(byte *frame, uint8_t sync) {
  if (sync == 2) {
    // Impuls wake-up TYLKO przy pierwszej ramce.
    // Uwaga: wbrew starszym opisom protokolu, w oryginale ESPSomfy-RTS
    // po impulsie NIE ma dlugiej przerwy (testowane z pilotem Telis).
    digitalWrite(_gdoPin, HIGH);
    delayMicroseconds(10920);
    digitalWrite(_gdoPin, LOW);
    delayMicroseconds(7357);
  }

  // Hardware sync: 2 impulsy dla pierwszej ramki 56-bit, 7 dla powtorzen
  for (uint8_t i = 0; i < sync; i++) {
    digitalWrite(_gdoPin, HIGH);
    delayMicroseconds(4 * RTS_SYMBOL);
    digitalWrite(_gdoPin, LOW);
    delayMicroseconds(4 * RTS_SYMBOL);
  }

  // Software sync
  digitalWrite(_gdoPin, HIGH);
  delayMicroseconds(4850);
  digitalWrite(_gdoPin, LOW);
  delayMicroseconds(RTS_SYMBOL);   // start "0"

  // Payload: 56 bitow, MSB first, kodowanie Manchester
  uint8_t last_bit = 0;
  for (byte i = 0; i < 56; i++) {
    if (((frame[i / 8] >> (7 - (i % 8))) & 1) == 1) {
      digitalWrite(_gdoPin, LOW);
      delayMicroseconds(RTS_SYMBOL);
      digitalWrite(_gdoPin, HIGH);
      delayMicroseconds(RTS_SYMBOL);
      last_bit = 1;
    } else {
      digitalWrite(_gdoPin, HIGH);
      delayMicroseconds(RTS_SYMBOL);
      digitalWrite(_gdoPin, LOW);
      delayMicroseconds(RTS_SYMBOL);
      last_bit = 0;
    }
  }

  // Zakonczenie "1" jesli ostatni bit byl 0 — informuje silnik
  // (protokol 56-bit), ze nie ma juz kolejnych bitow.
  if (last_bit == 0) {
    digitalWrite(_gdoPin, HIGH);
  }

  // Cisza miedzy ramkami ~27.4 ms — w dwoch krokach, bo
  // delayMicroseconds nie przyjmuje wartosci > 16383.
  digitalWrite(_gdoPin, LOW);
  delayMicroseconds(13717);
  delayMicroseconds(13717);
}

uint16_t SomfyRtsTx::sendCommand(const char *shadeKey, uint32_t remoteAddress,
                                 SomfyCommand cmd, uint8_t repeats) {
  // Rolling code pobierany i zapisywany w NVS PRZED nadaniem
  uint16_t rollingCode = nextRollingCode(shadeKey);

  byte frame[10];
  encodeFrame(frame, remoteAddress, rollingCode, cmd);

  beginTransmit();
  transmitFrame(frame, 2);              // pierwsza ramka: wake-up + 2x sync
  for (uint8_t i = 0; i < repeats; i++) {
    transmitFrame(frame, 7);           // powtorzenia: 7x sync, bez wake-up
    esp_task_wdt_reset();              // jak w oryginale ESPSomfy-RTS
  }
  endTransmit();

  return rollingCode;
}
