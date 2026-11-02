# Grzałki PV v4, ESP32 DevKit V1 (30 pinów)

Wersja poprawiająca błędy v3. **Nie jest jeszcze sprawdzona na sprzęcie.**
Nie używać v2/v3 do sterowania podłączonymi grzałkami.

## Zabezpieczenia i ograniczenia

- Przekaźnik główny musi odcinać zasilanie **wszystkich trzech grzałek**, także SSR.
  Wymagany niezależny termostat bezpieczeństwa, odpowiednie zabezpieczenia,
  stycznik i obudowa. Montaż części sieciowej powinien wykonać elektryk.
- Domyślna logika wszystkich modułów przekaźnikowych: HIGH = załączenie.
  Dla innych modułów ustawić `RELAY_ON` i `RELAY_OFF`. Zapewnić sprzętowy
  stan OFF podczas resetu ESP32, zanim program ustawi GPIO.
- Każda nieudana transmisja do DAC zatrzaskuje awarię i wyłącza tor główny.
  Nie ma automatycznego wznowienia, potrzebny jest restart po usunięciu awarii.
  ACK I2C potwierdza transmisję, **nie potwierdza napięcia ani wyłączenia SSR**.
- Wymagany dokładnie jeden DS18B20 zasilany trzema przewodami. Brak czujnika,
  odłączenie, błędny pomiar i wartość startowa 85°C blokują grzanie.
  Konwersja co 3 s, odbiór po 800 ms; brak świeżego wyniku przez 6 s blokuje pracę.
  Czujnik jest wykrywany podczas startu, po zmianie połączeń należy zrestartować ESP32.
- Limit temperatury maks. 80°C; temperatura powrotu musi być niższa od limitu.
  Nieprawidłowe pary progów lub IP blokują grzanie w obu trybach.
- W trybie konfiguracji, aktualizacji i przed inicjalizacją grzanie jest wyłączone.
- WWW w LAN jest **bez uwierzytelnienia**. Nie jest bezpieczne tylko dlatego,
  że działa w domu. Używać wyłącznie zaufanej, odizolowanej sieci; nie przekierowywać
  portu ani udostępniać urządzenia niezaufanym klientom. Logowanie nie jest wdrożone.

## Konfiguracja i uruchomienie

1. Kompilować cały folder v4, nie pojedynczy plik ani folder nadrzędny.
2. Na pustej pamięci standardowy mechanizm SUPLA uruchamia konfigurację.
   Przycisk BOOT/GPIO0 pozwala ponownie wejść do konfiguracji.
3. Wprowadzić WiFi, e-mail SUPLA, serwer jeśli wymagany, IPv4 falownika
   oraz sprawdzone parametry Modbus. **Rejestr 5004 to odziedziczona wartość
   robocza, nie potwierdzona mapa rejestrów Sigenergy.**
4. Zapis i restart stosują parametry sterowania. Sam zapis bez restartu nie
   zmienia aktywnych nastaw. Dla błędnej konfiguracji pojawi się alarm i komunikat
   Serial; poprawić pola, ponownie zapisać i zrestartować.
5. GUID/AUTHKEY generuje SUPLA i zapisuje w LittleFS. Nie wpisywać zer ani kluczy
   w kod. Istniejąca tożsamość z LittleFS jest zachowana. Nie kasować flash:
   kasowanie usuwa również WiFi, ustawienia i tożsamość.
6. Po połączeniu WWW jest dostępne pod IP urządzenia. Serwer uruchamia się ponownie
   także po wyjściu z konfiguracji wskutek timeoutu.

## Sterowanie

AUTO wymaga poprawnej temperatury, DAC, konfiguracji i świeżego Modbus.
MANUAL nie wymaga danych falownika, ale wszystkie pozostałe blokady działają.
Zmiana AUTO/MANUAL wyłącza wszystkie wyjścia i kasuje poprzednie żądania.
W MANUAL po zmianie trybu należy ponownie włączyć wybraną grzałkę w SUPLA.
SSR w MANUAL pozostaje wyłączony.

Zwykłe kanały przekaźnikowe nie sterują GPIO. Żądanie ON poza MANUAL lub przy
aktywnej blokadzie jest odrzucane. Fizyczne wyjścia zmienia tylko kontroler.
W MANUAL polecenie ON oczekuje na upływ minimalnego odstępu, a OFF i koniec
odliczania wyłączają grzałkę przy następnym cyklu sterowania (maks. około 1 s).
Stan pokazywany w SUPLA jest stanem wyjścia, nie oczekującym żądaniem.
Wyłączenie awaryjne zawsze omija minimalny odstęp i kasuje oczekujące żądania.

AUTO rekonstruuje budżet grzania:

`budżet = zadana moc grzałek w chwili zapytania − moc sieci`

Moc sieci dodatnia oznacza import, ujemna eksport. Każda odpowiedź jest używana
raz. Po zmianie wyjść program czeka min. 2 s przed kolejnym zapytaniem; odpowiedź
otrzymana po kolejnej zmianie wyjść jest odrzucana. Błąd transakcji lub dane starsze
niż 10 s blokują AUTO. Grzałki 2/3 mają histerezę i minimalny odstęp przełączeń.
SSR otrzymuje pozostały budżet po odjęciu ich mocy.

