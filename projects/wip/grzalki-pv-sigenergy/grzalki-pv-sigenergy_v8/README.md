# Grzałki PV v8, bez MAIN, ESP32 DevKit V1, DFRobot DFR0971

V8 powstała na prośbę użytkownika jako oddzielna wersja **bez stycznika MAIN**.
Folder v7 pozostaje bez zmian. GPIO27 nie jest inicjalizowane ani sterowane.
Pozostają dwa wyjścia H2/H3 i **DFRobot DFR0971 z GP8403 (12 bitów)**.

Zmiany względem v7:
- Usunięto MAIN również z obliczania obciążenia i raportowania zadanej mocy.
- Po błędzie DAC sterownik wyłącza H2/H3 i próbuje wysłać 0 V, ponawiając
  nieudane zerowanie co około sekundę. Alarm pozostaje do restartu.
- Odczyt Sigenergy domyślnie: **30005/30006, FC04, unit 247, gain 1000**.
  W SUPLA jest jedna podpisana wartość mocy sieci: `+` pobór, `-` eksport.
- BOOT: przytrzymanie 5 s **lub 5 szybkich kliknięć** otwiera konfigurację.
  Ponowne użycie przycisku nie kasuje ustawień ani nie zamyka konfiguracji.
- LED GPIO2 miga w konfiguracji **100 ms ON / 100 ms OFF** (około 5 Hz),
  poza nią jest wyłączona. Konfiguracja nie ma timeoutu bezczynności.

**Bez MAIN nie ma programowo sterowanego odcięcia zasilania grzałki SSR.**
Awaria DAC może zostawić ostatnie napięcie, a uszkodzony regulator może grzać
nawet przy 0 V. Niezależny termostat bezpieczeństwa musi fizycznie odcinać
zasilanie wszystkich grzałek, także przy zawieszeniu ESP32.
**Nie wgrano ani nie testowano v8 na ESP32. Najpierw testy niskonapięciowe.**
Wyniki kompilacji i kontroli źródeł: sekcja walidacji oraz `../STATUS.md`.

## DAC zachowany z v7

- `gp8403.h`: kod 0–4095 przesunięty o **4 bity**, zamiast 0–32767 o 1 bit.
  Wartości są ograniczane do zakresu przed konwersją na liczbę całkowitą.
- Zapis little-endian: najpierw młodszy bajt. Pełna skala to `F0 FF`.
- Adres I²C `0x58`, rejestr kanału OUT0 `0x02`. Zakres 0–10 V:
  zapis `0x11` do rejestru `0x01`, zgodny z DFRobot_GP8XXX 1.1.0.
- Przy starcie zerowane są oba wyjścia, a regulacja korzysta tylko z OUT0.
- Nadal używamy sprawdzanych zapisów `Wire`, nie wywołań bez kontroli ACK.
  Błąd I²C zatrzaskuje alarm DAC; próby zerowania nie kasują alarmu.
  Nie zapisujemy nastawy napięcia do pamięci trwałej DAC (`store()` nie jest
  wywoływane). ACK nie potwierdza faktycznego napięcia ani prądu grzałki.
- `ssr_max_power` nadal oznacza moc grzałki przy 10 V. Kanały SUPLA zachowują
  numery 0–8; GPIO25/26 pozostają, GPIO27 nie jest używane.

## Zachowane sterowanie i zabezpieczenia v6

- Jeden właściciel sprzętu grzałek: `Controller::Engine` obsługuje DS18B20,
  GP8403 i GPIO25/26. Pętla SUPLA inicjalizuje przekaźniki jako OFF przed
  startem zadania, a potem już ich nie zapisuje. LED obsługuje biblioteka SUPLA.
- Zadanie `heater-control` (FreeRTOS, rdzeń 1, priorytet 2, stos 8192 B)
  wykonuje pomiary i sterowanie niezależnie od `SuplaDevice.iterate()`.
  Normalna przerwa między obiegami zadania wynosi 50 ms. Nie wywołuje sieci.
- `Mailbox` przekazuje kopie stanu pod blokadą `portMUX_TYPE`. Nie używa
  `volatile` jako substytutu synchronizacji. Sekcje krytyczne zawierają
  operacje na danych i krótkie zapisy GPIO, nigdy I2C, 1-Wire ani sieć.
- Po zapisie DAC ponownie sprawdzane są blokady, terminy i generacja/rewizja
  żądań. OFF lub zmiana trybu podczas I2C unieważnia wcześniejszą decyzję ON.
