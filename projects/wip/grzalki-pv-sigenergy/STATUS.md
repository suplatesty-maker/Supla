# grzalki-pv-sigenergy — STATUS

## Aktualizacja 2026-11-06: v15 — suwak grzałki regulowanej, usunięte PV/ESS/SOC

**Aktualna wersja robocza: `grzalki-pv-sigenergy_v15/`. Skompilowana i
sprawdzona, nie wgrana** (brak portu USB na Macu).

Zmiany v15 względem v12:

- **Nowy kanał 9 „Grzałka regulowana (SSR)”.** Do v12 tryb MANUAL zawsze
  wymuszał 0 V na DAC, więc grzałką regulowaną nie dało się sterować z SUPLA.
  Teraz kanał 9 włącza ją na pełną moc (`ssr_max_power`, np. 10 V = 1500 W)
  po 1 s anti-chatter i wyłącza natychmiast.
- **Usunięte kanały PV, baterii ESS i SOC** wraz z odczytami Modbus
  30035–30038 i 30014. Moc sieci (kanał 5), status CT 30004 i kontrola
  on-grid 30009 zostają.
- Nowa numeracja powyżej 8: 9 = grzałka regulowana, 10 = alarm CT.
  Kanały 0–8 bez zmian.
- `Commands::heater[3]`, trzeci slot popytu; `Heating::manualSsr()` decyduje
  o SSR w MANUAL; `sameCommand()` porównuje też rewizję SSR.
- `cfg.ssrValid` (`ssr_max_power > 0`) rozdzielony od `safetyValid`, bo
  `writeSsr()` dzieli przez tę wartość, a zła konfiguracja SSR nie może
  blokować H2/H3.

Zachowanie bezpieczeństwa (bez zmian intencji): awaria DAC blokuje kanał 9
i zapala kanał 7; niepotwierdzony zapis DAC wyłącza **wszystkie** grzałki
i próbuje wyzerować wyjście.

Weryfikacja:

- kompilacja `EXIT=0`, flash 1 400 995 B (44%), RAM 52 212 B (15%),
  `--warnings all` bez ostrzeżeń własnych (tylko deprecjacje modbus-esp8266),
- `verify_source.py` **31/31 OK**,
- test decyzji na hoście (`g++`, ten sam `control_logic.h`): **18/18 OK**.
  Sprawdzone: ON po 1 s → pełna moc, ON przed upływem → 0, OFF natychmiast,
  OFF przy awarii DAC → 0, `ssr_max_power=0` → 0, AUTO nadal proporcjonalne,
  brak czujnika/przegrzanie/wygasły timer blokują SSR.

**Do zrobienia przy wgraniu:** sprawdzić `CFG ... ssrValid=1`, potem kanał 9
bez grzałki (multimetr na wyjściu DAC: 0 → ~10 V → 0). Kanały 9–12 z v12
zniknęły, więc SUPLA przydzieli numery od nowa powyżej 8.

Gotowy obraz do wgrania z laptopa: `firmware-v15-merged.bin` (1 466 688 B,
flash od 0x0). Offsety sprawdzone: bootloader `E9` @0x1000, partycje `AA50`
@0x8000, aplikacja `E9` @0x10000. Układ `huge_app`, ten sam co v11, więc
konfiguracja LittleFS zostaje. SHA256:

```
8e90fc8fd2e0af5e1bb56637f7f20bb4a68c38fe55e79f57e147f80338c7e8a8
```

```powershell
esptool --chip esp32 --port COM3 --baud 115200 write-flash 0x0 firmware-v15-merged.bin
```

## Aktualizacja 2026-11-02 (2): v12 zapisana na Macu, nie wgrana

**Aktualna wersja robocza: `grzalki-pv-sigenergy_v12/`.** v12 jest kopią v11
plus poprawka wyświetlania kanału 5. **Nie wgrana** — port USB zniknął
z systemu i nadal go nie ma (`arduino-cli board list` pokazuje tylko porty
Bluetooth i debug). Na płytce działa v11 **bez** poprawki kanału 5.

Zmiany v12 względem v11:

- `controller.h`: `publish()` wysyła ostatnią zaakceptowaną moc sieci zamiast
  `NAN`, więc kanał 5 nie gaśnie przy przełączaniu grzałek. Nieświeżość
  sygnalizuje kanał 4, a AUTO nadal wymaga świeżej próbki.
