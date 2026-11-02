# Hydrofor v2: obsługa SUPLA i diagnostyka zawieszeń

Wersja z 2026-10-22 po zgłoszeniu zawieszania po około minucie lub przy
ręcznym włączeniu „wody miejskiej” w SUPLA. **Skompilowana, niewgrana,
bez testu sprzętowego.** V1 pozostaje niezmienioną historią.

## Co ustalono

- V1 nadzorowała watchdogiem tylko `hydro-control`. Główna pętla
  `SuplaDevice.iterate()` / WWW / zapisu stanu nie była zarejestrowana.
  Jej blokada nie musiała więc wywołać resetu, jeśli sterowanie nadal działało.
- Polecenie trybu serwisowego przekazywano dopiero po powrocie z
  `SuplaDevice.iterate()`. Zablokowanie się w dalszej obsłudze tego wywołania
  opóźniało przekazanie odebranego polecenia do zadania sterującego.
- Zadanie sterujące czytało bezpośrednio stan zegara SUPLA, aktualizowanego
  przez inne zadanie. V2 przenosi ten odczyt do głównej pętli.

**Nie odtworzono zgłoszonego zawieszenia na ESP32 i nie ustalono jego
pierwotnej przyczyny.** Brak podłączonej płytki i logu UART. Te poprawki
usuwają luki w nadzorze i przekazywaniu poleceń, ale nie dowodzą usunięcia
przyczyny blokady w bibliotece ani problemu elektrycznego.

## Zmiany względem v1

1. Watchdog obejmuje **obie pętle**. Każda potwierdza wyłącznie własny postęp.
   Wspólny limit wynosi **30 s** zamiast dotychczasowych 10 s dla samego
   sterowania, aby pozostawić czas na ograniczone czasowo operacje WiFi/TLS.
   Dłuższa blokada wywołuje panic/reset, a nie pozostaje bezterminowo niewidoczna.
   Nie jest to niezależne zabezpieczenie pompy ani gwarantowany czas odcięcia.
2. Rejestracja watchdoga jest sprawdzana. Gdy się nie powiedzie, sterowanie
   nie startuje, pompa pozostaje OFF, przekaźnik zaworu zwolniony, kanał alarmu ON.
3. `ServiceRelay` przekazuje stan w `setNewChannelValue()`, przed dalszą
   obsługą kanału/zapisu. Dotyczy to też przywracania stanu i timerów SUPLA.
4. Zegar jest odczytywany w głównej pętli raz na sekundę. Zadanie sterujące
   otrzymuje gotową godzinę przez krótką sekcję krytyczną i nie wywołuje SUPLA.
5. Kanały aktualizowane są raz na nową próbkę sterowania, zamiast wielokrotnie
   przetwarzać tę samą próbkę w pętli. Częstotliwość wysyłki kanału ciśnienia
   do SUPLA nie ulega zmianie.
6. UART 115200: przy starcie przyczyna resetu i ostatni etap głównej pętli;
   co 10 s licznik próbek, stan sterownika i wolna pamięć.

## Co pozostaje bez zmian

- GPIO34: czujnik przez dzielnik; GPIO26: pompa; GPIO25: zawór.
- Odczyt: 8 próbek co około 50 ms, średnia z 20 cykli; sterowanie co około 250 ms.
- Kalibracja, progi, harmonogram, suchobieg i czasy regeneracji.
- Numery kanałów, GUID, backendy EEPROM/LittleFS i klucze ustawień.
- Kanał 1 to nadal **SERWIS: pompa STOP + woda miejska**, nie sam zawór.
  Wyłączenie serwisu przywraca automatykę i kasuje blokadę jak w v1.
- Po resecie obowiązuje dotychczasowa 2-minutowa blokada startu pompy.
  Stan kanału 1 jest zapisywany z opóźnieniem biblioteki, więc reset tuż po
  kliknięciu może przywrócić poprzedni zapisany stan. V2 tego nie zmienia.

Opis hydrauliki i parametrów: [README v1](../hydrofor-esp32-supla_v1/README.md).
Pliki `hydro_logic.h`, `settings.h`, `hydro_tests.h`, `notify.h` i
`status_page.h` są identyczne z v1.

## Walidacja (2026-10-22)

