# supla-esp32-base

Baza wiedzy i gotowych rozwiązań: **ESP32 + SUPLA** (własny kod na SuplaDevice
oraz GUI Generic). Repo służy jako źródło, z którego Droid/agent korzysta przy
kolejnych projektach — gotowe sprawdzone wzorce + projekty w trakcie pracy.

## Struktura

```
supla-esp32-base/
├── README.md                 # ten plik - zasady bazy
├── docs/                     # analizy i wiedza (ŹRÓDŁO PRAWDY o API)
│   ├── analiza-supla-device.md    # zweryfikowane fakty o API supla-device
│   ├── analiza-gui-generic.md     # analiza GUI Generic
│   └── notes-hardware.md          # lekcje sprzętowe (MOSFET-y, NPN, zasilanie)
├── lib/                      # klony repo (TYLKO do czytania, nie do kompilacji)
│   ├── supla-device/         # github.com/SUPLA/supla-device
│   ├── gui-generic/          # github.com/krycha88/GUI-Generic
│   └── home_io_control/      # github.com/laberning/home_io_control (protokół io-homecontrol)
└── projects/
    ├── ready/                # GOTOWE, sprawdzone na sprzęcie projekty
    │   └── dimmer-esp32c3-levelshifter/
    │       ├── esp32c3_dimmer_supla.ino     (v1 - ma błąd ITERATE_DIM_ON_PRESS!)
    │       └── esp32c3_dimmer_supla_v2.ino  (v1 z poprawioną akcją przycisku)
    └── wip/                  # projekty W TRAKCIE PRACY (niesprawdzone)
        └── dimmer-esp32c3-npn/
            ├── esp32c3_dimmer_supla_npn.ino     (v1 - błędy, NIE kompilować)
            ├── esp32c3_dimmer_supla_npn_v2.ino  (v2 - zła sygnatura override)
            └── esp32c3_dimmer_supla_npn_v3.ino  (v3 - POPRAWNA, do wgrania)
```

## Zasady bazy

1. **`docs/` = źródło prawdy.** Zanim napiszemy kod SuplaDevice, sprawdzamy
   faktyczne API w `docs/analiza-supla-device.md` i w źródłach `lib/supla-device`.
   Nigdy nie zgadujemy nazw metod/akcji.

2. **`projects/ready/`** — tylko projekty sprawdzone fizycznie na sprzęcie
   (działają, zmierzone temperatury, potwierdzone na forum/użytkowniku).
   Każdy projekt ma STATUS.md z wynikami testów.

3. **`projects/wip/`** — projekty użytkownika dodawane "na bieżąco", traktowane
   jako szkice. Każdy nowy projekt użytkownika ląduje tutaj z dopiskiem skąd
   pochodzi i co jeszcze wymaga zrobienia. Po sprawdzeniu na sprzęcie przenosimy
   do `ready/`.

4. **Wersjonowanie plików:** jeśli poprawiamy kod, tworzymy nowy plik z wyższym
   numerem wersji (v2, v3...), stare zostają jako historia. Każda wersja ma w
   komentarzu na górze listę poprawek względem poprzedniej.

5. **`lib/` nie jest kompilowane** — to referencja. Do kompilacji projektów
   używamy biblioteki SuplaDevice z Arduino Library Manager.

6. **Zasady sprzętowe** (MOSFET-y, stopnie NPN, zasilanie 24 V, chłodzenie) —
   zebrane w `docs/notes-hardware.md`.

## Aktualne projekty

| Projekt | Status | Uwagi |
|---|---|---|
| dimmer-esp32c3-levelshifter | wip → gotowy do testów | dimmer 24 V, level-shifter 3.3→5 V, IRLZ44N/IRLB8721 |
| dimmer-esp32c3-npn | wip | stopień NPN (inwersja PWM), v3 poprawna |
| hydrofor-esp32-supla | wip, v2 skompilowana, do testu sprzętowego | v1: zgłoszone zawieszanie; v2: nadzór obu zadań, przekazywanie poleceń i diagnostyka. Pompa + zbiorniki 200 l, słaba studnia, zawór na wodę miejską |
| shelly-2pmg3-rolety-supla | wip, v2 skompilowany, bez testów sprzętowych | własny firmware SuplaDevice: roleta, ręczne AP, czasy z SUPLA, mono/bi z WWW; aktualizacja samej aplikacji; GG pozostawione jako historia z opisanymi błędami |
| velux-io-supla | wip, firmware kompiluje się (72 % flash, bez testów sprzętowych) | ESP32 DevKit V1 30-pin + SX1276 868 MHz jako kontroler io-homecontrol dla rolet i okien VELUX: **1W** (nauka gestem Gear na pilocie KLI, pozycja szacowana) oraz **2W** (ekstrakcja klucza z pilota KLR 200, skan instalacji roll-call, uwierzytelnione komendy i realny status/pozycja); konfiguracja z panelu WWW; protokół z `lib/home_io_control` |
| pompa-ciepla-sigenergy | wip, firmware kompiluje się (44 % flash, bez testów sprzętowych) | ESP32 DevKit V1 + PZEM-004T v3.0 + Sigenergy Modbus TCP: dwa liczniki energii pompy (z sieci / autokonsumpcja) liczone z mocy fazy CT, na której wisi pompa; suwak **Blokada pompy**, przełącznik **Sprzedaż energii** i świetlna informacja o blokadzie (automat: brak PV + eksport powyżej progu); przekaźnik w osobnym zadaniu FreeRTOS, blokada i odblokowanie natychmiastowe (ochronę sprężarki ma sterownik pompy) |

## Jak dodać nowy projekt (dla użytkownika)

Wrzuć pliki (.ino, szkice, notatki) do nowego folderu w `projects/wip/<nazwa>/`
i napisz agentowi, co to jest — agent zanalyzuje kod, zweryfikuje z API
supla-device i dopisze STATUS.md.

## Środowisko kompilacji

- Arduino IDE + Boards Manager: "esp32" by Espressif Systems (ESP32C3 Dev Module)
- Biblioteki z Library Manager: **SuplaDevice** (najnowsza), ew. zależności wg projektu
- GUID/AUTHKEY: https://www.supla.org/arduino/get-authkey

### macOS (Apple Silicon) - sprawdzone przy hydrofor v1 (2026-10-10)

- `arduino-cli` 1.3.1 w `~/.local/bin/arduino-cli` (wydanie `macOS_ARM64`
  z GitHub Releases, bez Homebrew; `~/.local/bin` jest już w PATH z `~/.zshrc`).
- `arduino-cli core install esp32:esp32@3.3.3`
- `arduino-cli lib install SuplaDevice@26.8.0`
- Rosetta 2 jest wymagana (dołączany przez Arduino `ctags` jest x86_64):
  `softwareupdate --install-rosetta --agree-to-license` (działa bez sudo).
- Kompilacja:
  `arduino-cli compile --jobs 4 --fqbn esp32:esp32:esp32:PartitionScheme=huge_app .`
- Port płytki: `arduino-cli board list`; wgranie:
  `arduino-cli upload -p /dev/cu.usbserial-XXXX --fqbn esp32:esp32:esp32:PartitionScheme=huge_app,UploadSpeed=115200 .`
  (na klonach z CP2102 921600 gubi kontakt z chipem — patrz `docs/notes-hardware.md`).
