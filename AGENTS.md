# AGENTS.md — supla-esp32-base

Baza wiedzy i gotowych rozwiązań: **ESP32 + SUPLA** (własny kod na SuplaDevice
oraz GUI Generic). Służy jako źródło sprawdzonych wzorców przy kolejnych projektach.

## Zasady

1. **`docs/` = źródło prawdy.** Zanim napiszesz kod SuplaDevice, sprawdź faktyczne
   API w `docs/analiza-supla-device.md` i w źródłach `lib/supla-device`.
   Nigdy nie zgaduj nazw metod ani akcji.
2. **`projects/ready/`** — tylko projekty sprawdzone fizycznie na sprzęcie.
3. **`projects/wip/`** — szkice, niesprawdzone. Każdy ma STATUS.md.
4. **Wersjonowanie:** poprawka = nowy plik `*_v2.ino`, `*_v3.ino`. Stare wersje
   zostają jako historia, każda ma w komentarzu listę zmian.
5. **`lib/` nie jest kompilowane** — to klony repo do czytania. Do kompilacji
   używaj SuplaDevice z Library Manager.
6. **Nie zmieniaj kolejności kanałów Supla** — numer kanału w kodzie odpowiada
   numerowi w cloud.supla.org.

## Kompilacja (arduino-cli, sprawdzone na macOS Apple Silicon)

```bash
arduino-cli compile --jobs 4 --fqbn esp32:esp32:esp32:PartitionScheme=huge_app .
arduino-cli board list
arduino-cli upload -p /dev/cu.usbserial-XXXX \
  --fqbn esp32:esp32:esp32:PartitionScheme=huge_app,UploadSpeed=115200 .
```

- rdzeń: `esp32:esp32@3.3.3`, biblioteka `SuplaDevice`, Rosetta 2 wymagana
  (dołączany `ctags` jest x86_64).
- Na klonach z CP2102 prędkość 921600 gubi kontakt z chipem — używaj 115200.

## Wgrywanie z innego komputera

Build robi Mac mini, fizyczne wgranie — laptop z kablem USB:

```bash
# Mac mini
arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=huge_app .
# plik .bin: build/esp32.esp32.esp32/<nazwa>.ino.bin

# Lenovo (Windows)
pip install esptool
esptool --chip esp32 --port COM3 --baud 115200 write_flash 0x10000 firmware.bin
```

Firmware przekazuj przez GitHub Releases albo Google Drive — `*.bin` jest
w `.gitignore`, historia Gita nie jest do tego odpowiednia.

## Pułapki

- Monitor portu szeregowego wymaga fizycznego dostępu do płytki.
- Po zmianie `GUID` / `AUTHKEY` wygeneruj je ponownie w cloud.supla.org.
- `docs/notes-hardware.md` zbiera lekcje sprzętowe (MOSFET, NPN, 24 V, chłodzenie).