- `SuplaDevice.setName("PV heaters v12")` i baner `=== PV heaters v12 ===`,
  żeby logu i nazwy urządzenia nie można było pomylić z v11.
- Plik szkicu to `grzalki-pv-sigenergy_v12.ino`, ścieżki w README i testach
  wskazują na v12.
- Weryfikacja: kompilacja **EXIT=0** (flash 1 401 703 B / 44%,
  RAM 52 260 B / 15%), `verify_source.py` **26/26 OK**.

Wgranie, gdy port wróci:

```bash
arduino-cli upload -p /dev/cu.usbserial-XXXX \
  --fqbn esp32:esp32:esp32:PartitionScheme=huge_app,UploadSpeed=460800 \
  "/Users/serwerhome/Projects/supla-esp32-base/projects/wip/grzalki-pv-sigenergy/grzalki-pv-sigenergy_v12"
```

**Gotowy plik do wgrania z innego komputera:**
`firmware-v12-merged.bin` w katalogu projektu (1 467 408 B, obraz od adresu
0x0). Plik jest w `.gitignore`, więc nie trafi do repozytorium — trzeba go
skopiować na laptopa osobno. SHA256:

```
643816de123edc525406a75ea2e8c29d0cee204f157b07625f1910e4b9c15b38
```

Zawartość obrazu sprawdzona: bootloader pod 0x1000 (`E9`), tablica partycji
pod 0x8000 (`AA50`), aplikacja pod 0x10000 (`E9`). Schemat `huge_app`, czyli
ten sam układ partycji, na którym działa v11 — wgranie nie zmienia układu
partycji i nie kasuje konfiguracji LittleFS.

Wgranie na Windows (Lenovo, kabel USB):

```powershell
pip install esptool
esptool --chip esp32 --port COM3 --baud 115200 write-flash 0x0 firmware-v12-merged.bin
```

Po wgraniu sprawdź w Serialu baner `=== PV heaters v12 ===` i stabilny GUID.
Jeśli GUID się zmienił, urządzenie trzeba ponownie dodać w cloud.supla.org.

## Aktualizacja 2026-11-02: v11 wgrana, kanał mocy nie gaśnie przy przełączaniu

**v11 wgrana na ESP32 (port CH340, 460800, hash zweryfikowany). Grzałki 230 V
były odłączone.** Testy z obciążeniem nadal niewykonane.

- **Naprawiony błąd konfiguracji:** domyślny bufor LittleFS 1024 B był za mały
  dla v11. Log pokazywał `serialized configuration size is too big` i
  `Key authkey, curBlockSize 34, maxSize 10`, więc AuthKey nie był zapisywany
  i powstawał nowy przy każdym starcie (nowa tożsamość w SUPLA). Ustawiono
  `Supla::LittleFsConfig storage(2048)`. Po dwóch restartach GUID i AuthKey są
  stabilne, brak komunikatu o przepełnieniu. Konfiguracji nie kasowano.
- **Naprawiony wygląd kanału 5:** `publish()` wysyłał `NAN`, gdy próbka sieci
  była nieświeża. Każda zmiana trybu MANUAL i każde unieważnienie po awarii
  zwiększa generację, więc kanał „Sigenergy moc sieci” pokazywał `---` na
  kilka sekund przy każdym przełączeniu grzałki. Wyglądało to jak reset kanału
  albo kolizja. Teraz publikowana jest ostatnia zaakceptowana wartość, a
  nieświeżość sygnalizuje osobny kanał 4 („Brak danych Modbus”). Sterowanie
  AUTO nadal wymaga świeżej próbki, więc zmiana jest wyłącznie wyświetlaniem.
- Weryfikacja: `verify_source.py` **26/26 OK**, kompilacja **EXIT=0**,
  flash 1 401 703 B (44%), RAM 52 260 B (15%).
- **Uwaga:** ostatnia próba wgrania tej poprawki nie powiodła się, bo port
  `/dev/cu.usbserial-2110` zniknął z systemu przed zapisem. Poprawka jest
  skompilowana, ale na płytce działa jeszcze wersja bez zmiany wyświetlania.
- Objawy gołej płytki są poprawne: `DACfault=1` (brak GP8403), temperatura
  `-275` i `sensorFault=1` (brak DS18B20), kanał 3 i 7 w stanie alarmu.
