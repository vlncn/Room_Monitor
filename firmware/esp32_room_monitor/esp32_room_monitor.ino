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
const char* TEMP_FILE      = "/records.temp";
constexpr uint32_t MEASURE_INTERVAL_MS = 10UL * 60UL * 1000UL; // 10 minutes
// -----------------------------

#define SCREEN_W 128
#define SCREEN_H 64
#define JSON_BUFFER_SIZE 192

WiFiClient espClient;
PubSubClient client(espClient);

Adafruit_BME280  bme;
Adafruit_SSD1306 oled(SCREEN_W, SCREEN_H, &Wire, -1);
Preferences preferences;

QueueHandle_t jsonQueue; // Creating Queue to not mismatch between timestamp and sensor reading
QueueHandle_t jsonQueueBacklog; // To upload old data that doesn't get sent

// Task handles (needed for task notifications)
TaskHandle_t schedulerTaskHandle = nullptr;
TaskHandle_t sensorTaskHandle    = nullptr;
TaskHandle_t displayTaskHandle   = nullptr;
TaskHandle_t networkTaskHandle   = nullptr;
TaskHandle_t oldDataTaskHandle   = nullptr;

struct ReadingResult {
    float temp = 0;
    float hum = 0;
    char time_measure[25] = "";
};

bool sensorOk = false;
bool connectedtoMQTT = false;
size_t retryCount = 0;
ReadingResult latestread; //Creating latest read Struct to display in OLED
uint32_t sequenceNumber = 0;

void GetTime(char *out, size_t len){ // Getting Timestamp from NTP Server
    time_t now = time(nullptr);
    struct tm utcInfo;
    gmtime_r(&now, &utcInfo);
    strftime(
    out,
    len,
    "%Y-%m-%dT%H:%M:%SZ",
    &utcInfo);
    return;
};

void WaitForTimeSync(uint32_t timeoutMs) {
    // time() returns ~0 (1970) until NTP has synced
    uint32_t start = millis();
    while (time(nullptr) < 1700000000) {
        if (millis() - start > timeoutMs) return;
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    return;
}

bool CheckConnection() {
    Serial.println("Check Connection");
    if (WiFi.status() != WL_CONNECTED) {WiFi.reconnect(); return false;}
    if (!client.connected())  {client.connect("ESP32_01_ROOM_LEVEL_1");return false;}
    return true;
}

void initSequenceNumber() {
    preferences.begin("room-monitor", false);
    sequenceNumber = preferences.getUInt("sequence", 0);
}

uint32_t getNextSequenceNumber() {
    sequenceNumber++;
    preferences.putUInt("sequence", sequenceNumber);
    return sequenceNumber;
}

void SavetoLittleFS(char JSON[JSON_BUFFER_SIZE]){
    File file = LittleFS.open(LOG_FILE, FILE_APPEND);
    if (!file) return;
    file.println(JSON);
    file.close();
    if (connectedtoMQTT) xTaskNotifyGive(oldDataTaskHandle);
    return;
}
void ReadFromLittleFS(){
    File file = LittleFS.open(LOG_FILE, FILE_READ);
    File out = LittleFS.open(TEMP_FILE, FILE_WRITE);
    bool failed = false;
    if (!file) return;
    if (!out) { file.close(); return; }
    if (!file.available()) return;

    char jsonBuffer[JSON_BUFFER_SIZE];
    while(file.available()){
        size_t len = file.readBytesUntil(
            '\n',
            jsonBuffer,
            sizeof(jsonBuffer) - 1
        );

        jsonBuffer[len] = '\0';

        // Skip empty lines
        if (len == 0) continue;

        if (!client.publish("ROOM_LEVEL_1/backlog/data", jsonBuffer)){out.println(jsonBuffer);failed = true;}
        vTaskDelay(pdMS_TO_TICKS(15)); // Change this to make sure the data in SQL is saved then resume upload old data

    }
    file.close();
    out.close();
    LittleFS.remove(LOG_FILE);
    if (failed) LittleFS.rename(TEMP_FILE, LOG_FILE);
    else        LittleFS.remove(TEMP_FILE);

    return;
}


void datatoJSon(struct ReadingResult data){ // Convert data into JSON FILE and queue the data before sending it to mqtt broker
    uint32_t seq = sequenceNumber++;
    char localBuffer[JSON_BUFFER_SIZE];

    snprintf(localBuffer, sizeof(localBuffer),
           "{\"message_id\":%lu,\"device_id\":\"ESP32_01_ROOM_LEVEL_1\",\"temp\":%.2f,\"hum\":%.2f,\"measure_at\":\"%s\"}",
           seq, data.temp, data.hum, data.time_measure);

    if (xQueueSend(jsonQueue, &localBuffer, 0) != pdPASS) SavetoLittleFS(localBuffer);
    return;
}

/*                             TASK                       */
void SchedulerTask(void *Parameters) {
    TickType_t xLastWakeTime = xTaskGetTickCount();
    while (true) {
        xTaskNotifyGive(sensorTaskHandle);   // first measurement happens immediately at boot
        xTaskNotifyGive(networkTaskHandle);
        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(MEASURE_INTERVAL_MS));
    }
}

