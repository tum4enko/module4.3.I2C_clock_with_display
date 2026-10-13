/*
 * I2C годинник: ESP32-S3 + SSD1306 (128x64) + DS1307 + DS18B20
 *
 * Встановлення часу через Serial Monitor (115200, кінець рядка — NL або CR+NL):
 *   set 2026-10-13 19:21:00
 */

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <RTClib.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// ===================== Конфігурація =====================
constexpr uint8_t PIN_SDA     = 4;
constexpr uint8_t PIN_SCL     = 5;
constexpr uint8_t PIN_ONEWIRE = 7;

constexpr uint8_t SCREEN_W  = 128;
constexpr uint8_t SCREEN_H  = 64;
constexpr uint8_t OLED_ADDR = 0x3C;
constexpr uint8_t RTC_ADDR  = 0x68;

constexpr uint32_t DISPLAY_INTERVAL_MS = 100;   // 10 FPS — плавна анімація
constexpr uint32_t TEMP_INTERVAL_MS    = 1000;  // >= 750 мс для 12-біт DS18B20

// 1 — примусово записати в RTC час компіляції при кожному старті
// (поставити 1, прошити, потім повернути 0 і прошити ще раз!)
#define FORCE_SET_TIME 0

// ===================== Об'єкти =====================
Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);
RTC_DS1307 rtc;
OneWire oneWire(PIN_ONEWIRE);
DallasTemperature tempSensor(&oneWire);

struct ClockState {
  DateTime now;
  bool     rtcInitialized = false;
  bool     rtcOk          = false;
  uint32_t secondStartMs  = 0;      // millis() у момент зміни секунди
  float    temperature    = DEVICE_DISCONNECTED_C;
  bool     tempOk         = false;
};
ClockState state;

static const char* const DAY_NAMES[7] = {
  "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

// ===================== Ініціалізація =====================
bool initDisplay() {
  // periphBegin = false: Wire вже запущено на наших пінах у setup()
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR, true, false)) {
    return false;
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextWrap(false);
  display.cp437(true);              // коректна таблиця символів (° = 0xF8)
  display.display();
  return true;
}

bool initRTC() {
  if (!rtc.begin(&Wire)) {
    return false;
  }
  // Біт CH=1 (годинник зупинено) — новий чип або сіла батарейка
  if (!rtc.isrunning() || FORCE_SET_TIME) {
    Serial.println(F("[RTC] Встановлюю час компіляції"));
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }
  return true;
}

void initTempSensor() {
  tempSensor.begin();
  tempSensor.setResolution(12);
  tempSensor.setWaitForConversion(false);  // неблокуючий режим
  tempSensor.requestTemperatures();
  Serial.printf("[DS18B20] Знайдено датчиків: %d\n", tempSensor.getDeviceCount());
}

// ===================== Зчитування даних =====================
void readTime() {
  if (!state.rtcInitialized) {
    state.rtcInitialized = initRTC();      // повторна спроба, якщо RTC з'явився
    if (!state.rtcInitialized) {
      state.rtcOk = false;
      return;
    }
  }

  // Перевірка, що DS1307 відповідає на шині
  Wire.beginTransmission(RTC_ADDR);
  state.rtcOk = (Wire.endTransmission() == 0);
  if (!state.rtcOk) return;

  DateTime t = rtc.now();
  if (!t.isValid()) {
    state.rtcOk = false;
    return;
  }

  if (t.second() != state.now.second()) {
    state.secondStartMs = millis();       // синхронізація анімації з RTC
    Serial.printf("%02d:%02d:%02d  %s %02d.%02d.%04d\n",
                  t.hour(), t.minute(), t.second(),
                  DAY_NAMES[t.dayOfTheWeek()], t.day(), t.month(), t.year());
  }
  state.now = t;
}

void updateTemperature() {
  float t = tempSensor.getTempCByIndex(0);   // результат попереднього запиту
  state.tempOk = (t != DEVICE_DISCONNECTED_C);
  if (state.tempOk) state.temperature = t;
  tempSensor.requestTemperatures();          // запуск наступного вимірювання
}

// ===================== Малювання =====================
void drawBoldText(int16_t x, int16_t y, const char* text, uint8_t size) {
  display.setTextSize(size);
  display.setCursor(x, y);
  display.print(text);
  display.setCursor(x + 1, y);               // зсув на 1 px = "жирний"
  display.print(text);
}

void drawDate(const DateTime& t) {
  char buf[20];
  snprintf(buf, sizeof(buf), "%s %02d.%02d.%04d",
           DAY_NAMES[t.dayOfTheWeek()], t.day(), t.month(), t.year());

  display.setTextSize(1);
  int16_t w = strlen(buf) * 6;
  display.setCursor((SCREEN_W - w) / 2, 0);
  display.print(buf);
  display.drawFastHLine(0, 10, SCREEN_W, SSD1306_WHITE);
}

