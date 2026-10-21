~~1. **Invalid JSON.**~~ In `datatoJSon()`, `\"up\":%s` has no quotes around the timestamp. It should be `\"up\":\"%s\"`. The message is about 90 bytes with the quotes, so it still fits in `JSON_BUFFER_SIZE`.
   
1. **Data race on a `String` in a shared struct.** `ReadingResult` holds a `String`. `SensorTask` writes `latestread` while `DisplayTask` copies it with `snap = latestread;`. Copying a `String` while another task reassigns it can corrupt the heap or crash, and it may happen days in. Use a `char[]` for the timestamp, or protect the struct with a mutex.
   
2. **No mutex on the I2C bus.** `SensorTask` (priority 3) uses the BME280 and `DisplayTask` (priority 1) uses the OLED on the same bus. Both are on core 1, but the higher-priority task can still interrupt the display in the middle of a transfer. Use one mutex around every I2C access.
   
3. **Timestamps before NTP syncs.** `configTime()` returns immediately, and the tasks start right away. The first readings can carry a 1970 date. Wait until `time(nullptr)` is a plausible value before starting `SensorTask`.
   
4. **`sensorOk = true` is always set.** There's no NaN check on the BME280 reads, so the "SENSOR ERROR" screen can never appear. Check `isnan()` on the values and set `sensorOk` from that.

## Gaps that match your to-do list

6. **WiFi blocks `setup()` forever.** The `while (WiFi.status() != WL_CONNECTED)` loop means that if the router is down at boot, nothing starts: no sensor, no display, no logging.
   
7. **Missing BME280 or OLED halts the device.** Both use `while (true) delay(1000)`, which fits your "error handling" item. A missing OLED shouldn't stop the sensing.

9.  **`NetworkTask` blocks while MQTT is down.** It sits in the `while (!client.connected())` retry loop, so it never drains the queue. With a 5-message queue and 1 reading per second, data starts dropping after about 5 seconds.
    
10. **Failed publishes are lost.** `Publish Failed` only prints. A message isn't kept for a retry, and the queue-full branch is empty (your LittleFS TODO).
    
11. **No WiFi check in the network loop.** The task only checks the MQTT connection. WiFi usually auto-reconnects, but nothing reports or handles a WiFi outage.
    
12. **MQTT keepalive depends on message rate.** `client.loop()` runs once per received message. With a message every second that's fine. If you switch to hourly uploads, the connection will time out between messages. Call `loop()` on a timer.

## Cleanup and smaller points

12. **Unused variables.** `lastMsg`, `msg[128]` and `value` are leftovers from the PubSubClient example.
    
13. **Heap churn from `String`.** `GetTime()` builds `String`s every second, which fragments the heap over days. A plain `char` buffer avoids it.
    
14. **`sensorOk` isn't `volatile`.** It's shared between tasks.
    
15. **Network task stack is 4096 bytes.** That's tight for WiFi and MQTT. Check `uxTaskGetStackHighWaterMark()` or raise it to 8192.
    
16. **Hardcoded credentials.** The WiFi password is in the source. Use a `secrets.h` file in `.gitignore`.
    
17. **Fixed MQTT client ID and no login.** `ESP32_Task_Client` works on an open broker. If you turn on authentication, `connect()` needs a username and password. Two devices with the same ID would keep disconnecting each other.
    
18. **Topic name.** `ROOM_LEVEL_1` is flat. A structure like `suhu/<device>/data` scales to more devices and works with wildcard subscriptions.
    
19. **Decimal places.** `temp` uses `%.1f` and `hum` uses `%.2f`, which is inconsistent.
    
20. **File name.** `room_baremetal.cpp` is actually the RTOS version, which will confuse you later.
    
21. **Data volume.** One reading per second is about 86,000 rows a day. That's fine for now, but it's far more than the 10 s averages you planned.
