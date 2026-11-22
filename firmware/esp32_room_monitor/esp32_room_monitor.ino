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
#include <LittleFS.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <Update.h>

// ---------- CONFIG ----------
const int      SDA_PIN    = 21;
const int      SCL_PIN    = 22;
const uint8_t  BME_ADDR   = 0x76;
const uint8_t  OLED_ADDR  = 0x3C;
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 8 * 3600;
const int   daylightOffset_sec = 0;
const char* LOG_FILE       = "/records.csv";
const char* TEMP_FILE      = "/records.temp";
const char* versionUrl= "https://raw.githubusercontent.com/vlncn/Room_Monitor/tree/Product-grade/version.txt";
const char* firmwareUrl = "";
const char* currentFirmwareVersion = "1.0.0";

constexpr uint32_t MEASURE_INTERVAL_MS = 10UL * 60UL * 1000UL;
// -----------------------------

#define SCREEN_W 128
#define SCREEN_H 64
#define JSON_BUFFER_SIZE 192
#define SEQUENCE_BLOCK 100
#define uS_TO_MS_FACTOR 1000

WiFiClient espClient;
WiFiManager wm;
PubSubClient client(espClient);

Adafruit_BME280  bme;
Adafruit_SSD1306 oled(SCREEN_W, SCREEN_H, &Wire, -1);
Preferences preferences;

static QueueHandle_t jsonQueue; // Creating Queue to not mismatch between timestamp and sensor reading
static SemaphoreHandle_t LittleFSMutex;
static SemaphoreHandle_t I2CMutex;
static SemaphoreHandle_t TaskSemaphore;


// Task handles
TaskHandle_t schedulerTaskHandle = nullptr;
TaskHandle_t sensorTaskHandle    = nullptr;
TaskHandle_t displayTaskHandle   = nullptr;
TaskHandle_t networkTaskHandle   = nullptr;
TaskHandle_t SleepTaskHandle     = nullptr;

struct ReadingResult {
    float temp = 0;
    float hum = 0;
    char time_measure[25] = "";
};

bool sensorOk = false;
bool connectedtoMQTT = false;
bool bypasstobuffer = false;
uint8_t retryCount;
uint8_t num_of_task = 4;
ReadingResult latestread; //Creating latest read Struct to display in OLED
RTC_DATA_ATTR time_t Startuptime;
RTC_DATA_ATTR uint16_t sequenceNumber;
RTC_DATA_ATTR bool alreadyOn;
uint16_t reservedLimit;
char mqtt_server[41];
char mqtt_port[7];


/*                             NTP SERVER                       */
void GetTime(char *out, size_t len){ // Getting Timestamp from NTP Server
    time_t now = time(nullptr);
    if (!alreadyOn) Startuptime = time(nullptr);
    struct tm utcInfo;
    gmtime_r(&now, &utcInfo);
    strftime(
    out,
    len,
    "%Y-%m-%dT%H:%M:%SZ",
    &utcInfo);
    return;
};

