# Hydrofor (ESP32 DevKit V1 + SUPLA): status

Źródło: szkic użytkownika `hydrofor_esp32.ino` (kopia w `v0-oryginal/`)
i rozmowa projektowa 2026-10-09.

Sprzęt: czujnik GS07381-05233 (0–150 psi, 0,5–4,5 V, 5 V), Aquasystem
150 l + 50 l, pompa głębinowa IBO 250 W 1000 l/h (silnik w oleju), studnia
około 37 l na jedno ładowanie z regeneracją około 1 h, siłownik zaworu 3D
230 V (3 przewody), przełączanie na wodę miejską.

| Wersja | Stan |
|---|---|
| v0-oryginal | szkic użytkownika, tylko historia. Błędy: kalibracja dla 0–5 V (odczyt około 12% za niski, pompowanie do około 4,5 bar), stały czas letni, blokada bez WiFi/NTP, brak rozróżnienia „pompa doszła do swojego maksimum” od suchobiegu, przepadające nocne terminy, brak awarii czujnika, limitu czasu i blokady |
| v1 | wgrana 2026-10-10, skonfigurowana w SUPLA; użytkownik potwierdza odczyt ciśnienia. **2026-10-22 zgłoszono zawieszanie po około minucie / po ręcznym włączeniu wody miejskiej.** Zachowana bez zmian jako historia. |
| **v2** | poprawki nadzoru obu zadań, przekazywanie serwisu w callbacku, zegar przez skrzynkę danych, diagnostyka resetów. Kompilacja 2026-10-22 EXIT=0, bez ostrzeżeń, flash 1 372 579 B, RAM 50 668 B; testy hostowe ASan/UBSan i `static_assert` PASS. **Niewgrana, brak potwierdzenia na sprzęcie; pierwotna przyczyna zawieszenia nieustalona bez logu.** |

Do zrobienia przed przeniesieniem do `ready/`:
- test v2 bez 230 V: przełączanie kanału 1, zapis i odtworzenie stanu,
  działanie przez co najmniej 10 min w serwisie, WWW i utrata WiFi,
- zebranie logu UART 115200, jeśli objaw powróci (procedura w README v2),
- testy stanowiskowe z README v1 (diody, potencjometr zamiast czujnika),
- pomiar ciśnienia powietrza w zbiornikach i maksimum pompy, wpisanie nastaw,
- kalibracja zera czujnika (napięcie na stronie WWW urządzenia),
- sprawdzenie kierunków siłownika zaworu i polaryzacji modułów przekaźników,
- kilka nocy pracy: „ostatnia porcja” na stronie WWW, brak fałszywych blokad.