- Błąd czujnika, przegrzanie, brak świeżego odczytu lub DAC unieważniają
  ręczne żądania. Generacja awarii pozostaje zmieniona również po odzyskaniu
  temperatury, więc opóźnione ON sprzed awarii nie odżyje po powrocie SUPLA.
- Odrzucone ON nie usuwa timera wyłączenia trwającego żądania.
- Czas zakończenia ręcznego ON jest przekazywany ze skutecznego timera SUPLA
  do zadania sterującego. Wyłączenie nie czeka na powrót z TLS/WWW.
- Nieudane utworzenie zadania pozostawia H2/H3 OFF i uruchamia alarm sprzętowy.
  DAC nie jest wtedy inicjalizowany; jeśli ma osobne zasilanie, może zachować
  poprzednie napięcie. Reset ESP32 nie gwarantuje wyłączenia regulatora.
- Zachowano prawidłową inicjalizację zatrzasku GPIO, limit połączenia Modbus
  300 ms i wymóg IPv4. Progi grzania i kolejność kanałów są zachowane;
  klucze WWW dla mapy Modbus zostały zmienione, aby nie odziedziczyć 5004.

## Temperatura i odcięcie

Wymagany dokładnie jeden DS18B20, zasilanie trzyprzewodowe, nie pasożytnicze.
Wykrywanie przy starcie; po zmianie połączeń należy zrestartować urządzenie.
Konwersja co 3 s, odczyt po co najmniej 800 ms. Błędny odczyt, odłączenie,
85°C lub wartość poza zakresem −20…125°C blokują grzanie. CRC sprawdza biblioteka.

**Limit świeżości wynosi ponownie 6 s, nie 30 s.** Pomiar wykonuje zadanie
sterujące, więc powolny klient WWW nie powoduje starzenia odczytu.
Wszystkie tryby podlegają temperaturze STOP (maks. 80°C) i histerezie powrotu.
Po awarii MANUAL wymaga nowego polecenia ON. AUTO może wznowić pracę po usunięciu
blokady, uzyskaniu nowego pomiaru sieci i upływie odstępu przełączeń.
Błąd DAC pozostaje zatrzaśnięty do restartu.
Podczas grzania kod DAC jest potwierdzany transmisją co około 1 s również przy
stałej mocy. Ta kontrola nie resetuje epoki wyjść ani czasu stabilizacji Modbus.

To nie jest układ bezpieczeństwa czasu rzeczywistego z gwarantowanym terminem:
50 ms to przerwa zadania, nie gwarancja odcięcia. Trzeba doliczyć odczyt czujnika,
czas I2C (ustawiony limit transakcji 50 ms), planowanie zadań i pracę stycznika.
Zawieszenie całego ESP32, awaria DAC, regulatora lub sklejony przekaźnik wymagają
zabezpieczenia sprzętowego. Opisy OFF/mocy w tej instrukcji dotyczą poleceń,
nie zmierzonego przepływu prądu.
Alarm w chmurze jest wysyłany dopiero, gdy komunikacja SUPLA może działać.

## Sterowanie i timery

- AUTO: budżet = moc zadana podczas zapytania − podpisana moc sieci.
  Dodatnia moc sieci oznacza import, ujemna eksport.
- H2/H3 mają histerezę i minimalny odstęp. SSR dostaje resztę budżetu.
- MANUAL: tylko H2/H3; SSR = 0. Nie wymaga sprawnego falownika.
- Zmiana AUTO/MANUAL albo wyjście z normalnego trybu urządzenia zadaje OFF/0 V
  i unieważnia żądania. ON wysłane od razu podczas przejścia może zostać odrzucone;
  poczekać na przetworzenie zmiany trybu i ponowić świadomie.
- OFF i koniec czasu grzania omijają minimalny odstęp przełączeń.
  Polecenie sieciowe może zostać odebrane z opóźnieniem, jeżeli sieć się blokuje;
  nie obiecujemy czasu od naciśnięcia przycisku w aplikacji.
- Kanały H2/H3 i trybu udostępniają funkcję **przełącznika zasilania**.
  Timery są jednorazowe. Cykle i funkcja schodowa nie są obsługiwane.
  Po aktualizacji sprawdzić wybór funkcji kanałów w SUPLA Cloud.
