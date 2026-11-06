# grzalki-pv-sigenergy v15

Wersja robocza po przeglądzie v1–v8. **Nie jest to wersja przetestowana na
grzałkach.** Przed podłączeniem odbiorników wykonaj testy niskonapięciowe z
punktu „Testy bez grzałek”.

## Co zmieniło się w v15 względem v12

- **Grzałka regulowana ma własny suwak w SUPLA (kanał 9).** Do v12 tryb MANUAL
  zawsze wymuszał 0 V na DAC, więc grzałką regulowaną nie dało się sterować
  z aplikacji. Od v15 kanał 9 włącza ją na pełną moc (`ssr_max_power`) i
  wyłącza natychmiast. Szczegóły w rozdziale „Grzałka regulowana (SSR)
  w MANUAL”.
- **Usunięte kanały Sigenergy: PV, bateria ESS i SOC.** Zniknęły też ich
  odczyty Modbus (30035–30038, 30014). Moc sieci, status CT i praca on-grid
  zostają bez zmian. Mniej ruchu po Modbus TCP i mniej kanałów w aplikacji.
- **Nowa numeracja powyżej 8:** 9 = grzałka regulowana, 10 = alarm CT.
  Kanały 0–8 zachowują numery.
- Logika MANUAL/AUTO, mapy Sigenergy, auto-wykrywanie rejestru i blokady
  bezpieczeństwa są bez zmian względem v12.

## Co zmieniło się w v12 względem v11

- **Kanał 5 nie gaśnie przy przełączaniu grzałek.** v11 wysyłał `NAN`, gdy
  próbka sieci była nieświeża. Każda zmiana trybu MANUAL unieważnia próbkę,
  więc kanał „Sigenergy moc sieci” pokazywał `---` na kilka sekund przy każdym
  przełączeniu. W aplikacji wyglądało to jak reset kanału albo kolizja
  z Modbusem. v12 publikuje **ostatnią zaakceptowaną** wartość, a nieświeżość
  nadal sygnalizuje kanał 4 („Brak danych Modbus”). AUTO nadal nie grzeje bez
  świeżej próbki, więc zmiana dotyczy tylko wyświetlania.
- **Nazwa urządzenia i baner startowy to v12.** Log zaczyna się od
  `=== PV heaters v12 ===`, a nazwa w SUPLA to `PV heaters v12`, więc nie da
  się pomylić z v11. (W v15 oba napisy zmieniono na v15.)
- Poza tym v12 jest identyczna z v11. Wszystko poniżej opisuje zachowanie
  odziedziczone po v11.

## Co było nie tak w v6–v8

### 1. Odczyt Sigenergy: 5004/FC03 vs 30005/FC04

W repozytorium nie ma v9 ani v10 — są tylko v1–v8. Ostatnia wersja, v8,
domyślnie czyta rejestr **30005/30006** przez **FC04**, unit **247**,
skala **1000** (raw = kW × 1000, czyli W). To jest mapa z dokumentacji
Sigenergy: `[Grid sensor] Active power`, S32, dodatnia = pobór, ujemna =
oddawanie.

Wcześniejsze wersje (v3–v7) miały w kodzie **5004** i **FC03**. Rejestru 5004
nie ma w dokumentacji Sigenergy. Z samych plików nie da się ustalić, czy v6/v7
czytały 5004, czy 30005 wpisane ręcznie w WWW. Dlatego v15 domyślnie działa
w trybie **auto**: najpierw próbuje plant 30005/FC04, a jeśli odpowiedź jest
błędna albo nierealna, próbuje 5004/FC03. Pierwsza mapa, która da sensowną
moc, zostaje zapamiętana do restartu. W logu pojawi się:

```
SIG AUTO: wybrano mape 0 (30005/FC04)
```

albo:

```
SIG AUTO: wybrano mape 1 (5004/FC03)
```