- WiFi `HUAWEI` nie łączy się: `WiFi Station disconnected` / `sta is connecting,
  return error` w pętli. Najczęściej sieć nadaje tylko w 5 GHz; ESP32 obsługuje
  wyłącznie 2,4 GHz.
- Bez DS18B20 MANUAL jest zablokowany celowo. Do testu suwaków trzeba podłączyć
  czujnik na GPIO4.

## Aktualizacja 2026-10-28: v11, rozdzielenie MANUAL od DAC/Sigenergy

**Nowy szkic: `grzalki-pv-sigenergy_v11/`. Gałąź od v8. Nie wgrany na płytkę,
testy sprzętowe niewykonane.** W repozytorium nadal nie ma v9 ani v10 — są
tylko v1–v8. Jeśli v9/v10 istnieją na komputerze z Windows, należy je zachować
jako historię; v11 nie udaje ich kopii.

Przyczyny objawów „v9 źle czyta Sigenergy” i „restart przy MANUAL H2/H3”:

- v8 zmienił domyślną mapę z `5004/FC03` na `30005/FC04`. Rejestru 5004 nie ma
  w dokumentacji Sigenergy. Z plików nie da się ustalić, którą mapę naprawdę
  czytały v6/v7. v11 ma domyślnie `sig_map=2` (auto): najpierw próbuje plant
  30004–30009/FC04, potem 5004/FC03 i zapamiętuje pierwszą sensowną mapę.
  Wymuszenie: `sig_map=0` (plant) albo `sig_map=1` (własny rejestr).
- `dacFault` przy braku/szumie GP8403 blokował **wszystkie** grzałki, także
  H2/H3. W v11 awaria DAC wyłącza tylko SSR; H2/H3 działają w MANUAL.
- `Mailbox::demand()` odrzucał pierwsze MANUAL ON, bo `status.requestAllowed`
  jest o jeden takt 50 ms później niż zmiana trybu. v11 nie sprawdza tego pola
  przy przyjmowaniu żądania.
- DAC był zerowany co sekundę również przy pracy tylko H2/H3. Jeden szum I²C
  zatrzaskiwał `dacFault` i unieważniał żądania manualne. v11 zapisuje DAC
  tylko przy niezerowym SSR; zerowanie ponawia wyłącznie po niepotwierdzonym
  zerze.
- MANUAL używał 60 s `min_switch_s`. v11 ma 1 s anti-chatter dla MANUAL, a
  AUTO nadal 60 s.
- `ModeChannel::sync()` czyści stary MANUAL w skrzynce po wygaśnięciu timera
  lub wyjściu z trybu normalnego.

Nowe elementy:

- Blok 30004–30009: CT 30004, moc 30005/6, on-grid 30009. Osobno PV/ESS
  30035–30038 i SOC 30014. Kontrola `require_ct` i `require_ongrid`.
- Modbus: dopasowanie transaction ID, timeout biblioteki 1000 ms plus własny
  backstop 2000 ms, log surowych słów, `word_auto` z logowaniem `swapped=1`.
- Diagnostyka restartów: `esp_reset_reason()` z podpowiedziami BROWNOUT/WDT/PANIC
  oraz `heap`/`min` przy starcie i w `DIAG` co 30 s.
- Kanały 0–8 zachowane, dodane 9–12: PV, ESS, SOC, alarm CT.
- `verify_source.py`: **25/25 OK** (kontrakty strukturalne, nie test sprzętu).
- Kompilacja (2026-10-28, arduino-cli 1.3.1, esp32:esp32 3.3.3,
  `PartitionScheme=huge_app`): **EXIT=0**, flash 1 401 719 B (44% z 3 145 728),
  RAM 52 260 B (15% z 327 680). `--warnings all`: tylko ostrzeżenia biblioteki
  modbus-esp8266 o przestarzałych API, bez ostrzeżeń szkicu.
- **Nie wgrano v11. Testy BOOT/LED/AP, DAC, przekaźników, DS18B20 i Modbus
  Sigenergy niewykonane.** README v11 zawiera konfigurację `sig_map=0/1/2`,
  testy bez grzałek i ostrzeżenie o braku fizycznego odcięcia.

## Aktualizacja 2026-10-06: v8 bez MAIN, BOOT i LED konfiguracji

**Nowy szkic roboczy: `grzalki-pv-sigenergy_v8/`. V7 pozostawiono bez zmian
jako oddzielną wersję z MAIN. V8 nie wgrano na płytkę.**

