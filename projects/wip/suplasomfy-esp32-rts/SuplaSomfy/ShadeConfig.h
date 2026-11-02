// ShadeConfig.h — wersja 2 (12.09.2026)
//
// V2: 20 slotow rolet. Adres RTS kazdej rolety ustawiasz w panelu WWW
// (tryb konfiguracji) — pole "Adres RTS (dec)". Wartosci ponizej to tylko
// DOMYSLNE wartosci startowe / fallback, gdy w panelu nic nie wpisano.
//
// Zasada dzialania:
//   - adres z panelu WWW (Supla::Storage::Config, klucz "shadeN_addr")
//     ma PRIORYTET,
//   - gdy w panelu brak wartosci, brany jest remoteAddress stad,
//   - adres 0 = slot nieaktywny (kanal istnieje w Supli, ale komendy
//     RTS sa ignorowane i logowane).
//
// UWAGA na shadeKey: max 10 znakow (klucz NVS rolling code to
// "<shadeKey>_addr", limit magazynu Config to 15 znakow + NUL).
#pragma once
#include <Arduino.h>

struct ShadeConfig {
  const char *shadeKey;    // klucz slotu, max 10 znakow ("shade1"..."shade20")
  uint32_t remoteAddress;  // DOMYSLNY adres RTS (fallback, panel WWW ma priorytet)
  uint8_t repeats;         // powtorzenia ramki RTS (4 = dobry start)
  int upTimeMs;            // domyslny czas otwarcia (nadpisze kalibracja/WWW)
  int downTimeMs;          // domyslny czas zamkniecia (j.w.)
};

static ShadeConfig SHADES[] = {
  { "shade1",  0x000000, 4, 10000, 10000 },
  { "shade2",  0x000000, 4, 10000, 10000 },
  { "shade3",  0x000000, 4, 10000, 10000 },
  { "shade4",  0x000000, 4, 10000, 10000 },
  { "shade5",  0x000000, 4, 10000, 10000 },
  { "shade6",  0x000000, 4, 10000, 10000 },
  { "shade7",  0x000000, 4, 10000, 10000 },
  { "shade8",  0x000000, 4, 10000, 10000 },
  { "shade9",  0x000000, 4, 10000, 10000 },
  { "shade10", 0x000000, 4, 10000, 10000 },
  { "shade11", 0x000000, 4, 10000, 10000 },
  { "shade12", 0x000000, 4, 10000, 10000 },
  { "shade13", 0x000000, 4, 10000, 10000 },
  { "shade14", 0x000000, 4, 10000, 10000 },
  { "shade15", 0x000000, 4, 10000, 10000 },
  { "shade16", 0x000000, 4, 10000, 10000 },
  { "shade17", 0x000000, 4, 10000, 10000 },
  { "shade18", 0x000000, 4, 10000, 10000 },
  { "shade19", 0x000000, 4, 10000, 10000 },
  { "shade20", 0x000000, 4, 10000, 10000 },
};

static constexpr uint8_t SHADE_COUNT = sizeof(SHADES) / sizeof(SHADES[0]);
