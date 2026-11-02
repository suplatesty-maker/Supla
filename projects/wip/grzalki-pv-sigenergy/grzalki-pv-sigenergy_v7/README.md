# Grzałki PV v7, ESP32 DevKit V1 (30 pinów), DFRobot DFR0971

V7 dostosowuje v6 do modułu **DFRobot DFR0971 z GP8403 (12 bitów)**.
Nie jest to GP8413 (15 bitów). Sterowanie, zabezpieczenia i ustawienia v6
pozostają zachowane; zmienia się kodowanie napięcia DAC.
**Nie kompilowano i nie testowano na ESP32. Nie podłączać grzałek przed
kompilacją i testami niskonapięciowymi.** V2–v6 pozostają wyłącznie historią.

## Zmiana DAC względem v6

- `gp8403.h`: kod 0–4095 przesunięty o **4 bity**, zamiast 0–32767 o 1 bit.
  Wartości są ograniczane do zakresu przed konwersją na liczbę całkowitą.
- Zapis little-endian: najpierw młodszy bajt. Pełna skala to `F0 FF`.
- Adres I²C `0x58`, rejestr kanału OUT0 `0x02`. Zakres 0–10 V:
  zapis `0x11` do rejestru `0x01`, zgodny z DFRobot_GP8XXX 1.1.0.
- Przy starcie zerowane są oba wyjścia, a regulacja korzysta tylko z OUT0.
- Nadal używamy sprawdzanych zapisów `Wire`, nie wywołań bez kontroli ACK.
  Błąd I²C zatrzaskuje alarm DAC i odcina MAIN. Nie zapisujemy nastawy
  napięcia do pamięci trwałej DAC (`store()` nie jest wywoływane).
- `ssr_max_power` nadal oznacza moc grzałki przy 10 V. Kanały SUPLA i GPIO
  nie zmieniają numerów.

## Zachowane sterowanie i zabezpieczenia v6

- Jeden właściciel sprzętu: `Controller::Engine` obsługuje DS18B20, GP8403
  i GPIO. Pętla SUPLA inicjalizuje wyjścia jako OFF przed startem zadania,
  a potem już ich nie zapisuje.
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
- Nieudane utworzenie zadania pozostawia OFF i uruchamia alarm sprzętowy.
- Zachowano prawidłową inicjalizację zatrzasku GPIO, limit połączenia Modbus
  300 ms i wymóg IPv4. Parametry WWW i kolejność kanałów są zachowane.

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
Zawieszenie całego ESP32 lub awaria stycznika wymagają zabezpieczenia sprzętowego.
Alarm w chmurze jest wysyłany dopiero, gdy komunikacja SUPLA może działać.

## Sterowanie i timery

- AUTO: budżet = moc zadana podczas zapytania − podpisana moc sieci.
  Dodatnia moc sieci oznacza import, ujemna eksport.
- H2/H3 mają histerezę i minimalny odstęp. SSR dostaje resztę budżetu.
- MANUAL: tylko H2/H3; SSR = 0. Nie wymaga sprawnego falownika.
- Zmiana AUTO/MANUAL albo wyjście z normalnego trybu urządzenia odcina wyjścia
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

1. Po przyszłej kompilacji całego folderu v7 skonfigurować urządzenie przez AP.
   V7 jawnie ustawia `StartInCfgMode`. BOOT/GPIO0 przytrzymany 5 s otwiera
   konfigurację ponownie.
2. Wprowadzić WiFi, SUPLA i IPv4 falownika. Rejestr **5004 jest wartością
   roboczą, nie potwierdzoną mapą Sigenergy**.
3. Zapis i restart stosują parametry. Samego zapisu bez restartu nie traktować
   jako zmiany nastaw pracującego kontrolera.
4. GUID/AUTHKEY przechowuje/generuje SUPLA w LittleFS. Nie kasować flash ani
   zmieniać partycji bez kopii ustawień i tożsamości.
5. WWW pozostaje dostępne w LAN oraz wraca po timeoutcie trybu konfiguracji.
   **Brak uwierzytelnienia WWW: wymagana zaufana, odizolowana sieć, bez
   przekierowania portu do Internetu.**

| Parametr | Domyślnie / znaczenie |
|---|---|
| `inverter_ip` | wymagany poprawny IPv4 |
| `reg_addr` | 5004; adres PDU dwóch rejestrów, 0–65534 |
| `mb_input` | 0 = Holding FC03; 1 = Input FC04 |
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