Jeśli auto wybierze złą mapę, wpisz `sig_map=0` albo `sig_map=1` na sztywno.
Tryb `sig_map=0` daje dodatkowo status CT 30004 i kontrolę pracy on-grid 30009.
Tryb `sig_map=1` czyta tylko dwa rejestry mocy, jak v6/v7, więc status CT nie
jest znany (log pokazuje `ct=0`, kontrola `require_ct` nie działa).

> W v12 i starszych tryb plant czytał też moc PV, moc baterii i SOC. **v15 tych
> odczytów nie wykonuje** — rejestry 30035–30038 i 30014 nie są już pytane.

Dodatkowo (od v11):

- sprawdza **status CT 30004** (0 = brak przekładnika, 1 = podłączony),
- sprawdza **pracę on-grid 30009** (0 = on-grid; 1/2 = off-grid),
- czyta cały blok 30004–30009 jednym żądaniem, więc moc i statusy pochodzą
  z tego samego pomiaru,
- dopasowuje **transaction ID** odpowiedzi Modbus (stara odpowiedź nie może
  zakończyć nowego żądania),
- ma **timeout**: biblioteka modbus-esp8266 kończy żądanie po 1000 ms, a szkic
  ma dodatkowy backstop 2000 ms na wypadek braku callbacku,
- loguje surowe słowa `raw=HHHH:HHHH` i wynik transakcji, więc można sprawdzić,
  która mapa odpowiedziała i z jaką wartością,
- potrafi automatycznie zamienić kolejność słów, jeśli wartość jest nierealna
  (`word_auto=1`), ale loguje `swapped=1`.

### 2. Ręczne H2/H3: dlaczego nie działało lub restartowało

W v6–v8 były co najmniej cztery niezależne problemy:

1. **`dacFault` blokował wszystko.** Brak lub szum GP8403 ustawiał zatrzask
   awarii przy starcie. Reakcja łańcuchowa blokowała także H2/H3, mimo że
   przekaźniki nie potrzebują DAC. W v11 awaria DAC wyłącza **tylko SSR**;
   H2/H3 działają dalej w MANUAL.
2. **Pierwsze naciśnięcie po włączeniu MANUAL było odrzucane.** `demand()`
   sprawdzał `status.requestAllowed`, który jest publikowany przez zadanie
   sterownika o jeden takt (50 ms) później niż zmiana trybu. v11 nie sprawdza
   tego pola przy przyjmowaniu żądania; bezpieczeństwo egzekwuje zadanie
   sterownika.
3. **DAC był zerowany co sekundę, także gdy pracowały tylko H2/H3.** Jeden
   zakłócony transfer I²C ustawiał `dacFault` i unieważniał żądania manualne.
   v11 zapisuje DAC tylko wtedy, gdy SSR ma faktycznie niezerową moc; zerowanie
   jest ponawiane wyłącznie po niepotwierdzonym zerze.
4. **MANUAL używał 60-sekundowego `min_switch_s`.** Pierwsze włączenie po
   zmianie trybu wyglądało jak ignorowane. W v11 MANUAL ma 1 s opóźnienia
   przeciw drganiom, a AUTO nadal używa `min_switch_s` (domyślnie 60 s).

Dodatkowo `ModeChannel::sync()` czyści stary stan MANUAL w skrzynce, gdy timer
trybu wygasł albo urządzenie wyszło z trybu normalnego. Dzięki temu aplikacja
nie pokazuje „MANUAL ON”, gdy sterownik już tego nie realizuje.

### 3. Restarty

Kod nie może zagwarantować, że „restart” to reset ESP32, a nie np. utrata WiFi
albo ponowne połączenie SUPLA. Dlatego v15 wypisuje przy starcie:

```
=== PV heaters v15 ===
Reset reason: 3 (TASK_WDT), heap=... min=...
```

oraz co 30 s:

```
DIAG heap=... min=... h2=0 h3=1 ssr=0 temp=42.1 sensorFault=0 grid=1 850.0W ...
OUT H2=0 H3=1 SSR=0W DACfault=0 gen=...
```

