# grzalki-pv-sigenergy v16

Wersja robocza po przeglądzie v1–v8. **Nie jest to wersja przetestowana na
grzałkach.** Przed podłączeniem odbiorników wykonaj testy niskonapięciowe z
punktu „Testy bez grzałek”.

## Co zmieniło się w v16 względem v15

**Suwak „Grzałka regulowana (SSR)" (kanał 9) to teraz wyłącznik główny, a nie
osobna grzałka.** W v15 był to zwykły suwak MANUAL, więc:

- grzałki regulowanej nie dało się użyć bez włączenia MANUAL,
- a włączenie MANUAL **wyłączało** H2/H3 z trybu nadwyżki — powyżej 2 kW
  grzałki 2 i 3 przestawały reagować na produkcję.

v16 przywraca zasadę z v11/v12: **MANUAL ON/OFF dotyczy wyłącznie H2/H3.**
Grzałka regulowana działa jak wcześniej — proporcjonalnie od nadwyżki w AUTO,
z histerezą `ssr_on`/`ssr_off`, maksymalnie `ssr_max_power`. Suwak 9 służy tylko
do tego, żeby **całkowicie ją wyłączyć**, np. gdy energia jest droga:

| Suwak 9 | Zachowanie |
|---|---|
| **ON** (domyślnie) | regulator uzbrojony: 0 V bez nadwyżki, proporcjonalnie przy nadwyżce |
| **OFF** | DAC na stałe 0 V, niezależnie od nadwyżki i trybu |

Ważne szczegóły:

- Włączenie suwarka **nie grzeje**. Uzbraja tylko regulator — bez nadwyżki DAC
  pozostaje na 0 V.
- Suwak **nie jest kasowany** przy zmianie MANUAL/AUTO ani przy chwilowej
  awarii. To preferencja użytkownika, a nie żądanie grzania.
- **Wyłączenie działa zawsze**, także przy awarii DAC, braku czujnika czy
  przegrzaniu. Odcięcie grzałki nigdy nie może być zablokowane przez błąd.
- **Włączenie jest blokowane**, gdy DAC jest uszkodzony, brak GP8403 albo
  `ssr_max_power = 0`. Log: `SSR ON rejected safety=... dacFault=...`.
- Po restarcie suwak wraca na **ON** (`setDefaultStateOn`), bo takie było
  zachowanie przed v15.

Kolejność grzania w AUTO (bez zmian względem v11/v12): najpierw SSR dostaje
nadwyżkę do `ssr_max_power` (domyślnie 1500 W), a powyżej progu `h2_on`
(domyślnie 1700 W) dołączają H2 i H3. Przy `budget=1600 W` SSR ma pełne
1500 W, H2 jeszcze nie startuje.

## Co zmieniło się w v15 względem v12

- **Usunięte kanały Sigenergy: PV, bateria ESS i SOC.** Zniknęły też ich
  odczyty Modbus (30035–30038, 30014). Moc sieci, status CT i praca on-grid
  zostają bez zmian. Mniej ruchu po Modbus TCP i mniej kanałów w aplikacji.
- **Nowa numeracja powyżej 8:** 9 = grzałka regulowana, 10 = alarm CT.
  Kanały 0–8 zachowują numery.
- v15 dodała suwak SSR jako osobne żądanie MANUAL. **v16 to cofa** — patrz
  wyżej.

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
czytały 5004, czy 30005 wpisane ręcznie w WWW. Dlatego v16 domyślnie działa
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

> W v12 i starszych tryb plant czytał też moc PV, moc baterii i SOC.
> **Od v15 tych odczytów nie ma** — rejestry 30035–30038 i 30014 nie są już
> pytane.

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
albo ponowne połączenie SUPLA. Dlatego v16 wypisuje przy starcie:

