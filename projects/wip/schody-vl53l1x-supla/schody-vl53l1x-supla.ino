/*
 * ESP32-C3 Super Mini + 2x VL53L1X/TOF400C + SUPLA
 *
 * Funkcje:
 * - dwa czujniki ToF na jednej magistrali I2C,
 * - wymaganie kolejności SENSOR_A -> SENSOR_B albo SENSOR_B -> SENSOR_A,
 * - filtr czasowy, który odrzuca pojedyncze/krótkie impulsy,
 * - kanał 1 SUPLA: dimmer PWM taśmy LED,
 * - kanał 2 SUPLA: blokada automatyki czujników.
 *
 * Wymagana biblioteka Arduino:
 * - VL53L1X by Pololu
 * - SuplaDevice
 *
 * Domyślne połączenia ESP32-C3:
 *   GPIO6  SDA
 *   GPIO7  SCL
 *   GPIO5  XSHUT czujnika A
 *   GPIO10 XSHUT czujnika B
 *   GPIO4  PWM -> driver MOSFET
 *
 * TOF400C: oba czujniki muszą mieć wspólne SDA/SCL/GND, ale osobne XSHUT.
 */

#include <Arduino.h>
#include <Wire.h>
#include <VL53L1X.h>

#include <SuplaDevice.h>
#include <supla/network/esp_wifi.h>
#include <supla/network/html/custom_parameter.h>
#include <supla/control/dimmer_leds.h>
#include <supla/control/virtual_relay.h>
#include <supla/storage/eeprom.h>

// ---------- WiFi i SUPLA ----------
#define WIFI_SSID     "TWOJE_WIFI"
#define WIFI_PASSWORD "TWOJE_HASLO"
#define SUPLA_SERVER  "svr1.supla.org"
#define EMAIL         "twoj@email.pl"

char GUID[SUPLA_GUID_SIZE] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
char AUTHKEY[SUPLA_AUTHKEY_SIZE] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

// ---------- Piny ----------
constexpr uint8_t PIN_I2C_SDA = 6;
constexpr uint8_t PIN_I2C_SCL = 7;
constexpr uint8_t PIN_XSHUT_A = 5;
constexpr uint8_t PIN_XSHUT_B = 10;
constexpr uint8_t PIN_PWM_LED = 4;

// ---------- Parametry automatyki ----------
// Wykrycie jest ważne, gdy odległość mieści się w tym przedziale.
constexpr uint16_t DETECT_MIN_MM = 300;
constexpr uint16_t DETECT_MAX_MM = 800;

// Czujnik musi być aktywny przez ten czas, aby rozpocząć sekwencję.
constexpr uint32_t DETECT_CONFIRM_MS = 120;
// Po pierwszym czujniku drugi musi zadziałać w tym czasie.
constexpr uint32_t SEQUENCE_TIMEOUT_MS = 2500;
// Automatyczne świecenie. 0 oznacza bezterminowo, ale tu używamy timera.
constexpr uint32_t LIGHT_ON_TIME_MS = 30000;
// Częstotliwość odczytu VL53L1X.
constexpr uint32_t SENSOR_READ_MS = 60;

constexpr uint8_t SENSOR_A_ADDRESS = 0x30;
constexpr uint8_t SENSOR_B_ADDRESS = 0x31;

Supla::ESPWifi wifi(WIFI_SSID, WIFI_PASSWORD);
Supla::Eeprom eeprom;
Supla::Control::DimmerLeds dimmer(PIN_PWM_LED);
// Drugi kanał SUPLA. ON = automatyka zablokowana.
Supla::Control::VirtualRelay sensorLock;

// Parametry dostępne w konfiguracji WWW urządzenia.
// Są przechowywane w konfiguracji SUPLA, więc nie znikają po restarcie.
Supla::Html::CustomParameter softStartMs(
    "stairs_soft_start_ms", "Schody - soft start LED [ms]",
    500, 0, 10000);
Supla::Html::CustomParameter sensorLockoutMs(
    "stairs_sensor_lockout_ms", "Schody - blokada kolejnego sygnału [ms]",
    3000, 0, 30000);

VL53L1X sensorA;
VL53L1X sensorB;

enum class SequenceState : uint8_t {
  IDLE,
  WAIT_FOR_B,
  WAIT_FOR_A
};