- Opóźnione ON jest wykonywane przez bibliotekę SUPLA i może spóźnić się
  podczas blokady jej pętli. Awaria lub zmiana trybu kasuje taki timer.
- Jeżeli upłynie timer kanału AUTO/MANUAL podczas blokady SUPLA, sterownik
  wyłącza wyjścia i czeka na przetworzenie zmiany trybu przez bibliotekę.

## Modbus i bilans mocy

Limit nawiązania TCP: 300 ms; przerwa prób liczona od zakończenia połączenia.
Tylko jedna transakcja naraz, stały dwurejestrowy bufor. Odpowiedzi po zmianie
generacji sterowania lub epoki wyjść są odrzucane. Podczas zmiany wyjść nie
przyjmuje się próbek; po zmianie odczekuje się co najmniej 2 s.
Odpowiedź starsza niż 1 s od wysłania zapytania jest odrzucana. AUTO przestaje
grzać po błędzie zaakceptowanej transakcji lub 10 s bez świeżych danych, również
gdy pętla sieciowa jest zablokowana. MANUAL nie ma tej zależności.

Moc jest **szacowana**, nie mierzona. Zależność wejścia 0–10 V od mocy regulatora
musi być zweryfikowana. Zwykły SSR ON/OFF nie nadaje się do tego sterowania.
Opóźnienie pomiaru falownika, moc znamionowa i nieliniowość regulatora wpływają
na bilans. Minimalny odstęp przełączeń może powodować przejściowy import.

## Konfiguracja

1. Przy pierwszym uruchomieniu bez wymaganych ustawień SUPLA uruchamia AP.
   Aktualizacja v7 → v8 bez kasowania flash zachowuje ustawienia i nie wymusza AP.
   Aby otworzyć konfigurację działającego urządzenia, przytrzymać BOOT/GPIO0
   przez około 5–6 s **albo nacisnąć go 5 razy**, przerwy krótsze niż 400 ms.
   Nie trzymać BOOT podczas resetu/włączania zasilania, bo uruchomi bootloader.
2. Szybkie miganie LED GPIO2 oznacza konfigurację. Połączyć telefon z siecią
   `PV-HEATERS-V8-...` i otworzyć **http://192.168.4.1**. Zachowana własna nazwa
   urządzenia może zmienić nazwę AP. Zgodzić się na WiFi bez Internetu;
   w razie problemów wyłączyć dane komórkowe i VPN w telefonie.
3. Wprowadzić WiFi, SUPLA i IPv4 falownika. Dla mocy sieci Sigenergy:
   `sig_grid_addr=30005`, `sig_grid_input=1`, `mb_unit=247`, `mb_port=502`,
   `grid_scale=1000`. To odpowiada tabeli 5.1 protokołu Modbus v1.7.
4. **SAVE & RESTART** zapisuje i stosuje parametry. EN/RST robi zwykły restart,
   nie reset fabryczny. Samego zapisu bez restartu nie traktować jako zmiany
   nastaw pracującego kontrolera. Nie ma automatycznego wyjścia z konfiguracji
   po bezczynności; pojedynczy BOOT też jej nie zamyka.
5. GUID/AUTHKEY przechowuje/generuje SUPLA w LittleFS. Nie kasować flash ani
   zmieniać partycji bez kopii ustawień i tożsamości.
6. WWW pozostaje dostępne w LAN pod adresem ESP32 przydzielonym przez router.
   **Brak uwierzytelnienia WWW: wymagana zaufana, odizolowana sieć, bez
   przekierowania portu do Internetu.**

### Odzyskanie dostępu, kiedy na płytce nadal jest v7

Odłączyć zasilanie grzałek. Puścić BOOT, nacisnąć i puścić EN/RST, odczekać
około 10 s. Dopiero wtedy przytrzymać BOOT przez 5–6 s i puścić. Połączyć się
z `PV-HEATERS-V7-...`, otworzyć `http://192.168.4.1`.
**V7 nie obsługuje pięciu kliknięć ani LED konfiguracji z v8.**
W v7 kolejne długie przytrzymanie już w konfiguracji może uruchomić reset
fabryczny, więc nie powtarzać go w ciemno. Zwykły EN/RST nie usuwa WiFi.
Wcześniejsza informacja o obowiązkowym timeoutcie 5 min w v7 była nieprecyzyjna:
przy `StartInCfgMode` biblioteka nie włącza tego ogólnego timeoutu.

