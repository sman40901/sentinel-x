# Manual MQTT Testing Guide

This guide shows how to manually test MQTT functionality without running unit tests.

## Prerequisites

1. **Mosquitto MQTT Broker** running
2. **MQTT Client** (choose one):
   - Mosquitto CLI tools (`mosquitto_pub`, `mosquitto_sub`)
   - MQTT Explorer (GUI)
   - MQTT.fx (GUI)

## Option 1: Using Mosquitto CLI Tools

### Install Mosquitto Clients

**Windows:**
```bash
choco install mosquitto
```

**Linux:**
```bash
sudo apt-get install mosquitto-clients
```

**macOS:**
```bash
brew install mosquitto
```

### Test MQTT Connection

### 1. Subscribe to topics (receive messages)

Open a terminal and subscribe to all Sentinel-X topics:

```bash
mosquitto_sub -h localhost -p 1883 -t "sentinelx/g0X/#" -v
```

Flags:
- `-h localhost`: MQTT broker host
- `-p 1883`: MQTT broker port
- `-t "sentinelx/g0X/#"`: Topic pattern (# = wildcard)
- `-v`: Verbose (show topic names)

### 2. Publish test messages

Open another terminal and publish test messages:

**Test telemetry:**
```bash
mosquitto_pub -h localhost -p 1883 -t "sentinelx/g0X/telemetry" -m '{"t": 24.1, "h": 48, "gaz": 312, "pir": 0}'
```

**Test alert:**
```bash
mosquitto_pub -h localhost -p 1883 -t "sentinelx/g0X/alerts" -m '{"type": "gaz", "niveau": "critique"}'
```

**Test status:**
```bash
mosquitto_pub -h localhost -p 1883 -t "sentinelx/g0X/status" -m "online"
```

**Test command:**
```bash
mosquitto_pub -h localhost -p 1883 -t "sentinelx/g0X/cmd" -m '{"buzzer": 1, "led": "rouge"}'
```

### 3. Test with authentication (if enabled)

```bash
mosquitto_sub -h localhost -p 1883 -t "sentinelx/g0X/#" -u api -P your_password -v
mosquitto_pub -h localhost -p 1883 -t "sentinelx/g0X/telemetry" -m '{"t": 24.1, "h": 48, "gaz": 312, "pir": 0}' -u api -P your_password
```

## Option 2: Using MQTT Explorer (GUI)

### Installation

Download from: https://mqtt-explorer.com/

### Setup

1. Open MQTT Explorer
2. Click "Advanced Connections"
3. Configure:
   - **Host**: localhost
   - **Port**: 1883 (or 8883 for TLS)
   - **Username**: (if auth enabled)
   - **Password**: (if auth enabled)
4. Click "Connect"

### Test Topics

1. **Subscribe to topics**:
   - Click "Subscriptions" tab
   - Add: `sentinelx/g0X/#`
   - You'll see incoming messages

2. **Publish messages**:
   - Click "Message" tab
   - Topic: `sentinelx/g0X/telemetry`
   - Payload: `{"t": 24.1, "h": 48, "gaz": 312, "pir": 0}`
   - Click "Publish"

## Option 3: Using Python Script

Create a test script `test_mqtt_manual.py`:

```python
import paho.mqtt.client as mqtt
import json
import time

# Configuration
MQTT_HOST = "localhost"
MQTT_PORT = 1883
GROUP_ID = "g0X"

# Callbacks
def on_connect(client, userdata, flags, reason_code, properties):
    print(f"Connected with result code {reason_code}")
    client.subscribe(f"sentinelx/{GROUP_ID}/#")

def on_message(client, userdata, msg):
    print(f"Topic: {msg.topic}")
    print(f"Payload: {msg.payload.decode()}")

# Create client
client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
client.on_connect = on_connect
client.on_message = on_message

# Connect
client.connect(MQTT_HOST, MQTT_PORT, 60)
client.loop_start()

# Publish test messages
time.sleep(1)  # Wait for connection

print("Publishing telemetry...")
client.publish(f"sentinelx/{GROUP_ID}/telemetry",
              json.dumps({"t": 24.1, "h": 48, "gaz": 312, "pir": 0}))

time.sleep(1)
print("Publishing alert...")
client.publish(f"sentinelx/{GROUP_ID}/alerts",
              json.dumps({"type": "gaz", "niveau": "critique"}))

time.sleep(1)
print("Publishing status...")
client.publish(f"sentinelx/{GROUP_ID}/status", "online")

time.sleep(1)
print("Publishing command...")
client.publish(f"sentinelx/{GROUP_ID}/cmd",
              json.dumps({"buzzer": 1, "led": "rouge"}))

# Keep running to see messages
print("Listening for messages (Ctrl+C to stop)...")
try:
    while True:
        time.sleep(1)
except KeyboardInterrupt:
    print("\nStopping...")
    client.loop_stop()
    client.disconnect()
```

Run it:
```bash
python test_mqtt_manual.py
```

## Option 4: Test with Your API

### Start the API

```bash
cd api
pip install -r requirement.txt
# Set environment variables or use .env file
python -m uvicorn src.main:app --reload
```

### Send HTTP requests to trigger MQTT

**Publish command via HTTP (if you add this endpoint):**

```bash
curl -X POST http://localhost:8000/api/v1/command \
  -H "Content-Type: application/json" \
  -d '{"buzzer": 1, "led": "rouge"}'
```

## Common Test Scenarios

### Test 1: Basic Connectivity
```bash
# Terminal 1: Subscribe
mosquitto_sub -h localhost -p 1883 -t "sentinelx/g0X/#" -v

# Terminal 2: Publish
mosquitto_pub -h localhost -p 1883 -t "sentinelx/g0X/test" -m "Hello MQTT"
```

### Test 2: Telemetry Format Validation
```bash
mosquitto_pub -h localhost -p 1883 -t "sentinelx/g0X/telemetry" -m '{"t": 24.1, "h": 48, "gaz": 312, "pir": 0}'
```

### Test 3: Missing Field Handling
```bash
# Missing 'pir' field
mosquitto_pub -h localhost -p 1883 -t "sentinelx/g0X/telemetry" -m '{"t": 24.1, "h": 48, "gaz": 312}'
```

### Test 4: Invalid JSON
```bash
mosquitto_pub -h localhost -p 1883 -t "sentinelx/g0X/telemetry" -m 'invalid json'
```

### Test 5: Command with Invalid LED Color
```bash
mosquitto_pub -h localhost -p 1883 -t "sentinelx/g0X/cmd" -m '{"buzzer": 1, "led": "purple"}'
```

### Test 6: Gas Alert Trigger
```bash
# High gas value to trigger alert
mosquitto_pub -h localhost -p 1883 -t "sentinelx/g0X/telemetry" -m '{"t": 24.1, "h": 48, "gaz": 500, "pir": 0}'
```

## Troubleshooting

### Connection Refused
- Check if Mosquitto is running: `ps aux | grep mosquitto`
- Check port: `netstat -an | grep 1883`
- Check firewall settings

### Authentication Errors
- Verify username/password in broker config
- Use `-u` and `-P` flags with mosquitto_pub/sub

### No Messages Received
- Check topic spelling (case-sensitive)
- Verify wildcard usage (`#` vs `+`)
- Check subscription in client code

## Testing with ESP32 Firmware

### 1. Flash firmware to ESP32
```bash
cd firmware
pio run -e esp32dev --target upload
pio device monitor
```

### 2. Monitor ESP32 logs
You should see:
```
Sentinel-X ESP32 Firmware
Environment: TEST/DEVELOPMENT
MQTT Port: 1883
MQTT TLS: disabled
MQTT Auth: disabled
Connecting to MQTT broker...
connected
Subscribed to: sentinelx/g0X/cmd
```

### 3. Send command to ESP32
```bash
mosquitto_pub -h localhost -p 1883 -t "sentinelx/g0X/cmd" -m '{"buzzer": 1, "led": "rouge"}'
```

### 4. Monitor telemetry from ESP32
```bash
mosquitto_sub -h localhost -p 1883 -t "sentinelx/g0X/telemetry" -v
```

You should see telemetry every 2 seconds:
```
sentinelx/g0X/telemetry {"t": 24.1, "h": 48, "gaz": 312, "pir": 0}
```

## Quick Reference

| Action | Command |
|--------|---------|
| Subscribe to all topics | `mosquitto_sub -h localhost -p 1883 -t "sentinelx/g0X/#" -v` |
| Publish telemetry | `mosquitto_pub -h localhost -p 1883 -t "sentinelx/g0X/telemetry" -m '{"t": 24.1, "h": 48, "gaz": 312, "pir": 0}'` |
| Publish alert | `mosquitto_pub -h localhost -p 1883 -t "sentinelx/g0X/alerts" -m '{"type": "gaz", "niveau": "critique"}'` |
| Publish status | `mosquitto_pub -h localhost -p 1883 -t "sentinelx/g0X/status" -m "online"` |
| Publish command | `mosquitto_pub -h localhost -p 1883 -t "sentinelx/g0X/cmd" -m '{"buzzer": 1, "led": "rouge"}'` |