SequenceState sequenceState = SequenceState::IDLE;
uint32_t firstSensorTime = 0;
uint32_t lightOnTime = 0;
uint32_t lockoutUntil = 0;
uint32_t activeSinceA = 0;
uint32_t activeSinceB = 0;
uint32_t lastSensorRead = 0;
bool detectedA = false;
bool detectedB = false;
bool sensorAReady = false;
bool sensorBReady = false;

bool timeReached(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}

bool initSensor(VL53L1X &sensor, uint8_t xshutPin, uint8_t address) {
  digitalWrite(xshutPin, HIGH);
  delay(10);

  // Każdy nowy czujnik startuje pod adresem 0x29.
  if (!sensor.init()) {
    return false;
  }

  sensor.setAddress(address);
  sensor.setDistanceMode(VL53L1X::Long);
  sensor.setMeasurementTimingBudget(50000);
  sensor.startContinuous(60);
  return true;
}

bool readDetected(VL53L1X &sensor, bool &ready, uint32_t &activeSince,
                  uint32_t now) {
  if (!ready) {
    return false;
  }

  uint16_t distance = sensor.read();
  bool inRange = !sensor.timeoutOccurred() &&
                 distance >= DETECT_MIN_MM &&
                 distance <= DETECT_MAX_MM;

  if (inRange) {
    if (activeSince == 0) {
      activeSince = now;
    }
    return now - activeSince >= DETECT_CONFIRM_MS;
  }

  activeSince = 0;
  return false;
}

void resetSequence() {
  sequenceState = SequenceState::IDLE;
  firstSensorTime = 0;
}

void triggerLights(uint32_t now) {
  // Parametr może być zmieniony z WWW bez ponownego wgrywania programu.
  dimmer.setFadeEffectTime(softStartMs.getParameterValue());
  dimmer.turnOn();
  lightOnTime = now;
  lockoutUntil = now + sensorLockoutMs.getParameterValue();
  resetSequence();
}

void processSequence(uint32_t now) {
  if (sensorLock.isOn()) {
    resetSequence();
    return;
  }

  if (!timeReached(now, lockoutUntil)) {
    resetSequence();
    return;
  }

  switch (sequenceState) {
    case SequenceState::IDLE:
      if (detectedA && !detectedB) {
        sequenceState = SequenceState::WAIT_FOR_B;
        firstSensorTime = now;
      } else if (detectedB && !detectedA) {
        sequenceState = SequenceState::WAIT_FOR_A;
        firstSensorTime = now;
      }
      break;

    case SequenceState::WAIT_FOR_B:
      if (detectedB) {
        triggerLights(now);
      } else if (now - firstSensorTime > SEQUENCE_TIMEOUT_MS) {
        resetSequence();
      }
      break;

    case SequenceState::WAIT_FOR_A:
      if (detectedA) {
        triggerLights(now);
      } else if (now - firstSensorTime > SEQUENCE_TIMEOUT_MS) {
        resetSequence();
      }
      break;
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(PIN_XSHUT_A, OUTPUT);
  pinMode(PIN_XSHUT_B, OUTPUT);
  digitalWrite(PIN_XSHUT_A, LOW);
  digitalWrite(PIN_XSHUT_B, LOW);

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  delay(20);

  sensorAReady = initSensor(sensorA, PIN_XSHUT_A, SENSOR_A_ADDRESS);
  sensorBReady = initSensor(sensorB, PIN_XSHUT_B, SENSOR_B_ADDRESS);

  if (!sensorAReady || !sensorBReady) {
    Serial.println("BLAD: nie znaleziono obu czujnikow VL53L1X.");
  }

  dimmer.setDefaultDimmedBrightness(80);
  dimmer.setFadeEffectTime(softStartMs.getParameterValue());
  sensorLock.setDefaultStateOff();

  SuplaDevice.setName("Schody - czujniki ToF");
  SuplaDevice.begin(GUID, SUPLA_SERVER, EMAIL, AUTHKEY);
}

void loop() {
  SuplaDevice.iterate();

  uint32_t now = millis();

  if (now - lastSensorRead >= SENSOR_READ_MS) {
    lastSensorRead = now;
    detectedA = readDetected(sensorA, sensorAReady, activeSinceA, now);
    detectedB = readDetected(sensorB, sensorBReady, activeSinceB, now);
    processSequence(now);
  }

  if (lightOnTime != 0 && now - lightOnTime >= LIGHT_ON_TIME_MS) {
    dimmer.turnOff();
    lightOnTime = 0;
    resetSequence();
  }
}
