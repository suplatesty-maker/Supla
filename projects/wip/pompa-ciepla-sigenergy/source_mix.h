#pragma once
// Podzial energii pompy. Czysta matematyka: bez Arduino, bez sieci, bez SUPLA.
//
// Pompa jest jednofazowa i wisi na jednej fazie przyłącza. PZEM mierzy CALA
// moc pompy. Z Sigenergy czytamy moc TEJ SAMEJ fazy (przekladnik CT).
//
// Do wezla z pompa moc moze doplynac tylko dwiema drogami: z falownika
// (przez przekladnik CT) albo z sieci. Trzeciej drogi nie ma. Wiec jesli
// falownik oddaje 400 W, a pompa pobiera 1000 W, to 400 W pochodzi z
// autokonsumpcji, a 600 W z sieci. To prawo Kirchhoffa, nie szacunek.
//
// Znaki: Ppump >= 0, Pgrid > 0 = pobor z sieci, Pgrid < 0 = eksport.

#include <stdint.h>

namespace SourceMix {

constexpr float maxf(float a, float b) {
  return a > b ? a : b;
}

constexpr float minf(float a, float b) {
  return a < b ? a : b;
}

struct Split {
  float gridW;
  float autoW;
};

// auto = min(Ppump, max(-Pgrid, 0))
// grid = Ppump - auto
constexpr Split split(float pumpW, float gridW) {
  const float autoW = minf(pumpW, maxf(-gridW, 0.0f));
  return Split{pumpW - autoW, autoW};
}

// Udzial autokonsumpcji w mocy pompy (0..1). 0 gdy pompa nie pracuje.
constexpr float autoRatio(float pumpW, float gridW) {
  return pumpW > 0.0f ? split(pumpW, gridW).autoW / pumpW : 0.0f;
}

// Pompa moze pracowac tylko gdy nie ma zadnej blokady.
constexpr bool pumpAllowed(bool manualBlock, bool saleBlock) {
  return !manualBlock && !saleBlock;
}

// Ujemny przyrost energii PZEM = licznik zostal wyzerowany albo sie zawinął.
constexpr bool pzemReset(float deltaKwh) {
  return deltaKwh < -0.001f;
}

// Zbyt dluga przerwa albo zbyt duzy skok nie nadaje sie do przypisania.
constexpr bool allocatable(float deltaKwh, uint32_t sampleAgeMs,
                           float maxStepKwh, uint32_t maxAgeMs) {
  return deltaKwh > 0.0f && deltaKwh <= maxStepKwh &&
         sampleAgeMs <= maxAgeMs;
}

// Sprzedaz energii = eksport przy braku produkcji PV. Falownik oddaje wtedy
// energie z baterii, wiec pompa nie moze jej zjadac. Eksport przy pracujacym
// PV to normalna autokonsumpcja i NIE jest sprzedaza.
constexpr bool saleWanted(float pvW, float gridTotalW, float pvMaxW,
                          float onW) {
  const float exportW = gridTotalW < 0.0f ? -gridTotalW : 0.0f;
  return pvW <= pvMaxW && exportW >= onW;
}

// Histereza wyjscia: trzymaj blokade, dopoki eksport jest powyzej offW.
constexpr bool saleKeep(float pvW, float gridTotalW, float pvMaxW,
                        float offW) {
  const float exportW = gridTotalW < 0.0f ? -gridTotalW : 0.0f;
  return pvW <= pvMaxW && exportW > offW;
}

// --- Asercje: przyklad uzytkownika i przypadki graniczne -------------------
// Pompa 1000 W, eksport 400 W -> 400 W autokonsumpcji, 600 W z sieci.
static_assert(split(1000.0f, -400.0f).autoW == 400.0f, "");
static_assert(split(1000.0f, -400.0f).gridW == 600.0f, "");
// Pompa 1500 W, eksport 500 W -> 500 W auto, 1000 W z sieci.
static_assert(split(1500.0f, -500.0f).autoW == 500.0f, "");
static_assert(split(1500.0f, -500.0f).gridW == 1000.0f, "");
// Brak eksportu -> calosc z sieci.
static_assert(split(1000.0f, 0.0f).autoW == 0.0f, "");
static_assert(split(1000.0f, 0.0f).gridW == 1000.0f, "");
// Import -> calosc z sieci.
static_assert(split(1000.0f, 800.0f).autoW == 0.0f, "");
static_assert(split(1000.0f, 800.0f).gridW == 1000.0f, "");
// Eksport wiekszy od pompy -> calosc z autokonsumpcji.
static_assert(split(1000.0f, -2500.0f).autoW == 1000.0f, "");
static_assert(split(1000.0f, -2500.0f).gridW == 0.0f, "");
// Pompa stoi -> nic nie przypisujemy.
static_assert(split(0.0f, -500.0f).autoW == 0.0f, "");
static_assert(split(0.0f, -500.0f).gridW == 0.0f, "");
// Proporcje.
static_assert(autoRatio(1000.0f, -400.0f) == 0.4f, "");
static_assert(autoRatio(1000.0f, 0.0f) == 0.0f, "");
static_assert(autoRatio(0.0f, -500.0f) == 0.0f, "");
// Blokady.
static_assert(pumpAllowed(false, false), "");
static_assert(!pumpAllowed(true, false), "");
static_assert(!pumpAllowed(false, true), "");
// Reset licznika PZEM.
static_assert(pzemReset(-0.002f), "");
static_assert(!pzemReset(0.0f), "");
// Limity przypisania.
static_assert(allocatable(0.01f, 2000, 0.25f, 30000), "");
static_assert(!allocatable(0.01f, 30001, 0.25f, 30000), "");
static_assert(!allocatable(0.30f, 2000, 0.25f, 30000), "");
// Sprzedaz: PV 0 W + eksport 1000 W -> blokuj.
static_assert(saleWanted(0.0f, -1000.0f, 100.0f, 1000.0f), "");
// Sprzedaz: PV 2000 W + eksport 2000 W -> to autokonsumpcja, nie blokuj.
static_assert(!saleWanted(2000.0f, -2000.0f, 100.0f, 1000.0f), "");
// Sprzedaz: PV 0 W, ale eksport tylko 500 W -> ponizej progu.
static_assert(!saleWanted(0.0f, -500.0f, 100.0f, 1000.0f), "");
// Histereza: eksport 400 W trzyma blokade przy progu wyjscia 300 W.
static_assert(saleKeep(0.0f, -400.0f, 100.0f, 300.0f), "");
static_assert(!saleKeep(0.0f, -100.0f, 100.0f, 300.0f), "");

}  // namespace SourceMix
