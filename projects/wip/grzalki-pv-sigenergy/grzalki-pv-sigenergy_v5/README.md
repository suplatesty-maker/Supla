# Grzałki PV v5, ESP32 DevKit V1 (30 pinów)

Wersja poprawiająca 6 błędów znalezionych w drugim przeglądzie v4.
**Nie skompilowana i nie sprawdzona na sprzęcie** (na życzenie użytkownika
poprawki wprowadzono bez kompilacji — przed wgraniem trzeba skompilować).
Nie używać v2/v3/v4 do sterowania podłączonymi grzałkami.

## Co poprawiono względem v4

| # | Problem w v4 | Poprawka w v5 |
|---|---|---|
| P1 | `digitalWrite` przed `pinMode` jest ignorowane w core 3.x, więc przy modułach LOW-active przekaźniki były załączone przez cały start | `initRelayPin()`: `pinMode(INPUT)` → `digitalWrite(OFF)` → `pinMode(OUTPUT)` |
| P1 | zablokowana pętla główna (handshake TLS do 120 s, powolny klient WWW) wstrzymywała odcięcie temperaturowe | zadanie `safetyTask` na rdzeniu 0 odcina wyjścia niezależnie od pętli, pętla po powrocie porządkuje stan (`handleSafetyTrip`) |
| P1 | `clearRequest()` nie kasował uzbrojonego timera SUPLA (opóźnione ON = OFF + DurationMS), grzałka mogła wystartować po ustaniu awarii | kasowanie także przy stanie OFF, gdy `durationMs != 0` |
| P2 | `mb.connect()` blokował pętlę do 3 s przy martwym falowniku | `#define MODBUSIP_CONNECT_TIMEOUT 300` przed nagłówkami Modbus |
| P2 | okno 6 s bez odczytu temperatury powodowało fałszywe odcięcie i kasowanie ręcznych żądań | odcięcie 30 s (`TEMP_STALE_MS`), alarm 8 s (`TEMP_ALARM_MS`), żądania kasowane tylko przy awarii zatrzaskowej |
| P2 | walidacja IP przepuszczała literał IPv6, Modbus łączył się z 0.0.0.0 | wymagane IPv4 (`type() == IPv4`, brak `:` w tekście) |
| P3 | odrzucone żądanie ON z uzbrojonym timerem powodowało ponawianie próby w każdej iteracji | `turnOn()` zeruje odliczanie odrzuconego polecenia |

## Zabezpieczenia i ograniczenia

- Przekaźnik główny musi odcinać zasilanie **wszystkich trzech grzałek**, także SSR.
  Wymagany niezależny termostat bezpieczeństwa, odpowiednie zabezpieczenia,
  stycznik i obudowa. Montaż części sieciowej powinien wykonać elektryk.
  Zadanie nadzoru w firmware **nie zastępuje** termostatu sprzętowego.
- Domyślna logika wszystkich modułów przekaźnikowych: HIGH = załączenie.
  Dla modułów LOW-active ustawić `RELAY_ON`/`RELAY_OFF`. Zapewnić sprzętowy
  stan OFF podczas resetu ESP32, zanim program ustawi GPIO (rezystor podciągający
  na wejściu modułu) — firmware ustala zatrzask OFF w pierwszych instrukcjach
  `setup()`, ale nie działa w trakcie samego resetu.
- Nadzór bezpieczeństwa (`safetyTask`, rdzeń 0, co 100 ms) odcina MAIN/H2/H3,
  gdy: brak inicjalizacji, awaria DAC, awaria czujnika, przegrzanie lub brak
  świeżego pomiaru przez 30 s. Operuje wyłącznie na GPIO — nie używa I2C, 1-Wire
  ani SUPLA, więc działa także gdy pętla główna jest zablokowana przez sieć.
  Po odcięciu pętla główna wykonuje pełne `stopAll()` (DAC, kanały, stan).
- Każda nieudana transmisja do DAC zatrzaskuje awarię i wyłącza tor główny.
  Nie ma automatycznego wznowienia, potrzebny jest restart po usunięciu awarii.
  ACK I2C potwierdza transmisję, **nie potwierdza napięcia ani wyłączenia SSR**.
- Wymagany dokładnie jeden DS18B20 zasilany trzema przewodami. Brak czujnika,
  odłączenie, błędny pomiar i wartość startowa 85°C blokują grzanie.
  Konwersja co 3 s, odbiór po 800 ms. Brak świeżego wyniku: alarm po 8 s,
  odcięcie po 30 s. Czujnik jest wykrywany podczas startu — po zmianie połączeń
  zrestartować ESP32.
- Limit temperatury maks. 80°C; temperatura powrotu musi być niższa od limitu.
  Nieprawidłowe pary progów lub IP blokują grzanie w obu trybach.
- W trybie konfiguracji, aktualizacji i przed inicjalizacją grzanie jest wyłączone.
- WWW w LAN jest **bez uwierzytelnienia**. Używać wyłącznie zaufanej,
  odizolowanej sieci; nie przekierowywać portu. Powolny klient może chwilowo
  zablokować pętlę główną — nadzór bezpieczeństwa działa wtedy dalej.

