/*
  ESP32 Room Monitor (standalone / local), bare-metal style
  BME280 + 0.96" SSD1306 OLED (128x64)

  One loop(), no RTOS tasks. Timing is done with millis().
  Every SAMPLE_MS: read the sensor, print to Serial, redraw the OLED.

  Libraries: Adafruit BME280 (+ Unified Sensor), Adafruit SSD1306 (+ GFX)
  Wiring (both devices): VIN->3V3, GND->GND, SDA->GPIO21, SCL->GPIO22
*/

#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ---------- CONFIG ----------
const int      SDA_PIN    = 21;
const int      SCL_PIN    = 22;
const uint8_t  BME_ADDR   = 0x76;   // try 0x77 if not found
const uint8_t  OLED_ADDR  = 0x3C;   // try 0x3D if not found
const uint32_t SAMPLE_MS  = 1000;
const int      FAIL_LIMIT = 5;      // consecutive failed reads before showing an error
// -----------------------------

#define SCREEN_W 128
#define SCREEN_H 64

Adafruit_BME280  bme;
Adafruit_SSD1306 oled(SCREEN_W, SCREEN_H, &Wire, -1);

float    temp = NAN, hum = NAN, pres = NAN;
bool     sensorOk = false;
int      failCount = 0;
uint32_t lastSample = 0;

void drawScreen() {
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);

  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.print("Room Monitor");
  oled.drawFastHLine(0, 10, SCREEN_W, SSD1306_WHITE);

  if (sensorOk) {
    oled.setTextSize(3);
    oled.setCursor(0, 16);
    oled.printf("%.1fC", temp);

    oled.setTextSize(1);
    oled.setCursor(0, 46);
    oled.printf("Humidity: %.1f %%", hum);
    oled.setCursor(0, 56);
    oled.printf("Pressure: %.1f hPa", pres);
  } else {
    oled.setTextSize(2);
    oled.setCursor(0, 24);
    oled.print("SENSOR");
    oled.setCursor(0, 42);
    oled.print("ERROR");
  }

  oled.display();
}

void readSensor() {
  float t = bme.readTemperature();
  float h = bme.readHumidity();
  float p = bme.readPressure() / 100.0F;   // Pa -> hPa

  bool valid = !isnan(t) && !isnan(h) && !isnan(p);
  failCount = valid ? 0 : failCount + 1;

  if (valid) {
    temp = t; hum = h; pres = p;
    sensorOk = true;
    Serial.printf("%lu,%.2f,%.2f,%.2f\n", millis() / 1000, t, h, p);
  } else {
    if (failCount >= FAIL_LIMIT) sensorOk = false;   // sensor considered lost
    Serial.println("Sensor read failed");
  }
}

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

  Serial.println("uptime_s,temp_C,hum_pct,pres_hPa");
}

void loop() {
  if (millis() - lastSample >= SAMPLE_MS) {
    lastSample += SAMPLE_MS;   // fixed schedule, no drift
    readSensor();
    drawScreen();
  }
  // Other non-blocking work can go here
}
