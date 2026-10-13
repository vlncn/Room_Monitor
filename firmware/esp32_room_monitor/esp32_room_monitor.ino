/*
  ESP32 Room Monitor (standalone / local)
  BME280 + 0.96" SSD1306 OLED (128x64), FreeRTOS tasks

  - SensorTask  : reads temperature / humidity / pressure every second
  - DisplayTask : shows the latest values on the OLED
  - Serial log  : one CSV line per reading (115200 baud)

  Libraries: Adafruit BME280 (+ Unified Sensor), Adafruit SSD1306 (+ GFX)
  Wiring (both devices): VIN->3V3, GND->GND, SDA->GPIO21, SCL->GPIO22
*/

#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ---------- CONFIG ----------
const int      SDA_PIN     = 21;
const int      SCL_PIN     = 22;
const uint8_t  BME_ADDR    = 0x76;   // try 0x77 if not found
const uint8_t  OLED_ADDR   = 0x3C;   // try 0x3D if not found
const uint32_t SAMPLE_MS   = 1000;
const uint32_t DISPLAY_MS  = 500;
const int      FAIL_LIMIT  = 5;      // consecutive failed reads before showing an error
// -----------------------------

#define SCREEN_W 128
#define SCREEN_H 64

struct Reading {
  float temp = NAN;
  float hum  = NAN;
  float pres = NAN;
  bool  ok   = false;     // false until the first valid read, and after repeated failures
};

Adafruit_BME280  bme;
Adafruit_SSD1306 oled(SCREEN_W, SCREEN_H, &Wire, -1);

SemaphoreHandle_t i2cMutex;    // guards the shared I2C bus
SemaphoreHandle_t dataMutex;   // guards 'latest'
Reading           latest;

// ======================= Tasks =======================

void SensorTask(void*) {
  TickType_t lastWake = xTaskGetTickCount();
  int failCount = 0;

  for (;;) {
    float t = NAN, h = NAN, p = NAN;

    if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
      t = bme.readTemperature();
      h = bme.readHumidity();
      p = bme.readPressure() / 100.0F;   // Pa -> hPa
      xSemaphoreGive(i2cMutex);
    }

    bool valid = !isnan(t) && !isnan(h) && !isnan(p);
    failCount = valid ? 0 : failCount + 1;

    if (xSemaphoreTake(dataMutex, portMAX_DELAY) == pdTRUE) {
      if (valid) {
        latest.temp = t; latest.hum = h; latest.pres = p;
        latest.ok = true;
      } else if (failCount >= FAIL_LIMIT) {
        latest.ok = false;               // sensor considered lost
      }
      xSemaphoreGive(dataMutex);
    }

    if (valid) Serial.printf("%lu,%.2f,%.2f,%.2f\n", millis() / 1000, t, h, p);
    else       Serial.println("Sensor read failed");

    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(SAMPLE_MS));
  }
}

void DisplayTask(void*) {
  TickType_t lastWake = xTaskGetTickCount();

  for (;;) {
    Reading r;
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      r = latest;
      xSemaphoreGive(dataMutex);
    }

    if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
      oled.clearDisplay();
      oled.setTextColor(SSD1306_WHITE);

      oled.setTextSize(1);
      oled.setCursor(0, 0);
      oled.print("Room Monitor");
      oled.drawFastHLine(0, 10, SCREEN_W, SSD1306_WHITE);

      if (r.ok) {
        oled.setTextSize(3);
        oled.setCursor(0, 16);
        oled.printf("%.1fC", r.temp);

        oled.setTextSize(1);
        oled.setCursor(0, 46);
        oled.printf("Humidity: %.1f %%", r.hum);
        oled.setCursor(0, 56);
        oled.printf("Pressure: %.1f hPa", r.pres);
      } else {
        oled.setTextSize(2);
        oled.setCursor(0, 24);
        oled.print("SENSOR");
        oled.setCursor(0, 42);
        oled.print("ERROR");
      }

      oled.display();
      xSemaphoreGive(i2cMutex);
    }

    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(DISPLAY_MS));
  }
}

// ======================= Setup / loop =======================

void setup() {
  Serial.begin(115200);
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);

  if (!bme.begin(BME_ADDR)) {
    Serial.println("BME280 not found. Check wiring and address (0x76 / 0x77).");
    while (true) delay(1000);
  }
  // 1x oversampling, 1 s standby: low self-heating for room measurements
  bme.setSampling(Adafruit_BME280::MODE_NORMAL,
                  Adafruit_BME280::SAMPLING_X1,
                  Adafruit_BME280::SAMPLING_X1,
                  Adafruit_BME280::SAMPLING_X1,
                  Adafruit_BME280::FILTER_OFF,
                  Adafruit_BME280::STANDBY_MS_1000);

  if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED not found. Check wiring and address (0x3C / 0x3D).");
    while (true) delay(1000);
  }
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(0, 28);
  oled.print("Starting...");
  oled.display();

  i2cMutex  = xSemaphoreCreateMutex();
  dataMutex = xSemaphoreCreateMutex();

  Serial.println("uptime_s,temp_C,hum_pct,pres_hPa");

  xTaskCreatePinnedToCore(SensorTask,  "sensor",  4096, nullptr, 2, nullptr, 1);
  xTaskCreatePinnedToCore(DisplayTask, "display", 4096, nullptr, 1, nullptr, 1);
}

void loop() {
  vTaskDelete(nullptr);   // all work runs in tasks
}
