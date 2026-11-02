# Pompa ciepła + Sigenergy + SUPLA (ESP32 DevKit V1)

Sterownik jednofazowej pompy ciepła. PZEM-004T mierzy całą moc pompy,
Sigenergy podaje po Modbus TCP moc tej samej fazy. Program dzieli energię
pompy na dwie liczby: **ile poszło z sieci** i **ile z autokonsumpcji**.
Pokazuje je w SUPLA jako dwa osobne liczniki kWh.

Dodatkowo: suwak **Blokada pompy**, przełącznik **Sprzedaż energii (blokada
pompy)** i lampka **Blokada sprzedaży energii**.

> **Status:** kompiluje się, testy źródłowe przechodzą, **ale nie było wgrane
> na ESP32 ani testowane na sprzęcie**. 230 V podłącza elektryk.

## 1. Zasada podziału

Pompa wisi na jednej fazie. Do węzła z pompą moc może dopłynąć tylko dwiema
drogami: z falownika (widzi to przekładnik CT) albo z sieci. Trzeciej drogi
nie ma, więc rachunek jest dokładny:

```
auto = min(P_pompa, max(-P_sieć, 0))
grid = P_pompa - auto
```

| Pompa | Eksport fazy | Z autokonsumpcji | Z sieci |
|---:|---:|---:|---:|
| 1000 W | 400 W | **400 W** | **600 W** |
| 1500 W | 500 W | 500 W | 1000 W |
| 1500 W | 0 W | 0 W | 1500 W |
| 1500 W | 2000 W | 1500 W | 0 W |
| 0 W | cokolwiek | 0 W | 0 W |

Energię liczymy przyrostami: co 2 s PZEM podaje nowy stan licznika kWh, a my
mnożymy przyrost przez chwilowy udział autokonsumpcji. Suma liczników zawsze
równa się licznikowi PZEM.

**Faza, nie suma.** Parametr `sig_grid_phase` mówi, którą moc czytać: `1/2/3`
= faza CT z pompą, `0` = suma całej instalacji. Domyślnie **1**, bo tak jest
fizycznie poprawnie. Suma ma sens tylko przy instalacji jednofazowej — inaczej
falownik oddający moc na innej fazie zawyży autokonsumpcję.

Odczyt fazy wymaga bloku **54 rejestrów** (`sig_block_count = 54`); przy
krótszym bloku konfiguracja jest odrzucana i świeci alarm.

**Brak danych nie jest zgadywany.** Jeśli próbka Sigenergy jest starsza niż
10 s, PZEM nie odpowiada albo przyrost jest nierealny, energia trafia do
licznika **Pompa – energia niesklasyfikowana**. W normalnej pracy stoi on
blisko zera.

## 2. Blokada sprzedaży

Sprzedaż energii to **eksport przy braku produkcji PV** — falownik oddaje
wtedy energię z baterii i pompa nie może jej zjadać. Eksport przy pracującym
PV to normalna autokonsumpcja i **nie** blokuje pompy.

```
blokuj, gdy: PV <= sale_pv_max_w  ORAZ  eksport >= sale_on_w
odblokuj, gdy: eksport <= sale_off_w (albo PV wróci)
```

Automat działa tylko gdy: `sale_auto = 1`, konfiguracja poprawna, falownik
on-grid, status CT = 1 i próbka Sigenergy nie starsza niż 10 s. Inaczej
**zamraża ostatni stan** — nie blokuje i nie odblokowuje po omacku.

### Strefy eksportu (gdy PV ≤ 100 W)

| Eksport | Start blokady | Utrzymanie | Akcja |
|---:|:---:|:---:|---|
| **≥ 1000 W** | tak | – | po **30 s** blokuje pompę |
| **301–999 W** | nie | tak | trzyma obecny stan (histereza) |
| **≤ 300 W** | nie | nie | po **60 s** odblokowuje |

Histereza 300–1000 W jest po to, żeby eksport oscylujący wokół progu nie
klikał przekaźnikiem. Gdy PV wzrośnie powyżej 100 W, blokada schodzi po 60 s
niezależnie od eksportu.

Do decyzji używana jest **suma mocy sieci**, nie wybrana faza — sprzedaż
dotyczy całego przyłącza.

Przełącznik ręczny działa niezależnie i jest pamiętany po restarcie. Lampka
pokazuje stan faktyczny (ręczny **lub** automatyczny).

| Parametr | Domyślnie | Znaczenie |
|---|---:|---|
| `sale_auto` | 1 | 1 = automat działa, 0 = tylko ręczny |
| `sale_on_w` | 1000 W | eksport, od którego blokować |
| `sale_off_w` | 300 W | eksport, poniżej którego odblokować |
| `sale_pv_max_w` | 100 W | PV poniżej = brak produkcji |
| `sale_on_delay_s` | 30 s | jak długo eksport musi trwać |
| `sale_off_delay_s` | 60 s | jak długo niski eksport musi trwać |

