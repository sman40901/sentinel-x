# Environment Configuration Guide

## Overview

Both the ESP32 firmware and Python API now support environment-based configuration. You can switch between **test** (development) and **prod** (production) modes.

## Test/Development Mode (Default)

### Characteristics:
- **MQTT Port**: 1883 (plain MQTT, no encryption)
- **TLS**: Disabled
- **Authentication**: Disabled (no username/password)
- **Certificates**: Not required

### When to use:
- Local development
- Testing without certificates
- Initial setup and debugging

### API Configuration (.env):
```bash
ENVIRONMENT=test
MQTT_HOST=mosquitto
MQTT_PORT=1883
MQTT_USERNAME=
MQTT_PASSWORD=
MQTT_USE_TLS=false
```

### Firmware Configuration:
Build with default environment:
```bash
pio run -e esp32dev
```

## Production Mode

### Characteristics:
- **MQTT Port**: 8883 (MQTTS with TLS)
- **TLS**: Enabled
- **Authentication**: Enabled (username/password required)
- **Certificates**: Required (CA certificate)

### When to use:
- Final evaluation
- Production deployment
- Security-critical environments

### API Configuration (.env):
```bash
ENVIRONMENT=prod
MQTT_HOST=mosquitto
MQTT_PORT=8883
MQTT_USERNAME=api
MQTT_PASSWORD=your_secure_password
MQTT_USE_TLS=true
MQTT_CA_CERT=/certs/ca.crt
```

### Firmware Configuration:
Build with production environment:
```bash
pio run -e esp32dev_prod
```

## Firmware Environment Switching

### Option 1: Use pre-configured environments (Recommended)

**Test mode:**
```bash
pio run -e esp32dev
pio run -e esp32dev --target upload
```

**Production mode:**
```bash
pio run -e esp32dev_prod
pio run -e esp32dev_prod --target upload
```

### Option 2: Override at build time

```bash
pio run -e esp32dev --build-flags="-DIS_PRODUCTION=1"
```

## API Environment Switching

Simply set the `ENVIRONMENT` environment variable in your `.env` file:

```bash
# For development
ENVIRONMENT=test

# For production
ENVIRONMENT=prod
```

The API will automatically:
- Set appropriate default MQTT port (1883 or 8883)
- Enable/disable TLS based on environment
- Set default username/password (empty for test, configured for prod)

## Firmware Serial Output

When the firmware starts, it will log the current configuration:

```
Sentinel-X ESP32 Firmware
Environment: TEST/DEVELOPMENT
MQTT Port: 1883
MQTT TLS: disabled
MQTT Auth: disabled
```

Or for production:

```
Sentinel-X ESP32 Firmware
Environment: PRODUCTION
MQTT Port: 8883
MQTT TLS: enabled
MQTT Auth: enabled
```

## Security Notes

### Test Mode:
- ⚠️ **Not secure** - No encryption, no authentication
- ⚠️ Do not use in production or public networks
- ✅ Perfect for local development and testing

### Production Mode:
- ✅ **Secure** - TLS encryption, authentication
- ✅ Meets workshop cybersecurity requirements
- ⚠️ Requires proper certificate setup

## Certificate Setup for Production

### Mosquitto Broker:
1. Generate CA certificate
2. Configure Mosquitto to use TLS
3. Set up user authentication

### ESP32 Firmware:
Currently uses `wifiClient.setInsecure()` for self-signed certificates. For production:
1. Embed CA certificate in firmware
2. Use `wifiClient.setCACert(ca_cert)` instead of `setInsecure()`

### Python API:
Ensure `MQTT_CA_CERT` points to the correct CA certificate file.

## Quick Reference

| Setting | Test Mode | Production Mode |
|---------|-----------|------------------|
| ENVIRONMENT | test | prod |
| MQTT_PORT | 1883 | 8883 |
| MQTT_USE_TLS | false | true |
| MQTT_USERNAME | (empty) | api |
| MQTT_PASSWORD | (empty) | (configured) |
| Certificates | Not needed | Required |
| Firmware build | `esp32dev` | `esp32dev_prod` |