Jeśli po zdarzeniu w logu nadal jest `POWERON`, ESP32 **nie** został
zresetowany — to była utrata WiFi/SUPLA albo zerwanie połączenia, a nie restart
procesora. Dopiero `BROWNOUT`, `TASK_WDT`, `PANIC` itd. oznaczają reset.

Jeśli zobaczysz `BROWNOUT`, to problem zasilania, a nie logiki:

- osobny zasilacz 5 V dla cewek przekaźników,
- wspólna masa ESP32 i przekaźników,
- kondensator 1000 µF przy zasilaniu ESP32,
- diody gaszące na cewkach (jeśli moduł ich nie ma),
- przewody zasilania i masy grubsze niż sygnałowe,
- nie zasilaj cewek z pinów 3.3 V ESP32.

Jeśli zobaczysz `TASK_WDT` lub `INT_WDT`, sprawdź I²C (GP8403), 1-Wire
(DS18B20) i sekcje krytyczne. v11 skrócił część blokad, ale nie zastąpi
poprawnego okablowania.

## Konfiguracja WWW

Po wejściu w tryb konfiguracji (przytrzymanie BOOT 5 s albo 5 kliknięć)
ustaw:

| Klucz | Domyślnie | Znaczenie |
|---|---:|---|
| `sig_map` | `2` | `2` = auto: najpierw plant 30005/FC04, potem 5004/FC03; pierwsza sensowna mapa zostaje zapamiętana. `0` = tylko plant, `1` = własny rejestr. |
| `sig_grid_addr` | `30005` | Używany tylko przy `sig_map=1`; auto dla mapy 1 używa 5004. |
| `sig_grid_input` | `1` | `1` = FC04 (input), `0` = FC03 (holding). Tylko `sig_map=1`; auto dla mapy 1 używa FC03. |
| `word_order` | `1` | `1` = starsze słowo pierwsze, `0` = zamienione. |
| `word_auto` | `1` | Automatyczna korekta kolejności słów przy nierealnej wartości. |
| `grid_scale` | `1000` | W = raw × `grid_scale` / 1000. Dla 30005 (raw kW×1000) zostaw 1000. |
| `require_ct` | `1` | Wymagaj 30004 = 1. `0` tylko świadomie, jeśli nie ma przekładnika. |
| `require_ongrid` | `1` | Wymagaj 30009 = 0 (praca on-grid). |
| `min_switch_s` | `60` | Minimalny odstęp przełączeń H2/H3 w AUTO. |
| `mb_unit` | `247` | Unit ID falownika (plant = 247). |
| `mb_port` | `502` | Port Modbus TCP. |
| `inverter_ip` | — | IPv4 falownika; bez tego AUTO nie działa, MANUAL działa. |
| `ssr_on` / `ssr_off` | `200` / `80` | Progi załączania/wyłączania SSR [W]. |
| `h2_on` / `h2_off` | `1700` / `1450` | Progi H2 [W]. |
| `h3_on` / `h3_off` | `3200` / `2950` | Progi H3 [W]. |
| `temp_limit` / `temp_reset` | `80` / `75` | STOP i powrót [°C]. |
| `heater_power` | `1500` | Moc jednej grzałki H2/H3 do szacowania obciążenia [W]. |
| `ssr_max_power` | `1500` | Moc SSR przy 10 V [W]. |

### Jak odtworzyć zachowanie v6/v7

Jeśli chcesz sprawdzić starą mapę:

```
sig_map = 1
sig_grid_addr = 5004
sig_grid_input = 0     # FC03, tak jak w v6/v7
word_order = 1
word_auto = 1
grid_scale = 1000
```

Jeśli 5004 nadal nie daje sensownych wartości, wpisz `30005` i `sig_grid_input=1`.
To jest mapa zgodna z dokumentacją. Nie zmieniaj obu naraz; po każdej zmianie
zapisz i zrestartuj urządzenie.