- Na prośbę użytkownika usunięto sterowanie MAIN/GPIO27 oraz zależność
  szacowania mocy od stanu MAIN. GPIO27 pozostaje nieużywane.
- H2/H3: GPIO25/26, domyślnie HIGH = ON. DFR0971/GP8403: I²C 21/22,
  0x58, VOUT0 0–10 V. Numery kanałów 0–8 zachowane.
- Domyślny odczyt Sigenergy ustawiono na mapę plant v1.7: `30005/30006`,
  `FC04`, `unit=247`, `port=502`, `word_order=1`, `grid_scale=1000`.
  Klucze WWW zmieniono na `sig_grid_addr` i `sig_grid_input`, żeby nie
  odziedziczyć błędnej wartości 5004 z wcześniejszych wersji.
- Kanał mocy (nr 5) pokazuje jedną podpisaną wartość mocy sieci:
  dodatnia = pobór, ujemna = oddawanie. Bez rozdzielania na dwa kanały.
- STOP wyłącza H2/H3 i próbuje wyzerować DAC. Nieudane zerowanie jest
  ponawiane co około 1 s również przy zatrzaśniętym alarmie DAC.
  Przywrócenie transmisji nie kasuje alarmu i nie wznawia grzania.
- **Bez MAIN nie ma fizycznego odcięcia toru SSR przez sterownik**.
  Uszkodzony DAC/regulator albo zawieszony ESP32 może pozostawić grzanie.
  Wymagane niezależne sprzętowe odcięcie termiczne wszystkich grzałek.
- BOOT: przytrzymanie 5 s lub 5 szybkich kliknięć, wyłącznie wejście do
  konfiguracji. Ponowne przytrzymanie nie robi resetu fabrycznego.
  Wyjście: SAVE & RESTART albo zwykły EN/RST; bez timeoutu bezczynności.
- LED konfiguracji GPIO2, aktywna HIGH: `Supla::Device::StatusLed`,
  `LED_IN_CONFIG_MODE_ONLY`, 100 ms ON / 100 ms OFF, poza konfiguracją OFF.
  GPIO diody wymaga potwierdzenia na wariancie płytki użytkownika.
- README v8 zawiera odzyskanie dostępu na nadal wgranej v7, różnicę
  między konfiguracją a bootloaderem oraz plan testów bez zasilania grzałek.
- Walidacja źródeł: **27/27 OK**, bez dowodu działania sprzętu.
- Kompilacja v8 (2026-10-06, `huge_app`): **EXIT=0**, flash 1 394 967 B
  (44% z 3 145 728), RAM 51 964 B (15% z 327 680).
- Komputer przy sprawdzaniu nie wykrył portu ESP32. **Nie wgrano v8,
  testy BOOT/LED/AP, DAC, przekaźników i falownika niewykonane.**

## Aktualizacja 2026-10-06: v7 dla DFRobot DFR0971 / GP8403

**Wersja z MAIN: `grzalki-pv-sigenergy_v7/`. Skompilowany i wgrany
na płytkę; testy sprzętowe nadal nie wykonane.** Użytkownik potwierdził moduł
DFR0971, który używa GP8403 (12 bitów), nie GP8413 (15 bitów).

- Kodowanie DAC: 0–4095, przesunięcie o 4 bity, młodszy bajt pierwszy.
- `gp8403.h` zawiera jawne stałe protokołu i funkcje używane w sterowniku.
  Adres 0x58, kanał OUT0 (rejestr 0x02), zakres 0–10 V (0x01 = 0x11).
- Zachowano zerowanie obu kanałów podczas inicjalizacji, sprawdzanie I²C,
  zatrzask awarii DAC i architekturę bezpieczeństwa v6. Kanały SUPLA,
  GPIO, parametry WWW i reguły sterowania pozostają bez zmian.
- README v7 zawiera połączenia DFR0971 z ESP32 i plan pomiaru napięć.
- Walidacja bez kompilacji: porównanie ze źródłami DFRobot_GP8XXX 1.1.0
  oraz **20/20 poprawnych kontroli** źródeł/modelu ramek w `verify_source.py`.
  Wynik z 2026-10-06. Model w Pythonie
  nie wykonuje firmware C++ i nie zastępuje pomiarów.