- Bazowa v1: kompilacja EXIT=0, flash 1 371 059 B, RAM 50 644 B.
- V2: ESP32 core 3.3.3, SuplaDevice 26.8, arduino-cli 1.3.1;
  `--warnings all`, EXIT=0, brak ostrzeżeń;
  **flash 1 372 579 B (43%), RAM 50 668 B (15%)**.
- Wszystkie dotychczasowe `static_assert` przechodzą.
- Testy hostowe clang++ z AddressSanitizer i UndefinedBehaviorSanitizer:
  PASS, bez zgłoszonych błędów. Sprawdzają:
  - model watchdoga: blokada każdej z pętli, inicjalizacja i błędy rejestracji;
  - przekazanie polecenia przed callbackiem kanału;
  - 30 minut **symulowanego** przełączania ON/OFF, również po pierwszej minucie;
  - zatrzymanie aktywnej porcji bez wielokrotnego naliczania jej końca;
  - współbieżne aktualizacje poleceń, czasu i spójnych odczytów statusu.

Stuby testowe nie zastępują rzeczywistego FreeRTOS, połączenia SUPLA ani
zapisu flash. Test działania z serwerem, utraty WiFi, restartu i załączania
przekaźników **pozostaje do wykonania na płytce**.

Kompilacja (z tego folderu):

```sh
arduino-cli compile --jobs 4 --warnings all \
  --fqbn esp32:esp32:esp32:PartitionScheme=huge_app .
```

Testy hostowe (z folderu projektu `hydrofor-esp32-supla`):

```sh
clang++ -std=c++17 -Wall -Wextra -Werror -pedantic -pthread \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -fconstexpr-steps=10000000 -I tests/stubs tests/runtime_tests.cpp \
  -o /tmp/hydrofor-v2-runtime-tests
/tmp/hydrofor-v2-runtime-tests
```

## Test na płytce

1. Najpierw **bez podłączonej części 230 V**, na stabilnym zasilaniu.
   Wgrywać bez kasowania flash i z tym samym układem partycji `huge_app`.
   Na tej płytce używać `UploadSpeed=115200`.
2. Monitor UART 115200 uruchomić przed resetem. Sprawdzić `HYDRO v2 boot`,
   a następnie `HYDRO alive` co 10 s. `seq` powinno rosnąć, `taskFail=0`.
3. Włączyć/wyłączyć kanał 1 po około minucie, następnie wielokrotnie po
   upływie 2 min. ON: pompa OFF, zawór CITY; OFF: powrót do automatyki.
   Ciśnienie i `seq` mają zmieniać się także przez co najmniej 10 min w serwisie.
4. Zostawić stan ON na co najmniej 10 s, zrestartować i sprawdzić odtworzenie.
5. Otwierać WWW podczas przełączania; wyłączyć i przywrócić WiFi.
   Brak połączenia sam w sobie nie wyłącza automatyki.
6. Gdy wystąpi reset/zawieszenie, zachować `HYDRO`, `Task watchdog`,
   `Guru Meditation`, `Backtrace` i `Brownout`. Logi biblioteki mogą zawierać
   dane sieciowe: przed udostępnieniem usunąć hasła, identyfikatory i adresy.
7. Dopiero po teście niskonapięciowym sprawdzać przekaźniki/siłownik
   z elektrykiem, z presostatem i ochroną instalacji.

`previous_stage`: 0 setup, 1 SUPLA (obejmuje też obsługę WWW i zapisy w
`iterate()`), 2 zegar, 3 synchronizacja kanałów, 4 powiadomienia,
5 uruchamianie WWW, 6 koniec pętli/diagnostyka, −1 brak ważnego zapisu.
To wskazówka, nie pełny backtrace. `reset` to liczbowy `esp_reset_reason_t`
(m.in. 6 task watchdog, 9 brownout).

**Uwaga sprzętowa:** zawieszenie przy zmianie przekaźnika może wynikać
z zakłóceń lub zasilania, nie tylko z kodu. Wejścia modułów muszą być zgodne
z logiką 3,3 V. Nie podawać 5 V na GPIO, także przez lampkę lub podciąganie IN.
Wcześniejszy opis LED „od +5 V do IN” nie jest bezpiecznym uniwersalnym
połączeniem z ESP32 i nie należy go stosować bez odpowiedniego interfejsu.
Zwolnienie przekaźnika wybiera NC; przejazd zaworu na wodę miejską wymaga
sprawnego siłownika i obecności jego zasilania 230 V.
