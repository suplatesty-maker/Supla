
/*
 * Sterownik hydroforu ESP32 - Wojtek
 * Logika:
 * - Noc (23:00-6:00): 3 zaplanowane dobicia do 4 bar (godz. 23:00, 01:00, 04:00)
 * - Dzien: histereza dwuetapowa
 *     start przy 2.0 bar -> dobij do 3.0 bar -> STOP
 *     czekaj 1h -> jesli cisnienie wciaz <= 3.0 bar -> dobij do 4.0 bar
 *     jesli w trakcie oczekiwania cisnienie spadnie znow do 2.0 bar -> priorytet: dobij do 3.0 bar
 * - Detekcja suchobiegu: podczas kazdego pompowania, po starcie mierzony jest trend
 *   cisnienia w oknie 15s. Jesli przyrost < MIN_RISE -> STOP pompy, retry po 1h.
 * - Zasilanie czujnika: 5V (pelny zakres), sygnal przez dzielnik napiecia do ADC (3.3V max)
 */

#include <WiFi.h>
#include "time.h"

// ---------- KONFIGURACJA WiFi / NTP ----------
const char* ssid     = "TWOJA_SIEC";
const char* password = "TWOJE_HASLO";
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 3600;      // CET
const int   daylightOffset_sec = 3600; // CEST

// ---------- PINY ----------
const int PIN_SENSOR   = 34;   // ADC1_CH6, tylko wejscie analogowe
const int PIN_PUMP_RLY = 26;   // przekaznik pompy
const bool PUMP_ACTIVE_HIGH = true;

// ---------- KALIBRACJA CZUJNIKA ----------
// Czujnik 0-150psi (~0-10.3 bar), wyjscie 0-5V, zasilanie 5V
// Dzielnik napieciowy np. 4.7k / 8.2k -> Vadc = Vsensor * 8.2/(4.7+8.2) = 0.636 * Vsensor
// Kalibracja dwupunktowa: zmierz ADC przy 0 bar (atmosfera) i przy znanym cisnieniu referencyjnym
float ADC_AT_0BAR   = 0.30;    // [V] po dzielniku, do skalibrowania na realnym czujniku
float ADC_AT_10BAR  = 3.10;    // [V] po dzielniku, przy ~10 bar (na czujniku manometrycznym)
const float ADC_MAX_V = 3.3;
const int   ADC_RES   = 4096;

// ---------- PROGI CISNIENIA [bar] ----------
const float PRESSURE_LOW_DAY    = 2.0;  // dzien: start pompy
const float PRESSURE_MID_DAY    = 3.0;  // dzien: etap 1 - stop po dobiciu
const float PRESSURE_HIGH       = 4.0;  // docelowe cisnienie (dzien etap 2 / noc)
const float PRESSURE_NIGHT_TARGET = 4.0;

// ---------- CZASY NOCNYCH DOBIC ----------
const int nightHours[] = {23, 1, 4};
const int nightMinutes[] = {0, 0, 0};
const int NIGHT_SLOTS = 3;

// ---------- DETEKCJA SUCHOBIEGU ----------
const unsigned long TREND_WINDOW_MS = 15000;  // 15s okno pomiaru trendu
const float MIN_RISE_BAR = 0.05;              // minimalny wymagany przyrost w oknie, by uznac ze pompa dziala
const unsigned long DRY_RUN_RETRY_MS = 3600000UL; // 1h odstep po wykryciu suchobiegu
const unsigned long MID_STAGE_WAIT_MS = 3600000UL; // 1h odczekania miedzy etapem 2->3 i 3->4 bar

// ---------- STANY MASZYNY ----------
enum PumpState {
  IDLE,
  PUMPING,
  WAIT_MID_STAGE,
  DRY_RUN_COOLDOWN
};

PumpState state = IDLE;
float targetPressure = 0;
unsigned long pumpStartTime = 0;
float pressureAtPumpStart = 0;
unsigned long stateChangeTime = 0;
bool nightSlotDone[NIGHT_SLOTS] = {false, false, false};
int lastNightResetDay = -1;

// ---------- FUNKCJE POMOCNICZE ----------

float readPressureBar() {
  int raw = analogRead(PIN_SENSOR);
  float v = (raw / (float)ADC_RES) * ADC_MAX_V;
  // interpolacja liniowa miedzy punktami kalibracji (0 bar .. 10 bar)
  float bar = (v - ADC_AT_0BAR) * (10.0 / (ADC_AT_10BAR - ADC_AT_0BAR));
  if (bar < 0) bar = 0;
  return bar;
}

float readPressureAveraged(int samples = 10, int delayMs = 5) {
  float sum = 0;
  for (int i = 0; i < samples; i++) {
    sum += readPressureBar();
    delay(delayMs);
  }
  return sum / samples;
}

void pumpOn() {
  digitalWrite(PIN_PUMP_RLY, PUMP_ACTIVE_HIGH ? HIGH : LOW);
}

void pumpOff() {
  digitalWrite(PIN_PUMP_RLY, PUMP_ACTIVE_HIGH ? LOW : HIGH);
}

