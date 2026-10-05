# Sentinel-X ESP32 Firmware

Firmware for the Sentinel-X IoT monitoring system using ESP32, sensors, and MQTT.

## Hardware Components

- **ESP32 DevKit** - Microcontroller
- **DHT22** - Temperature and humidity sensor
- **MQ-2** - Gas and smoke sensor (analog)
- **PIR HC-SR501** - Motion detector
- **Active Buzzer** - Sound alarm
- **LEDs** - Red and yellow status indicators

## Wiring

### DHT22
- VCC → 3.3V
- DATA → GPIO 4 (configurable in `config.h`)
- GND → GND

### MQ-2
- VCC → 5V (VIN)
- GND → GND
- AO → GPIO 34 (ADC1_CH6, configurable in `config.h`)
- DO → Not used (we use analog output)

**Important**: MQ-2 outputs up to 5V, so ensure proper voltage divider if needed.

### PIR HC-SR501
- VCC → 5V (VIN)
- OUT → GPIO 27 (configurable in `config.h`)
- GND → GND

### Active Buzzer
- S → GPIO 26 (configurable in `config.h`)
- VCC → 5V
- GND → GND

### LEDs
- Red LED anode → GPIO 25 with 220Ω resistor → GND
- Yellow LED anode → GPIO 33 with 220Ω resistor → GND

## Installation

### Prerequisites

1. Install [PlatformIO](https://platformio.org/)
2. Install VS Code with PlatformIO extension (recommended)

### Configuration

Edit `include/config.h`:

```cpp
#define WIFI_SSID "your_wifi"
#define WIFI_PASSWORD "your_wifi_password"

#define MQTT_HOST "192.168.1.50"
#define GROUP_ID "g01"
```

### Environment Selection

The firmware supports two environments:

**Test/Development (default)**:
- MQTT Port: 1883 (plain MQTT)
- TLS: Disabled
- Authentication: Disabled
- Build with: `pio run -e esp32dev`

**Production**:
- MQTT Port: 8883 (MQTTS)
- TLS: Enabled
- Authentication: Enabled
- Build with: `pio run -e esp32dev_prod`

To switch environments, use the appropriate environment in `platformio.ini` or specify it when building.

### Build and Upload

Using PlatformIO CLI:
```bash
cd firmware
pio run
pio run --target upload
pio device monitor
```

Or use VS Code PlatformIO extension buttons.

## MQTT Topics

### Publishing

- **Telemetry**: `sentinelx/{GROUP_ID}/telemetry`
  - Payload: `{"t":24.1,"h":48,"gaz":312,"pir":0}`
  - Published every 2 seconds

- **Alerts**: `sentinelx/{GROUP_ID}/alerts`
  - Payload: `{"type":"gaz","niveau":"critique"}`
  - Published when gas threshold exceeded

- **Status**: `sentinelx/{GROUP_ID}/status`
  - Payload: `online` or `offline`
  - Published on connection/disconnection

### Subscribing

- **Commands**: `sentinelx/{GROUP_ID}/cmd`
  - Payload: `{"buzzer":1,"led":"rouge"}`
  - `buzzer`: 0 or 1
  - `led`: "rouge", "jaune", "blanc", or "eteint"

## Calibration

### MQ-2 Gas Sensor

The MQ-2 requires a warm-up period of 1-2 minutes before providing stable readings. Adjust the `GAS_ALERT_THRESHOLD` in `config.h` based on your environment.

### DHT22

Readings should be taken at most once every 2 seconds. The firmware respects this limitation.

## Troubleshooting

### WiFi Connection Issues
- Check SSID and password in `config.h`
- Ensure ESP32 is within range of your WiFi router
- Check Serial Monitor (115200 baud) for error messages

### MQTT Connection Issues
- Verify MQTT broker address and port
- Check if broker requires authentication
- Ensure broker is running and accessible

### Sensor Readings
- **DHT22**: If readings are NaN, check wiring and try a different GPIO
- **MQ-2**: If readings are always 0 or 4095, check power supply and voltage divider
- **PIR**: Use the onboard potentiometers to adjust sensitivity and delay

## API Integration

This firmware is designed to work with the Sentinel-X Python API located in the `../api` directory. The API expects telemetry in the format sent by this firmware.

## License

Educational project for EPSI Workshop Sentinel-X.
