# Status

## Stan

Szkielet konfiguracyjny i zalecenia sterowania przygotowane. Kod wykonawczy nie
jest jeszcze gotowy do wgrania, ponieważ nie znamy:

- dokładnego modelu ESP32,
- dokładnego modułu PCF8575,
- prądu wejściowego modułu przekaźnikowego,
- modelu siłowników i ich czasu pełnego ruchu,
- tego, czy siłowniki mają krańcówki,
- mapy czujników i wymagania dotyczącego deszczu/wiatru.

## Następny etap

1. Potwierdzić sprzęt i mapę pinów.
2. Zaimplementować klasę blokady pary przekaźników.
3. Dodać 8 kanałów roletowych przez IO ekspandera.
4. Dodać czujniki i lokalny regulator temperatury.
5. Dodać kanały diagnostyczne i testy bez obciążenia 230 V.