Jeśli `sig_map=2` nie wybierze żadnej mapy, w logu będą na przemian
`SIG AUTO: mapa 0 bez danych` i `mapa 1 bez danych`. Sprawdź IP, port, unit
i czy Modbus TCP jest włączony w falowniku. Jeśli `sig_map=0` kończy się
ciągłym `SIG GRID timeout`, a v8 czytał 30005, ustaw `sig_map=1`,
`sig_grid_addr=30005`, `sig_grid_input=1`. Tryb `sig_map=1` czyta tylko dwa
rejestry mocy, tak jak v8, więc nie sprawdza CT 30004 ani on-grid 30009.
To dobra ścieżka awaryjna, ale tracisz wtedy kontrolę statusu przekładnika.

### Co oznaczają komunikaty SIG

Kanał 5 pokazuje **ostatnią zaakceptowaną** wartość mocy sieci. Jeśli próbka
jest nieświeża, wartość zostaje na ekranie, a kanał 4 („Brak danych Modbus”)
przechodzi w alarm. Wcześniej program wysyłał `NAN`, więc kanał pokazywał `---`
na kilka sekund przy każdym przełączeniu MANUAL i wyglądało to jak reset kanału.

```
SIG GRID map=2 req=0 active=0 result=0 raw=0000:01F4 status=1 ongrid=0 grid=500.0W ct=1 on=1 swapped=0 age=12ms
```

- `raw=0000:01F4` = 0x000001F4 = 500; przy skali 1000 daje 500 W.
- `req` = mapa użyta w tym żądaniu; `active` = mapa zapamiętana przez auto
  (`-1` = jeszcze nie wybrano).
- `status=1` = CT podłączony; `ongrid=0` = falownik on-grid.
- Przy mapie 1 (`req=1`) `status=0` i `ongrid=0` są wartościami zastępczymi;
  ta mapa nie czyta rejestrów statusu.
- `grid` dodatnie = pobór, ujemne = oddawanie.
- `swapped=1` = użyto odwrotnej kolejności słów.
- `age` = wiek próbki w chwili logu.

Jeśli `status=0`, AUTO jest zablokowane. To nie jest błąd programu: falownik nie
widzi przekładnika. Sprawdź CT, adres i `require_ct`. Jeśli `ongrid=1` lub `2`,
falownik pracuje wyspowo i nie ma sensu sterować grzałkami z nadwyżki.

## Kanały SUPLA

Numeracja 0–8 jest zachowana z v8. Kanały 9–12 z v12 (PV, ESS, SOC, CT)
zostały zredukowane do dwóch:

| Nr | Kanał |
|---:|---|
| 0 | Temperatura zbiornika |
| 1 | Grzałka 2 (H2, GPIO25) |
| 2 | Grzałka 3 (H3, GPIO26) |
| 3 | Alarm temperatury/czujnika |
| 4 | Alarm braku danych Modbus |
| 5 | Moc sieci Sigenergy (+pobór/−eksport) |
| 6 | MANUAL ON / AUTO OFF |
| 7 | Awaria DAC (SSR) / zadania |
| 8 | Konfiguracja AUTO / IP |
| 9 | **Grzałka regulowana (SSR)** — nowy suwak włącz/wyłącz |
| 10 | Alarm CT / grid sensor |

**Uwaga przy aktualizacji z v12:** kanały 9–12 (PV, ESS, SOC, CT) zniknęły.
Po wgraniu v15 SUPLA przydzieli numery od nowa powyżej 8. Kanał 9 to teraz
grzałka regulowana, a nie moc PV. Jeśli w cloud.supla.org była historia albo
skrypty odwołujące się do starych numerów, trzeba je poprawić.

## Grzałka regulowana (SSR) w MANUAL

Do v12 tryb MANUAL zawsze wymuszał 0 V na DAC, więc grzałką regulowaną nie dało
się sterować z aplikacji. Od v15 kanał 9 działa jak pozostałe grzałki:

- **ON** — po ok. 1 s (anti-chatter) DAC dostaje pełne napięcie, czyli
  `ssr_max_power`, np. 10 V = 1500 W.