### Przykład

Eksport 3000 W, PV 0 W, pompa pracuje. Po 30 s automat blokuje pompę.
Eksport spada do 800 W — pompa **nadal zablokowana** (800 > 300).
Dopiero przy 250 W przez 60 s blokada schodzi.

Inny przebieg: eksport 3000 W, PV 0 W, blokada aktywna. W południe PV rośnie
do 1500 W — po 60 s blokada schodzi, choć eksport nadal 3000 W. To poprawnie:
energia pochodzi z PV, więc pompa może ją zużywać.

## 3. Kanały SUPLA

| Nr | Nazwa | Typ | Znaczenie |
|---:|---|---|---|
| 0 | Pompa - energia z sieci | licznik kWh | pobrana z sieci |
| 1 | Pompa - energia autokonsumpcji | licznik kWh | z PV/baterii przez falownik |
| 2 | Pompa - energia niesklasyfikowana | licznik kWh | diagnostyka, powinno być 0 |
| 3 | Pompa - PZEM | licznik energii | V, A, W, kWh, Hz, cos φ |
| 4 | Blokada pompy (0=brak) | dimmer | 0% = wolna, >0% = blokada |
| 5 | Sprzedaz energii (blokada pompy) | przełącznik | ręczna blokada sprzedaży |
| 6 | Blokada sprzedazy energii | czujnik binarny | lampka stanu faktycznego |
| 7 | Pompa - przekaznik (stan) | czujnik binarny | stan wyjścia przekaźnika |
| 8 | Sigenergy - moc fazy z pompa | pomiar W | + pobór, − eksport |
| 9 | Sigenergy - PV | pomiar W | moc PV |
| 10 | Sigenergy - bateria | pomiar W | + ładowanie, − rozładowanie |
| 11 | Sigenergy - SOC baterii | pomiar % | naładowanie |
| 12 | Alarm PZEM | czujnik binarny | brak/niepoprawny PZEM |
| 13 | Alarm Sigenergy / CT | czujnik binarny | brak danych, CT, off-grid |
| 14 | Blad konfiguracji / startu | czujnik binarny | zły IPv4/progi, tryb config |
| 15 | Awaria sterowania pompa | czujnik binarny | zadanie przekaźnika nie wstało |

- Suwak: każda wartość >0% to ta sama blokada. Nie steruje mocą pompy.
- Reset kanału 0, 1 lub 2 zeruje **wszystkie trzy** (grupa), żeby suma nadal
  odpowiadała energii od resetu. Reset kanału 3 zeruje tylko licznik PZEM.
- Włącz historię dla 0–2, żeby widzieć zużycie dzienne.

## 4. Sprzęt

| Element | Uwagi |
|---|---|
| ESP32 DevKit V1 30-pin | GPIO16/17 wolne (nie WROVER z PSRAM) |
| PZEM-004T v3.0 | TTL, przekładnik 100 A, adres 0xF8 |
| Moduł przekaźnika | transoptor, wejście pewnie działa przy 3,3 V |
| Stycznik | cewka 230 V, styki dobrane do prądu pompy |
| Konwerter 5→3,3 V | wymagany na linii PZEM TX → ESP32 RX |
| Bezpieczniki, RCD | instalacja przez elektryka |

| Sygnał | GPIO |
|---|---:|
| PZEM TX → ESP32 RX | 16 |
| ESP32 TX → PZEM RX | 17 |
| Przekaźnik / stycznik | 26 |
| LED konfiguracji | 2 |
| BOOT / wejście w config | 0 |

```
Tauron L ──┬──────────── stycznik ──── pompa L
           │                  │
           └── PZEM L         └── przekładnik PZEM (tylko faza pompy)

Tauron N ──┬──────────── stycznik ──── pompa N
           └── PZEM N

GPIO26 ──► moduł przekaźnika ──► cewka stycznika
PZEM TX ──► konwerter 5→3,3 V ──► GPIO16
PZEM RX ◄── GPIO17
PZEM zasilanie: izolowane 5 V, wspólna masa TTL
```

- PZEM zasilaj **przed stycznikiem**, inaczej traci zasilanie przy każdej
  blokadzie.
- Przekładnik tylko na przewodzie fazowym pompy, zgodnie ze strzałką.
- Zanik napięcia PZEM jest wykrywany (napięcie <80 V, częstotliwość poza
  40–70 Hz), więc brak zasilania nie jest liczony jako 0 W.
- **Izolacja:** strona TTL PZEM nie może dotykać 230 V. Nie łącz GND TTL
  z neutralnym sieci.

### Fail-safe przekaźnika

