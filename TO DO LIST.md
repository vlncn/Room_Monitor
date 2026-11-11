- Make it Product Grade
To be Added :
- Wi-Fi Manager (Medium Priority)
- Deep Sleep and power management (Low Priority)
~~- Docker (TIG Stack) (Highest Priority)~~
- OTA Firmware Update (High Priority 1)
- Reliable Data Delivery (High Priority 2)
  - Telegraf Check
  - Influxdb Check
  - ESP32 Check
    - Sensor Error Code 
  ~~- MQTT Check~~
- Error handling
- Edge Cases
  - Adding Mutex to protect I2C Buses and LittleFS(Storage)
- Readme

OLD 
FIRMWARE :
- Learning Wi-FI Manager
- Connecting to MQTT
- Queue Old Data
- Responding to Switch from Power Mode to Battery Mode
- Turning off Sending data to MQTT when in Battery Mode
- Learning the behaviour when wifi is down/ cant connect to MQTT

FIRMWARE (without Using AI):
- ~~Upgrade from baremetal to RTOS~~
- ~~Add Wi-FI and MQTT~~
~~- Add Queue and Buffer data for every failed attempt to upload data using LittleFS~~
~~- Add buffer data when database is down~~
~~- Structuring how to send old data after the database is back~~
  ~~- Using semaphore to tell sensor to take measurement~~ (Using Forced measurement from BME library)
~~  - then send the data to MQTT broker~~
- Fixing uploading sending old data (bugs)
- Error Handling (Sensor Off, ESP Off, and OLED Off)
~~- Data Loss Handling (MQTT OFF, DB OFF, Wi-Fi OFF)~~

MQTT :
- Learn MQTT
    - ~~Structuring Data~~
    - ~~How to send data to postgreSQL~~
    - Queue data from MQTT to website (NEXT MAKE WEB APP)
    - How to send data to ESP 32