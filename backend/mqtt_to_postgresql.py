
import json
import os
from datetime import datetime

import paho.mqtt.client as mqtt
import psycopg

MQTT_HOST = "192.168.1.12"
MQTT_PORT = 1883
MQTT_TOPIC = "ROOM_LEVEL_1"

DB_CONFIG = {
    "host": "localhost",
    "port": 5432,
    "dbname": "room_monitor",
    "user": os.getenv("PGUSER", "postgres"),
    "password": os.environ["PGPASSWORD"],
}

INSERT_SQL = """
    INSERT INTO sensor_readings
        (message_id, device_id, temperature_c, humidity_pct, measured_at)
    VALUES (%s, %s, %s, %s, %s)
    ON CONFLICT (message_id) DO NOTHING
"""

def on_connect(client, userdata, flags, reason_code, properties):
    if reason_code == 0:
        print("Connected to Mosquitto")
        client.subscribe(MQTT_TOPIC, qos=1)
        print(f"Subscribed to {MQTT_TOPIC}")
    else:
        print(f"MQTT connection failed: {reason_code}")

def on_message(client, userdata, msg):
    try:
        payload = json.loads(msg.payload.decode("utf-8"))

        message_id = payload["message_id"]
        device_id = payload["device_id"]
        temperature = float(payload["temp"])
        humidity = float(payload["hum"])

        # Parse the ISO 8601 timestamp and normalize to UTC.
        measured_at = datetime.fromisoformat(
            payload["measure_at"].replace("Z", "+00:00")
        )

        if measured_at.utcoffset() is None:
            raise ValueError("Timestamp must include a timezone")

        # Commit the database transaction before reporting success.
        with psycopg.connect(**DB_CONFIG) as conn:
            with conn.cursor() as cur:
                cur.execute(
                    INSERT_SQL,
                    (message_id, device_id, temperature, humidity, measured_at),
                )

        print(
            f"Saved {device_id}: "
            f"{temperature} C, {humidity}%"
        )

    except Exception as exc:
        # Important: this simple version logs failures but does not
        # durably buffer messages when PostgreSQL is unavailable.
        print(f"Failed to store message: {exc}")

client = mqtt.Client(
    mqtt.CallbackAPIVersion.VERSION2,
    client_id="room-monitor-postgres-bridge",
)

client.on_connect = on_connect
client.on_message = on_message

client.reconnect_delay_set(min_delay=1, max_delay=30)

print("Starting MQTT-to-PostgreSQL bridge...")
client.connect(MQTT_HOST, MQTT_PORT, keepalive=60)
client.loop_forever()