- Kompilacja (2026-10-06, arduino-cli 1.5.1, esp32:esp32 3.3.3,
  `PartitionScheme=huge_app`): **EXIT=0**, flash 1 392 231 B (44% z 3 145 728),
  RAM 51 948 B (15%). Uruchomiły się wszystkie asercje `static_assert`
  (31 w `control_logic.h`, 10 w `gp8403.h`). Po drodze poprawiono kolizję
  `lowByte`/`highByte` z makrami Arduino.h — funkcje to `Gp8403::low8`
  i `Gp8403::high8`.
- Wgranie (2026-10-06, esptool 5.1.0, port COM3 = USB-SERIAL CH340, 460800):
  zapis bootloadera 0x1000, partycji 0x8000, boot_app0 0xe000 i aplikacji
  0x10000, **hash zweryfikowany**. Automatyczny reset (DTR→GPIO0) nie działa
  na tej płytce — trzeba było przytrzymać BOOT podczas wgrywania; normalny
  upload działa wtedy poprawnie.
- Log rozruchu po wgraniu: urządzenie wystartowało (SPI_FAST_FLASH_BOOT),
  LittleFS sformatowany przy pierwszym boocie, Supla w trybie CONFIG,
  AP `PV-HEATERS-V7-8C4F00AD09BC`, GUID `DF4EE095F0DEC16B87FC11B985A87C31`,
  nazwa „PV heaters v7", nowy AuthKey wygenerowany, skan WiFi działa.
  Kanały 3 (czujnik), 4 (brak Modbus), 7 (awaria DAC) i 8 (błędna konfiguracja)
  mają stan 1 — zgodne z brakiem podłączonej elektroniki i brakiem adresu
  falownika w konfiguracji. W logu widać `CONFIG ERROR: popraw IPv4/progi`,
  co jest oczekiwane na pustym urządzeniu.
- Komunikaty `task_wdt: esp_task_wdt_reset ... task not found` podczas
  formatowania LittleFS to znany, nieszkodliwy komunikat esp_littlefs.
  Self-test Supla zgłasza braki (ProductID, klucz RSA, HTTPS) — normalne dla
  niezarrejestrowanego urządzenia i trybu config.

Nie podłączać grzałek przed testami niskonapięciowymi (pomiar VOUT0 DAC,
polaryzacja przekaźników, czujnik, Modbus).
V6 i wcześniejsze wersje pozostają historią, nie wersją docelową dla DFR0971.

## Aktualizacja 2026-09-14: v6, przebudowa sterowania po przeglądzie v5

**Wersja historyczna, zastąpiona przez v7 dla DFR0971. Bez kompilacji i bez
testów sprzętowych. Nie jest jeszcze dopuszczony do podłączenia grzałek.**
Poniższe opisy v2–v5 są historyczne, również ich wcześniejsze deklaracje
„naprawienia wszystkich błędów”.

W v6:
- Jeden właściciel GPIO/DAC/DS18B20: osobne zadanie `Controller::Engine`.
  SUPLA i Modbus pozostają w pętli sieciowej, wymieniają synchronizowane kopie
  danych przez `Mailbox`; brak wspólnych flag `volatile`.
- Pomiar niezależny od TLS/WWW, limit świeżości temperatury przywrócony do 6 s.
- Awaria czujnika kasuje żądania i unieważnia timery przez generację awarii.
  Odrzucone ON nie usuwa istniejącego timera OFF.
- Timery ręcznego grzania wygasają również bez `SuplaDevice.iterate()`.
- Zgoda na zapis GPIO sprawdzana ponownie po I2C, pod blokadą, z kontrolą
  generacji/rewizji żądań. Nieudane uruchomienie zadania pozostawia OFF.
- Kontrola transmisji DAC co około 1 s podczas grzania także przy stałej mocy,
  bez resetowania epoki stabilizacji pomiaru Modbus.
- Zachowane inicjalizacja zatrzasku GPIO, timeout Modbus 300 ms i IPv4.
  Kanały H2/H3/trybu ograniczono do przełącznika zasilania z timerami jednorazowymi.
  Ustawienia WWW i kolejność kanałów 0–8 zachowane.

Walidacja: sprawdzenie API w źródłach, dwa przeglądy zakresowe oraz 16 kontroli
struktury źródeł `verify_source.py`. To NIE są testy wykonania firmware.
31 asercji `static_assert` w `control_logic.h` pozostaje do uruchomienia podczas
przyszłej kompilacji. Nie uruchamiano Arduino CLI/kompilatora ani flashowania.
Instrukcja i plan testów niskonapięciowych: `grzalki-pv-sigenergy_v6/README.md`.

