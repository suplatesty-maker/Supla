# pompa-ciepla-sigenergy — STATUS

## 2026-10-26 — przepisane od nowa na jedną logikę

**Nie wgrano na ESP32. Brak testów sprzętowych.**

### Decyzje potwierdzone przez użytkownika

- Przekładnik Sigenergy **nie widzi pompy** (pompa jest między CT a licznikiem
  Tauron). Dlatego `auto = min(P_pompa, max(-P_sieć, 0))`.
- Przyłącze **trójfazowe**, pompa na **jednej fazie**. Podział liczony z mocy
  **tej fazy** (`sig_grid_phase = 1` domyślnie), nie z sumy.
- Blokada sprzedaży: **PV ≈ 0 ORAZ eksport ≥ próg**. Sam eksport nie wystarcza,
  bo przy dużym PV blokowałby pompę przez pół dnia.

### Co zawiera

- ESP32 DevKit V1 30-pin, SuplaDevice 26.8, LittleFS, WWW w LAN.
- PZEM-004T v3.0 na Serial2 (GPIO16/17), adres 0xF8, kanał licznika energii
  SUPLA (V, A, W, kWh, Hz, cos φ). Kontrola obecności napięcia (80 V, 40–70 Hz).
- Sigenergy Modbus TCP, unit 247, jeden blok od 30004 (domyślnie 54 rejestry):
  moc fazy CT z pompą, suma mocy sieci, PV, bateria, SOC, status CT, on-grid.
- Trzy liczniki: „z sieci”, „autokonsumpcja”, „niesklasyfikowana” (diagnostyka).
  Reset któregokolwiek zeruje całą grupę.
- Suwak „Blokada pompy” (0% = wolna, >0% = blokada), pamiętany po restarcie.
- Przełącznik „Sprzedaż energii” (ręczny, pamiętany) + lampka stanu.
  Automat: brak PV + eksport powyżej progu, histereza i opóźnienia.
- Osobne zadanie FreeRTOS `pump-relay` (rdzeń 0, 50 ms). Blokada i
  odblokowanie **natychmiastowe** — ochronę sprężarki ma sterownik pompy
  ciepła, więc ESP32 jej nie duplikuje. Pętla SUPLA nie może zostawić pompy
  włączonej.
- Fail-safe: NO (`false`, ESP32 bez zasilania = pompa off) lub NC w `config.h`.
- Alarmy: PZEM, Sigenergy/CT, konfiguracja/start (także tryb config), zadanie
  przekaźnika. Wszystkie bez timeoutu SUPLA.

### Co zostało usunięte przy przepisaniu

- Wariant B (`pump_ct_includes = 1`) — nie dotyczy tej instalacji i tylko
  komplikował kod. Została jedna formuła.
- Parametr `pump_ct_includes` z WWW.
- **Opóźnienie ponownego startu pompy (`min_switch_s`, 180 s)** — pompa ma
  własną ochronę sprężarki w sterowniku, więc ESP32 jej nie duplikuje.
  Blokada i odblokowanie są teraz natychmiastowe w obie strony.
- Zdublowane opisy w README.

### Walidacja wykonana

- **Kompilacja:** OK, Arduino CLI 1.3.1, core `esp32:esp32` 3.3.3,
  SuplaDevice 26.8, modbus-esp8266 4.1.0, PZEM004Tv30 1.2.1,
  FQBN `esp32:esp32:esp32:PartitionScheme=huge_app`.
  Flash **1 388 763 B (44 % z 3 145 728)**, RAM **50 684 B (15 % z 327 680)**.
- **`source_mix.h`:** 35 asercji `static_assert`, w tym przykład użytkownika
  (pompa 1000 W, eksport 400 W → 600/400) i przypadki graniczne.
- **`verify_source.py`:** **24/24 OK** — model podziału w Pythonie, kontrakty
  źródeł, kolejność kanałów, natychmiastowa blokada w obie strony, grupowy
  reset, warunek sprzedaży, wybór fazy.
- **API SUPLA:** sprawdzone w `lib/supla-device`.
- **Mapa Sigenergy:** potwierdzona w protokole v1.7/v2.x i w definicjach
  integracji `TypQxQ/Sigenergy-Local-Modbus` (30005, 30052/54/56, 30035,
  30037, 30014, 30004, 30009).

### Czego NIE sprawdzono

- Brak wgrania i uruchomienia na ESP32.
- Brak testu PZEM-004T (napięcie, prąd, energia, zanik AC).
- Brak testu Modbus TCP z rzeczywistym Sigenergy: blok 54 rejestry, unit 247,
  znak i skala mocy, status CT, adresacja 30052/30054/30056.
- Brak testu przekaźnika/stycznika, fail-safe, czasów przełączeń.
- Brak testu SUPLA Cloud (rejestracja, funkcje kanałów, harmonogram dimmera,
  zapis liczników, grupowy reset).
- Brak testu przy blokadzie TLS/WWW i przy zaniku WiFi.
- Brak testu z prawdziwą pompą ciepła.

### Znane ryzyka

1. **Adresacja rejestrów** — użyto 30004–30057 bez offsetu 30001. Pierwsza
   rzecz do sprawdzenia, jeśli w logu pojawi się `EX_ILLEGAL_ADDRESS` albo
   liczby nie będą miały sensu.
2. **Blok 54 rejestrów** — jeśli falownik odrzuci, zejdź do 35 i ustaw
   `sig_grid_phase = 0` (odczyt sumy, mniej dokładny przy 3 fazach).
3. **Czy rejestr 30052 to naprawdę faza z pompą** — sprawdzić, porównując
   z aplikacją Sigenergy przy znanym obciążeniu na tej fazie.
4. **PZEM TTL 5 V** — wymagany konwerter/dzielnik na TX PZEM → RX ESP32.
5. **PZEM zasilanie** — musi być stałe (przed stycznikiem).
6. **PZEM ~60 kWh** — znane klony zerują licznik; ujemny przyrost jest
   wykrywany, ale sumy PZEM i podziału się wtedy rozjadą.
7. **Reset ESP32 przy przekaźniku NO** — krótkie odcięcie zasilania pompy.
   Preferuj wejście „external block” w pompie.
8. **Bezpieczeństwo 230 V** — obowiązkowy elektryk, bezpieczniki, RCD,
   stycznik, zabezpieczenie termiczne.

### Następne kroki

1. Wgrać bez 230 V: log, tryb config, WWW, suwak, przekaźnik.
2. PZEM na znanym obciążeniu: porównać V/A/W/kWh.
3. Modbus do Sigenergy: sprawdzić znak i skalę mocy fazy oraz status CT.
4. Test podziału z grzałką o znanej mocy przy stabilnym eksporcie.
5. Test sprzedaży: PV≈0 + eksport > próg blokuje, PV pracujące nie blokuje.
6. Sprawdzić blokadę i odblokowanie — oba mają działać natychmiast.
7. Dopiero potem pompa przez stycznik, z elektrykiem.
8. Po testach przenieść do `projects/ready/`.