bool WaitForTimeSync(uint32_t timeoutMs) { // Function to wait for time sync from NTP Server
    uint32_t start = millis();
    while (time(nullptr) < 1700000000) {
        if (millis() - start > timeoutMs) return false;
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    return true;
}

/*                             SERVICES                       */
bool CheckConnection() { // Check if ESP is connected to Wi-Fi or MQTT Broker
    if (WiFi.status() != WL_CONNECTED) {WiFi.reconnect(); return false;}
    if (!client.connected())  {client.connect("ESP32_01_ROOM_LEVEL_1");return false;}
    return true;
}

void RetryConnection(){
    while (retryCount < 3 && !connectedtoMQTT) {
        if (connectedtoMQTT = CheckConnection()) break;
        retryCount++;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    client.loop();
    return;
}

void SetMQTTServerAddress() {
  WiFiManagerParameter custom_mqtt_server("server", "MQTT Broker", mqtt_server, 40);
  WiFiManagerParameter custom_mqtt_port("port", "MQTT Port", mqtt_port, 6);

  wm.addParameter(&custom_mqtt_server);
  wm.addParameter(&custom_mqtt_port);

  if (!wm.startConfigPortal("OnDemandAP")) {
    Serial.println("Failed to connect and hit timeout");
    ESP.restart();
  }

  strcpy(mqtt_server, custom_mqtt_server.getValue());
  strcpy(mqtt_port, custom_mqtt_port.getValue());

  preferences.putString("server", mqtt_server);
  preferences.putString("port", mqtt_port);

  return;
}

void LoadSavedMQTTServer(){
  preferences.begin("mqtt-config", false);
  bool savedmqtt = !preferences.getString("server", mqtt_server, sizeof(mqtt_server)) 
  || !preferences.getString("port", mqtt_port, sizeof(mqtt_port));
  if(savedmqtt) SetMQTTServerAddress(); // Has no way to dynamically change the MQTT Server, basically makes changing MQTT Server IP a more of a hassle
  else wm.autoConnect();
  preferences.end();
  
  return;
}

/*                                   OTA                                 */
bool CheckForUpdate(){
    if (time(nullptr) - Startuptime >= 86400){
        String latestVersion = fetchLatestVersion();
        if (latestVersion == "") {
            return;
        }
      // Step 2: Compare versions
        if (latestVersion != currentFirmwareVersion) {
            downloadAndApplyFirmware();
        }
    }
}

String fetchLatestVersion() {
  HTTPClient http;
  http.begin(versionUrl);

  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String latestVersion = http.getString();
    latestVersion.trim();
    http.end();
    return latestVersion;
  } else {
    http.end();
    return "";
  }
}

void downloadAndApplyFirmware() {
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.begin(firmwareUrl);

  int httpCode = http.GET();

  if (httpCode != HTTP_CODE_OK) return;
  int contentLength = http.getSize();
  if (contentLength <= 0)  return;
  WiFiClient* stream = http.getStreamPtr();
  if (startOTAUpdate(stream, contentLength)) ESP.restart();
  http.end();
}


bool startOTAUpdate(WiFiClient* client, int contentLength) {
  if (!Update.begin(contentLength)) return false;

  size_t written = 0;

  // Timeout variables
  const unsigned long timeoutDuration = 120*1000;  // 10 seconds timeout
  unsigned long lastDataTime = millis();

  while (written < contentLength) {
    if (client->available()) {
      uint8_t buffer[128];
      size_t len = client->read(buffer, sizeof(buffer));
      if (len > 0) {
        Update.write(buffer, len);
        written += len;
        lastDataTime = millis();
      }
    }
    // Check for timeout
    if (millis() - lastDataTime > timeoutDuration) {Update.abort(); return false;}
    yield();
  }

  if (written != contentLength) {Update.abort(); return false;}
  if (!Update.end()) return false;
  return true;
}

/*                             Message ID Handler                       */
void InitSequence() {
    preferences.begin("storage", false);
    uint16_t lastReserved = preferences.getUInt("seq", 0);
    reservedLimit = lastReserved + SEQUENCE_BLOCK;
    preferences.putUInt("seq", reservedLimit);
    sequenceNumber = lastReserved + 1;
    preferences.end();

    return;
}

uint16_t GetSequence() {
    if (sequenceNumber > reservedLimit) {
        preferences.begin("storage", false);
        uint16_t lastReserved = preferences.getUInt("seq", 0);
        reservedLimit = lastReserved + SEQUENCE_BLOCK;
        preferences.putUInt("seq", reservedLimit);
        preferences.end();
        sequenceNumber = lastReserved + 1;
    }
    return sequenceNumber++;
}

/*                            Backlog Data Handler                       */
void SavetoLittleFS(char JSON[JSON_BUFFER_SIZE]){  // Saving data in JSON format to littleFS to be used to upload old data
    if (xSemaphoreTake(LittleFSMutex, pdMS_TO_TICKS(500)) != pdTRUE) return;
    File file = LittleFS.open(LOG_FILE, FILE_APPEND);
    if (!file) {xSemaphoreGive(LittleFSMutex); return;}
    file.println(JSON);
    file.close();
    xSemaphoreGive(LittleFSMutex);
    return;
}

void ReadFromLittleFS(){ // Open Log File to be uploaded to MQTT Broker while also save any failed upload 
    if (xSemaphoreTake(LittleFSMutex, pdMS_TO_TICKS(500)) != pdTRUE) return;
    File file = LittleFS.open(LOG_FILE, FILE_READ);
    if (!file) {xSemaphoreGive(LittleFSMutex); return;}
    if (!file.available()) {xSemaphoreGive(LittleFSMutex); return;}
    File out = LittleFS.open(TEMP_FILE, FILE_WRITE);
    if (!out) { file.close(); xSemaphoreGive(LittleFSMutex); return;}
    bool failed = false;

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

        if (!client.publish("ROOM_LEVEL_1/backlog/data", jsonBuffer)){out.println(jsonBuffer); failed = true ;}
    }
    file.close();
    out.close();
    LittleFS.remove(LOG_FILE);
    if (failed) LittleFS.rename(TEMP_FILE, LOG_FILE);
    else        LittleFS.remove(TEMP_FILE);
    xSemaphoreGive(LittleFSMutex);
    return;
}

/*                            Parsing Data                       */
void datatoJSon(struct ReadingResult data){ // Convert data into JSON FILE and queue the data before sending it to mqtt broker
    uint16_t seq = sequenceNumber++; // GetSequence()
    char localBuffer[JSON_BUFFER_SIZE];

    snprintf(localBuffer, sizeof(localBuffer),
           "{\"message_id\":%lu,\"device_id\":\"ESP32_01_ROOM_LEVEL_1\",\"temp\":%.2f,\"hum\":%.2f,\"measure_at\":\"%s\"}",
           seq, data.temp, data.hum, data.time_measure);

    if(xQueueSend(jsonQueue, &localBuffer, 0) != pdPASS) {
        SavetoLittleFS(localBuffer);
    }
    return;
}

