# Room Monitor

![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg) ![Status](https://img.shields.io/badge/status-Prototype-brightgreen) ![Platform](https://img.shields.io/badge/platform-ESP32-blue)

An ESP32 + BME280 node that logs room temperature and humidity, compares them with the outdoor conditions in my city, and charts both in Grafana.

**Hypothesis:** My room is cooler than outside on hot days and warmer on cool days.

## Result 
True to some extent.

## Status: Done

**Working Features**

- Periodic BME280 readings, timestamped on the device with NTP time
- MQTT delivery into InfluxDB 3 via Telegraf, with the city's temperature and humidity polled from Open-Meteo alongside
- A Grafana dashboard: current room and city values, room-vs-city charts, their difference, and a time-since-last-reading indicator
- Offline tolerance : readings are buffered to flash when Wi-Fi or MQTT is down and replayed afterwards
- Everything server-side runs from one `docker compose up`

<!-- ![Grafana dashboard](docs/dashboard.png) -->

## Architecture

```mermaid
flowchart LR
  subgraph Node["ESP32 node"]
    BME["BME280"] --> ST["SensorTask"]
    ST -->|"queue"| NT["NetworkTask"]
    ST -->|"queue full"| FS[("LittleFS")]
    NT -->|"publish failed"|
    ST --> DT["DisplayTask"] --> OLED["OLED"]
  end
  NT -->|"ROOM_LEVEL_1/now/data"| MQ["Mosquitto"]
  UT -->|"ROOM_LEVEL_1/backlog/data"| MQ
  MQ --> TG["Telegraf"]
  OM["Open-Meteo"] -->|"poll 15 min"| TG
  TG --> DB[("InfluxDB 3 Core")]
  DB --> GF["Grafana"]
```

## Firmware

Arduino-framework sketch (`firmware/esp32_room_monitor.ino`) running FreeRTOS tasks on the ESP32. BME280 and a 0.96" SSD1306 OLED share one I2C bus (SDA GPIO21, SCL GPIO22, 400 kHz; addresses 0x76 and 0x3C).

| Task | Core | Priority | Role |
|---|---|---|---|
| `SchedulerTask` | 1 | 4 | Waits for NTP sync, then wakes the sensor and network tasks every `MEASURE_INTERVAL_MS` using `vTaskDelayUntil` |
| `SensorTask` | 1 | 3 | Takes a forced BME280 measurement, stamps it, builds the JSON and enqueues it |
| `NetworkTask` | 0 | 2 | Maintains Wi-Fi/MQTT, publishes queued readings, falls back to flash on failure |
| `UpOldDataTask` | 0 | 1 | Replays buffered readings from LittleFS once the link is back |
| `DisplayTask` | 1 | 1 | Shows the latest reading, or a sensor error, on the OLED |

**Design notes**

- **Event-driven, no polling.** Tasks wake each other with FreeRTOS task notifications.
- **Fixed-period sampling.** `vTaskDelayUntil` prevents drift, and the scheduler waits for NTP so nothing is stamped 1970.
- **Timestamp travels with the reading.** UTC time is captured at measurement and queued with the values, so a reading buffered for an hour still lands at the right place on the chart.
- **Low self-heating.** The BME280 runs in forced mode (1x oversampling, pressure off, filter off) and measures only when triggered.
- **Store-and-forward.** If the queue is full or a publish fails, the reading goes to a LittleFS log. Replay uses a separate `backlog` topic, and lines that fail to send are kept via a temp-file swap.
- **Safe to replay.** InfluxDB overwrites a point with the same series and timestamp, so duplicate deliveries don't create duplicate points.

**Payload** (JSON over MQTT)

```json
{"message_id":1042,"device_id":"ESP32_01_ROOM_LEVEL_1","temp":28.40,"hum":71.20,"measure_at":"2026-11-11T08:30:00Z"}
```

## Feature
 
- [x] Bare-metal to FreeRTOS migration
- [x] Wi-Fi + MQTT, NTP-stamped JSON payloads
- [x] LittleFS buffering and replay (first version)
- [x] Dockerised Mosquitto, Telegraf, InfluxDB 3 and Grafana stack
- [x] Grafana dashboard with city comparison and device-status indicator 
- [ ] **OTA firmware updates**
- [x] **Reliable data delivery**
  - [x] Any failed data upload is stored inside ESP32
  - [x] Mutexes for LittleFS and I2C bus
  - [x] Persist `message_id` across reboots (the `Preferences` code exists but is disabled)
- [x] **Wi-Fi Manager:** provisioning without reflashing (medium priority)
- [x] Deep sleep

## Run it

**Clone** 
```sh
git clone https://github.com/vlncn/Room_Monitor.git
cd Room_Monitor
```

**Backend** 
1. Configure the `.env` file. Fill the missing value.
2. `docker compose up -d`  


**Grafana:** 
1. Add an InfluxDB data source in **SQL** mode (URL `http://<influxdb-host>:8181`, database = your bucket) 
2. Add dashboard by importing `grafana/suhu-dashboard.json`.

**Firmware:**
1. In the Arduino IDE, install the ESP32 board package and the libraries PubSubClient, Adafruit BME280, Adafruit Unified Sensor, Adafruit GFX, Adafruit SSD1306 and Wi-Fi Manager.
   const char* ssid        = "YOUR_WIFI";
   const char* password    = "YOUR_PASSWORD";
   const char* mqtt_server = "192.168.x.x"; 
   ```
2. Select your ESP32 board, then build and upload.

## Repository layout

```
firmware/            ESP32 sketch
mosquitto/config/    broker configuration
telegraf/            Telegraf Configuration
grafana/             dashboard JSON
docker-compose.yml   Mosquitto + Telegraf + InfluxDB 3 Core + Grafana
```

## License

MIT, see [LICENSE](LICENSE).