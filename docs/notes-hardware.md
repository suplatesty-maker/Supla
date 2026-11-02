# Notatki sprzętowe — dimmer PWM 24 V LED pod ESP (z rozmów projektowych)

## Sterowanie MOSFET z GPIO 3.3 V — kluczowe fakty

| MOSFET | Pełne otwarcie przy 3.3 V? | Uwagi |
|---|---|---|
| IRLB8721 | ❌ słabe (dane od 4.5 V) | popularny, ale wymaga podniesienia Vgs |
| IRLZ44N  | ⚠️ częściowe (dane od 5 V) | przy 3.3 V Rds(on) rośnie ~2-3× |
| IRLB8743 | ✅ dobre przy 3.3 V | najlepszy "bezpośredni" wybór TO-220 |
| FDN337N  | ✅ sprawdzony przy 3.3 V | SOT-23, do ~3-5 A (limit termiczny) |

## Dwie drogi podniesienia napięcia bramki

1. **Level shifter 3.3→5 V** (moduł BSS138 lub 74HCT125) — prosty, sygnał
   NIEodwrócony. Uwaga: moduły dwustronne "I2C" są wolne; do PWM użyć
   szybkiego jednostromnnego.
2. **Stopień NPN** (BC547 + 2×1k + 220-330R + 10k pull-down) — tani, ale
   **odwraca sygnał** → w kodzie potrzebna klasa `InvertedDimmerLeds`
   (patrz analiza-supla-device.md).

Napięcie podciągania kolektora: 5 V wystarcza dla IRLZ44N (28-35 mΩ),
12 V (step-down z 24 V) daje pełne parametry IRLB8721 (~8.7-16.5 mΩ).
Nigdy nie podciągać bramki do 24 V (limit Vgs = ±20 V).

## Straty cieplne (orientacyjnie, Rds @ Vgs≈5 V)

| Prąd | IRLZ44N (~30 mΩ) | IRLB8721 (~16 mΩ) |
|---|---|---|
| 5 A  | ~0.7-0.9 W | ~0.4 W |
| 10 A | ~2.8-3.5 W | ~1.3-1.7 W |
| 15 A | ~6.3-7.9 W | ~3.0-3.7 W |

- TO-220 bez radiatora: do ~1 W strat (Rth ≈ 60-70°C/W).
- Mały radiator 20×20 mm: do ~10 A (IRLB8721 @5 V) — Tj ~66-74°C, margines duży.
- Pasta termoprzewodząca obowiązkowa; łapka TO-220 = Drain → izolowana
  podkładka, jeśli radiator styka się z obudową.

## Schemat połączeń (low-side, wariant NPN)

```
24V (+) ──────────────────────────────► "+" taśmy LED
24V (–) ──┬─► GND ESP32-C3
          ├─► Emiter NPN
          └─► Source MOSFET

"–" taśmy ──► Drain MOSFET

GPIO4 ──► 1k ──► Baza NPN
Kolektor NPN ──┬─► 1k ──► +5V/+12V
               └─► 220-330R ──► Gate MOSFET
Gate ──► 10k ──► GND (pull-down)

GPIO3 ──► przycisk ──► GND
```

## ESP32 DevKit V1 (klon, mostek CP2102): prędkość wgrywania

- Przy domyślnych 921600 kbps wgrywanie wysypuje się na tej płytce: po zmianie
  baud esptool zgłasza „The chip stopped responding” / `StopIteration` przy
  `flash_id`. Chip jest rozpoznawany poprawnie (ESP32-D0WDQ6 rev v1.1), więc to
  nie jest problem trybu downloadu — winna jest prędkość (i/lub kabel USB).
- Działa 115200:
  `arduino-cli compile -u -p /dev/cu.usbserial-XXXX \
  --fqbn esp32:esp32:esp32:PartitionScheme=huge_app,UploadSpeed=115200 .`
  (1,37 MB, około 81 s).
- Auto-reset przez DTR/RTS działa — nie trzeba trzymać BOOT. BOOT/EN przydaje
  się dopiero, gdy esptool nie potrafi się połączyć („Timed out waiting for
  packet header”).
- macOS: port pojawia się jako `/dev/cu.usbserial-XXXX` bez instalowania
  dodatkowych sterowników.

## Zasady montażu

- **Wspólna masa** zasilacza 24 V + ESP + Source MOSFET (jeden punkt lutowniczy) — bez tego nic nie działa.
- Przewody do prądu taśmy: ≥1 mm² przy 5 A, 1.5-2.5 mm² przy 10 A+.
- Krótki przewód "–" taśmy → Drain.
- ESP32-C3 Super Mini: unikać GPIO2/8/9 jako wejść (strapping); GPIO4 (PWM),
  GPIO3 (przycisk), GPIO8 (LED, aktywna LOW) — sprawdzone bezpieczne piny.
