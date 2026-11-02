# Hydrofor v1: ESP32 DevKit V1 + SUPLA

Sterownik pompy głębinowej (IBO 250 W, 1000 l/h) i zbiorników hydroforowych
Aquasystem 150 l + 50 l przy słabej studni (około 37 l na jedno ładowanie,
regeneracja około 1 godziny). Siłownik zaworu 3D (230 V, 3 przewody)
przełącza dom między studnią a wodą miejską.

**Nie wgrano ani nie testowano v1 na ESP32.** Najpierw testy bez 230 V
(sekcja „Testy przed podłączeniem pompy”).

## Jak działa

| Sytuacja | Co robi sterownik |
|---|---|
| **Noc 23:00–05:00** | Start po spadku o 0,1 bar poniżej maksimum. Porcja trwa do maksimum albo do suchobiegu (silnik w oleju to toleruje). Potem przerwa 60 min i kolejna porcja. Rano zbiorniki są pełne. |
| **Dzień** | Start po spadku o 0,3 bar poniżej maksimum. Porcja kończy się przy maksimum, po 37 l albo przy suchobiegu. Potem przerwa 60 min. |
| **Próg krytyczny 1,8 bar** (przez 5 s) | Zawór na wodę miejską. Hydrofor jest odcięty od domu, a pompa porcjami dobija zbiorniki. Przy pełnych zbiornikach (maksimum − 0,15 bar) zawór wraca na studnię. |
| **Suchobieg** | Ciśnienie w oknie 40 s (dzień) / 20 s (noc) urosło mniej niż 0,03 bar. Jeśli było to blisko maksimum, zbiornik uznaje się za pełny (pompa doszła do swojego maksimum). |
| **Nieudana porcja** | Suchobieg albo przekroczony czas porcji, gdy wtłoczono mniej niż 5 l. 3 takie porcje z rzędu: blokada, woda miejska, powiadomienie. Ponowna próba co 6 h. |
| **Awaria czujnika** (napięcie poniżej 0,25 V lub powyżej 4,8 V przez 2 s) | Pompa stop, woda miejska. Powrót do pracy po 10 s poprawnego odczytu. |
| **Nadciśnienie 5 bar** | Pompa stop, alarm. Wskazuje na sklejony przekaźnik albo uszkodzony czujnik. |
| **Błędne nastawy** | Pompa stop, woda miejska, alarm. |
| **Start / restart ESP** | Przez pierwsze 2 min pompa nie rusza. Chroni to przed pętlą „pompa rusza → spadek napięcia → reset”. |
| **Maks. czas porcji** | 8 min. |