## Aktualizacja 2026-09-14: v5 (poprawki po drugim przeglądzie)

**Historyczna próba poprawek, zastąpiona przez v6.** Przegląd v5 wykazał
niepełne kasowanie żądań po awarii czujnika, utratę timera OFF przy odrzuconym ON,
wyścig zapisów GPIO oraz osłabienie limitu świeżości temperatury do 30 s.
Poniżej zachowany opis ówczesnych zmian, nie potwierdzenie ich poprawności.
`settings.h` i `control_logic.h` bez zmian merytorycznych (skopiowane z v4).

Zakres zmian w `grzalki-pv-sigenergy_v5.ino`:
1. `initRelayPin()` — kolejność `pinMode(INPUT)` → `digitalWrite(OFF)` →
   `pinMode(OUTPUT)`, zgodna z API core 3.x (zatrzask OFF od pierwszej instrukcji).
2. `safetyTask` — zadanie FreeRTOS na rdzeniu 0 (co 100 ms, prio 2) odcinające
   MAIN/H2/H3 niezależnie od pętli głównej; `handleSafetyTrip()` w pętli
   porządkuje DAC, kanały i stan wewnętrzny po zadziałaniu nadzoru.
3. `HeaterChannel::clearRequest()` — kasuje też uzbrojony timer
   (`durationMs != 0`), czyli opóźnione ON wysyłane jako OFF + DurationMS.
4. `#define MODBUSIP_CONNECT_TIMEOUT 300` przed nagłówkami Modbus.
5. `TEMP_STALE_MS` 30 s (odcięcie) i `TEMP_ALARM_MS` 8 s (alarm);
   `stopAll(bool clearRequests)` — żądania kasowane tylko przy awarii
   zatrzaskowej (`latchingFault()`), nie przy przejściowej zwłoce pomiaru.
6. Walidacja adresu falownika wymaga IPv4 (`type() == IPv4`, brak `:`).
7. Dodatkowo: `HeaterChannel::turnOn()` zeruje odliczanie odrzuconego
   polecenia, żeby `iterateAlways()` nie ponawiało wygasłego timera.

**Kompilacja v5: NIE wykonana** (zgodnie z prośbą użytkownika poprawki
wprowadzono bez kompilacji). Do potwierdzenia przez kompilator: makro
`MODBUSIP_CONNECT_TIMEOUT` przed include, dostęp do chronionego `durationMs`
z klasy pochodnej, `IPAddress::type()`, `xTaskCreatePinnedToCore`, użycie
zmiennych `volatile` w wyrażeniach sterowania. Testy sprzętowe: niewykonane.

## Aktualizacja 2026-09-13: v4

**Zastąpiona przez v5 (2026-09-14). V2 i v3 zawierają potwierdzone
błędy sterowania i zabezpieczeń. Ich udana kompilacja NIE oznacza gotowości
do podłączenia grzałek.** Opisy poniżej tej sekcji są historyczne.

V4 poprawia bilans mocy, blokady temperaturowe, kontrolę transmisji GP8413,
rozdzielenie żądania i rzeczywistego stanu przekaźników, tryb konfiguracji oraz
uruchamianie WWW. Zachowuje kanały 0–6 i dodaje alarmy DAC/konfiguracji.
GUID/AUTHKEY pochodzą z LittleFS albo są generowane przez SUPLA, nie z kodu.
Rejestr Sigenergy 5004 nadal wymaga weryfikacji z dokumentacją falownika.
Strona WWW nadal nie ma uwierzytelnienia, wymaga odizolowanej, zaufanej sieci.

Instrukcja, parametry, połączenia niskonapięciowe i testy sprzętowe:
`grzalki-pv-sigenergy_v4/README.md`.

Walidacja finalnej v4 (2026-09-13):
- Arduino CLI, core ESP32 3.3.3, SuplaDevice 26.8:
  `esp32:esp32:esp32:PartitionScheme=huge_app` — **kompilacja OK**.
- Flash **1 388 511 / 3 145 728 B (44%)**, RAM **51 940 / 327 680 B (15%)**.
- 20 testów `static_assert` funkcji używanych w sterowniku — **OK**, również
  sprawdzone niezależnie kompilatorem ESP32 (`-std=c++17 -fsyntax-only`).