```
=== PV heaters v16 ===
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
| 9 | **Grzałka regulowana (SSR)** — wyłącznik główny regulatora |
| 10 | Alarm CT / grid sensor |

**Uwaga przy aktualizacji z v12:** kanały 9–12 (PV, ESS, SOC, CT) zniknęły.
Po wgraniu v15/v16 SUPLA przydzieli numery od nowa powyżej 8. Kanał 9 to teraz
grzałka regulowana, a nie moc PV. Jeśli w cloud.supla.org była historia albo
skrypty odwołujące się do starych numerów, trzeba je poprawić.

## Grzałka regulowana (SSR) — wyłącznik główny

Kanał 9 **nie jest osobną grzałką** i nie działa jak H2/H3. To wyłącznik
główny regulatora:

- **ON** — regulator uzbrojony. Bez nadwyżki DAC ma 0 V; przy nadwyżce napięcie
  rośnie proporcjonalnie do `ssr_max_power`. Włączenie samo w sobie **nie
  grzeje**.
- **OFF** — DAC natychmiast na 0 V i pozostaje tam niezależnie od nadwyżki.
  Służy do wyłączenia grzałki regulowanej, np. gdy energia jest droga.
- Suwak **nie wymaga MANUAL** i **nie jest kasowany** przez zmianę
  MANUAL/AUTO ani przez chwilową awarię.
- **Wyłączenie działa zawsze** — nawet przy awarii DAC, braku czujnika czy
  przegrzaniu. Odcięcie grzałki nie może być zablokowane przez błąd.
- **Włączenie jest blokowane** przy uszkodzonym DAC, braku GP8403 albo
  `ssr_max_power = 0`. Log: `SSR ON rejected safety=... dacFault=...`.
- Po restarcie suwak wraca na **ON** (`setDefaultStateOn`), czyli tak jak
  zachowywała się grzałka przed v15.
- Jeżeli zapis DAC nie zostanie potwierdzony, silnik wyłącza **wszystkie**
  grzałki (także H2/H3) i próbuje wyzerować DAC. To celowe: niepotwierdzony
  DAC może nadal grzać.

W MANUAL regulator jest celowo wyłączony (0 V), bo MANUAL należy do H2/H3 —
dokładnie jak w v11/v12.

## Testy bez grzałek

1. **Nie podłączaj grzałek.** Zmierz napięcia i sprawdź logikę na przekaźnikach
   lub diodach LED zamiast odbiorników.
2. Wgraj v16, otwórz Serial 115200. Sprawdź `Reset reason`, `CFG`, `START`.
3. Ustaw `inverter_ip`, zapisz, zrestartuj. Sprawdź, czy pojawia się
   `SIG GRID ... raw=...` i czy `grid` zmienia znak przy eksporcie/poborze.
4. Sprawdź `status` i `ongrid`. Przy mapie plant (auto wybrało 0) `status=0`
   blokuje AUTO. Przy mapie 1 statusy nie są czytane (log pokazuje 0), ale AUTO
   działa na podstawie dwóch rejestrów mocy.
5. **Sprawdź, że MANUAL nie rusza grzałki regulowanej.** Włącz MANUAL, potem
   H2. W logu: `MODE MANUAL ON`, `H2 ON request`, `OUT H2=1`. DAC musi zostać
   na 0 V. Jeśli jest `H2 ON rejected ...`, log poda przyczynę (`sensorFault`,
   `hot`, `manual=0`, `op=0`, `expired=1`).
6. Włącz H3 przy H2. Sprawdź, że nie ma resetu i że `OUT H2=1 H3=1`.
7. Sprawdź suwak 9 w AUTO: przy braku nadwyżki DAC ma 0 V, przy nadwyżce
   napięcie rośnie proporcjonalnie. **Nie podłączaj grzałki SSR bez obciążenia
   rezystancyjnego i bezpiecznika.**
8. Wyłącz suwak 9 przy nadwyżce. Napięcie DAC musi natychmiast spaść do 0 V,
   a H2/H3 mają działać dalej. W logu: `SSR master switch OFF (0 V)`.
9. Włącz suwak 9 z powrotem — regulator wraca, ale dopiero od nadwyżki.
10. Zostaw urządzenie na 30 min i sprawdź `DIAG heap`, `min`, brak `BROWNOUT`.

## Bezpieczeństwo

- **Brak fizycznego odcięcia.** v16 nie zastępuje termostatu ani bezpiecznika.
  Zespawane styki przekaźnika, zwarcie SSR albo zawieszony ESP32 mogą pozostawić
  grzanie. Wymagane niezależne zabezpieczenie termiczne wszystkich grzałek.
- **Suwak 9 nie jest regulatorem ręcznym.** Włącza tylko pozwolenie na pracę
  automatycznego regulatora; nie ustawia mocy. Nie da się nim grzać „na 50%”.
- **SSR sterowany 0–10 V** może pozostać załączony przy uszkodzonym DAC.
  v16 próbuje zerować DAC i zgłasza alarm, ale to nie jest odcięcie.
- **H2/H3 to przekaźniki.** Sprawdź, czy moduł jest aktywny HIGH, czy LOW.
  Domyślnie `RELAY_ON=HIGH`, `RELAY_OFF=LOW`.
- **Czujnik DS18B20 jest wymagany.** Bez poprawnego odczytu MANUAL też jest
  blokowany. To celowe: grzanie bez pomiaru temperatury jest niebezpieczne.
- **Nie zasilaj cewek przekaźników z ESP32.** Brownout przy załączaniu H2/H3
  może być przyczyną restartów; osobny zasilacz i wspólna masa są obowiązkowe.

## Kompilacja

```bash
arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=huge_app \
  "/Users/serwerhome/Projects/supla-esp32-base/projects/wip/grzalki-pv-sigenergy/grzalki-pv-sigenergy_v16"