```cpp
// config.h
constexpr bool PUMP_RELAY_ACTIVE_HIGH = true;
constexpr bool PUMP_RELAY_NC = false;   // false = styk NO
```

| `PUMP_RELAY_NC` | Brak zasilania ESP32 |
|---|---|
| `false` (NO) | pompa **wyłączona** |
| `true` (NC) | pompa **może pracować** |

**Blokada i odblokowanie są natychmiastowe.** ESP32 nie opóźnia ponownego
startu — ochronę sprężarki przed zbyt częstym rozruchem realizuje sterownik
pompy ciepła, więc duplikowanie jej w firmware tylko przeszkadza (np. przy
sprzedaży energii pompa stałaby 3 minuty bez powodu). Po restarcie ESP32 stan
przekaźnika wynika wprost z aktualnych blokad.

Przy NO każdy reset ESP32 to krótkie odcięcie zasilania pompy. Jeśli pompa ma
wejście „external block/enable”, bezpieczniej sterować nim niż odcinać
zasilanie kompresora. Dodaj też zewnętrzny rezystor na wejściu modułu, żeby
stan przed startem `setup()` był zdefiniowany.

## 5. Sigenergy — Modbus TCP

Włącz Modbus TCP w falowniku (port 502, plant unit 247), ustaw stały IP.

| Rejestr | Znaczenie |
|---:|---|
| 30004 | status CT: 0 brak, 1 OK |
| 30005–30006 | moc sieci, suma: + pobór, − eksport |
| 30009 | 0 on-grid, 1/2 off-grid |
| 30014 | SOC ×0,1 % |
| 30035–30036 | moc PV |
| 30037–30038 | moc baterii: + ładowanie, − rozładowanie |
| 30052–30053 | moc fazy A |
| 30054–30055 | moc fazy B |
| 30056–30057 | moc fazy C |

Jeden blok od **30004**, domyślnie 54 rejestry (do 30057 włącznie).

### Parametry WWW

| Klucz | Domyślnie | Znaczenie |
|---|---:|---|
| `inverter_ip` | – | **wymagany** IPv4 falownika |
| `mb_unit` | 247 | unit ID |
| `mb_port` | 502 | port |
| `sig_block_count` | 54 | długość bloku (35–120) |
| `sig_word_order` | 1 | 1 = starsze słowo pierwsze |
| `sig_scale` | 1000 | W = surowa × scale / 1000 |
| `sig_require_status` | 1 | wymagaj statusu CT = 1 |
| `sig_grid_phase` | 1 | faza z pompą; 0 = suma (tylko 1-fazowa) |
| `sale_auto` | 1 | automat sprzedaży |
| `sale_on_w` | 1000 | eksport, od którego blokować [W] |
| `sale_off_w` | 300 | eksport, poniżej którego odblokować [W] |
| `sale_pv_max_w` | 100 | PV poniżej = brak produkcji [W] |
| `sale_on_delay_s` | 30 | opóźnienie wejścia [s] |
| `sale_off_delay_s` | 60 | opóźnienie wyjścia [s] |
| `pump_min_w` | 10 | poniżej tej mocy energia idzie do sieci [W] |

Zły IPv4, odwrócone progi albo faza bez bloku 54 → `CONFIG ERROR` w logu
i alarm. Falownik nie jest wtedy odpytywany, ale PZEM i blokady działają.

## 6. Uruchomienie

### Bez 230 V

1. Zasil ESP32 z USB, PZEM z 5 V.
2. Sprawdź log 115200: `CONFIG valid=1 ip=... faza=1 blok=30004/54`,
   brak `MODBUS error`.
3. `Alarm PZEM` będzie zapalony — to oczekiwane bez 230 V.
4. Suwak 0% → przekaźnik załączony. 100% → wyłączony. Powrót na 0% →
   załączenie od razu, bez czekania.

### Konfiguracja

1. Pierwszy start = tryb config: LED miga 100/100 ms, AP bez hasła o nazwie
   z nazwy urządzenia + fragment MAC (dokładna nazwa w logu).
2. `http://192.168.4.1` → WiFi, dane SUPLA, **IPv4 falownika**.
3. SAVE & RESTART.
4. W SUPLA Cloud dodaj urządzenie i ustaw funkcje kanałów wg tabeli.

Wejście w tryb config: przytrzymaj BOOT 5 s lub 5 szybkich kliknięć.
Wyjście: SAVE & RESTART albo EN/RST. Timeoutu nie ma.

### Z 230 V (elektryk)

1. PZEM na stałym zasilaniu obwodu pompy, przekładnik na jej fazie.
2. Porównaj V/A/W z PZEM z mocą z aplikacji pompy.
3. Sprawdź podział: przy eksporcie 400 W i pompie 1000 W autokonsumpcja ma
   rosnąć 2,5× wolniej niż „z sieci”.
