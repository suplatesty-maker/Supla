# Schody VL53L1X + SUPLA — STATUS

Parametry WWW:

- `Schody - soft start LED [ms]`
- `Schody - blokada kolejnego sygnału [ms]`

## Założenia

- Płytka: ESP32-C3 Super Mini.
- Dwa moduły TOF400C z czujnikami VL53L1X.
- Taśma LED 12/24 V sterowana przez MOSFET z PWM.
- Kanał SUPLA 1: dimmer LED.
- Kanał SUPLA 2: wirtualny przekaźnik „Blokada czujników”.
- Automatyczne włączenie wymaga kolejności A→B albo B→A.

## Połączenia domyślne

| Sygnał | GPIO |
|---|---:|
| SDA | 6 |
| SCL | 7 |
| XSHUT czujnika A | 5 |
| XSHUT czujnika B | 10 |
| PWM MOSFET | 4 |

## Do sprawdzenia przed wgraniem

- Czy oba moduły mają wyprowadzony XSHUT i czy poziomy logiczne są 3,3 V.
- Czy biblioteka `VL53L1X by Pololu` jest zainstalowana.
- Uzupełnić WiFi, GUID, AUTHKEY i e-mail.
- Potwierdzić piny z konkretną wersją płytki ESP32-C3 Super Mini.
- Ustawić `DETECT_MIN_MM`, `DETECT_MAX_MM`, `SEQUENCE_TIMEOUT_MS`
  i `LIGHT_ON_TIME_MS` po testach na schodach.

Kod nie został jeszcze skompilowany ani sprawdzony na fizycznym module.