- **OFF** — natychmiast, bez czekania na czas.
- W MANUAL grzałka regulowana chodzi **na pełnej mocy**, a nie proporcjonalnie
  do nadwyżki. Regulacja proporcjonalna działa tylko w AUTO.
- Kanał 9 pokazuje **rzeczywisty stan DAC** (`ssrWatts > 0`), a nie stan
  przycisku. Jeżeli DAC jest uszkodzony, kanał zostanie OFF.
- **Awaria DAC blokuje kanał 9.** `writeSsr()` dzieli przez `ssr_max_power`,
  więc przy `ssr_max_power=0` kanał też jest zablokowany. Log poda
  `SSR ON rejected ... powerOk=0`.
- Jeżeli zapis DAC nie zostanie potwierdzony, silnik wyłącza **wszystkie**
  grzałki (także H2/H3) i próbuje wyzerować DAC. To celowe: niepotwierdzony
  DAC może nadal grzać.
- W AUTO kanał 9 pokazuje, co robi regulator. Wciśnięcie go w AUTO nic nie
  zmienia — AUTO ma własną logikę nadwyżki.

## Testy bez grzałek

1. **Nie podłączaj grzałek.** Zmierz napięcia i sprawdź logikę na przekaźnikach
   lub diodach LED zamiast odbiorników.
2. Wgraj v15, otwórz Serial 115200. Sprawdź `Reset reason`, `CFG`, `START`.
3. Ustaw `inverter_ip`, zapisz, zrestartuj. Sprawdź, czy pojawia się
   `SIG GRID ... raw=...` i czy `grid` zmienia znak przy eksporcie/poborze.
4. Sprawdź `status` i `ongrid`. Przy mapie plant (auto wybrało 0) `status=0`
   blokuje AUTO. Przy mapie 1 statusy nie są czytane (log pokazuje 0), ale AUTO
   działa na podstawie dwóch rejestrów mocy.
5. Włącz MANUAL, potem H2. W logu powinno być:
   `MODE MANUAL ON`, `H2 ON request`, `OUT H2=1`.
   Jeśli jest `H2 ON rejected ...`, log poda przyczynę (`sensorFault`, `hot`,
   `manual=0`, `op=0`, `expired=1`).
6. Włącz H3 przy H2. Sprawdź, że nie ma resetu i że `OUT H2=1 H3=1`.
7. Sprawdź kanał 9 bez grzałki: włącz go i zmierz multimierzem napięcie na
   wyjściu DAC (0–10 V). Powinno pojawić się ok. 10 V po 1 s, a po wyłączeniu
   spaść do 0 V. W logu: `SSR ON request`, potem `OUT ... SSR=1500W`.
8. Sprawdź SSR w AUTO: przy braku nadwyżki DAC ma 0 V; przy nadwyżce napięcie
   rośnie. **Nie podłączaj grzałki SSR bez obciążenia rezystancyjnego i
   bezpiecznika.**
9. Zostaw urządzenie na 30 min i sprawdź `DIAG heap`, `min`, brak `BROWNOUT`.

## Bezpieczeństwo

- **Brak fizycznego odcięcia.** v15 nie zastępuje termostatu ani bezpiecznika.
  Zespawane styki przekaźnika, zwarcie SSR albo zawieszony ESP32 mogą pozostawić
  grzanie. Wymagane niezależne zabezpieczenie termiczne wszystkich grzałek.
- **SSR w MANUAL to pełna moc.** Kanał 9 nie jest regulatorem — włącza grzałkę
  na 100% `ssr_max_power`. Nie używaj go do grzania „trochę”.
- **SSR sterowany 0–10 V** może pozostać załączony przy uszkodzonym DAC.
  v15 próbuje zerować DAC i zgłasza alarm, ale to nie jest odcięcie.
- **H2/H3 to przekaźniki.** Sprawdź, czy moduł jest aktywny HIGH, czy LOW.
  Domyślnie `RELAY_ON=HIGH`, `RELAY_OFF=LOW`.