Litry w zbiornikach liczone są z ciśnienia (prawo Boyle'a) na podstawie
ciśnienia powietrza w zbiornikach i ich łącznej pojemności. Dla 1,5 bar
ciśnienia powietrza i 200 l:

| Ciśnienie | Woda w zbiornikach |
|---|---|
| 4,0 bar | ok. 100 l (pełne) |
| 3,7 bar | ok. 94 l (dzienny start pompy) |
| 2,65 bar | ok. 63 l (37 l poniżej pełnych) |
| 1,8 bar | ok. 21 l (próg krytyczny) |
| 1,5 bar | 0 l |

Zbiorniki z wymiennym workiem mieszczą realnie około 5–10% mniej.

Czas jest pobierany z serwera SUPLA, w strefie czasowej ustawionej w SUPLA
Cloud (z czasem letnim). Bez połączenia z SUPLA od startu sterownik działa
w trybie dziennym. Po pierwszej synchronizacji zegar chodzi dalej także bez
sieci. Sterowanie działa w osobnym zadaniu z watchdogiem 10 s i nie zależy
od WiFi, SUPLA ani strony WWW.

## Schemat podłączenia

Rysunek: [`schemat-polaczen.svg`](schemat-polaczen.svg) (do powiększania)
i [`schemat-polaczen.png`](schemat-polaczen.png) (do druku).

- **Panel A — część niskonapięciowa (5 V / 3,3 V):** zasilacz HLK-5M05,
  ESP32 DevKit V1, czujnik ciśnienia, dzielnik 4,7 kΩ / 8,2 kΩ z 100 nF,
  rezystory 10 kΩ na wejściach IN modułów przekaźników, wskaźniki LED
  (pompa, zawór) z rezystorami 1 kΩ.
  Ten panel można złożyć i przetestować bez 230 V.
- **Panel B — część 230 V:** sieć L/N/PE, bezpiecznik 2 A, styki przekaźników,
  presostat w szeregu z pompą, siłownik zaworu 3D (COM/NO/NC).
  Montaż i podłączenie tej części wykonuje elektryk.

Tagi `+5 V` i symbole GND w różnych miejscach rysunku oznaczają ten sam
punkt — wszystko schodzi się przy zasilaczu.

### Wskaźniki LED (opcjonalne)

Dwie diody pokazują na obudowie, co aktualnie robi sterownik. Podłącza się je
równolegle do wejść IN modułów przekaźników (moduły HIGH-active):

| Dioda | Gdzie | Kiedy świeci |
|---|---|---|
| Zielona „POMPA” | od IN modułu pompy (D26) przez **1 kΩ** do GND | przekaźnik pompy załączony |
| Żółta „ZAWÓR” | od IN modułu zaworu (D25) przez **1 kΩ** do GND | przekaźnik zaworu załączony = kierunek STUDNIA |

Dioda przewodzi w kierunku od IN do GND (anoda do IN, katoda przez rezystor
do GND). 1 kΩ daje około 3 mA — bezpiecznie dla pinu ESP32 i wystarczająco
jasno dla zwykłej diody LED. **Dla modułów LOW-active diodę włącza się
odwrotnie:** od +5 V przez rezystor do IN (świeci, gdy IN jest niski).

Dioda „ZAWÓR” świeci, gdy zawór idzie na studnię; zgaszona oznacza wodę
miejską (albo brak zasilania sterownika).

## Połączenia

| Element | ESP32 DevKit V1 (30 pin) |
|---|---|
| Czujnik: czerwony | **5V / VIN** (zasilanie czujnika 5 V) |
| Czujnik: czarny | GND |
| Czujnik: zielony (sygnał) | przez **4,7 kΩ** do **D34 (GPIO34)**; z GPIO34 **8,2 kΩ** do GND i **100 nF** do GND |
| Przekaźnik pompy IN | **D26 (GPIO26)** |
| Przekaźnik zaworu IN | **D25 (GPIO25)** |
| Przycisk BOOT (konfiguracja) | GPIO0 (na płytce) |
| LED konfiguracji | GPIO2 (na płytce) |

Dzielnik 4,7k/8,2k daje 0,636. Czujnik 0,5–4,5 V daje na ADC 0,32–2,86 V.
Nawet zwarcie sygnału do 5 V daje tylko 3,18 V. **Nie podłączać sygnału
czujnika bezpośrednio do GPIO.**

Zmierzony egzemplarz (4,767 kΩ i 8,13 kΩ) daje stosunek 0,6304 — taką wartość
wpisać w `h_cal_div` na stronie WWW urządzenia.

### Pompa (230 V)
Przekaźnik **10 A** (pompa 250 W pobiera około 1,2 A, przy rozruchu kilka razy
więcej). Przełącza fazę L do pompy. **Zostawić mechaniczny presostat w
szeregu** jako niezależne zabezpieczenie, z wyłączeniem około 0,4–0,5 bar
powyżej ustawionego maksimum (np. 4,5 bar przy maksimum 4,0 bar).

### Zawór 3D (230 V, 3 przewody: N, otwórz, zamknij)
**Jeden przekaźnik przełączny** (COM/NO/NC), dzięki czemu oba kierunki nie
dostaną napięcia jednocześnie:

```
L ──► COM przekaźnika zaworu
      NO  ──► przewód kierunku „STUDNIA”         (przekaźnik załączony)
      NC  ──► przewód kierunku „WODA MIEJSKA”    (przekaźnik zwolniony)
N ──────────► N siłownika
```

Wyłączone albo uszkodzone ESP zwalnia przekaźnik, więc zawór idzie na wodę
miejską. Siłownik ma własne krańcówki. **Który przewód siłownika otwiera
który kierunek, trzeba sprawdzić na tabliczce albo próbnie, przed
podłączeniem do ESP.**

Moduły przekaźników: domyślnie stan wysoki = załączony. Dla modułów
aktywnych stanem niskim zamienić `PUMP_ON/PUMP_OFF` oraz `VALVE_WELL/VALVE_CITY`
w `engine.h`. Dla modułów aktywnych stanem wysokim dodać **10 kΩ do GND** na
wejściach IN, żeby podczas resetu (piny „pływają”) przekaźniki nie drgały.
Część 230 V zamknąć w puszce z bezpiecznikiem. Podłączenie zlecić elektrykowi.

## Kanały SUPLA

| Nr | Kanał | Uwagi |
|---|---|---|
| 0 | Ciśnienie [bar] | brak wartości przy awarii czujnika |
| 1 | Serwis: pompa STOP, woda miejska | **wyłączenie kasuje blokadę suchobiegu** |
| 2 | Alarm hydroforu | blokada suchobiegu, awaria czujnika, nadciśnienie albo błędne nastawy |

Kanał 1 pamięta stan po restarcie. Przy pierwszym uruchomieniu jest wyłączony,
czyli działa automatyka. Co wywołało alarm, mówi powiadomienie push:
blokada, przejście na wodę miejską, powrót na studnię, awaria czujnika,
nadciśnienie, błędna konfiguracja. W aplikacji SUPLA trzeba zezwolić na
powiadomienia urządzenia.

Szczegóły widać tylko na stronie WWW urządzenia (w LAN, adres z routera),
w nagłówku nad nastawami: ciśnienie, litry w zbiornikach, **napięcie czujnika**
(do kalibracji), stan sterownika, pompa, zawór i ostatnia porcja ze studni.
Stan jest aktualny w chwili otwarcia strony, odświeżenie strony go aktualizuje.

### Jak często czytany jest czujnik

| Co | Jak często |
|---|---|
| Odczyt ADC | 8 próbek co 50 ms (160 na sekundę) |
| Wartość do sterowania | średnia z ostatniej 1 s, czyli z 20 odczytów |
| Decyzja sterownika (pompa/zawór) | co 250 ms |
| Ciśnienie w SUPLA | co 5 s (domyślne odświeżanie kanału, zmiana w SUPLA Cloud) |

## Nastawy (strona WWW urządzenia)

Zmiany obowiązują po **SAVE & RESTART**.

| Klucz | Domyślnie | Znaczenie |
|---|---|---|
| `h_p_max` | 4,00 bar | pełne zbiorniki; **ustawić 0,3–0,5 bar poniżej maksimum, które pompa realnie osiąga** |
| `h_day_drop` | 0,30 bar | dzień: start po takim spadku poniżej maksimum |
| `h_night_drop` | 0,10 bar | noc: start po takim spadku poniżej maksimum |
| `h_p_crit` | 1,80 bar | próg krytyczny, musi być wyżej niż ciśnienie powietrza |
| `h_p_emerg` | 5,00 bar | awaryjny STOP, co najmniej maksimum + 0,3 bar |
| `h_full_margin` | 0,15 bar | „pełny” = maksimum minus ta wartość |
| `h_precharge` | 1,50 bar | ciśnienie powietrza w pustych zbiornikach |
| `h_capacity` | 200 l | łączna pojemność zbiorników |
| `h_day_batch` | 37 l | dzień: limit wody na porcję (0 = bez limitu) |
| `h_night_batch` | 0 l | noc: limit (0 = do suchobiegu) |
| `h_min_yield` | 5 l | porcja zakończona suchobiegiem z mniejszą ilością = nieudana |
| `h_dry_rise` | 0,030 bar | min. przyrost ciśnienia w oknie |
| `h_day_win` / `h_night_win` | 40 / 20 s | okno detekcji suchobiegu |
| `h_rest_min` | 60 min | przerwa po każdej porcji |
| `h_max_run` | 8 min | maks. czas porcji |
| `h_night_from` / `h_night_to` | 23 / 5 | okno nocne (godziny) |
| `h_lock_count` | 3 | blokada po tylu nieudanych porcjach z rzędu |
| `h_lock_retry` | 6 h | ponowna próba po blokadzie (0 = tylko ręcznie) |
| `h_cal_zero_v` | 0,500 V | napięcie czujnika przy 0 bar |
| `h_cal_full_v` | 4,500 V | napięcie czujnika przy pełnej skali |
| `h_cal_full_bar` | 10,342 bar | pełna skala (150 psi) |
| `h_cal_div` | 0,6357 | dzielnik 8,2k / (4,7k + 8,2k) |

Sterownik sprawdza zależności między nastawami: ciśnienie powietrza < próg
krytyczny < maksimum − spadek, a ciśnienie awaryjne ≥ maksimum + 0,3 bar.
Błędne nastawy: pompa stop, woda miejska, alarm.

## Uruchomienie i kalibracja

1. Pierwsze uruchomienie: urządzenie tworzy sieć WiFi `HYDROFOR-...`.
   Otworzyć **http://192.168.4.1** i wpisać WiFi, serwer SUPLA, e-mail oraz nastawy.
   Później konfigurację otwiera przytrzymanie **BOOT 5 s** albo **5 kliknięć**
   (LED GPIO2 miga szybko). Nie trzymać BOOT podczas włączania zasilania.
2. **Ciśnienie powietrza:** pompa w trybie serwisowym (kanał 1), kran odkręcony
   aż woda przestanie lecieć. Zmierzyć ciśnienie na wentylach obu zbiorników
   manometrem do opon, ustawić je jednakowo i wpisać do `h_precharge`.
3. **Zero czujnika:** przy 0 bar (krok 2) odczytać „czujnik … V” na stronie WWW
   urządzenia i wpisać do `h_cal_zero_v`.
4. **Maksimum pompy:** jeszcze na starym presostacie, przy zakręconych kranach,
   sprawdzić na manometrze, czy pompa dobija do jego progu wyłączenia i jak
   szybko rośnie ciśnienie pod koniec. Po uruchomieniu ESP obserwować historię
   kanału 0 i „ostatnią porcję” na stronie WWW. Jeśli przy pełnej studni porcje kończą się suchobiegiem
   tuż poniżej maksimum, maksimum jest za wysokie. `h_p_max` ustawić 0,3–0,5 bar
   poniżej ciśnienia, przy którym pompa przestaje tłoczyć. Nie mostkować
   przekaźników 230 V ręcznie.
   Za wysokie maksimum sprawia, że pompa przy maksimum ledwo tłoczy, a koniec
   porcji wygląda jak suchobieg. Po przejściu na wodę miejską zawór nie wróci
   wtedy na studnię aż do nocy.
5. Porównać kanał 0 z manometrem przy kilku ciśnieniach. Jeśli różnica rośnie
   z ciśnieniem, skorygować `h_cal_full_v`.
6. Po kilku nocach „ostatnia porcja” na stronie WWW pokaże, ile realnie daje
   studnia (nocna porcja kończy się suchobiegiem). Jeśli w dzień
   wypada mniej niż 37 l, zmniejszyć `h_day_batch`.

## Ograniczenia

- Bez przepływomierza i pomiaru mocy sterownik zna tylko ciśnienie. W dzień
  duży pobór wody (np. prysznic) w trakcie pompowania może wyglądać jak
  suchobieg. Skutkiem jest przerwa 60 min, a w razie potrzeby przejście na
  wodę miejską. Pompa nie jest przy tym zagrożona.
- Limit 37 l liczy przyrost wody w zbiornikach. Woda pobrana w tym czasie
  przez dom nie jest wliczona, więc ze studni może wypłynąć więcej.
- Za słaba pompa względem `h_p_max` (patrz kalibracja, krok 4).
- To nie jest układ bezpieczeństwa z gwarantowanym czasem reakcji. Ochrona
  przed nadciśnieniem wymaga mechanicznego presostatu w szeregu z pompą.
- WWW nie ma hasła: tylko zaufana sieć LAN, bez przekierowania portu.

## Pliki

| Plik | Zawartość |
|---|---|
| `hydrofor-esp32-supla_v1.ino` | kanały SUPLA, WWW, synchronizacja stanu |
| `hydro_logic.h` | cała logika sterowania (bez Arduino, constexpr) |
| `hydro_tests.h` | testy `static_assert`: reguły i symulacja zbiorników, studni i pompy |
| `engine.h` | zadanie sterujące: ADC, GPIO, watchdog |
| `settings.h` | nastawy WWW |
| `notify.h` | powiadomienia push SUPLA |
| `status_page.h` | bieżący stan na stronie WWW urządzenia |
| `schemat-polaczen.svg` | schemat podłączenia (wektorowy, do powiększania) |
| `schemat-polaczen.png` | ten sam schemat jako obraz (do druku) |

## Walidacja

Środowisko: ESP32 core 3.3.3, SuplaDevice 26.8, arduino-cli 1.3.1.

- Kompilacja (2026-10-09): **EXIT=0**, bez ostrzeżeń z plików projektu
  (`--warnings all`), flash 1 371 075 B (43%), RAM 50 644 B (15%).
  Wersja z 3 kanałami SUPLA i stanem na stronie WWW.
- Kompilacja na macOS (2026-10-10, Apple Silicon, arduino-cli 1.3.1):
  **EXIT=0**, bez ostrzeżeń, flash 1 371 059 B (43%), RAM 50 644 B (15%).
  Te same wersje (ESP32 core 3.3.3, SuplaDevice 26.8.0) i ta sama komenda.
  Na macOS potrzebna jest Rosetta 2 (dołączany `ctags` jest x86_64):
  `softwareupdate --install-rosetta --agree-to-license`.
- `hydro_tests.h` jest kompilowany razem ze szkicem. Każdy nieudany scenariusz
  przerywa kompilację. Zakres testów:
  - **reguły:** opóźnienie startu, progi dzień/noc, stop przy maksimum,
    „pełny” przy maksimum pompy, suchobieg z przerwą, krótsze okno nocą,
    blokada po 3 porcjach i ponowienie po 6 h, porcja z wodą nie jest porażką,
    limit 37 l w dzień i brak limitu w nocy, maks. czas porcji, próg krytyczny
    z opóźnieniem i powrotem, awaria czujnika, nadciśnienie, kasowanie blokady
    trybem serwisowym, wymuszona woda miejska (w logice, bez kanału SUPLA),
    błędne nastawy, przepełnienie `millis()`;
  - **symulacja (studnia 37 l, pompa 1000 l/h, 200 l):**
    - noc od 2,0 bar: pełne o 5:00 w 2 porcjach,
    - noc od 1,6 bar: pełne w 3 porcjach,
    - lekki pobór w dzień: bez wody miejskiej, minimum 3,03 bar,
    - duży pobór 3 l/min: przejście na wodę miejską przy 1,79 bar i 3 powroty na studnię w 10 h,
    - jednorazowy spadek: woda miejska tylko przez około 131 min,
    - sucha studnia: blokada i najwyżej 4 próby po 40 s w 10 h.
  
  Symulacja wykryła błąd: po przejściu na wodę miejską zbiorniki
  zatrzymywały się na 3,77 bar, a zawór zostawał na wodzie miejskiej do nocy.
  Poprawione przed wydaniem v1.
- Wgranie (2026-10-10, macOS, ESP32 DevKit V1 z mostkiem CP2102): **wykonane**,
  hash zweryfikowany. Uwaga: na tej płytce 921600 kbps gubi kontakt z chipem
  („the chip stopped responding”) — używać `UploadSpeed=115200`.
- Testy na płytce, czujnika, przekaźników i zaworu: **niewykonane**.

Kompilacja z folderu v1:

```powershell
arduino-cli compile --jobs 4 --fqbn esp32:esp32:esp32:PartitionScheme=huge_app .
```

## Testy przed podłączeniem pompy (diody/miernik zamiast 230 V)

1. Start i reset: pompa wyłączona przez 2 min, zawór (dioda) najpierw „miejska”,
   po około 1 s „studnia”.
2. Potencjometr 0–4,5 V (przez dzielnik) zamiast czujnika: sprawdzić
   ciśnienie w SUPLA, start przy 3,7 bar, stop przy 4,0 bar.
3. Stałe napięcie przy pracującej pompie: stop po 40 s, po 3 próbach blokada,
   kanał 2 (alarm) aktywny, powiadomienie, na stronie WWW „BLOKADA” i zawór
   „woda miejska”. Kanał 1 ON→OFF kasuje blokadę.
4. Odłączenie przewodu sygnału: po 2 s pompa stop, zawór „miejska”, alarm.
5. Spadek poniżej 1,8 bar na 5 s: zawór „miejska”. Powrót przy 3,85 bar.
6. Wyłączenie routera: sterowanie działa dalej.