/*                             TASK                       */
void SchedulerTask(void *Parameters) {
    TickType_t lastWake = xTaskGetTickCount();
    while(!WaitForTimeSync(15000)){}
    xTaskNotifyGive(sensorTaskHandle);
    xTaskNotifyGive(networkTaskHandle);
    xSemaphoreGive(TaskSemaphore);
    while (true){
        if(uxSemaphoreGetCount(TaskSemaphore) == num_of_task) xTaskNotifyGive(SleepTaskHandle);
        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(1));
    }
    
}

void SleepTask(void *Parameters){
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    alreadyOn = 1;
    CheckForUpdate();
    esp_sleep_enable_timer_wakeup((MEASURE_INTERVAL_MS - millis()) * uS_TO_MS_FACTOR );
    esp_deep_sleep_start();
}

void NetworkTask(void *Parameters){
    char txBuffer[JSON_BUFFER_SIZE];
    client.connect("ESP32_01_ROOM_LEVEL_1");
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    RetryConnection();
    // This thing need refactor
    if (xQueueReceive(jsonQueue, &txBuffer, pdMS_TO_TICKS(30000)) == pdTRUE);
        if (!client.publish("ROOM_LEVEL_1/now/data", txBuffer) || !connectedtoMQTT) {
            SavetoLittleFS(txBuffer);
        }
    if (LittleFS.exists(LOG_FILE) && connectedtoMQTT) { 
        ReadFromLittleFS();
    }
    xSemaphoreGive(TaskSemaphore);
    vTaskDelete(nullptr);
}

void DisplayTask(void *Parameters) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    xSemaphoreTake(I2CMutex, pdMS_TO_TICKS(500));
    ReadingResult snap;
    snap = latestread;
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
    oled.display();
    xSemaphoreGive(TaskSemaphore);
    xSemaphoreGive(I2CMutex);
    vTaskDelete(nullptr);
}

void SensorTask(void *Parameters) {
    float h;
    float t;
    bool ok;
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (xSemaphoreTake(I2CMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        if (ok = bme.takeForcedMeasurement()) {
            t = bme.readTemperature();
            h = bme.readHumidity();
        }
        xSemaphoreGive(I2CMutex);
    }
    if (ok && !isnan(t) && !isnan(h)){
        sensorOk = true;
        latestread.temp = t; latestread.hum = h;
        GetTime(latestread.time_measure, sizeof(latestread.time_measure));
        datatoJSon(latestread);
    } else {
        sensorOk = false;
    }
    xSemaphoreGive(TaskSemaphore);
    xTaskNotifyGive(displayTaskHandle);
    vTaskDelete(nullptr);
}

void setup() {
  // ESP32 STARTUP
  Serial.begin(115200);
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);
  bme.begin(BME_ADDR);
  oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  LittleFS.begin(true);

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
  LoadSavedMQTTServer();
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  // MQTT
  client.setServer(mqtt_server, atoi(mqtt_port));
  client.connect("ESP32_01_ROOM_LEVEL_1");
  
  // OLED SETUP
  if (alreadyOn == 0){
    //InitSequence();
    oled.clearDisplay();
    oled.setTextColor(SSD1306_WHITE);
    oled.setTextSize(1);
    oled.setCursor(0, 28);
    oled.print("Starting...");
    oled.display();
  }
  // RTOS STUFF
  jsonQueue = xQueueCreate(1, JSON_BUFFER_SIZE);
  LittleFSMutex = xSemaphoreCreateMutex();
  I2CMutex = xSemaphoreCreateMutex();
  TaskSemaphore = xSemaphoreCreateCounting(num_of_task,0);
  if (jsonQueue == NULL) {
    Serial.println("Error creating the FreeRTOS Queue");
    ESP.restart();
  }
  xTaskCreatePinnedToCore(NetworkTask,   "network",   4096, nullptr, 2, &networkTaskHandle, 0);
  xTaskCreatePinnedToCore(SensorTask,    "sensor",    4096, nullptr, 3, &sensorTaskHandle, 1);
  xTaskCreatePinnedToCore(DisplayTask,   "display",   4096, nullptr, 1, &displayTaskHandle, 1);
  xTaskCreatePinnedToCore(SchedulerTask, "scheduler", 2048, nullptr, 4, &schedulerTaskHandle, 1);
  xTaskCreatePinnedToCore(SleepTask,     "sleep",     2048, nullptr, 1, &SleepTaskHandle, 1);

}

void loop() {
  vTaskDelete(nullptr);   // all work runs in tasks
}
