# velux-io-supla — SUPLA dla rolet i okien VELUX (io-homecontrol 1W + 2W)

Firmware SuplaDevice na **ESP32 DevKit V1 (30 pin)** z modułem **SX1276/RFM95W
868 MHz**, który steruje produktami VELUX (rolety, okna dachowe) protokołem
**io-homecontrol** — tym samym, którego używa pilot KLI 310/312/313 (1W)
oraz pilot/hub dwukierunkowy KLR 200 (2W).

Kod protokołu pochodzi z [laberning/home_io_control](https://github.com/laberning/home_io_control)
(MIT, © Lars Berning); kopia referencyjna leży w `../../../lib/home_io_control`,
a źródła użyte do kompilacji w `firmware/velux_io_supla/src/hioc/` (bez zmian,
patrz `LICENSE-home_io_control.txt`).

## Dwie drogi: 1W i 2W

| | 1W (pilot KLI, np. 310/312/313) | 2W (pilot/hub KLR 200, KLF 200, TaHoma…) |
|---|---|---|
| Sterowanie | tak, po nauce gestem Gear | tak, po ekstrakcji klucza instalacji |
| Potwierdzenie komendy | **brak** | tak (challenge 0x3C/0x3D + odpowiedź 0x04) |
| Pozycja w SUPLA | szacowana (czas przejazdu) | **realna** z urządzenia (0x04 / 0x71) |
| Adresowanie | klasa urządzeń (`00 00 3F`) | konkretne urządzenie (node ID) |
| Nauka | gest na pilocie + „Nauka" w panelu | ekstrakcja klucza z KLR + skan + przypisanie |

Firmware obsługuje obie drogi **jednocześnie**: kanał, do którego przypiszesz
urządzenie 2W, używa wymiany dwukierunkowej i pokazuje realny status; kanał bez
przypisania działa jak dotąd — 1W z szacowaną pozycją.

## Jak to działa (1W) i czego nie da się zrobić

io-homecontrol **1W jest jednokierunkowy**: pilot (a więc i nasze ESP32)
wysyła komendę i nie dostaje żadnej odpowiedzi — ani potwierdzenia, ani
pozycji, ani błędu.

Z tego wynikają trzy rzeczy, o których trzeba wiedzieć:

1. **Komenda adresuje klasę urządzeń, nie konkretny produkt.** Nasza ramka
   leci na adres „wszystkie urządzenia" (`00 00 3F`), tak jak w prawdziwym
   pilocie KLI. Produkt wykona ją tylko wtedy, gdy nasz adres (tożsamość 1W)
   jest wpisany na jego listę kontrolerów — dlatego **nauka (Enroll) jest
   obowiązkowa** i robi się ją raz na produkt.
2. **Pozycja w SUPLA jest szacowana**, nie mierzona. Po wysłanej komendzie
   animujemy pozycję przez czas otwarcia/zamknięcia (domyślnie 30 s dla
   rolety, 20 s dla okna — do zmiany w szkicu lub w aplikacji). STOP zamraża
   szacunek. Po użyciu fizycznego pilota KLI szacunek się rozjeżdża —
   koryguje go najbliższa komenda z aplikacji.
3. **Jeden kanał SUPLA = jedna tożsamość 1W** (adres + klucz, zapisane
   w NVS). Jeśli dwa produkty wykonują tę samą komendę, to znaczy, że oba
   mają tę samą tożsamość na liście kontrolerów — patrz „Nauka" niżej.

## Jak to działa (2W)

2W to protokół **uwierzytelniony**: każda komenda przechodzi wymianę
challenge/response z kluczem instalacji (16 B), a urządzenie odpowiada
ramką statusu z realną pozycją. Żeby to działało, musimy znać klucz
instalacji — i tu jest cały trik:

1. **Ekstrakcja klucza z istniejącego pilota/huba.** Nasze radio udaje
   nieznane, nieparowane urządzenie. Uruchamiasz na KLR 200 kreator
   „dodaj produkt" (tryb parowania), pilot łączy się z naszym radiem
   i — tak jak z prawdziwym produktem — przekazuje mu swój `node_id`
   oraz `system_key` instalacji. Klucz zapisujemy w NVS (przestrzeń
   `veluxid`), a produkty **zostają sparowane z pilotem** — nic nie tracimy.
2. **Skan instalacji (roll-call).** Broadcastem `0x2A` pytamy „kto tu jest?";
   każde urządzenie znające klucz odpowiada `0x2B` ze swoim adresem, typem
   i klasą zasilania (bateria/sieć). Sześć prób (2 przebiegi × 3 kanały)
   daje listę wszystkich produktów instalacji.
3. **Przypisanie do kanału.** Wybierasz z listy urządzenie dla danego kanału
   SUPLA (adres + typ + preambuła lądują w NVS).
4. **Sterowanie i status.** Komenda idzie jako uwierzytelniona wymiana 2W;
   po niej (i co 2 minuty) odpytujemy urządzenie o status `0x03` → `0x04`
   i pokazujemy **realną pozycję** w SUPLA. Nieusolicytowane statusy `0x71`
   też są obsługiwane (uwierzytelniamy je po naszej stronie i potwierdzamy).

Uwagi do 2W:

- Pilot KLR 200 **nie pokazuje adresów produktów** i nie ma listy node ID —
  skan po ekstrakcji klucza jest jedyną drogą do adresów (i tak go robimy).
- Nasz `node_id` po ekstrakcji to adres **KLR-a** — występujemy jako ten
  pilot (tak samo robi hioc). KLR dalej działa normalnie.
- Ekstrakcję można powtórzyć w każdej chwili (nowy kreator parowania);
  stary klucz jest nadpisywany.


## Podłączenie

| SX1276 / RFM95W | ESP32 DevKit V1 | Uwagi |
|---|---|---|
| VCC / 3V3 | 3V3 | **tylko 3,3 V** — moduł nie jest odporny na 5 V |
| GND | GND | |
| SCK | GPIO18 | SPI VSPI |
| MISO | GPIO19 | |
| MOSI | GPIO23 | |
| NSS / CS | GPIO5 | sterowany ręcznie |
| RESET | GPIO14 | |
| DIO0 | GPIO26 | przerwanie „PayloadReady" |
| ANT | antena 868 MHz | **bez anteny nie ma zasięgu** (kilkanaście cm) |

Dodatkowo: dioda stanu SUPLA na GPIO2 (wbudowana; jeśli świeci odwrotnie,
ustaw `VELUX_LED_INVERT 1`), przycisk konfiguracji na GPIO0 (BOOT).

Uwagi sprzętowe:

- **KUX 110 to zasilacz 24 V, nie mostek radiowy.** Odbiornik radiowy siedzi
  w silniku produktu; pilot KLI dogaduje się z produktem bezpośrednio.
  Interrupt zasilania na KUX 110 odcina wszystkie produkty, które z niego
  korzystają.
- Moc nadawania ustawiona na **14 dBm (~25 mW)** — maksimum dozwolone dla
  868,95 MHz (kanał CH2, na którym pracuje 1W). Nie zwiększać.
- Moduł warto trzymać z dala od zasilacza i przewodów 230 V; antena
  odsunięta od obudowy.

## Kompilacja i wgranie

```bash
cd firmware/velux_io_supla
arduino-cli compile --jobs 4 \
  --fqbn "esp32:esp32:esp32:PartitionScheme=min_spiffs" --output-dir build .
arduino-cli upload -p /dev/cu.usbserial-XXXX \
  --fqbn "esp32:esp32:esp32:PartitionScheme=min_spiffs,UploadSpeed=115200" .
```

`min_spiffs` daje aplikacji 1,875 MB (zapas na OTA) i 128 kB LittleFS na
konfigurację SUPLA. Na klonach z CP2102 używaj 115200 (921600 gubi kontakt).

## Pierwsze uruchomienie

1. Po wgraniu włącza się **AP konfiguracyjne** (pusta konfiguracja):
   połącz się z siecią urządzenia i wejdź na `http://192.168.4.1`.
2. W panelu: ustaw Wi-Fi, adres serwera SUPLA i GUID/AuthKey
   (`https://www.supla.org/arduino/get-authkey`), zapisz.
3. W panelu jest sekcja **VELUX io-homecontrol (1W + 2W)** (patrz niżej).
4. Później tryb konfiguracji wywołuje się przyciskiem GPIO0: przytrzymaj
   ~5 s (AP wstaje), a w trybie konfiguracji krótkie kliknięcie wychodzi
   z niego i restartuje urządzenie.

### Panel WWW działa tylko w trybie konfiguracji

Serwer WWW SuplaDevice startuje **wyłącznie** w trybie konfiguracji
(`enterConfigMode()`) i jest zatrzymywany przy wyjściu z niego
(`leaveConfigModeWithoutRestart()`), a razem z nim gaśnie AP. Konsekwencje:

- panel (`192.168.4.1`) widzisz **tylko** gdy urządzenie nadaje własną sieć
  `SUPLA-VELUX-1W` (nazwa może mieć dopisek z adresem MAC);
- po zapisaniu Wi-Fi urządzenie restartuje się do trybu normalnego i **panel
  przestaje być dostępny** (nie ma go pod adresem IP w domowej sieci);
- żeby wrócić do panelu: **przytrzymaj BOOT (GPIO0) ~5 s** → urządzenie
  wchodzi w tryb konfiguracji → pojawia się AP → łączysz się i wchodzisz
  na `192.168.4.1`;
- w trybie konfiguracji urządzenie **nie łączy się z serwerem SUPLA**
  (kanały znikają z aplikacji) — to normalne, wracają po wyjściu z trybu;
- **UWAGA:** będąc już w trybie konfiguracji, przytrzymanie BOOT przez ~5 s
  to **reset do ustawień fabrycznych SUPLA** (czyści Wi-Fi/GUID/AuthKey, ale
  *nie* nasze dane w NVS `veluxid` — klucz instalacji i tożsamości 1W
  zostają). Wyjście z trybu to **krótkie kliknięcie**, nie przytrzymanie.

Dlatego **ekstrakcję klucza i skan instalacji robi się w trybie
konfiguracji** — panel jest wtedy otwarty, a radio działa niezależnie od
trybu SUPLA.

## Kanały (ile produktów, jakie)

Liczbę i rodzaj kanałów ustawia się w szkicu, w tablicy `VELUX_SLOTS`
(`velux_io_supla.ino`). Domyślnie jest **9 kanałów: 4 rolety i 5 okien
dachowych**.

```cpp
static const velux::SlotConfig VELUX_SLOTS[] = {
  {"Roleta 1",       velux::ProductKind::ROLLER_SHUTTER, 30000, 30000, false, velux::EnrollProfile::AUTO},
  {"Roleta 2",       velux::ProductKind::ROLLER_SHUTTER, 30000, 30000, false, velux::EnrollProfile::AUTO},
  {"Roleta 3",       velux::ProductKind::ROLLER_SHUTTER, 30000, 30000, false, velux::EnrollProfile::AUTO},
  {"Roleta 4",       velux::ProductKind::ROLLER_SHUTTER, 30000, 30000, false, velux::EnrollProfile::AUTO},
  {"Okno dachowe 1", velux::ProductKind::WINDOW_OPENER,  30000, 30000, false, velux::EnrollProfile::AUTO},
  {"Okno dachowe 2", velux::ProductKind::WINDOW_OPENER,  30000, 30000, false, velux::EnrollProfile::AUTO},
  {"Okno dachowe 3", velux::ProductKind::WINDOW_OPENER,  30000, 30000, false, velux::EnrollProfile::AUTO},
  {"Okno dachowe 4", velux::ProductKind::WINDOW_OPENER,  30000, 30000, false, velux::EnrollProfile::AUTO},
  {"Okno dachowe 5", velux::ProductKind::WINDOW_OPENER,  30000, 30000, false, velux::EnrollProfile::AUTO},
};
```

- `ProductKind` wybiera klasę urządzenia, na którą adresujemy komendy
  (`ROLLER_SHUTTER`, `WINDOW_OPENER`, `AWNING`) — dla okna dachowego kanał
  SUPLA dostaje funkcję „okno dachowe".
- Czasy otwarcia/zamknięcia sterują tylko animacją pozycji: w 2W realną
  pozycję podaje samo urządzenie, a w 1W jest szacowana. Można je też
  zmienić z aplikacji SUPLA (konfiguracja kanału).
- Piąty argument (`false`/`true`) to domyślne ustawienie „odbiornik
  bateryjny/solarny" (długa preambuła). Zmienia się je też z panelu WWW
  (a przy przypisaniu urządzenia 2W ustawia się samo ze skanu).
- Maksymalnie 12 wpisów (`velux::MAX_SLOTS`).
- Zmiana liczby wpisów zmienia listę kanałów zgłaszaną do SUPLA. Po wgraniu
  nowego firmware urządzenie rejestruje się ponownie i lista kanałów w
  aplikacji się odświeża; jeśli nie — usuń urządzenie z SUPLA i dodaj je
  ponownie. Ustawienia w NVS (`veluxid`: klucz instalacji, przypisania,
  tożsamości 1W) zostają, więc nie trzeba powtarzać ekstrakcji klucza ani
  skanu. Kanał 1 = pierwszy wpis w tablicy.

## Nauka (Enroll) — krok po kroku

Nauka jest dwustronna: jedną połowę robisz fizycznie na pilocie/produkcie,
drugą — przyciskiem w panelu WWW.

1. **Na pilocie KLI przytrzymaj przycisk Gear** („otwórz do rejestracji")
   przez **około 1 sekundę**. Nie „Pair" — Pair jest dla nowego kontrolera
   i zastępuje go nasz Enroll; Gear + Pair na tym samym pilocie kasuje jego
   produkty.
2. Produkt uruchomi **sekwencję gotowości** (potrafi potrwać do ~30 s:
   przejazd do ~10 %, kilka szarpnięć, powrót).
3. Poczekaj, aż sekwencja się skończy, i **natychmiast w panelu WWW**
   (sekcja VELUX, dany kanał) wybierz akcję **„Nauka (Enroll)"** i zapisz.
4. Produkt **zamknie się** (prawdziwy pilot wysyła STOP + DOWN na koniec
   rejestracji) — to jest sygnał sukcesu. Jeśli produkt był już zamknięty,
   ruchu nie widać: sprawdź komendą „Otwórz" w aplikacji.

Jeśli nauka nie działa, sprawdź **klasy ramki 0x30**:

- włącz w panelu wyższy poziom logów i po przytrzymaniu Gear na pilocie
  zobacz w logu linie `RX 1W <adres> -> <klasa> ...` oraz dopisek
  `^ klasa nauki pilota: <klasa>`;
- w panelu WWW ustaw dla tego kanału **profil nauki** zgodny z tym, co
  wypisał pilot (np. `rolety wewnętrzne: blind, venetian_blind` dla KLI 312),
  zapisz i powtórz naukę.

Uwagi:

- **Gear otwiera rejestrację na wszystkich produktach sterowanych tym
  pilotem.** Jeśli jeden pilot obsługuje kilka produktów, każdy z nich
  zarejestruje naszą tożsamość i będzie reagował na komendy tego kanału.
  Dla osobnego sterowania: użyj pilota, który obsługuje tylko jeden produkt,
  albo dodaj kolejne kanały (każdy ma własną tożsamość) i przeprowadź naukę
  osobno, zamykając w międzyczasie pozostałe produkty (np. pojedynczo
  wyłączając ich zasilanie).
- **Nauka jest addytywna** — nie kasuje istniejącego pilota; produkt
  przyjmuje komendy i od pilota, i od nas.
- „Wypisz ten kontroler" (`0x39`) jest **niepotwierdzone przez producenta**
  — traktuj jako eksperyment, nie jako pewne kasowanie rejestracji.

## 2W krok po kroku (KLR 200)

> Kroki 1–6 robi się **w trybie konfiguracji** (AP `SUPLA-VELUX-1W`,
> `192.168.4.1`) — w trybie normalnym panel jest niedostępny. Wejście:
> przytrzymaj BOOT ~5 s. Wyjście: **krótkie kliknięcie** BOOT (restart
> i powrót do aplikacji SUPLA).

1. **Wejdź w tryb konfiguracji** (przytrzymaj BOOT ~5 s), połącz się z siecią
   `SUPLA-VELUX-1W` i otwórz `192.168.4.1`.
2. **Uzbrój ekstrakcję klucza** w panelu WWW: sekcja *2W: klucz instalacji
   i urządzenia*, pole „Klucz instalacji / ekstrakcja" → *Uzbrój ekstrakcję
   klucza (10 min)* → zapisz. Responder działa 10 minut.
3. **Na pilocie KLR 200 uruchom dodawanie produktu**: menu → *New product*
   → *Add new products* → poczekaj, aż pilot znajdzie produkty → potwierdź.
   Pilot nadaje broadcast `0x28`, nasze radio odpowiada jak nieznane
   urządzenie i odbiera klucz.
4. Poczekaj, aż w panelu (albo w logu szeregowym) pojawi się
   `Klucz instalacji: node_id … system_key …`. **Zapisz te wartości** —
   są też w NVS, ale kopia na papier się przyda. Jeśli pilot zgłosi błąd,
   uzbrój ponownie i powtórz.
5. **Skanuj instalację**: pole „Skan urządzeń (roll-call 0x2A)" → *Skanuj*
   → zapisz. Skan idzie krokami (~2 s na próbę, 6 prób); po zakończeniu
   odśwież stronę — pojawi się lista znalezionych urządzeń (adres, typ,
   zasilanie, RSSI).
6. **Przypisz urządzenie do kanału**: przy wybranym wpisie z listy wybierz
   kanał SUPLA i zapisz. Adres, typ i preambuła lądują w NVS.
7. **Sprawdź**: pole „Test 2W" w okienku kanału → *Otwórz*/*Zamknij*/ *Stop*/
   /*Odczytaj status*. Realna pozycja pojawi się w polu „Status 2W".
8. **Wyjdź z trybu konfiguracji** (krótkie kliknięcie BOOT) — urządzenie
   wróci do domowej sieci, a w aplikacji SUPLA kanały pokażą realną pozycję.

Gdy chcesz wrócić do 1W dla danego kanału: „Test 2W" → *Usuń przypisanie 2W*.
Klucz instalacji można usunąć osobno („Usuń zapisany klucz instalacji").

## Używanie w aplikacji SUPLA

Kanał jest zwykłą roletą/oknem: otwórz, zamknij, STOP, suwak pozycji,
„krok po kroku". Wysyłka jednej komendy to seria 4 kopii ramki (1W) albo
uwierzytelniona wymiana (2W) i trwa ~0,2–2 s (blokuje pętlę programu —
to normalne).

- **Kanał 1W**: aplikacja pokazuje **szacunek** pozycji. Po użyciu pilota KLI
  albo po zaniku zasilania stan może być inny niż pokazany — najbliższe
  „Otwórz"/„Zamknij" z aplikacji go wyrówna.
- **Kanał 2W**: pozycja jest **realna** — po każdej komendzie (po ~4 s)
  i co 2 minuty urządzenie jest odpytywane o status; animacja w aplikacji
  to tylko wypełnienie między odczytami.

## Panel WWW (sekcja VELUX)

- **Radio** — czy SX1276 odpowiedział (jeśli „BRAK", sprawdź SPI/piny).
- **Ostatnie zdarzenie / ostatni pilot 1W** — co urządzenie ostatnio
  wysłało lub usłyszało (nasłuchuje cały czas, m.in. ramki 0x2E pilota).
- **2W: klucz instalacji i urządzenia** — klucz (`node_id` + `system_key`),
  stan ekstrakcji (uzbrojona/wyłączona, pozostały czas), ostatnie zdarzenie
  2W, uzbrojenie/wyłączenie ekstrakcji, usunięcie klucza, skan instalacji
  i lista znalezionych urządzeń z przypisaniem do kanału.
- Dla każdego kanału: **tożsamość 1W**, **urządzenie 2W** i jego **status**,
  **odbiornik (preambuła)**, **klasy nauki**, **akcja 1W** (nauka /
  wypisanie / nowa tożsamość), **test 2W** (otwórz/zamknij/stop/wentylacja/
  status/usuń przypisanie).

Ustawienia kanałów, tożsamości 1W, klucz instalacji 2W i przypisania urządzeń
są zapisane w NVS w przestrzeni `veluxid` — **przeżywają „reset do ustawień
fabrycznych" SUPLA** (nową naukę trzeba zrobić dopiero po zmianie tożsamości).

## Ograniczenia (świadome)

- **1W**: brak jakiegokolwiek statusu z produktu. Pozycja to szacunek
  (patrz wyżej). „Vent"/„My" nie są wystawione w interfejsie 1W.
- **2W**: realny status jest, ale tylko dla produktów, które odpowiedzą na
  skan (muszą mieć klucz instalacji — czyli być w tej samej instalacji co
  pilot, z którego robiliśmy ekstrakcję). Produkt z innej instalacji nie
  odpowie i nie da się nim sterować po 2W.
- **2W**: status odświeżamy po komendzie i co 2 minuty; między odczytami
  pozycja w aplikacji jest animowana (szacunek).
- **2W**: „wentylacja" okna dachowego jest dostępna jako komenda testowa
  w panelu WWW (kanał SUPLA to zwykłe okno/roleta — nie ma osobnej funkcji
  wentylacji).
- Sterowanie wymaga, by produkt znajdował się w zasięgu 868 MHz.
- Po zmianie tożsamości 1W („Nowa tożsamość") trzeba enrolować od nowa.
- Firmware sam przeskakuje po kanałach CH1/CH2/CH3 (nasłuch 2W); 1W nadaje
  na CH2. W trybie ekstrakcji klucza trzyma CH2.

## Pytania otwarte

- Ile rolet i ile okien ma zostać obsłużonych (ile wpisów w `VELUX_SLOTS`)?
- Jaki dokładnie model produktów (INTEGRA/SML/…) i czy są zasilane z sieci
  (KUX 110 / INTEGRA) czy solarnie/bateryjnie? (Wpływa na preambułę 1W
  i na to, czy urządzenie odpowie na skan 2W w pierwszym przebiegu.)
- Czy któryś produkt to INTEGRA z czujnikiem deszczu (ograniczenia
  w 2W są raportowane w statusie — warto sprawdzić na sprzęcie)?