bool getTimeNow(struct tm &timeinfo) {
  return getLocalTime(&timeinfo);
}

bool isNightWindow(struct tm &t) {
  // noc = 23:00 - 6:00
  return (t.tm_hour >= 23 || t.tm_hour < 6);
}

// sprawdza, czy nadeszla pora jednego z 3 nocnych slotow i jeszcze nie zostal wykonany
int getPendingNightSlot(struct tm &t) {
  for (int i = 0; i < NIGHT_SLOTS; i++) {
    if (!nightSlotDone[i] && t.tm_hour == nightHours[i] && t.tm_min >= nightMinutes[i]) {
      return i;
    }
  }
  return -1;
}

void resetNightSlotsIfNewDay(struct tm &t) {
  if (t.tm_hour == 12 && lastNightResetDay != t.tm_yday) {
    // reset w poludnie, przed noca
    for (int i = 0; i < NIGHT_SLOTS; i++) nightSlotDone[i] = false;
    lastNightResetDay = t.tm_yday;
  }
}

void startPumping(float target) {
  targetPressure = target;
  pressureAtPumpStart = readPressureAveraged();
  pumpStartTime = millis();
  pumpOn();
  state = PUMPING;
  Serial.print("Start pompy, cel: ");
  Serial.print(target);
  Serial.print(" bar, cisnienie startowe: ");
  Serial.println(pressureAtPumpStart);
}

void stopPumping(const char* reason) {
  pumpOff();
  Serial.print("Stop pompy: ");
  Serial.println(reason);
}

// ---------- SETUP ----------

void setup() {
  Serial.begin(115200);
  pinMode(PIN_PUMP_RLY, OUTPUT);
  pumpOff();

  WiFi.begin(ssid, password);
  Serial.print("Laczenie z WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.println(" OK");

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  struct tm timeinfo;
  int tries = 0;
  while (!getLocalTime(&timeinfo) && tries < 20) {
    delay(500);
    tries++;
  }
  Serial.println("Czas zsynchronizowany.");
}

// ---------- LOOP ----------

void loop() {
  struct tm t;
  if (!getTimeNow(t)) {
    delay(1000);
    return;
  }

  resetNightSlotsIfNewDay(t);
  float currentPressure = readPressureAveraged();
  bool night = isNightWindow(t);
  unsigned long now = millis();

  switch (state) {

    case IDLE: {
      if (night) {
        int slot = getPendingNightSlot(t);
        if (slot >= 0 && currentPressure < PRESSURE_NIGHT_TARGET) {
          nightSlotDone[slot] = true;
          startPumping(PRESSURE_NIGHT_TARGET);
        }
      } else {
        // dzien: nadrzedny warunek - spadek do 2 bar zawsze uruchamia dobicie do 3 bar
        if (currentPressure <= PRESSURE_LOW_DAY) {
          startPumping(PRESSURE_MID_DAY);
        }
      }
      break;
    }

    case PUMPING: {
      unsigned long elapsed = now - pumpStartTime;

      if (currentPressure >= targetPressure) {
        stopPumping("cel osiagniety");
        if (!night && targetPressure == PRESSURE_MID_DAY) {
          // etap 1 dzienny zakonczony, zaplanuj etap 2 po 1h
          state = WAIT_MID_STAGE;
          stateChangeTime = now;
        } else {
          state = IDLE;
        }
        break;
      }

      if (elapsed >= TREND_WINDOW_MS) {
        float rise = currentPressure - pressureAtPumpStart;
        if (rise < MIN_RISE_BAR) {
          stopPumping("wykryto suchobieg (brak przyrostu cisnienia)");
          state = DRY_RUN_COOLDOWN;
          stateChangeTime = now;
        } else {
          // trend jest dobry, kontynuuj pompowanie, resetuj okno pomiaru
          pressureAtPumpStart = currentPressure;
          pumpStartTime = now;
        }
      }

      // dzienny priorytet: jesli podczas oczekiwania na etap 2 spadnie znow do 2 bar,
      // to i tak jestesmy w PUMPING wiec nic dodatkowego nie trzeba robic
      break;
    }

    case WAIT_MID_STAGE: {
      // priorytet nadrzedny: jesli w trakcie oczekiwania spadnie do 2 bar, dobij ponownie do 3 bar
      if (!night && currentPressure <= PRESSURE_LOW_DAY) {
        startPumping(PRESSURE_MID_DAY);
        break;
      }
      if (now - stateChangeTime >= MID_STAGE_WAIT_MS) {
        if (currentPressure <= PRESSURE_MID_DAY) {
          startPumping(PRESSURE_HIGH);
        } else {
          state = IDLE;
        }
      }
      break;
    }

    case DRY_RUN_COOLDOWN: {
      if (now - stateChangeTime >= DRY_RUN_RETRY_MS) {
        state = IDLE;
        Serial.println("Koniec cooldown po suchobiegu, ponowna proba dozwolona.");
      }
      break;
    }
  }

  delay(500);
}