## Konfiguracja i uruchomienie

1. Kompilować cały folder v5, nie pojedynczy plik ani folder nadrzędny.
2. Na pustej pamięci standardowy mechanizm SUPLA uruchamia tryb konfiguracji
   (pusty LittleFS wymusza `DEVICE_MODE_CONFIG` i punkt dostępowy).
   Przycisk BOOT/GPIO0 (5 s) pozwala wejść do konfiguracji ponownie.
3. Wprowadzić WiFi, e-mail SUPLA, serwer jeśli wymagany, **IPv4** falownika
   oraz sprawdzone parametry Modbus. **Rejestr 5004 to odziedziczona wartość
   robocza, nie potwierdzona mapa rejestrów Sigenergy.**
4. Zapis i restart stosują parametry sterowania. Sam zapis bez restartu nie
   zmienia aktywnych nastaw. Błędna konfiguracja: alarm i komunikat Serial.
5. GUID/AUTHKEY generuje SUPLA i zapisuje w LittleFS. Nie wpisywać kluczy w kod.
   Kasowanie flash usuwa WiFi, ustawienia i tożsamość.
6. Po połączeniu WWW jest dostępne pod IP urządzenia, również po wyjściu
   z konfiguracji wskutek timeoutu (5 min bezczynności).

## Sterowanie

AUTO wymaga poprawnej temperatury, DAC, konfiguracji i świeżego Modbus.
MANUAL nie wymaga danych falownika, ale wszystkie pozostałe blokady działają.
Zmiana AUTO/MANUAL wyłącza wszystkie wyjścia oraz kasuje żądania i timery SUPLA.
SSR w MANUAL pozostaje wyłączony.

Zwykłe kanały przekaźnikowe nie sterują GPIO. Żądanie ON poza MANUAL lub przy
aktywnej blokadzie jest odrzucane. Fizyczne wyjścia zmienia tylko kontroler.
W MANUAL polecenie ON oczekuje na upływ minimalnego odstępu, a OFF i koniec
odliczania wyłączają grzałkę przy następnym cyklu sterowania (maks. około 1 s).
Stan pokazywany w SUPLA jest stanem wyjścia, nie oczekującym żądaniem.

Kasowanie żądań (v5): przy awarii **zatrzaskowej** — przegrzanie, awaria DAC,
błędna konfiguracja, wyjście z trybu normalnego oraz zmiana trybu — żądania
i oczekujące timery są kasowane i grzałkę trzeba włączyć ponownie w SUPLA.
Przy zwłoce **przejściowej** (chwilowo brak świeżego pomiaru) wyjścia są
wyłączane, ale żądanie MANUAL pozostaje i grzanie wraca samo po odzyskaniu
świeżego pomiaru; w AUTO po upływie minimalnego odstępu.

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

Bez zmian względem v4 (`settings.h` identyczny), z jednym zaostrzeniem:
`inverter_ip` musi być adresem **IPv4** — literał IPv6 jest teraz odrzucany
i podnosi alarm błędnej konfiguracji.

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

Kod interpretuje rejestr jako **signed int32**, nie IEEE754 float.
Typ i znak muszą być sprawdzone w dokumentacji falownika.

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

Kanały 0–8 jak w v4: temperatura, H2, H3, alarm temperatury, alarm Modbus,
**zadana** moc SSR, tryb MANUAL, awaria DAC, błędna konfiguracja.

## Build i weryfikacja

Docelowe środowisko: Arduino core ESP32 3.3.3, SuplaDevice 26.8,
modbus-esp8266 4.1.0, DallasTemperature 4.0.6, OneWire 2.3.8.

Z folderu v5 (PowerShell):

```powershell
arduino-cli compile --jobs 4 --fqbn esp32:esp32:esp32:PartitionScheme=huge_app .
```

**Kompilacja v5 nie została jeszcze wykonana.** Elementy wymagające
potwierdzenia przez kompilator: `MODBUSIP_CONNECT_TIMEOUT` przed nagłówkami
Modbus, dostęp do `durationMs` z klasy pochodnej, `IPAddress::type()`,
`xTaskCreatePinnedToCore` oraz zmienne `volatile` w wyrażeniach sterowania.

`control_logic.h` (identyczny z v4) zawiera 20 testów `static_assert`
kompilujących rzeczywiste funkcje regulatora.

Przed podłączeniem grzałek sprawdzić na diodach/mierniku:
- start i restart, brak impulsu załączenia przekaźników (kluczowe dla poprawki P1);
- odłączenie DS18B20 podczas AUTO i MANUAL (odcięcie do 30 s);
- utratę Modbus, martwy falownik (brak zauważalnego przestoju pętli);
- odłączenie SDA/SCL przy zasilanym DAC;
- próby ON z SUPLA podczas AUTO i aktywnego alarmu, w tym opóźnione ON;
- powolne/porzucone połączenie WWW podczas grzania w MANUAL;
- zapis błędnej histerezy, adresu IPv6 i ponowny start;
- odpowiedź regulatora na stałą nadwyżkę i nagły zanik PV.