Wymagany niezależny termostat bezpieczeństwa oraz normalnie otwarty stycznik
MAIN odcinający **wszystkie trzy grzałki**, także SSR. ACK DAC nie potwierdza
napięcia ani rzeczywistego prądu. Montaż części sieciowej zlecić elektrykowi.

| Połączenie niskonapięciowe | GPIO |
|---|---|
| DS18B20 DQ, 4,7 kΩ do 3,3 V | 4 |
| Sterowniki H2 / H3 / MAIN | 25 / 26 / 27 |
| DFR0971 (GP8403) SDA / SCL | 21 / 22 |
| BOOT/config do GND | 0 |

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
zadana moc SSR, MANUAL, awaria DAC/zadania sterującego, błędna konfiguracja.

## Walidacja bez kompilacji

Środowisko referencyjne: ESP32 core 3.3.3, SuplaDevice 26.8,
modbus-esp8266 4.1.0, DallasTemperature 4.0.6, OneWire 2.3.8.
Interfejsy sprawdzono w zainstalowanych źródłach.

- `verify_source.py`: 20 kontroli, w tym dotychczasowe warunki strukturalne
  oraz sprawdzenie stałych GP8403, ścieżki zapisu i modelu ramek w Pythonie.
  Model sprawdza wszystkie 4096 kodów i wektory 0/25/50/75/100% oraz ograniczenie
  zakresu, ale nie wykonuje funkcji C++.
  Wynik 2026-10-06: **20/20 kontroli zakończonych poprawnie**.
  **To nie są testy działania firmware ani dowód poprawnej kompilacji.**
- `control_logic.h`: 31 asercji `static_assert` rzeczywistych funkcji;
  `gp8403.h`: 10 dodatkowych asercji kodowania DAC. Wszystkie są
  **nieuruchomione**, ponieważ nie kompilowano.
- W v6 wykonano dwa przeglądy Engine/Mailbox i adaptera SUPLA/Modbus.
  Zmiany v7 porównano ze sterownikiem producenta DFRobot_GP8XXX 1.1.0.
- Testy ESP32, falownika, WWW i napięcia DAC: **niewykonane**.

Uruchomienie kontroli źródeł, bez kompilatora:

```powershell
py -3 .\verify_source.py
```

Przyszła kompilacja (nie wykonana w tej zmianie), z folderu v7:

```powershell
arduino-cli compile --jobs 4 --fqbn esp32:esp32:esp32:PartitionScheme=huge_app .
```

Docelowo ESP32 z 4 MB flash. `huge_app` daje 3 MB aplikacji, **bez OTA**.
Nie zakładać rozmiaru binarki v7 na podstawie wcześniejszej v4.

### Przed podłączeniem grzałek, testy na diodach/mierniku

1. Start/reset z obiema polaryzacjami modułów: brak impulsu ON.
2. Ręczne ON i opóźnione ON, potem odłączenie/ponowne podłączenie DS18B20:
   brak wznowienia bez nowego żądania; powtórzyć przy zablokowanym WWW/TLS.
3. ON na 10 s i blokada komunikacji na ponad 10 s: wyjście gaśnie niezależnie
   od chmury; po powrocie komunikacji nie włącza się ponownie.
4. Odrzucone ON przy istniejącym timerze OFF: brak utraty terminu wyłączenia.
5. OFF i zmiana trybu podczas I2C: brak załączenia z wcześniejszej decyzji.
6. Odłączenie SDA/SCL: MAIN OFF, zatrzaśnięty alarm DAC.
7. Zablokowany falownik: AUTO gaśnie po utracie danych, MANUAL zachowuje pomiar
   temperatury i lokalne terminy wyłączenia.
8. Wejście do konfiguracji, zapis/restart, timeout AP i ponowny dostęp WWW.
9. Stała nadwyżka oraz nagły zanik PV: sprawdzić znak, skalę i stabilność.
10. Bez podłączonych grzałek zmierzyć VOUT0 względem GND: po inicjalizacji
    i w MANUAL około 0 V (SSR w MANUAL jest wyłączony). Przy kontrolowanej
    nastawie SSR 25/50/75/100% oczekiwać około 2,5/5/7,5/10 V, z uwzględnieniem
    rozdzielczości 12-bitowej i dokładności modułu. To plan testu stanowiskowego,
    nie informacja, że takie pomiary już wykonano.