**Moc jest szacowana, a nie mierzona.** Zależność 0–10 V → moc musi odpowiadać
regulatorowi mocy grzałki. Zwykły SSR z wejściem ON/OFF nie nadaje się do tego
sterowania. Opóźnienie pomiaru falownika, nieliniowość regulatora i błędna moc
znamionowa mogą pogorszyć regulację. `SETTLE_MS` trzeba zweryfikować na sprzęcie.
Minimalny czas przełączeń może powodować przejściowy pobór energii z sieci.

## Parametry WWW

| Parametr | Domyślnie / znaczenie |
|---|---|
| `inverter_ip` | wymagany poprawny adres IPv4 |
| `reg_addr` | 5004; adres PDU, 0–65534 dla odczytu dwóch rejestrów |
| `mb_input` | 0 = Holding FC03; 1 = Input FC04 |
| `mb_unit`, `mb_port` | 247, 502 |
| `word_order` | 1 = starsze słowo pierwsze; 0 = zamienione |
| `grid_scale` | 1000; mnożnik do W = wartość / 1000 |
| `ssr_on`, `ssr_off` | 200 / 80 W budżetu |
| `h2_on`, `h2_off` | 1700 / 1450 W budżetu |
| `h3_on`, `h3_off` | 3200 / 2950 W budżetu |
| `temp_limit`, `temp_reset` | 80 / 75°C |
| `heater_power` | 1500 W na każdą z grzałek 2/3; zakładamy jednakową moc |
| `ssr_max_power` | 1500 W przy 10 V |
| `min_switch_s` | 60 s, również po awaryjnym wyłączeniu |

Dla rejestru wyrażonego w W mnożnik to 1, czyli `grid_scale=1000`.
Dla rejestru wyrażonego w mW to `grid_scale=1`.
Dla całkowitych kW to `grid_scale=1000000`. Kod interpretuje rejestr jako
**signed int32**, nie IEEE754 float. Typ i znak muszą być sprawdzone w dokumentacji.

## GPIO i kanały SUPLA

| Połączenie niskonapięciowe | GPIO |
|---|---|
| DS18B20 DQ, rezystor 4,7 kΩ do 3,3 V | 4 |
| Wejście sterownika przekaźnika H2 / H3 / głównego | 25 / 26 / 27 |
| GP8413 SDA / SCL | 21 / 22 |
| BOOT/config do GND | 0 |

DS18B20 VDD = 3,3 V, GND wspólna z ESP32. Zasilanie GP8413 dobrać według
dokumentacji modułu; poziomy SDA/SCL nie mogą przekraczać 3,3 V na ESP32.
OUT0 DAC łączyć z wejściem 0–10 V odpowiedniego regulatora, nie z GPIO.
Nie podłączać cewki stycznika bezpośrednio do ESP32.

Kanały 0–6 zachowują kolejność v3: temperatura, H2, H3, alarm temperatury,
alarm Modbus, **zadana** moc SSR, tryb MANUAL. Dodano 7: awaria DAC,
8: błędna konfiguracja. Zmiana liczby kanałów może wymagać ponownej konfiguracji
urządzenia w chmurze SUPLA.

## Build i weryfikacja

Zweryfikowano z Arduino core ESP32 3.3.3, SuplaDevice 26.8,
modbus-esp8266 4.1.0, DallasTemperature 4.0.6 i OneWire 2.3.8.
V4 obsługuje GP8413 bezpośrednio przez Wire, aby sprawdzać błędy transmisji;
nie modyfikuje biblioteki DFRobot ani innych zainstalowanych bibliotek.

Z folderu v4 (PowerShell):

```powershell
arduino-cli compile --jobs 4 --fqbn esp32:esp32:esp32:PartitionScheme=huge_app .
```

ESP32 DevKit V1 powinien mieć 4 MB flash, należy potwierdzić rzeczywistą pojemność.
`huge_app` zapewnia 3 MB aplikacji i obszar plików, **bez OTA**.
Nie zmieniać schematu partycji ani nie kasować pamięci bez kopii konfiguracji.

`control_logic.h` zawiera testy regresyjne `static_assert` kompilujące rzeczywiste
funkcje regulatora: błędna histereza, odcięcie temperatury, bilans mocy, przepełnienie
millis, odstępy przełączeń i blokady MANUAL/AUTO.

Końcowa kompilacja 2026-09-13: OK, flash 1 388 511 B (44% partycji),
RAM statyczny 51 940 B (15%). Wszystkie 20 asercji regresyjnych przeszło.
To nie zastępuje testów na ESP32 i falowniku, których nie wykonano.

Przed podłączeniem grzałek sprawdzić na diodach/mierniku:
- start i restart, poprawny stan OFF przekaźników;
- odłączenie DS18B20 podczas AUTO i MANUAL;
- utratę Modbus, odłączenie SDA/SCL przy zasilanym DAC;
- próby ON z SUPLA podczas AUTO i aktywnego alarmu;
- zapis błędnej histerezy i ponowny start;
- wejście/wyjście z konfiguracji i dostępność WWW;
- odpowiedź regulatora na stałą nadwyżkę i nagły zanik PV.
