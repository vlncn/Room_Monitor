/*
  A
*/
#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Preferences.h>
#include <time.h>
#include "config.h"
#include <LittleFS.h>

// ---------- CONFIG ----------
const int      SDA_PIN    = 21;
const int      SCL_PIN    = 22;
const uint8_t  BME_ADDR   = 0x76;   // try 0x77 if not found
const uint8_t  OLED_ADDR  = 0x3C;   // try 0x3D if not found
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 8 * 3600;
const int   daylightOffset_sec = 0;
const char* LOG_FILE       = "/records.csv";
const char* CURSOR_FILE    = "/cursor.dat";
// -----------------------------

#define SCREEN_W 128
#define SCREEN_H 64
#define JSON_BUFFER_SIZE 128

WiFiClient espClient;
PubSubClient client(espClient);

Adafruit_BME280  bme;
Adafruit_SSD1306 oled(SCREEN_W, SCREEN_H, &Wire, -1);
//Preferences preferences;

QueueHandle_t jsonQueue; // Creating Queue to not mismatch between timestamp and sensor reading
struct ReadingResult {
    float temp = 0;
    float hum = 0;
    String time_measure = "";
};

bool sensorOk = false;
ReadingResult latestread; //Creating latest read Struct to display in OLED
uint32_t sequenceNumber = 0;

String GetTime(){ // Getting Timestamp from NTP Server
    time_t now = time(nullptr);

    struct tm utcInfo;
    gmtime_r(&now, &utcInfo);

    char timestamp[25];
    strftime(
    timestamp,
    sizeof(timestamp),
    "%Y-%m-%dT%H:%M:%SZ",
    &utcInfo);

    return String(timestamp);
};

void initSequenceNumber() {
    preferences.begin("room-monitor", false);

    // Read the last saved sequence; use 0 if it doesn't exist.
    sequenceNumber = preferences.getUInt("sequence", 0);
}

uint32_t getNextSequenceNumber() {
    sequenceNumber++;

    // Persist the new value in flash.
    preferences.putUInt("sequence", sequenceNumber);

    return sequenceNumber;
}

void SavetoLittleFS(struct ReadingResult data){
    File file = LittleFS.open(LOG_FILE, FILE_APPEND);
    if (!file) return;

    size_t written = file.printf("%d,%.2f,%.2f,%s\n", 
        sequenceNumber, data.temp, data.hum, data.time_measure.c_str());
    file.flush();
    file.close();
    return;
}

void datatoJSon(struct ReadingResult data){ //struct ReadingResult
    uint32_t seq = sequenceNumber++;
    char localBuffer[JSON_BUFFER_SIZE];

    snprintf(localBuffer, sizeof(localBuffer),
           "{\"message_id\":%d,\"device_id\":\"ESP32_01_ROOM_LEVEL_1\",\"temp\":%.2f,\"hum\":%.2f,\"measure_at\":\"%s\"}",
           seq, data.temp, data.hum, data.time_measure.c_str());

    if (xQueueSend(jsonQueue, &localBuffer, 0) != pdPASS) SavetoLittleFS(data);
    return;
}

/*                             TASK                       */
void NetworkTask(void *Parameters){
    /*
    TODO :
    Check Connection to Wi-FI
    Connect to MQTT
    Handle Error
    */
    TickType_t xLastWakeTime = xTaskGetTickCount();
    char txBuffer[JSON_BUFFER_SIZE];
    while(true){
        while (!client.connected()) { // Stuck on Connecting to MQTT should give a way to make room for unsend data
            if (client.connect("ESP32_Task_Client")) { break; }
            delay(3000);
            Serial.println("Connecting to MQTT");
            Serial.printf("MQTT failed, state = %d\n", client.state());
        }
        client.loop(); // Keep MQTT alive

        if (xQueueReceive(jsonQueue, &txBuffer, portMAX_DELAY) == pdTRUE) {
            Serial.print("[Task MQTT] Uploading payload: ");
            Serial.println(txBuffer);

            if (client.publish("ROOM_LEVEL_1", txBuffer)) {
                Serial.println("[Task MQTT] Publish Success");
            } else {
                Serial.println("[Task MQTT] Publish Failed");
                }
            }
    }
}

void DisplayTask(void *Parameters) {
  TickType_t xLastWakeTime = xTaskGetTickCount();
  while (true){
    ReadingResult snap;
    snap = latestread;
    // Serial.printf("From Display Task, Temp :%.2fC, Hum: %.2f\n", snap.temp, snap.hum);
    oled.clearDisplay();
    oled.setTextColor(SSD1306_WHITE);

    oled.setTextSize(1);
    oled.setCursor(0, 0);
    oled.print("Room Monitor");
    oled.drawFastHLine(0, 10, SCREEN_W, SSD1306_WHITE);

    if (sensorOk) {
        oled.setTextSize(3);
        oled.setCursor(0, 16);
        oled.printf("%.1fC", snap.temp);

        oled.setTextSize(1);
        oled.setCursor(0, 46);
        oled.printf("Humidity: %.1f %%", snap.hum);
    } else {
        oled.setTextSize(2);
        oled.setCursor(0, 24);
        oled.print("SENSOR");
        oled.setCursor(0, 42);
        oled.print("ERROR");
    }
    // ADD ERROR HANDLE ON SENSOR OR MQTT
    oled.display();
    vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(500));
  }
}

void SensorTask(void *Parameters) {
    TickType_t xLastWakeTime = xTaskGetTickCount();
    while(true){
        String timestamp = GetTime();
        float t = bme.readTemperature();
        float h = bme.readHumidity();
        latestread.temp = t; latestread.hum = h; latestread.time_measure = timestamp;
        // Serial.printf("From Sensor Task, Temp :%.2fC, Hum: %.2f\n", latestread.temp, latestread.hum);
        sensorOk = true;
        datatoJSon(latestread);
        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(5000));
    }
}

void setup() {
  // ESP32 STARTUP
  Serial.begin(115200);
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);
  //initSequenceNumber();
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
    }

  // SENSOR STARTUP
  // BME STARTUP
  if (!bme.begin(BME_ADDR)) {
    Serial.println("BME280 not found. Check wiring and address (0x76 / 0x77).");
    while (true) delay(1000);
  }
  // 1x oversampling, 1 s standby: low self-heating for room measurements
  bme.setSampling(Adafruit_BME280::MODE_NORMAL,
                  Adafruit_BME280::SAMPLING_X1,
                  Adafruit_BME280::SAMPLING_NONE,
                  Adafruit_BME280::SAMPLING_X1,
                  Adafruit_BME280::FILTER_OFF,
                  Adafruit_BME280::STANDBY_MS_1000);

  if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED not found. Check wiring and address (0x3C / 0x3D).");
    while (true) delay(1000);
  }

  // NETWORK STARTUP
  // Wi-Fi
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println(WiFi.localIP());
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  // MQTT
  client.setServer(MQTT_SERVER, 1883);
  
  
  // OLED SETUP
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(0, 28);
  oled.print("Starting...");
  oled.display();

  // RTOS STUFF
  jsonQueue = xQueueCreate(5, JSON_BUFFER_SIZE);
  if (jsonQueue == NULL) {
    Serial.println("Error creating the FreeRTOS Queue");
    return;
  }
  //xTaskCreatePinnedToCore(NetworkTask, "network",  4096, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(SensorTask, "sensor",  4096, nullptr, 3, nullptr, 1);
  xTaskCreatePinnedToCore(DisplayTask, "display", 4096, nullptr, 1, nullptr, 1);
}

void loop() {
  vTaskDelete(nullptr);   // all work runs in tasks
}