Jeżeli AP nie pojawi się i urządzenia nie ma na liście DHCP routera, podłączyć
ESP32 przewodem USB **z transmisją danych**, sprawdzić port COM i log 115200.
Nie kasować całej pamięci jako pierwszej próby. Ponowne wgranie tego samego
układu partycji bez erase-all zachowuje konfigurację.
Na tej płytce przy poprzednim wgrywaniu trzeba było **trzymać BOOT podczas
uploadu**, następnie puścić i nacisnąć EN/RST. To odrębna procedura od konfiguracji.

| Parametr | Domyślnie / znaczenie |
|---|---|
| `inverter_ip` | wymagany poprawny IPv4 |
| `sig_grid_addr` | 30005; adres PDU dwóch rejestrów mocy sieci |
| `sig_grid_input` | 1 = Input FC04 (zalecane); 0 = Holding FC03 |
| `mb_unit`, `mb_port` | 247, 502 |
| `word_order` | 1 = starsze słowo pierwsze; 0 = zamiana |
| `grid_scale` | 1000; mnożnik do W = parametr / 1000 |
| `ssr_on`, `ssr_off` | 200 / 80 W |
| `h2_on`, `h2_off` | 1700 / 1450 W |
| `h3_on`, `h3_off` | 3200 / 2950 W |
| `temp_limit`, `temp_reset` | 80 / 75°C |
| `heater_power` | 1500 W na każdą H2/H3 |
| `ssr_max_power` | 1500 W przy 10 V |
| `min_switch_s` | 60 s, także po awaryjnym wyłączeniu |

Typ danych Modbus: signed int32, nie IEEE754 float. Dla W `grid_scale=1000`,
dla mW `1`, dla całkowitych kW `1000000`. Sprawdzić typ, znak i kolejność słów.

## Sprzęt i kanały

Nie podłączać przekaźnika MAIN do GPIO27 w v8. Niezależny termostat bezpieczeństwa
odcinający **wszystkie trzy grzałki**, także SSR, pozostaje wymagany.
Zmianę okablowania części sieciowej i dobór odcięcia zlecić elektrykowi.

| Połączenie niskonapięciowe | GPIO |
|---|---|
| DS18B20 DQ, 4,7 kΩ do 3,3 V | 4 |
| Sterowniki H2 / H3 | 25 / 26 |
| MAIN, nieużywany | 27, zostawić niepodłączone |
| Wbudowana LED konfiguracji, domyślnie aktywna HIGH | 2 |
| DFR0971 (GP8403) SDA / SCL | 21 / 22 |
| BOOT/config do GND | 0 |

LED jest obsługiwana przez `Supla::Device::StatusLed` w trybie
`LED_IN_CONFIG_MODE_ONLY`. Sekwencja biblioteki: 100/100 ms, poza konfiguracją OFF.
Domyślnie `CONFIG_LED_PIN = 2`, `CONFIG_LED_INVERTED = false` w szkicu `.ino`.
Sprawdzić wariant płytki; dioda zasilania nie jest sterowalna programowo.
Miganie zależy od wykonywania pętli SUPLA, nie jest watchdogiem ani sygnałem
potwierdzającym fizyczne wyłączenie grzałek.

DS18B20 VDD = 3,3 V. DFR0971 obsługuje zasilanie 3,3–5 V; w tym połączeniu
z ESP32 użyć **3V3**, żeby zachować poziomy logiczne magistrali.

| ESP32 | DFR0971 |
|---|---|
| 3V3 | VCC / + |
| GND | GND / − |
| GPIO21 | SDA / D |
| GPIO22 | SCL / C |

Ustawić przełączniki adresowe modułu na **0x58**. Wyjście **VOUT0** połączyć
z wejściem 0–10 V odpowiedniego regulatora, jego analogowy COM z GND DAC.
VOUT1 pozostaje niepodłączone. Nie podawać 10/12 V na VCC ani na GPIO ESP32.
Zwykły SSR opisany „3–32 V DC” nie jest przez to regulatorem proporcjonalnym.
Schemat i specyfikacja modułu:
https://wiki.dfrobot.com/SKU_DFR0971_2_Channel_I2C_0_10V_DAC_Module

Nie podłączać cewki stycznika bezpośrednio do GPIO.
Domyślnie HIGH = ON. Dla modułów LOW-active zmienić `RELAY_ON`/`RELAY_OFF`
w `controller.h`. Wymagany sprzętowy stan OFF podczas resetu ESP32.

