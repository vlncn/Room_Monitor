/*
  ESP32 + BME280 + 0.96" OLED: low-power logger with hourly MQTT upload

  Behaviour
    - Reads the BME280 every 1 s and keeps only the 10 s average (temperature, humidity).
    - Each 10 s average is appended to a CSV buffer file in LittleFS (ts,temp,hum).
    - Once an hour the buffered records are published to MQTT, one JSON message per
      record, so the existing mqtt_to_postgres.py subscriber works unchanged.
    - MQTT/WiFi is used ONLY while external (mains) power is present. On battery the
      board just samples and stores; the backlog is sent when mains power returns.
    - On battery: OLED off, CPU at 80 MHz, ESP32 in light sleep between samples.
    - On mains: OLED shows live values, buffer size and time to next upload.

  Why light sleep and not deep sleep
    Waking from deep sleep reboots the chip (~100-300 ms at full current). Doing that
    once per second costs more energy than light sleep, which keeps RAM and the clock
    and wakes in a few ms. Deep sleep only pays off for wake intervals of many seconds.

  Single-threaded on purpose: manual light sleep and several RTOS tasks do not mix.

  Libraries: Adafruit BME280 (+ Unified Sensor), Adafruit SSD1306 (+ GFX), PubSubClient

  Wiring
    I2C (both devices): VIN->3V3, GND->GND, SDA->GPIO21, SCL->GPIO22
    Power sense: external supply (adapter side, BEFORE any battery/UPS module) ->
                 10k -> GPIO34 -> 15k -> GND   (gives about 3.0 V when 5 V is present)
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
#include <esp_sleep.h>
#include <esp_timer.h>
#include <esp_sntp.h>

// ---------- CONFIG ----------
const char* WIFI_SSID   = "YOUR_WIFI";
const char* WIFI_PASS   = "YOUR_PASSWORD";

const char* MQTT_HOST   = "192.168.1.50";   // LAN IP of the broker PC
const uint16_t MQTT_PORT = 1883;
const char* MQTT_USER   = "esp32";
const char* MQTT_PASS   = "yourpassword";
const char* DEVICE_ID   = "esp32-room-01";
const char* MQTT_TOPIC  = "suhu/esp32-room-01/data";

const int     POWER_SENSE_PIN   = 34;       // -1 = always treat as mains
const bool    MAINS_ACTIVE_HIGH = true;     // pin HIGH when external power is present

const uint8_t BME_ADDR  = 0x76;
const uint8_t OLED_ADDR = 0x3C;

const int      SAMPLES_PER_RECORD = 10;     // 1 s samples per averaged record
const int      MIN_VALID_SAMPLES  = 6;      // below this the record is skipped
const uint32_t SEND_INTERVAL_S    = 3600;   // hourly upload
const uint32_t RETRY_INTERVAL_S   = 300;    // wait after a failed upload attempt
const uint32_t MAINS_STABLE_S     = 5;      // mains must be present this long before WiFi starts
const size_t   MAX_BUFFER_BYTES   = 900 * 1024;   // about 5 days of 10 s records
const char*    BUFFER_FILE = "/buffer.csv";
const char*    TEMP_FILE   = "/buffer.tmp";
const char*    TZ_WITA     = "<+08>-8";     // display only; stored timestamps are UTC
// -----------------------------

#define SCREEN_W 128
#define SCREEN_H 64

const int64_t TICK_US          = 1000000LL;
const int64_t SEND_INTERVAL_US = (int64_t)SEND_INTERVAL_S * 1000000LL;
const int64_t RETRY_US         = (int64_t)RETRY_INTERVAL_S * 1000000LL;
const int64_t MAINS_STABLE_US  = (int64_t)MAINS_STABLE_S * 1000000LL;

Adafruit_BME280  bme;
Adafruit_SSD1306 oled(SCREEN_W, SCREEN_H, &Wire, -1);
WiFiClient       wifiClient;
PubSubClient     mqtt(wifiClient);

// averaging window
float sumT = 0, sumH = 0;
int   sampleCount = 0, validCount = 0;

// latest values (for the display)
float lastT = NAN, lastH = NAN;

// state
int     bufferedCount = 0;
bool    oledOk = false;
bool    displayOn = true;
int64_t nextTickUs = 0;
int64_t lastSendOkUs = -1;          // -1 = never uploaded since boot
int64_t lastAttemptUs = 0;
int64_t mainsSinceUs = -1;

// ======================= Power / clock helpers =======================

bool mainsPresent() {
  if (POWER_SENSE_PIN < 0) return true;
  bool level = digitalRead(POWER_SENSE_PIN);
  return MAINS_ACTIVE_HIGH ? level : !level;
}

bool timeSynced() { return time(nullptr) > 1700000000; }

void setDisplayPower(bool on) {
  if (oledOk) oled.ssd1306_command(on ? SSD1306_DISPLAYON : SSD1306_DISPLAYOFF);
  displayOn = on;
}

// Wait until targetUs. On battery: light sleep (RAM and clock keep running).
void waitUntil(int64_t targetUs, bool lightSleep) {
  int64_t remain = targetUs - esp_timer_get_time();
  if (remain <= 0) return;
  if (lightSleep) {
    Serial.flush();
    esp_sleep_enable_timer_wakeup((uint64_t)remain);
    esp_light_sleep_start();
  } else {
    delay((uint32_t)(remain / 1000));
  }
}

// ======================= Buffer file =======================

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

void appendRecord(time_t ts, float t, float h) {
  File f = LittleFS.open(BUFFER_FILE, FILE_APPEND);
  if (!f) { Serial.println("Buffer open failed"); return; }
  if (f.size() > MAX_BUFFER_BYTES) {
    f.close();
    Serial.println("Buffer full: record dropped");
    return;
  }
  f.printf("%lld,%.2f,%.2f\n", (long long)ts, t, h);
  f.close();
  bufferedCount++;
}

// Publish buffered records oldest-first. Returns true if the buffer is empty afterwards.
// Stops (and keeps the rest) if MQTT drops or mains power disappears.
bool flushBuffer() {
  if (!LittleFS.exists(BUFFER_FILE)) { bufferedCount = 0; return true; }

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

    if (!failed && mqtt.connected() && mainsPresent() && mqtt.publish(MQTT_TOPIC, payload)) {
      sent++;
      mqtt.loop();
      delay(15);                      // don't flood the broker
    } else {
      failed = true;
      out.println(line);              // keep unsent lines
      remaining++;
    }
  }
  in.close();
  out.close();

  LittleFS.remove(BUFFER_FILE);
  if (failed) LittleFS.rename(TEMP_FILE, BUFFER_FILE);
  else        LittleFS.remove(TEMP_FILE);

  bufferedCount = remaining;
  Serial.printf("Uploaded %d records, %d left\n", sent, remaining);
  return !failed;
}

// ======================= Network (mains only) =======================

bool connectWiFi(uint32_t timeoutMs) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) delay(200);
  return WiFi.status() == WL_CONNECTED;
}

void stopNetwork() {
  if (mqtt.connected()) mqtt.disconnect();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);               // radio fully off so light sleep can work
}

// Fresh NTP request; returns true when the system clock was just updated.
bool syncClock(uint32_t timeoutMs) {
  sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
  configTzTime(TZ_WITA, "pool.ntp.org", "time.google.com");
  uint32_t t0 = millis();
  while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && millis() - t0 < timeoutMs) delay(200);
  return sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED;
}

void drawMessage(const char* msg) {
  if (!oledOk || !displayOn) return;
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(2);
  oled.setCursor(0, 24);
  oled.print(msg);
  oled.display();
}

bool sendDue() {
  int64_t now = esp_timer_get_time();
  bool intervalPassed = (lastSendOkUs < 0) || (now - lastSendOkUs >= SEND_INTERVAL_US);
  bool retryAllowed   = (now - lastAttemptUs >= RETRY_US);
  return intervalPassed && retryAllowed;
}

// One upload cycle: WiFi -> NTP -> MQTT -> flush -> everything off again.
// Sampling pauses while this runs (typically 10-30 s, once an hour).
void doSync() {
  lastAttemptUs = esp_timer_get_time();
  drawMessage("Syncing");
  Serial.println("Upload cycle started");

  bool ok = false;
  if (connectWiFi(15000)) {
    bool clockOk = syncClock(8000) || timeSynced();

    for (int i = 0; i < 3 && !mqtt.connected(); i++) {
      if (!mqtt.connect(DEVICE_ID, MQTT_USER, MQTT_PASS)) delay(500);
    }
    if (mqtt.connected()) ok = flushBuffer() && clockOk;
    else Serial.println("MQTT connect failed");
  } else {
    Serial.println("WiFi connect failed");
  }
  stopNetwork();

  if (ok) lastSendOkUs = esp_timer_get_time();
  else    Serial.printf("Upload not completed, retry in %lu s\n", (unsigned long)RETRY_INTERVAL_S);

  // restart the averaging window so it isn't polluted by the pause
  sumT = sumH = 0; sampleCount = validCount = 0;
}

// ======================= Sampling =======================

void sampleOnce() {
  if (!bme.takeForcedMeasurement()) return;     // sensor sleeps between forced reads
  float t = bme.readTemperature();
  float h = bme.readHumidity();
  if (isnan(t) || isnan(h)) return;
  sumT += t; sumH += h; validCount++;
  lastT = t; lastH = h;
}

void finishWindow() {
  if (validCount >= MIN_VALID_SAMPLES) {
    float avgT = sumT / validCount;
    float avgH = sumH / validCount;
    if (timeSynced()) {
      appendRecord(time(nullptr), avgT, avgH);
      Serial.printf("avg T=%.2f H=%.2f  (buffered %d)\n", avgT, avgH, bufferedCount);
    } else {
      Serial.println("Clock not synced yet (needs mains + WiFi once): record skipped");
    }
  } else {
    Serial.println("Too few valid samples: record skipped");
  }
  sumT = sumH = 0; sampleCount = validCount = 0;
}

// ======================= OLED (mains only) =======================

void drawScreen() {
  if (!oledOk || !displayOn) return;

  char clk[8] = "--:--";
  struct tm tmv;
  if (timeSynced() && getLocalTime(&tmv, 0)) strftime(clk, sizeof(clk), "%H:%M", &tmv);

  int nextMin = 0;
  if (lastSendOkUs >= 0) {
    int64_t rem = SEND_INTERVAL_US - (esp_timer_get_time() - lastSendOkUs);
    nextMin = rem > 0 ? (int)((rem + 59999999LL) / 60000000LL) : 0;
  }

  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.printf("MAINS  %s", clk);
  oled.drawFastHLine(0, 10, SCREEN_W, SSD1306_WHITE);

  oled.setTextSize(3);
  oled.setCursor(0, 15);
  if (!isnan(lastT)) oled.printf("%.1fC", lastT); else oled.print("--.-C");

  oled.setTextSize(1);
  oled.setCursor(0, 44);
  if (!isnan(lastH)) oled.printf("Hum %.0f%%", lastH); else oled.print("Hum --");
  oled.setCursor(0, 55);
  oled.printf("Buf:%d  Next:%dm", bufferedCount, nextMin);

  oled.display();
}

// ======================= Arduino entry points =======================

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
  bufferedCount = countBufferLines();

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

  WiFi.mode(WIFI_OFF);

  lastAttemptUs = esp_timer_get_time() - RETRY_US;   // first upload allowed immediately
  nextTickUs = esp_timer_get_time() + TICK_US;
  Serial.printf("Started. Buffered records from earlier: %d\n", bufferedCount);
}

void loop() {
  int64_t now = esp_timer_get_time();

  // --- power source ---
  bool mains = mainsPresent();
  if (mains) { if (mainsSinceUs < 0) mainsSinceUs = now; }
  else       { mainsSinceUs = -1; }
  bool mainsStable = mains && (now - mainsSinceUs >= MAINS_STABLE_US);
  if (mains != displayOn) setDisplayPower(mains);

  // --- 1 s sample, 10 s average ---
  sampleOnce();
  if (++sampleCount >= SAMPLES_PER_RECORD) finishWindow();

  // --- mains-only work ---
  if (mains) drawScreen();
  if (mainsStable && sendDue()) doSync();

  // --- wait for the next 1 s tick ---
  nextTickUs += TICK_US;
  int64_t after = esp_timer_get_time();
  if (nextTickUs <= after) nextTickUs = after + TICK_US;   // fell behind (e.g. after a sync)
  waitUntil(nextTickUs, !mains);                           // light sleep on battery
}