- Dodatkowy przegląd wykrył i usunięto gubienie ręcznego OFF/końca timera
  podczas minimalnego odstępu przełączeń. Końcowa kompilacja wykonana po poprawce.
- Testy sprzętowe, faktyczna praca WWW i falownika oraz pomiar napięcia DAC:
  **niewykonane**. Nie flashowano urządzenia.

## Aktualizacja 2026-09-13: drugi dogłębny przegląd v4

Cztery niezależne audyty (kanały SUPLA, temperatura/DAC/GPIO, Modbus,
konfiguracja WWW) + weryfikacja w źródłach bibliotek. **Poprawki wprowadzono
w v5 (2026-09-14); v4 nie nadaje się do podłączenia grzałek.**

**Błędy P1 (krytyczne):**

1. `.ino:255-257` — `digitalWrite` przed `pinMode` jest ignorowane w rdzeniu
   ESP32 3.x (`esp32-hal-gpio.c:174-177`). `pinMode(OUTPUT)` wystawia zatrzask
   LOW, więc przy logice aktywnej-niskim MAIN/H2/H3 są zasilane od `pinMode`
   do `stopAll()` na końcu `setup()` (przez cały `SuplaDevice.begin()`).
   Poprawka: `pinMode(INPUT)` → `digitalWrite(RELAY_OFF)` → `pinMode(OUTPUT)`.
2. Zawieszony handshake TLS blokuje pętlę do 120 s (`supla_srpc.cpp:2119` →
   `NetworkClientSecure` z domyślnym `handshake_timeout=120000`,
   `ssl_client.cpp:315-324`; SUPLA nigdzie go nie skraca). W tym czasie nie
   działa odcięcie temperaturowe ani kontrola świeżości odczytu — grzałki
   fizycznie grzeją dalej. Kierunek: niezależny wątek/watchdog bezpieczeństwa
   lub krótszy timeout; sprzętowy wyłącznik termiczny jako ostateczność.
3. `clearRequest()` (.ino:58-59) kasuje żądanie tylko przy `isOn()` — timer
   opóźnionego włączenia SUPLA (OFF + DurationMS) przeżywa `stopAll()` i zmianę
   trybu; po ustaniu awarii grzałka startuje bez nowego żądania (mechanizm
   potwierdzony w `relay_tests.cpp:596-608`). Poprawka: kasować timer także
   przy stanie OFF, bez planowania zapisu stanu w każdej pętli.

**Błędy P2:**

4. `mb.connect()` blokuje pętlę do 3 s przy martwym falowniku (domyślny timeout
   TCP ESP32; biblioteka ma wyłączony `MODBUSIP_CONNECT_TIMEOUT`) — opóźnia
   reakcję na OFF w trybie MANUAL.
5. Powolny klient WWW (slow-loris) blokuje `handleClient()` dłużej niż
   `TEMP_STALE_MS` (6 s) — błędne odcięcie i skasowanie ręcznych żądań grzania
   (`Parsing.cpp:44-52` resetuje budżet czasu po każdym odebranym bajcie).
   Kierunek: `TEMP_STALE_MS` ≈ 30 s; nie kasować żądań przy przejściowej zwłoce.
6. Walidacja IP falownika przepuszcza krótki literał IPv6 (np. "2001:db8::1")
   — `cfg.valid` pozostaje true, Modbus łączy się z 0.0.0.0, a alarm konfiguracji
   milczy (`IPAddress.cpp:115-120,288-293`). Poprawka: wymagać IPv4
   (`inverter.type() == IPv4` lub `uint32_t(inverter) != 0`).

**Obalone podejrzenie:** świeże urządzenie JEDNAK uruchamia AP konfiguracji —
pusty LittleFS wymusza `DEVICE_MODE_CONFIG` niezależnie od `initialMode`
(`SuplaDevice.cpp:390-392`). README zgodne z rzeczywistością.

**Zweryfikowane jako poprawne:** transakcje Modbus (epoki, bufory, ID, skala,
odrzucanie nieaktualnych), DS18B20 (CRC ROM/ramki, odrzucanie 85°C, konwersja
asynchroniczna), zapis GP8413 (adres/rejestry, latch błędu, timeout Wire),
cykl życia konfiguracji (AP na pustym urządzeniu, timeout 5 min, restart WWW,
przycisk BOOT, CSRF, zapis z restartem), kanały i alarmy SUPLA, czysta logika
`control_logic.h` (20 static_assert). Kompilacja v4 po przeglądzie: OK,
1 388 511 B (44%).