void drawTime(const DateTime& t) {
  char hhmm[8];
  char ss[6];
  snprintf(hhmm, sizeof(hhmm), "%02d:%02d", t.hour(), t.minute());
  snprintf(ss, sizeof(ss), ":%02d", t.second());

  constexpr int16_t X = 1;
  constexpr int16_t Y = 15;
  drawBoldText(X, Y, hhmm, 3);               // 5 символів × 18 px = 90 px

  display.setTextSize(2);
  display.setCursor(X + 90, Y + 7);          // вирівнювання по нижньому краю цифр
  display.print(ss);
}

void drawSecondsBar(const DateTime& t) {
  constexpr int16_t X = 4, Y = 42, W = 120, H = 7;

  uint32_t frac = millis() - state.secondStartMs;
  if (frac > 999) frac = 999;
  int16_t fill = ((t.second() * 1000UL + frac) * (W - 2)) / 60000UL;

  display.drawRect(X, Y, W, H, SSD1306_WHITE);
  display.fillRect(X + 1, Y + 1, fill, H - 2, SSD1306_WHITE);

  for (int i = 1; i < 12; i++) {             // мітки кожні 5 секунд
    int16_t px = X + 1 + i * (W - 2) / 12;
    display.drawFastVLine(px, Y + H, 2, SSD1306_WHITE);
  }
}

void drawTemperature() {
  display.setTextSize(1);
  display.setCursor(4, 55);
  if (state.tempOk) {
    display.printf("Temp: %.1f", state.temperature);
    display.write(0xF8);                     // символ °
    display.print('C');
  } else {
    display.print("Temp: --.-");
  }
}

void drawSpinner() {
  static const int8_t DX[8] = { 0, 3, 4, 3, 0, -3, -4, -3 };
  static const int8_t DY[8] = { -4, -3, 0, 3, 4, 3, 0, -3 };
  constexpr int16_t CX = 121, CY = 58;

  uint32_t frac = millis() - state.secondStartMs;
  uint8_t active = (frac / 125) % 8;         // повний оберт за 1 секунду

  for (uint8_t i = 0; i < 8; i++) {
    if (i == active) {
      display.fillCircle(CX + DX[i], CY + DY[i], 1, SSD1306_WHITE);
    } else {
      display.drawPixel(CX + DX[i], CY + DY[i], SSD1306_WHITE);
    }
  }
}

void drawMessage(const char* line1, const char* line2) {
  display.setTextSize(1);
  display.setCursor(0, 20);
  display.print(line1);
  display.setCursor(0, 34);
  display.print(line2);
}

void updateDisplay() {
  display.clearDisplay();

  if (!state.rtcOk) {
    drawMessage("RTC DS1307", "not responding...");
  } else {
    drawDate(state.now);
    drawTime(state.now);
    drawSecondsBar(state.now);
    drawTemperature();
    drawSpinner();
  }

  display.display();
}

// ===================== Serial-команди =====================
void processCommand(const char* cmd) {
  int Y, M, D, h, m, s;
  if (sscanf(cmd, "set %d-%d-%d %d:%d:%d", &Y, &M, &D, &h, &m, &s) == 6 &&
      Y >= 2000 && Y <= 2099) {
    DateTime dt(Y, M, D, h, m, s);
    if (dt.isValid()) {
      rtc.adjust(dt);
      Serial.println(F("[RTC] Час встановлено"));
      return;
    }
  }
  Serial.println(F("Формат: set YYYY-MM-DD HH:MM:SS"));
}

void handleSerial() {
  static char line[32];
  static uint8_t len = 0;

  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      line[len] = '\0';
      if (len > 0) processCommand(line);
      len = 0;
    } else if (len < sizeof(line) - 1) {
      line[len++] = c;
    }
  }
}

// ===================== setup / loop =====================
void setup() {
  Serial.begin(115200);

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(100000);                     // DS1307 — максимум 100 кГц

  if (!initDisplay()) {
    Serial.println(F("[OLED] SSD1306 не знайдено (0x3C)"));
    while (true) { delay(1000); }            // без дисплея працювати нема сенсу
  }

  state.rtcInitialized = initRTC();
  Serial.println(state.rtcInitialized ? F("[RTC] DS1307 OK")
                                      : F("[RTC] DS1307 не знайдено (0x68)"));

  initTempSensor();
}

void loop() {
  static uint32_t lastDisplayMs = 0;
  static uint32_t lastTempMs    = 0;
  const uint32_t nowMs = millis();

  handleSerial();

  if (nowMs - lastDisplayMs >= DISPLAY_INTERVAL_MS) {
    lastDisplayMs = nowMs;
    readTime();
    updateDisplay();
  }

  if (nowMs - lastTempMs >= TEMP_INTERVAL_MS) {
    lastTempMs = nowMs;
    updateTemperature();
  }
}