- **Czujnik DS18B20 jest wymagany.** Bez poprawnego odczytu MANUAL też jest
  blokowany. To celowe: grzanie bez pomiaru temperatury jest niebezpieczne.
- **Nie zasilaj cewek przekaźników z ESP32.** Brownout przy załączaniu H2/H3
  może być przyczyną restartów; osobny zasilacz i wspólna masa są obowiązkowe.

## Kompilacja

```bash
arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=huge_app \
  "/Users/serwerhome/Projects/supla-esp32-base/projects/wip/grzalki-pv-sigenergy/grzalki-pv-sigenergy_v15"
```

Sprawdzenie kontraktów źródeł (bez kompilatora i sprzętu):

```bash
python3 "/Users/serwerhome/Projects/supla-esp32-base/projects/wip/grzalki-pv-sigenergy/grzalki-pv-sigenergy_v15/verify_source.py"
```

## Wgranie

Normalnie, gdy ESP32 jest podłączony do komputera z Arduino CLI:

```bash
arduino-cli upload -p /dev/cu.usbserial-XXXX \
  --fqbn esp32:esp32:esp32:PartitionScheme=huge_app,UploadSpeed=460800 \
  "/Users/serwerhome/Projects/supla-esp32-base/projects/wip/grzalki-pv-sigenergy/grzalki-pv-sigenergy_v15"
```

Gdy buduje Mac, a wgrywa laptop z Windows (albo gdy portu nie ma na Macu),
użyj gotowego obrazu z katalogu nadrzędnego:

```
../firmware-v15-merged.bin
```

Obraz scala bootloader (0x1000), tablicę partycji (0x8000), `boot_app0`
(0xE000) i aplikację (0x10000) w jeden plik flashowany od 0x0. Ten sam układ
partycji `huge_app`, na którym działa v11 — wgranie nie zmienia partycji
i nie kasuje konfiguracji LittleFS.

```powershell
pip install esptool
esptool --chip esp32 --port COM3 --baud 115200 write-flash 0x0 firmware-v15-merged.bin
```

Plik `*.bin` jest w `.gitignore`, więc obraz nie trafia do repozytorium.
Po wgraniu w Serialu musi pojawić się `=== PV heaters v15 ===`.

## Pliki

- `grzalki-pv-sigenergy_v15.ino` — SUPLA, WiFi, WWW, Modbus Sigenergy,
  kanały, diagnostyka restartów.
- `controller.h` — jedyny właściciel GPIO, GP8403 i DS18B20; zadanie
  `heater-control` na rdzeniu 1.
- `control_logic.h` — czyste funkcje decyzyjne i asercje `static_assert`.
- `settings.h` — parametry WWW i rozdzielenie `safetyValid` / `ssrValid` /
  `autoValid`.
- `gp8403.h` — DFR0971/GP8403, 12 bitów, 0–10 V, adres 0x58.
- `verify_source.py` — kontrakty strukturalne, nie test sprzętowy.

## Ograniczenia

- v15 nie była jeszcze wgrana na płytkę i nie jest testowana na sprzęcie.
- v11 była wgrana na gołą płytkę (bez DAC, bez DS18B20, grzałki odłączone).
  v12 i v15 są skompilowane i sprawdzone kontraktami, ale nie wgrane.
- v11 była wgrana na gołą płytkę (bez DAC, bez DS18B20, grzałki odłączone);
  konfiguracja i AuthKey są stabilne, kanały w stanie alarmu zgodnie z brakiem
  czujników. Testów z obciążeniem nie było.
- Nie ma v9/v10 w repozytorium; jeśli użytkownik ma je na komputerze z
  Windows, warto je zachować jako historię. v11, v12 i v15 są gałęziami od v8.
- Automatyczna korekta słów (`word_auto`) nie naprawi złej skali ani złego
  rejestru. Służy tylko do wykrycia zamiany kolejności słów.
- Regulacja jest nadwyżkowa, nie certyfikowana. Nie zostawiaj urządzenia bez
  nadzoru przed wykonaniem testów termicznych.
