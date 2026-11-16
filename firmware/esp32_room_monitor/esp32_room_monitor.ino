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
constexpr uint32_t MEASURE_INTERVAL_MS = 1UL * 10UL * 1000UL; // 10 minutes
// -----------------------------

#define SCREEN_W 128
#define SCREEN_H 64
#define JSON_BUFFER_SIZE 192
#define SEQUENCE_BLOCK 100

WiFiClient espClient;
WiFiManager wm;
PubSubClient client(espClient);

Adafruit_BME280  bme;
Adafruit_SSD1306 oled(SCREEN_W, SCREEN_H, &Wire, -1);
Preferences preferences;

QueueHandle_t jsonQueue; // Creating Queue to not mismatch between timestamp and sensor reading
SemaphoreHandle_t LittleFSMutex;
SemaphoreHandle_t I2CMutex;


// Task handles (needed for task notifications)
TaskHandle_t schedulerTaskHandle = nullptr;
TaskHandle_t sensorTaskHandle    = nullptr;
TaskHandle_t displayTaskHandle   = nullptr;
TaskHandle_t networkTaskHandle   = nullptr;

struct ReadingResult {
    float temp = 0;
    float hum = 0;
    char time_measure[25] = "";
};

bool sensorOk = false;
bool connectedtoMQTT = false;
bool bypasstobuffer = false;
bool callbackCalled = false;
size_t retryCount;
ReadingResult latestread; //Creating latest read Struct to display in OLED
uint32_t sequenceNumber;
uint32_t reservedLimit;
char mqtt_server[41];
char mqtt_port[7];

/*                             NTP SERVER                       */
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

bool WaitForTimeSync(uint32_t timeoutMs) { // Function to wait for time sync from NTP Server
    // time() returns ~0 (1970) until NTP has synced
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
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    client.loop();
}

void ReadPublishdata(char* topic, byte* payload, unsigned int length){
    callbackCalled = true;
    Serial.printf("\n[MQTT RECEIVED] Topic: %s\n", topic);

    Serial.print("Payload: ");

    for (unsigned int i = 0; i < length; i++)
    {
        Serial.print((char)payload[i]);
    }

    // Your JSON parsing follows here
}

// bool CheckServices(){
//     if (connectedtoMQTT) return false;
//     int client.subscribe(infrastructure/health/);
//     return (connectedtoMQTT&&);
// }

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

/*                             Message ID Handler                       */
void InitSequence() {

    preferences.begin("storage", false);

    uint32_t lastReserved = preferences.getUInt("seq", 0);

    reservedLimit = lastReserved + SEQUENCE_BLOCK;

    preferences.putUInt("seq", reservedLimit);

    sequenceNumber = lastReserved + 1;

    preferences.end();
}

uint32_t GetSequence() {
    if (sequenceNumber > reservedLimit) {

        preferences.begin("storage", false);

        uint32_t lastReserved = preferences.getUInt("seq", 0);

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
    File file = LittleFS.open(LOG_FILE, FILE_APPEND); // Mutex Log File
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
        if (len == 0) continue; // Add verify if data is not garbage

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


void datatoJSon(struct ReadingResult data){ // Convert data into JSON FILE and queue the data before sending it to mqtt broker
    uint32_t seq = sequenceNumber++; // GetSequence()
    char localBuffer[JSON_BUFFER_SIZE];

    snprintf(localBuffer, sizeof(localBuffer),
           "{\"message_id\":%lu,\"device_id\":\"ESP32_01_ROOM_LEVEL_1\",\"temp\":%.2f,\"hum\":%.2f,\"measure_at\":\"%s\"}",
           seq, data.temp, data.hum, data.time_measure);

    if(connectedtoMQTT) bypasstobuffer = false;
    
    if(bypasstobuffer){
        SavetoLittleFS(localBuffer); 
        return;
    }

    if(xQueueSend(jsonQueue, &localBuffer, 0) != pdPASS) {
        SavetoLittleFS(localBuffer);
        bypasstobuffer = true;
    }
    return;
}

/*                             TASK                       */
void SchedulerTask(void *Parameters) {
    while(!WaitForTimeSync(15000)){}
    TickType_t xLastWakeTime = xTaskGetTickCount();
    while (true) {
        xTaskNotifyGive(sensorTaskHandle);   // first measurement happens immediately at boot
        xTaskNotifyGive(networkTaskHandle);
        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(MEASURE_INTERVAL_MS));
    }
}


void NetworkTask(void *Parameters){
    char txBuffer[JSON_BUFFER_SIZE];
    while(true){
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        RetryConnection();
        //Only try to upload when all services is online
        if (!connectedtoMQTT) continue;
        if (xQueueReceive(jsonQueue, &txBuffer, pdMS_TO_TICKS(30000)) == pdTRUE) {
            if (client.publish("ROOM_LEVEL_1/now/data", txBuffer)) {
                client.loop();
            } else {
                SavetoLittleFS(txBuffer);
                connectedtoMQTT = false;
                }
            } 
        if (LittleFS.exists(LOG_FILE)) { 
            ReadFromLittleFS();
        }
        retryCount = 0;
    }
}

void DisplayTask(void *Parameters) {
  while (true){
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (xSemaphoreTake(I2CMutex, pdMS_TO_TICKS(500)) != pdTRUE) continue;
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
    // ADD ERROR HANDLE ON SENSOR OR MQTT
    oled.display();
    xSemaphoreGive(I2CMutex);
  }
}

void SensorTask(void *Parameters) {
    float h;
    float t;
    bool ok;
    while(true){
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
  LittleFS.begin(true);
  //InitSequence();

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
  client.setCallback(ReadPublishdata);
  client.connect("ESP32_01_ROOM_LEVEL_1");
  client.subscribe("infrastructure/health/timestamp");
  
  
  // OLED SETUP
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(0, 28);
  oled.print("Starting...");
  oled.display();

  // RTOS STUFF
  jsonQueue = xQueueCreate(1, JSON_BUFFER_SIZE);
  LittleFSMutex = xSemaphoreCreateMutex();
  I2CMutex = xSemaphoreCreateMutex();
  if (jsonQueue == NULL) {
    Serial.println("Error creating the FreeRTOS Queue");
    ESP.restart();
  }
  xTaskCreatePinnedToCore(NetworkTask,   "network",   4096, nullptr, 2, &networkTaskHandle, 0);
  xTaskCreatePinnedToCore(SensorTask,    "sensor",    4096, nullptr, 3, &sensorTaskHandle, 1);
  xTaskCreatePinnedToCore(DisplayTask,   "display",   4096, nullptr, 1, &displayTaskHandle, 1);
  xTaskCreatePinnedToCore(SchedulerTask, "scheduler", 2048, nullptr, 4, &schedulerTaskHandle, 1);
}

void loop() {
  vTaskDelete(nullptr);   // all work runs in tasks
}