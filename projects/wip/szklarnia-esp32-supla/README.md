# Sterownik szklarni ESP32 + SUPLA

## Założenia projektu

- 8 siłowników 230 V AC, każdy ma osobne wyjście `OTWÓRZ` i `ZAMKNIJ`.
- 16 wyjść przekaźnikowych, po dwa na każdy siłownik.
- 3 punkty pomiarowe, docelowo DHT22/AM2302 dla temperatury i wilgotności.
- ESP32 wykonuje automat lokalnie. SUPLA służy do podglądu, nastaw i sterowania ręcznego.

## Zalecany model regulacji

Nie sterować oknami pojedynczym progiem. Używać histerezy:

- otwieranie, gdy temperatura uśredniona osiągnie `28.0 °C`,
- zatrzymanie po zejściu do `26.0 °C`,
- zamykanie dopiero poniżej `25.0 °C`, jeżeli nie ma alarmu,
- filtracja pomiaru: średnia z trzech czujników,
- decyzja dopiero po 120 s utrzymywania warunku,
- minimalna przerwa między zmianami kierunku: 5 s,
- maksymalnie 2 poruszające się siłowniki jednocześnie,
- maksymalny czas ruchu per siłownik: 90 s, do ustawienia po pomiarze.

To są wartości startowe dla szklarni, a nie wartości uniwersalne. Po obserwacji należy je dostroić do roślin, nasłonecznienia i bezwładności konstrukcji.

## Priorytety bezpieczeństwa

1. Alarm deszczu lub silnego wiatru zawsze ma pierwszeństwo przed temperaturą.
2. Przy błędnym lub nieaktualnym pomiarze temperatury nie otwierać automatycznie.
3. Przy restarcie ESP32 wszystkie przekaźniki muszą startować wyłączone.
4. Nigdy nie załączać `OTWÓRZ` i `ZAMKNIJ` tego samego siłownika jednocześnie.
5. Przed zmianą kierunku wymusić stan OFF i odczekać czas martwy.
6. Programowe ograniczenie czasu nie zastępuje krańcówek. Siłowniki muszą mieć własne wyłączniki krańcowe albo zewnętrzne zabezpieczenie.

## Co pokazywać w SUPLA

Najpraktyczniejszy układ kanałów:

- 8 kanałów roletowych: `Okno 1` ... `Okno 8`,
- 3 kanały temperatury/wilgotności,
- kanał lub przełącznik `Automatyka`,
- kanał `Alarm / blokada otwierania`,
- kanał diagnostyczny: `Błąd czujnika` i `Ograniczenie ruchu`.

Tryb automatyczny powinien być lokalny. Wyłączenie automatyki w SUPLA powinno przełączyć urządzenie w tryb ręczny, ale nie powinno omijać blokad kierunku, czasu ruchu ani alarmu.

## Ważna uwaga o PCF8575

Przed podłączeniem 16 wejść modułów przekaźnikowych zmierzyć prąd wejściowy `IN` przy 5 V. Nie zakładać, że opis „optoizolacja” oznacza dowolną kompatybilność z PCF8575.

Bezpieczniejsze warianty:

- PCF8575 -> tranzystor/ULN2803 -> wejścia modułów przekaźnikowych,
- 74HC595 -> ULN2803 -> wejścia przekaźnikowe,
- ekspander MCP23017 z odpowiednim stopniem wyjściowym.

PCF8575 ma słabe źródło prądu w stanie HIGH, dlatego moduły active-LOW mogą działać, ale trzeba zweryfikować poziomy i stan podczas resetu ESP32.

## Czujniki na jednej skrętce

Nie łączyć trzech niezależnych DHT22 na jednej linii danych. Każdy DHT22 potrzebuje własnego GPIO. Przy 8 m użyć 5 V, wspólnej masy oraz rezystora pull-up DATA do 5 V, typowo 4.7–10 kΩ. Przewód prowadzić z dala od przewodów 230 V.
