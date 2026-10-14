/*
  ESP32 + BME280 + 0.96" OLED: low-power logger with hourly MQTT upload (FreeRTOS + ISR)

  Behaviour
    - Reads the BME280 every 1 s and keeps only the 10 s average (temperature, humidity).
    - Each 10 s average is stored in a CSV buffer file in LittleFS (ts,temp,hum).
    - Once an hour the buffered records are published to MQTT, one JSON message per
      record, so the existing mqtt_to_postgres.py subscriber works unchanged.
    - WiFi/MQTT is used ONLY while external (mains) power is present, and the radio is
      OFF the rest of the time.
    - Mains presence is detected by a GPIO interrupt (no polling). When power is lost,
      NetTask aborts any upload, turns the radio off and parks until power is back.
    - Sampling and storing never stop, on battery or on mains.
    - On battery: OLED off, CPU at 80 MHz. On mains: OLED shows live values.

  Tasks
    SensorTask  (core 1, prio 3) : 1 s sampling, 10 s average -> record queue
    StorageTask (core 1, prio 2) : writes queued records to the buffer file (always runs)
    PowerTask   (core 1, prio 4) : sleeps until the mains-sense interrupt fires, debounces,
                                   publishes the power state and wakes/parks NetTask
    NetTask     (core 0, prio 2) : hourly upload (WiFi, NTP, MQTT). Parked on battery
    DisplayTask (core 1, prio 1) : OLED on/off and redraw

  Sharing rules
    - i2cMutex   : BME280 and OLED share one I2C bus
    - dataMutex  : latest reading shown on the display
    - fileMutex  : the buffer file (StorageTask appends, NetTask uploads/rewrites)
    - recordQueue: SensorTask -> StorageTask (~42 min of records if the file is busy)
    - task notifications: ISR -> PowerTask, PowerTask -> NetTask

  Libraries: Adafruit BME280 (+ Unified Sensor), Adafruit SSD1306 (+ GFX), PubSubClient

  Wiring
    I2C (both devices): VIN->3V3, GND->GND, SDA->GPIO21, SCL->GPIO22
    Power sense: external supply (adapter side, BEFORE any battery/UPS module) ->
                 10k -> GPIO34 -> 15k -> GND   (gives about 3.0 V when 5 V is present)
                 Add 100 nF from GPIO34 to GND to filter noise on the interrupt line.
    Set POWER_SENSE_PIN = -1 to treat the board as always on mains (bench testing).
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
#include <esp_timer.h>
#include <esp_sntp.h>

// ---------- CONFIG ----------
const char* WIFI_SSID   = "";
const char* WIFI_PASS   = "";

const char* MQTT_HOST   = "";   // LAN IP of the broker PC
const uint16_t MQTT_PORT = 1883;
const char* MQTT_USER   = "";
const char* MQTT_PASS   = "";
const char* DEVICE_ID   = "";
const char* MQTT_TOPIC  = "";

const int     POWER_SENSE_PIN   = 34;       // -1 = always treat as mains (needs interrupt-capable pin)
const bool    MAINS_ACTIVE_HIGH = true;     // pin HIGH when external power is present
const uint32_t DEBOUNCE_MS      = 30;       // let the edge settle before reading the pin

const uint8_t BME_ADDR  = 0x76;
const uint8_t OLED_ADDR = 0x3C;

const int      SAMPLES_PER_RECORD = 10;     // 1 s samples per averaged record
const int      MIN_VALID_SAMPLES  = 6;      // below this the record is skipped
const uint32_t SEND_INTERVAL_S    = 3600;   // hourly upload
const uint32_t RETRY_INTERVAL_S   = 300;    // wait after a failed upload attempt
const uint32_t MAINS_STABLE_S     = 5;      // mains must stay present this long before WiFi starts
const size_t   MAX_BUFFER_BYTES   = 900 * 1024;   // about 4.5 days of 10 s records
const int      QUEUE_LEN          = 256;    // records waiting to be written to flash
const char*    BUFFER_FILE = "/buffer.csv";
const char*    TEMP_FILE   = "/buffer.tmp";
const char*    TZ_WITA     = "<+08>-8";     // display only; stored timestamps are UTC
// -----------------------------

#define SCREEN_W 128
#define SCREEN_H 64

const int64_t SEND_INTERVAL_US = (int64_t)SEND_INTERVAL_S * 1000000LL;
const int64_t RETRY_US         = (int64_t)RETRY_INTERVAL_S * 1000000LL;

struct Record {
  time_t ts;
  float  temp;
  float  hum;
};

struct Latest {
  float temp = NAN, hum = NAN;
  bool  valid = false;
};

Adafruit_BME280  bme;
Adafruit_SSD1306 oled(SCREEN_W, SCREEN_H, &Wire, -1);
WiFiClient       wifiClient;
PubSubClient     mqtt(wifiClient);

QueueHandle_t     recordQueue;
SemaphoreHandle_t i2cMutex;
SemaphoreHandle_t dataMutex;
SemaphoreHandle_t fileMutex;
TaskHandle_t      netTaskHandle   = nullptr;
TaskHandle_t      powerTaskHandle = nullptr;

Latest            latest;                       // guarded by dataMutex
volatile bool     g_mains = false;              // PowerTask -> everyone (instant)
volatile bool     g_mainsStable = false;        // PowerTask -> NetTask (present for MAINS_STABLE_S)
volatile bool     g_syncing = false;            // NetTask -> DisplayTask
volatile int      g_buffered = 0;               // guarded by fileMutex
volatile bool     g_everSent = false;
volatile uint32_t g_lastSendOkS = 0;            // seconds since boot
bool              oledOk = false;

// ======================= Helpers =======================

bool readMainsPin() {
  if (POWER_SENSE_PIN < 0) return true;
  bool level = digitalRead(POWER_SENSE_PIN);
  return MAINS_ACTIVE_HIGH ? level : !level;
}

bool timeSynced() { return time(nullptr) > 1700000000; }

// Runs on every edge of the sense pin: just wake PowerTask, nothing else.
void IRAM_ATTR onPowerEdge() {
  BaseType_t woken = pdFALSE;
  vTaskNotifyGiveFromISR(powerTaskHandle, &woken);
  if (woken) portYIELD_FROM_ISR();
}

// ======================= Buffer file (hold fileMutex) =======================

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

void appendRecord(const Record& r) {
  File f = LittleFS.open(BUFFER_FILE, FILE_APPEND);
  if (!f) { Serial.println("Buffer open failed"); return; }
  if (f.size() > MAX_BUFFER_BYTES) {
    f.close();
    Serial.println("Buffer full: record dropped");
    return;
  }
  f.printf("%lld,%.2f,%.2f\n", (long long)r.ts, r.temp, r.hum);
  f.close();
  g_buffered++;
}

// Publish buffered records oldest-first. Returns true if the buffer is empty afterwards.
// Stops (and keeps the rest) if MQTT drops or mains power disappears.
bool flushBuffer() {
  if (!LittleFS.exists(BUFFER_FILE)) { g_buffered = 0; return true; }

  File in = LittleFS.open(BUFFER_FILE, FILE_READ);
  if (!in) return false;
  File out = LittleFS.open(TEMP_FILE, FILE_WRITE);
  if (!out) { in.close(); return false; }

  bool failed = false;
  int sent = 0, remaining = 0;
  while (in.available()) {
    String line = in.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    long long ts; float t, h;
    if (sscanf(line.c_str(), "%lld,%f,%f", &ts, &t, &h) != 3) continue;   // corrupt line: drop

    char payload[160];
    snprintf(payload, sizeof(payload),
             "{\"device\":\"%s\",\"ts\":%lld,\"temp\":%.2f,\"hum\":%.2f}",
             DEVICE_ID, ts, t, h);

    if (!failed && mqtt.connected() && g_mains && mqtt.publish(MQTT_TOPIC, payload)) {
      sent++;
      mqtt.loop();
      vTaskDelay(pdMS_TO_TICKS(15));   // don't flood the broker
    } else {
      failed = true;
      out.println(line);               // keep unsent lines
      remaining++;
    }
  }
  in.close();
  out.close();

  LittleFS.remove(BUFFER_FILE);
  if (failed) LittleFS.rename(TEMP_FILE, BUFFER_FILE);
  else        LittleFS.remove(TEMP_FILE);

  g_buffered = remaining;
  Serial.printf("Uploaded %d records, %d left\n", sent, remaining);
  return !failed;
}

// ======================= Network (NetTask only) =======================

bool connectWiFi(uint32_t timeoutMs) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && g_mains && millis() - t0 < timeoutMs)
    vTaskDelay(pdMS_TO_TICKS(200));
  return WiFi.status() == WL_CONNECTED;
}

void stopNetwork() {
  if (mqtt.connected()) mqtt.disconnect();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

// Fresh NTP request; returns true when the system clock was just updated.
bool syncClock(uint32_t timeoutMs) {
  sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
  configTzTime(TZ_WITA, "pool.ntp.org", "time.google.com");
  uint32_t t0 = millis();
  while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && g_mains && millis() - t0 < timeoutMs)
    vTaskDelay(pdMS_TO_TICKS(200));
  return sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED;
}

// One upload cycle: WiFi -> NTP -> MQTT -> flush -> everything off again.
// Every wait checks g_mains, so losing power aborts within ~200 ms and the radio goes off.
bool doSync() {
  g_syncing = true;
  Serial.println("Upload cycle started");
  bool ok = false;

  do {
    if (!connectWiFi(15000)) { Serial.println("WiFi connect failed"); break; }
    if (!g_mains) break;

    bool clockOk = syncClock(8000) || timeSynced();
    if (!g_mains) break;

    for (int i = 0; i < 3 && g_mains && !mqtt.connected(); i++) {
      if (!mqtt.connect(DEVICE_ID, MQTT_USER, MQTT_PASS)) vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (!g_mains) break;
    if (!mqtt.connected()) { Serial.println("MQTT connect failed"); break; }

    if (xSemaphoreTake(fileMutex, portMAX_DELAY) == pdTRUE) {
      ok = flushBuffer() && clockOk;
      xSemaphoreGive(fileMutex);
    }
  } while (false);

  stopNetwork();
  if (!g_mains)  Serial.println("Upload aborted: mains lost");
  else if (!ok)  Serial.printf("Upload not completed, retry in %lu s\n", (unsigned long)RETRY_INTERVAL_S);
  g_syncing = false;
  return ok;
}

// ======================= Tasks =======================

void SensorTask(void*) {
  TickType_t lastWake = xTaskGetTickCount();
  float sumT = 0, sumH = 0;
  int count = 0, valid = 0;

  for (;;) {
    float t = NAN, h = NAN;
    bool ok = false;

    if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
      if (bme.takeForcedMeasurement()) {          // sensor sleeps between forced reads
        t = bme.readTemperature();
        h = bme.readHumidity();
        ok = !isnan(t) && !isnan(h);
      }
      xSemaphoreGive(i2cMutex);
    }

    if (ok) {
      sumT += t; sumH += h; valid++;
      if (xSemaphoreTake(dataMutex, portMAX_DELAY) == pdTRUE) {
        latest.temp = t; latest.hum = h; latest.valid = true;
        xSemaphoreGive(dataMutex);
      }
    }

    if (++count >= SAMPLES_PER_RECORD) {
      if (valid >= MIN_VALID_SAMPLES) {
        if (timeSynced()) {
          Record r = { time(nullptr), sumT / valid, sumH / valid };
          if (xQueueSend(recordQueue, &r, 0) != pdTRUE) {   // queue full: drop oldest
            Record dropped;
            xQueueReceive(recordQueue, &dropped, 0);
            xQueueSend(recordQueue, &r, 0);
            Serial.println("Record queue full: oldest dropped");
          }
          Serial.printf("avg T=%.2f H=%.2f\n", r.temp, r.hum);
        } else {
          Serial.println("Clock not synced yet (needs mains + WiFi once): record skipped");
        }
      } else {
        Serial.println("Too few valid samples: record skipped");
      }
      sumT = sumH = 0; count = valid = 0;
    }

    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(1000));
  }
}

// Owns all appends to the buffer file. Blocks on the queue, so it costs nothing while idle.
// Keeps running on battery: this is what makes the blackout logging work.
void StorageTask(void*) {
  for (;;) {
    Record r;
    if (xQueueReceive(recordQueue, &r, portMAX_DELAY) == pdTRUE) {
      if (xSemaphoreTake(fileMutex, portMAX_DELAY) == pdTRUE) {   // waits while NetTask uploads
        appendRecord(r);
        xSemaphoreGive(fileMutex);
      }
    }
  }
}

// Event-driven: sleeps until the sense pin changes (no periodic reads).
void PowerTask(void*) {
  if (POWER_SENSE_PIN < 0) {                      // bench mode: no sense pin, always mains
    vTaskDelay(pdMS_TO_TICKS(MAINS_STABLE_S * 1000));
    g_mains = true;
    g_mainsStable = true;
    xTaskNotifyGive(netTaskHandle);
    vTaskDelete(nullptr);
  }

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));       // let contact bounce settle
    ulTaskNotifyTake(pdTRUE, 0);                  // swallow edges caused by the bounce

    if (!readMainsPin()) {
      g_mains = false;
      g_mainsStable = false;
      Serial.println("Mains lost: NetTask will abort and park");
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);    // sleep until the next edge
      continue;
    }

    g_mains = true;
    g_mainsStable = false;
    // Mains must stay present MAINS_STABLE_S with no further edge before NetTask is released
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(MAINS_STABLE_S * 1000)) == 0) {
      g_mainsStable = true;
      Serial.println("Mains stable: NetTask released");
      xTaskNotifyGive(netTaskHandle);             // wake NetTask if it is parked
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);    // sleep until the next edge
    }
  }
}

void NetTask(void*) {
  int64_t lastOkUs = -1;                 // -1 = never uploaded since boot
  int64_t lastAttemptUs = -RETRY_US;     // first upload allowed immediately

  for (;;) {
    // Park (blocked, no CPU) until mains is present and stable. The notification is
    // latched, so a wake-up that arrives before we get here is never lost.
    while (!g_mainsStable) ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    int64_t now = esp_timer_get_time();
    int64_t waitUs = 0;
    if (lastOkUs >= 0)                       waitUs = SEND_INTERVAL_US - (now - lastOkUs);
    if (now - lastAttemptUs < RETRY_US)      waitUs = max(waitUs, RETRY_US - (now - lastAttemptUs));

    if (waitUs > 0) {                        // not due yet: sleep until due (or until woken)
      uint32_t ms = (uint32_t)min<int64_t>(waitUs / 1000, 3600000);
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ms));
      continue;
    }

    lastAttemptUs = esp_timer_get_time();
    if (doSync()) {
      lastOkUs = esp_timer_get_time();
      g_lastSendOkS = (uint32_t)(lastOkUs / 1000000LL);
      g_everSent = true;
    } else if (!g_mains) {
      lastAttemptUs = -RETRY_US;             // aborted by power loss: retry as soon as power is back
    }
  }
}

void DisplayTask(void*) {
  TickType_t lastWake = xTaskGetTickCount();
  bool displayOn = true;

  for (;;) {
    bool mains = g_mains;

    Latest snap;
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      snap = latest;
      xSemaphoreGive(dataMutex);
    }

    char clk[8] = "--:--";
    struct tm tmv;
    if (mains && timeSynced() && getLocalTime(&tmv, 0)) strftime(clk, sizeof(clk), "%H:%M", &tmv);

    int nextMin = 0;
    if (g_everSent) {
      int32_t elapsed = (int32_t)((uint32_t)(esp_timer_get_time() / 1000000LL) - g_lastSendOkS);
      int32_t rem = (int32_t)SEND_INTERVAL_S - elapsed;
      nextMin = rem > 0 ? (rem + 59) / 60 : 0;
    }

    if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
      if (mains != displayOn) {                       // battery: OLED off, mains: on
        oled.ssd1306_command(mains ? SSD1306_DISPLAYON : SSD1306_DISPLAYOFF);
        displayOn = mains;
      }

      if (displayOn) {
        oled.clearDisplay();
        oled.setTextColor(SSD1306_WHITE);

        if (g_syncing) {
          oled.setTextSize(2);
          oled.setCursor(0, 24);
          oled.print("Syncing");
        } else {
          oled.setTextSize(1);
          oled.setCursor(0, 0);
          oled.printf("MAINS  %s", clk);
          oled.drawFastHLine(0, 10, SCREEN_W, SSD1306_WHITE);

          oled.setTextSize(3);
          oled.setCursor(0, 15);
          if (snap.valid) oled.printf("%.1fC", snap.temp); else oled.print("--.-C");

          oled.setTextSize(1);
          oled.setCursor(0, 44);
          if (snap.valid) oled.printf("Hum %.0f%%", snap.hum); else oled.print("Hum --");
          oled.setCursor(0, 55);
          oled.printf("Buf:%d  Next:%dm", (int)g_buffered, nextMin);
        }
        oled.display();
      }
      xSemaphoreGive(i2cMutex);
    }

    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(1000));
  }
}

// ======================= Setup / loop =======================

void setup() {
  Serial.begin(115200);
  setCpuFrequencyMhz(80);                       // lowest speed WiFi still works at
  if (POWER_SENSE_PIN >= 0) pinMode(POWER_SENSE_PIN, INPUT);

  Wire.begin(21, 22);
  Wire.setClock(400000);

  if (!LittleFS.begin(true)) Serial.println("LittleFS mount failed");
  // Recover from power loss during a buffer rewrite
  if (!LittleFS.exists(BUFFER_FILE) && LittleFS.exists(TEMP_FILE)) LittleFS.rename(TEMP_FILE, BUFFER_FILE);
  else if (LittleFS.exists(TEMP_FILE)) LittleFS.remove(TEMP_FILE);
  g_buffered = countBufferLines();

  if (!bme.begin(BME_ADDR)) {
    Serial.println("BME280 not found. Check wiring / address (0x76 or 0x77).");
    while (true) delay(1000);
  }
  // Forced mode: sensor sleeps between our reads. 1x temp/hum, pressure off.
  bme.setSampling(Adafruit_BME280::MODE_FORCED,
                  Adafruit_BME280::SAMPLING_X1,     // temperature
                  Adafruit_BME280::SAMPLING_NONE,   // pressure (not used)
                  Adafruit_BME280::SAMPLING_X1,     // humidity
                  Adafruit_BME280::FILTER_OFF);

  oledOk = oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  if (!oledOk) Serial.println("OLED not found, continuing without display.");

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(256);
  mqtt.setSocketTimeout(3);

  WiFi.mode(WIFI_OFF);                          // radio stays off until the hourly upload

  recordQueue = xQueueCreate(QUEUE_LEN, sizeof(Record));
  i2cMutex    = xSemaphoreCreateMutex();
  dataMutex   = xSemaphoreCreateMutex();
  fileMutex   = xSemaphoreCreateMutex();

  Serial.printf("Started. Buffered records from earlier: %d\n", (int)g_buffered);

  xTaskCreatePinnedToCore(StorageTask, "storage", 4096, nullptr, 2, nullptr, 1);
  xTaskCreatePinnedToCore(SensorTask,  "sensor",  4096, nullptr, 3, nullptr, 1);
  xTaskCreatePinnedToCore(NetTask,     "net",     8192, nullptr, 2, &netTaskHandle, 0);
  xTaskCreatePinnedToCore(PowerTask,   "power",   3072, nullptr, 4, &powerTaskHandle, 1);
  if (oledOk) xTaskCreatePinnedToCore(DisplayTask, "display", 4096, nullptr, 1, nullptr, 1);

  if (POWER_SENSE_PIN >= 0) {
    attachInterrupt(digitalPinToInterrupt(POWER_SENSE_PIN), onPowerEdge, CHANGE);
    xTaskNotifyGive(powerTaskHandle);           // evaluate the current level once, covers any missed edge
  }
}

void loop() {
  vTaskDelete(nullptr);   // all work runs in tasks
}