4. Sprawdź automat sprzedaży: PV≈0 + eksport > 1000 W → blokada po 30 s.
   Przy pracującym PV eksport **nie może** blokować pompy.
5. Zablokuj suwakiem — przekaźnik natychmiast. Odblokuj — powrót też
   natychmiast, bez opóźnienia w ESP32.
6. Porównaj licznik PZEM z sumą trzech liczników podziału.

## 7. Rozwiązywanie problemów

| Objaw | Co zrobić |
|---|---|
| `MODBUS error` | sprawdź IP, port 502, unit 247; skróć blok do 35 |
| `CONFIG ERROR: faza ... wymaga 54` | ustaw `sig_block_count = 54` |
| Moc fazy ma odwrotny znak | przełącz `sig_word_order` |
| Moc ×1000 | zmień `sig_scale` |
| `Alarm PZEM` | zasilanie 5 V, RX/TX, konwerter poziomów |
| PZEM 0 W przy pracującej pompie | przekładnik nie na tej żyle |
| Rośnie „niesklasyfikowana” | brak świeżych danych: WiFi, Modbus, długie blokady TLS |
| Liczniki nie rosną | `CONFIG valid=0` — brak IPv4 |
| Suwak nie pamięta stanu | odczekaj ~1 s po zmianie przed odcięciem zasilania |
| Pompa nie startuje po odblokowaniu | sprawdź własne opóźnienie w sterowniku pompy (ESP32 nie opóźnia) |

## 8. Ograniczenia i bezpieczeństwo

- Podział jest dokładny tylko wtedy, gdy moc sieci dotyczy **tej samej fazy**
  co pompa. Przy `sig_grid_phase = 0` na instalacji trójfazowej autokonsumpcja
  będzie zawyżona.
- „Autokonsumpcja” obejmuje też energię z baterii, jeśli falownik ją oddaje
  w tej chwili.
- Brak świeżych danych → energia do „niesklasyfikowanej”, nie jest zgadywana.
- Energia zużyta przy wyłączonym ESP32 nie jest przypisywana.
- PZEM v3.0 trzyma licznik w pamięci nieulotnej, ale znane są klony zerujące
  go po ~60 kWh. Ujemny przyrost jest wykrywany, ale sumy się wtedy rozjadą.
- **PZEM i przekaźnik nie są zabezpieczeniem.** Wymagany bezpiecznik, RCD,
  wyłącznik serwisowy, stycznik dobrany do pompy, zabezpieczenie termiczne.
- Strona WWW w LAN nie ma uwierzytelnienia. Nie przekierowuj portu do Internetu.
- Przekaźnikiem steruje osobne zadanie FreeRTOS, ale to nie jest układ
  bezpieczeństwa z certyfikatem. Przy zawieszeniu ESP32 potrzebne jest
  sprzętowe odcięcie lub termostat pompy.

## 9. Kompilacja

```bash
arduino-cli lib install "SuplaDevice@26.8"
arduino-cli lib install "modbus-esp8266@4.1.0"
arduino-cli lib install "PZEM004Tv30@1.2.1"

arduino-cli compile --jobs 4 \
  --fqbn esp32:esp32:esp32:PartitionScheme=huge_app .

arduino-cli upload -p /dev/cu.usbserial-XXXX \
  --fqbn esp32:esp32:esp32:PartitionScheme=huge_app,UploadSpeed=115200 .
```

Na klonach z CP2102 działa 115200, a 921600 gubi kontakt. Jeśli autoreset nie
działa, przytrzymaj BOOT przy wgrywaniu.

**Wynik referencyjny:** flash **1 389 015 B (44 %)**, RAM **50 740 B (15 %)**.
Testy źródłowe: **24/24 OK**, asercje `static_assert` w `source_mix.h`.

## 10. Pliki

| Plik | Rola |
|---|---|
| `pompa-ciepla-sigenergy.ino` | kanały SUPLA, Modbus, podział, pętla |
| `source_mix.h` | czysta logika podziału + asercje (bez Arduino) |
| `pump_control.h` | zadanie przekaźnika, fail-safe, czasy |
| `pump_meter.h` | PZEM-004T → licznik energii SUPLA |
| `settings.h` | parametry WWW + walidacja |
| `config.h` | piny i stałe sprzętowe |
| `verify_source.py` | testy źródeł i modelu podziału (24) |
| `STATUS.md` | co sprawdzone, co nie, następne kroki |

Pętla `loop()`: SUPLA → zapytanie Modbus → odbiór → przeliczenie PZEM →
blokady → kanały SUPLA. Przekaźnik jest poza pętlą: osobne zadanie co 50 ms
czyta flagi przez sekcję krytyczną i przestawia GPIO zgodnie z nimi.