Kanały 0–8: temperatura, H2, H3, alarm temperatury, alarm Modbus,
podpisana moc sieci Sigenergy, MANUAL, awaria DAC/zadania sterującego,
błędna konfiguracja.

## Walidacja

Środowisko referencyjne: ESP32 core 3.3.3, SuplaDevice 26.8,
modbus-esp8266 4.1.0, DallasTemperature 4.0.6, OneWire 2.3.8.
Interfejsy sprawdzono w zainstalowanych źródłach.

- `verify_source.py`: 27 kontroli, w tym dotychczasowe warunki strukturalne,
  brak MAIN, bilans mocy bez MAIN, ponawianie zerowania DAC, BOOT i LED.
  Zachowano sprawdzenie stałych GP8403, ścieżki zapisu i modelu ramek w Pythonie.
  Model sprawdza wszystkie 4096 kodów i wektory 0/25/50/75/100% oraz ograniczenie
  zakresu, ale nie wykonuje funkcji C++.
  Wynik 2026-10-06: **27/27 kontroli zakończonych poprawnie**.
  **To nie są testy działania firmware ani dowód poprawnej kompilacji.**
- `control_logic.h`: 31 asercji `static_assert` rzeczywistych funkcji;
  `gp8403.h`: 10 dodatkowych asercji kodowania DAC. Weryfikuje je pełna kompilacja.
- Kompilacja v8 (2026-10-06): **EXIT=0**, flash 1 394 967 B (44%),
  RAM 51 964 B (15%).
- Wgranie i testy ESP32, BOOT/LED/AP, falownika i napięcia DAC: **niewykonane**.

Uruchomienie kontroli źródeł, bez kompilatora:

```powershell
py -3 .\verify_source.py
```

Kompilacja z folderu v8:

```powershell
arduino-cli compile --jobs 4 --fqbn esp32:esp32:esp32:PartitionScheme=huge_app .
```

Docelowo ESP32 z 4 MB flash. `huge_app` daje 3 MB aplikacji, **bez OTA**.
Przy aktualizacji v7 zachować ten sam układ partycji; nie używać erase-all.

### Przed podłączeniem grzałek, testy na diodach/mierniku

1. Start/reset z obiema polaryzacjami modułów: brak impulsu ON.
2. Ręczne ON i opóźnione ON, potem odłączenie/ponowne podłączenie DS18B20:
   brak wznowienia bez nowego żądania; powtórzyć przy zablokowanym WWW/TLS.
3. ON na 10 s i blokada komunikacji na ponad 10 s: wyjście gaśnie niezależnie
   od chmury; po powrocie komunikacji nie włącza się ponownie.
4. Odrzucone ON przy istniejącym timerze OFF: brak utraty terminu wyłączenia.
5. OFF i zmiana trybu podczas I2C: brak załączenia z wcześniejszej decyzji.
6. Bez grzałek: odłączenie SDA/SCL przy niezerowej nastawie, H2/H3 OFF,
   zatrzaśnięty alarm DAC. Zmierzyć, czy DAC zachowuje poprzednie napięcie.
   Po przywróceniu połączenia sprawdzić ponowienie 0 V i utrzymanie alarmu
   do restartu. Sprawdzić też błąd pierwszego zapisu niezerowej nastawy.
7. Zablokowany falownik: AUTO gaśnie po utracie danych, MANUAL zachowuje pomiar
   temperatury i lokalne terminy wyłączenia.
8. Wejście do konfiguracji osobno przytrzymaniem i 5 kliknięciami, szybkie
   miganie LED, pozostanie AP po bezczynności ponad 5 min, SAVE & RESTART.
   Ponowne przytrzymanie w konfiguracji nie może kasować ustawień.
   Sprawdzić brak restartu po pojedynczym kliknięciu i ponowny dostęp WWW w LAN.
9. Stała nadwyżka oraz nagły zanik PV: sprawdzić znak, skalę i stabilność.
10. Bez podłączonych grzałek zmierzyć VOUT0 względem GND: po inicjalizacji
    i w MANUAL około 0 V (SSR w MANUAL jest wyłączony). Przy kontrolowanej
    nastawie SSR 25/50/75/100% oczekiwać około 2,5/5/7,5/10 V, z uwzględnieniem
    rozdzielczości 12-bitowej i dokładności modułu. To plan testu stanowiskowego,
    nie informacja, że takie pomiary już wykonano.