void UpOldDataTask(void *Paramaters){
    char txBuffer[JSON_BUFFER_SIZE];
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (retryCount < 3 && !connectedtoMQTT) { 
            connectedtoMQTT = CheckConnection();
            retryCount++;
            if (connectedtoMQTT) break;
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
        client.loop();
        if (uxQueueSpacesAvailable(jsonQueueBacklog) != 0) ReadFromLittleFS();
        while (uxQueueSpacesAvailable(jsonQueueBacklog) != 0){ // Currently Has no confirmation from database side if the data is already being stored or not
            if (xQueueReceive(jsonQueueBacklog, &txBuffer, 0) == pdTRUE && connectedtoMQTT) client.publish("ROOM_LEVEL_1/backlog", txBuffer);
        }
        xTaskNotifyGive(networkTaskHandle);
    }
}

void NetworkTask(void *Parameters){
    char txBuffer[JSON_BUFFER_SIZE];
    while(true){
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (retryCount < 3 && !connectedtoMQTT) { // Need to check if bugs exist like cannot reset the if statement from another if statement
            if (connectedtoMQTT = CheckConnection()) break;
            retryCount++;
            Serial.print(retryCount);
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
        client.loop();

        if (xQueueReceive(jsonQueue, &txBuffer, pdMS_TO_TICKS(5000)) == pdTRUE && connectedtoMQTT) {
            if (client.publish("ROOM_LEVEL_1/now/data", txBuffer)) {
                retryCount = 0;
            } else {
                SavetoLittleFS(txBuffer);
                }
            } 
        else {
            SavetoLittleFS(txBuffer); 
            retryCount = 0;
        }
        if (!LittleFS.exists(LOG_FILE)) xTaskNotifyGive(oldDataTaskHandle);
    }
}

void DisplayTask(void *Parameters) {
  while (true){
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
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
  }
}

void SensorTask(void *Parameters) {
    while(true){
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while(!WaitForTimeSync(15000)){vTaskDelay(pdMS_TO_TICKS(0));}
        bool ok = bme.takeForcedMeasurement();
        float t = bme.readTemperature();
        float h = bme.readHumidity();
        if (ok && !isnan(t) && !isnan(h)){
            sensorOk = true;
            latestread.temp = t; latestread.hum = h;
            GetTime(latestread.time_measure, sizeof(latestread.time_measure));
            datatoJSon(latestread);
        // Serial.printf("From Sensor Task, Temp :%.2fC, Hum: %.2f\n", latestread.temp, latestread.hum);
        } else {
            sensorOk = false;
        }
        xTaskNotifyGive(displayTaskHandle);
    }
}

void setup() {
  // ESP32 STARTUP
  Serial.begin(115200);
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);
  bme.begin(BME_ADDR);
  oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  LittleFS.begin();
  //initSequenceNumber();

  // SENSOR STARTUP
  
  // 1x oversampling, 1 s standby: low self-heating for room measurements
  bme.setSampling(Adafruit_BME280::MODE_FORCED,
                  Adafruit_BME280::SAMPLING_X1,
                  Adafruit_BME280::SAMPLING_NONE,
                  Adafruit_BME280::SAMPLING_X1,
                  Adafruit_BME280::FILTER_OFF,
                  Adafruit_BME280::STANDBY_MS_1000);


  // NETWORK STARTUP
  // Wi-Fi
  WiFi.begin(ssid, password);
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  // MQTT
  client.setServer(mqtt_server, 1883);
  client.connect("ESP32_01_ROOM_LEVEL_1");
  
  // OLED SETUP
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(0, 28);
  oled.print("Starting...");
  oled.display();

  // RTOS STUFF
  jsonQueue = xQueueCreate(6, JSON_BUFFER_SIZE);
  jsonQueueBacklog = xQueueCreate(18, JSON_BUFFER_SIZE);
  if (jsonQueue == NULL || jsonQueueBacklog == NULL) {
    Serial.println("Error creating the FreeRTOS Queue");
    return;
  }
  xTaskCreatePinnedToCore(UpOldDataTask, "upload",    4096, nullptr, 1, &oldDataTaskHandle, 0);
  xTaskCreatePinnedToCore(NetworkTask,   "network",   4096, nullptr, 2, &networkTaskHandle, 0);
  xTaskCreatePinnedToCore(SensorTask,    "sensor",    4096, nullptr, 3, &sensorTaskHandle, 1);
  xTaskCreatePinnedToCore(DisplayTask,   "display",   4096, nullptr, 1, &displayTaskHandle, 1);
  xTaskCreatePinnedToCore(SchedulerTask, "scheduler", 2048, nullptr, 4, &schedulerTaskHandle, 1);
}

void loop() {
  vTaskDelete(nullptr);   // all work runs in tasks
}