```

Sprawdzenie kontraktów źródeł (bez kompilatora i sprzętu):

```bash
python3 "/Users/serwerhome/Projects/supla-esp32-base/projects/wip/grzalki-pv-sigenergy/grzalki-pv-sigenergy_v16/verify_source.py"
```

## Wgranie

Normalnie, gdy ESP32 jest podłączony do komputera z Arduino CLI:

```bash
arduino-cli upload -p /dev/cu.usbserial-XXXX \
  --fqbn esp32:esp32:esp32:PartitionScheme=huge_app,UploadSpeed=460800 \
  "/Users/serwerhome/Projects/supla-esp32-base/projects/wip/grzalki-pv-sigenergy/grzalki-pv-sigenergy_v16"
```

Gdy buduje Mac, a wgrywa laptop z Windows (albo gdy portu nie ma na Macu),
użyj gotowego obrazu z katalogu nadrzędnego:

```
../firmware-v16-merged.bin
```

Obraz scala bootloader (0x1000), tablicę partycji (0x8000), `boot_app0`
(0xE000) i aplikację (0x10000) w jeden plik flashowany od 0x0. Ten sam układ
partycji `huge_app`, na którym działa v11 — wgranie nie zmienia partycji
i nie kasuje konfiguracji LittleFS.

```powershell
pip install esptool
esptool --chip esp32 --port COM3 --baud 115200 write-flash 0x0 firmware-v16-merged.bin
```

Plik `*.bin` jest w `.gitignore`, więc obraz nie trafia do repozytorium.
Po wgraniu w Serialu musi pojawić się `=== PV heaters v16 ===`.

## Pliki

- `grzalki-pv-sigenergy_v16.ino` — SUPLA, WiFi, WWW, Modbus Sigenergy,
  kanały, diagnostyka restartów.
- `controller.h` — jedyny właściciel GPIO, GP8403 i DS18B20; zadanie
  `heater-control` na rdzeniu 1.
- `control_logic.h` — czyste funkcje decyzyjne i asercje `static_assert`.
- `settings.h` — parametry WWW i rozdzielenie `safetyValid` / `ssrValid` /
  `autoValid`.
- `gp8403.h` — DFR0971/GP8403, 12 bitów, 0–10 V, adres 0x58.
- `verify_source.py` — kontrakty strukturalne, nie test sprzętowy.

## Ograniczenia

- **v16 nie jest wgrana.** Skompilowana, 31/31 kontraktów i 16/16 testów
  decyzyjnych na hoście. Testów z grzałkami i z DAC nie było.
- v15 była wgrana 2026-11-10, ale miała suwak SSR jako osobne żądanie MANUAL —
  **v16 to zachowanie cofa** i przywraca kolejność z v11/v12.
- v11 była wgrana na gołą płytkę (bez DAC, bez DS18B20, grzałki odłączone);
  konfiguracja i AuthKey są stabilne, kanały w stanie alarmu zgodnie z brakiem
  czujników. Testów z obciążeniem nie było.
- v12 jest skompilowana i sprawdzona kontraktami, ale nie była wgrana.
- Nie ma v9/v10 w repozytorium; jeśli użytkownik ma je na komputerze z
  Windows, warto je zachować jako historię. v11, v12, v15 i v16 są gałęziami
  od v8.
- Automatyczna korekta słów (`word_auto`) nie naprawi złej skali ani złego
  rejestru. Służy tylko do wykrycia zamiany kolejności słów.
- Regulacja jest nadwyżkowa, nie certyfikowana. Nie zostawiaj urządzenia bez
  nadzoru przed wykonaniem testów termicznych.
