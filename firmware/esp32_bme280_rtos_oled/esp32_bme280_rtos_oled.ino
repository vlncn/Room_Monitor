/*
  ESP32 + BME280 + 0.96" OLED (SSD1306 128x64) temperature monitor, FreeRTOS version

  Tasks:
    SensorTask  (core 1) : reads BME280 every SAMPLE_INTERVAL_MS, updates the
                           shared "latest" values and pushes readings to a queue
    NetTask     (core 0) : owns WiFi, NTP, MQTT and the LittleFS offline buffer.
                           Publishes readings, or buffers them if offline, and
                           flushes the backlog once MQTT is back
    DisplayTask (core 1) : refreshes the OLED once per second

  Both the BME280 and the OLED share one I2C bus, so access is guarded by a mutex.
  Only NetTask touches MQTT and the filesystem, so neither needs extra locking.

  Libraries (Library Manager):
    - Adafruit BME280 Library (+ Adafruit Unified Sensor)
    - Adafruit SSD1306 (+ Adafruit GFX Library)
    - PubSubClient (Nick O'Leary)

  Wiring (both devices on the same I2C pins):
    VIN -> 3V3, GND -> GND, SDA -> GPIO21, SCL -> GPIO22
    BME280 address 0x76 (or 0x77), OLED address 0x3C (or 0x3D)
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <LittleFS.h>
#include <time.h>

// ---------- CONFIG ----------
const char* WIFI_SSID    = "";
const char* WIFI_PASS    = "";

const char* MQTT_HOST    = "";
const uint16_t MQTT_PORT = 1883;
const char* MQTT_USER    = "";            // leave empty if no auth
const char* MQTT_PASS    = "";
const char* DEVICE_ID    = "";
const char* MQTT_TOPIC   = "";

const uint8_t  BME_ADDR           = 0x76;
const uint8_t  OLED_ADDR          = 0x3C;
const uint32_t SAMPLE_INTERVAL_MS = 30000;      // 30 s
const uint32_t DISPLAY_INTERVAL_MS = 1000;
const size_t   MAX_BUFFER_BYTES   = 200 * 1024;
const char*    BUFFER_FILE        = "/buffer.txt";
const char*    TEMP_FILE          = "/buffer.tmp";
const char*    TZ                 = "";
// -----------------------------

#define SCREEN_W 128
#define SCREEN_H 64

struct Reading {
  time_t ts;
  float  temp;
  float  hum;
};

struct Latest {
  float temp = NAN, hum = NAN;
  bool  valid = false;
};

Adafruit_BME280   bme;
Adafruit_SSD1306  oled(SCREEN_W, SCREEN_H, &Wire, -1);
WiFiClient        wifiClient;
PubSubClient      mqtt(wifiClient);

QueueHandle_t     readingQueue;
SemaphoreHandle_t i2cMutex;
SemaphoreHandle_t dataMutex;

Latest            latest;
volatile bool     mqttOk = false;
volatile int      bufferedCount = 0;
bool              hasOled = false;

bool timeSynced() { return time(nullptr) > 1700000000; }

// ======================= Offline buffer (NetTask only) =======================

int countBufferLines() {
  if (!LittleFS.exists(BUFFER_FILE)) return 0;
  File f = LittleFS.open(BUFFER_FILE, FILE_READ);
  if (!f) return 0;
  int n = 0;
  while (f.available()) {
    String l = f.readStringUntil('\n');
    l.trim();
    if (l.length()) n++;
  }
  f.close();
  return n;
}

void bufferReading(const String& line) {
  File f = LittleFS.open(BUFFER_FILE, FILE_APPEND);
  if (!f) { Serial.println("Buffer open failed"); return; }
  if (f.size() > MAX_BUFFER_BYTES) {
    f.close();
    Serial.println("Buffer full, dropping reading");
    return;
  }
  f.println(line);
  f.close();
  bufferedCount++;
  Serial.println("Offline -> buffered");
}

// Send buffered lines oldest-first; keep whatever fails
void flushBuffer() {
  if (bufferedCount <= 0 || !mqtt.connected()) return;
  if (!LittleFS.exists(BUFFER_FILE)) { bufferedCount = 0; return; }

  File in = LittleFS.open(BUFFER_FILE, FILE_READ);
  if (!in) return;
  File out = LittleFS.open(TEMP_FILE, FILE_WRITE);
  if (!out) { in.close(); return; }

  bool failed = false;
  int sent = 0, remaining = 0;
  while (in.available()) {
    String line = in.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    if (!failed && mqtt.connected() && mqtt.publish(MQTT_TOPIC, line.c_str())) {
      sent++;
      mqtt.loop();
      vTaskDelay(pdMS_TO_TICKS(20));   // don't flood the broker
    } else {
      failed = true;
      out.println(line);
      remaining++;
    }
  }
  in.close();
  out.close();

  LittleFS.remove(BUFFER_FILE);
  if (failed) LittleFS.rename(TEMP_FILE, BUFFER_FILE);
  else LittleFS.remove(TEMP_FILE);

  bufferedCount = remaining;
  if (sent) Serial.printf("Flushed %d buffered readings\n", sent);
}

// ======================= Network helpers (NetTask only) =======================

void maintainConnections() {
  static uint32_t lastWifiTry = 0, lastMqttTry = 0;
  static bool ntpStarted = false;
  uint32_t now = millis();

  if (WiFi.status() != WL_CONNECTED) {
    mqttOk = false;
    if (now - lastWifiTry > 10000) {
      lastWifiTry = now;
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
    return;
  }

  if (!ntpStarted) {
    configTzTime(TZ, "pool.ntp.org", "time.google.com");
    ntpStarted = true;
  }

  if (!mqtt.connected()) {
    mqttOk = false;
    if (now - lastMqttTry > 5000) {
      lastMqttTry = now;
      bool ok = (strlen(MQTT_USER) > 0)
                  ? mqtt.connect(DEVICE_ID, MQTT_USER, MQTT_PASS)
                  : mqtt.connect(DEVICE_ID);
      Serial.println(ok ? "MQTT connected" : "MQTT connect failed");
    }
  }
  mqttOk = mqtt.connected();
}

String buildPayload(const Reading& r) {
  char buf[200];
  snprintf(buf, sizeof(buf),
           "{\"device\":\"%s\",\"ts\":%lld,\"temp\":%.2f,\"hum\":%.2f}",
           DEVICE_ID, (long long)r.ts, r.temp, r.hum);
  return String(buf);
}

void handleReading(const Reading& r) {
  String payload = buildPayload(r);
  Serial.println(payload);

  if (mqtt.connected()) {
    flushBuffer();   // older data first
    if (!mqtt.publish(MQTT_TOPIC, payload.c_str())) bufferReading(payload);
  } else {
    bufferReading(payload);
  }
}

// ======================= Tasks =======================

void SensorTask(void*) {
  TickType_t lastWake = xTaskGetTickCount();
  for (;;) {
    float t = NAN, h = NAN;
    if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
      t = bme.readTemperature();
      h = bme.readHumidity();
      xSemaphoreGive(i2cMutex);
    }

    if (isnan(t) || isnan(h)) {
      Serial.println("BME280 read failed");
    } else {
      if (xSemaphoreTake(dataMutex, portMAX_DELAY) == pdTRUE) {
        latest.temp = t; latest.hum = h; latest.valid = true;
        xSemaphoreGive(dataMutex);
      }
      // Only queue timestamped data (needs one NTP sync after boot)
      if (timeSynced()) {
        Reading r = { time(nullptr), t, h};
        if (xQueueSend(readingQueue, &r, 0) != pdTRUE) {
          Reading dropped;                     // queue full: drop oldest, keep newest
          xQueueReceive(readingQueue, &dropped, 0);
          xQueueSend(readingQueue, &r, 0);
        }
      } else {
        Serial.println("Waiting for first NTP sync...");
      }
    }
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(SAMPLE_INTERVAL_MS));
  }
}

void NetTask(void*) {
  uint32_t lastFlushTry = 0;
  for (;;) {
    maintainConnections();
    if (mqtt.connected()) mqtt.loop();

    Reading r;
    if (xQueueReceive(readingQueue, &r, pdMS_TO_TICKS(200)) == pdTRUE) {
      handleReading(r);
    }

    // Drain the backlog even if no new reading arrives
    if (mqtt.connected() && bufferedCount > 0 && millis() - lastFlushTry > 5000) {
      lastFlushTry = millis();
      flushBuffer();
    }
  }
}

void DisplayTask(void*) {
  TickType_t lastWake = xTaskGetTickCount();
  for (;;) {
    Latest snap;
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      snap = latest;
      xSemaphoreGive(dataMutex);
    }

    char timeStr[8] = "--:--";
    if (timeSynced()) {
      struct tm tmv;
      if (getLocalTime(&tmv, 0)) strftime(timeStr, sizeof(timeStr), "%H:%M", &tmv);
    }
    bool wifiOk = (WiFi.status() == WL_CONNECTED);

    if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
      oled.clearDisplay();
      oled.setTextColor(SSD1306_WHITE);

      // Status line
      oled.setTextSize(1);
      oled.setCursor(0, 0);
      oled.printf("%s W:%s M:%s", timeStr, wifiOk ? "OK" : "--", mqttOk ? "OK" : "--");

      // Temperature (big)
      oled.setTextSize(3);
      oled.setCursor(0, 14);
      if (snap.valid) oled.printf("%.1fC", snap.temp);
      else            oled.print("--.-C");

      // Humidity
      oled.setTextSize(1);
      oled.setCursor(0, 44);
      if (snap.valid) oled.printf("Hum %.0f%%", snap.hum);
      else            oled.print("Waiting for sensor");

      // Backlog counter
      oled.setCursor(0, 56);
      oled.printf("Buffered: %d", (int)bufferedCount);

      oled.display();
      xSemaphoreGive(i2cMutex);
    }
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(DISPLAY_INTERVAL_MS));
  }
}

// ======================= Arduino entry points =======================

void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);
  Wire.setClock(400000);

  if (!LittleFS.begin(true)) Serial.println("LittleFS mount failed");
  bufferedCount = countBufferLines();

  if (!bme.begin(BME_ADDR)) {
    Serial.println("BME280 not found! Check wiring / address (0x76 or 0x77).");
    while (true) delay(1000);
  }

  bme.setSampling(Adafruit_BME280::MODE_NORMAL,
                Adafruit_BME280::SAMPLING_X1,     // temperature
                Adafruit_BME280::SAMPLING_NONE,   // pressure (not used)
                Adafruit_BME280::SAMPLING_X1,     // humidity
                Adafruit_BME280::FILTER_OFF,
                Adafruit_BME280::STANDBY_MS_1000);

  hasOled = oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  if (!hasOled) {
    Serial.println("OLED not found, continuing without display.");
  }

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(512);
  mqtt.setSocketTimeout(3);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  readingQueue = xQueueCreate(16, sizeof(Reading));
  i2cMutex     = xSemaphoreCreateMutex();
  dataMutex    = xSemaphoreCreateMutex();

  xTaskCreatePinnedToCore(NetTask,    "net",     8192, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(SensorTask, "sensor",  4096, nullptr, 3, nullptr, 1);
  if (hasOled) {
    xTaskCreatePinnedToCore(DisplayTask, "display", 4096, nullptr, 1, nullptr, 1);
  }
}

void loop() {
  vTaskDelete(nullptr);   // everything runs in tasks
}