---

**Pochodzenie:** `Desktop/grzalki_supla_esp32.ino` (v1, od użytkownika, 2026-09-10).
**Wersje historyczne:** v2 = `grzalki-pv-sigenergy.ino`, v3 = `grzalki-pv-sigenergy_v3/grzalki-pv-sigenergy_v3.ino`.

## Kompilacja

- **v2:** OK, FQBN `esp32:esp32:esp32` (partycja domyślna), flash 95%.
- **v3:** OK, FQBN `esp32:esp32:esp32:PartitionScheme=huge_app`
  (domyślna partycja 1,25 MB za mała po dodaniu WWW + LittleFS).
  Flash: 1 389 391 / 3 145 728 B (44%), RAM 15%.
- Arduino CLI 1.3.1, core `esp32:esp32` 3.3.3 (Espressif).
- Biblioteki: SuplaDevice 26.8, DFRobot_GP8XXX 1.1.0, modbus-esp8266 4.1.0,
  OneWire 2.3.8, DallasTemperature 4.0.6.

Komendy:

```
arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=huge_app <ścieżka>\grzalki-pv-sigenergy_v3
arduino-cli upload -p COMx --fqbn esp32:esp32:esp32:PartitionScheme=huge_app <ścieżka>\grzalki-pv-sigenergy_v3
```

## v3 — konfiguracja przez WWW (Supla::Html + EspWebServer)

- Storage: `Supla::LittleFsConfig` (parametry + WiFi/SUPLA trwale w LittleFS).
- Strona WWW: tryb config (przycisk GPIO0, 5 s) **oraz zawsze w sieci
  lokalnej pod adresem urządzenia** (bez uwierzytelnienia — nie wystawiać
  portu poza LAN; wzorzec z przykładu xMasBulb).
- Parametry z WWW (zatwierdzone przez użytkownika 2026-09-11):
  - progi zał/wył SSR oraz grzałek 2 i 3 [W],
  - limity temperatury (limit / powrót) [°C],
  - Modbus: adres rejestru, kolejność słów 32-bit, skala,
  - IP falownika (pole tekstowe),
  - propozycje agenta: moc grzałek 2/3, pełna moc SSR, min. czas przełączeń [s].
- WiFi/serwer SUPLA: standardowe sekcje WifiParameters/ProtocolParameters
  (wartości w kodzie są tylko domyślne, realne można nadać z WWW).
- GUID/AUTHKEY pozostają w kodzie (tożsamość urządzenia).

## Błędy v1 naprawione w v2

1. **Kompilacja:** `VirtualBinary::set(bool)` nie istnieje — API ma `set()`/`clear()`.
2. **Kompilacja:** callback Modbus (`cbTransaction`) musi zwracać `bool`, v1 zwracał `void`.
3. **Sprzęt:** dwie niezależne magistrale 1-Wire na pinie 4 (własna `OneWire` +
   `Supla::Sensor::DS18B20`) — konflikt bit-bangu. v2 czyta temperaturę wyłącznie
   z kanału SUPLA (`getValue()`, `-275.0` = brak czujnika).
4. **Logika:** `digitalWrite` na pinach 25/26, których właścicielem jest
   `Supla::Control::Relay`. v2 steruje wyłącznie przez `turnOn()/turnOff()/isOn()`.
5. Potwierdzone z źródeł DFRobot_GP8XXX 1.1.0: `begin()` zwraca 0 = OK;
   `setDACOutVoltage(data, ch)` przyjmuje surowy kod 0–32767 (15-bit), nie mV.

## Do sprawdzenia na sprzęcie (nieweryfikowalne z kodu)

- Kolejność słów rejestru 32-bit: teraz parametr WWW `word_order` (1=ABCD / 0=CDAB).
- Skala: parametr WWW `grid_scale` (1000=W, 1=tysiące).
- Adres/funkcja rejestru mocy: parametr WWW `reg_addr` (ewent. `readHreg`→`readIreg`).
- Adres I2C DAC (0x58) vs zwory A0/A1/A2 na module.
- Polaryzacja przekaźników (moduły LOW-active wymagają odwrócenia logiki).

## Uwaga

GUID/AUTHKEY w pliku są zerami — wygenerować na
https://www.supla.org/arduino/get-authkey i wypełnić przed flashem.

