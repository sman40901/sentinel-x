# API Setup Guide

## Prerequisites

- Python 3.8 or higher
- pip (Python package manager)

## Installation

### 1. Install Dependencies

```bash
cd api
pip install -r requirement.txt
```

This will install:
- paho-mqtt (MQTT client)
- fastapi (Web framework)
- uvicorn (ASGI server)
- pytest (Testing framework)
- pytest-asyncio (Async test support)

### 2. Set Environment Variables

Create a `.env` file in the `api` directory:

```bash
# Copy the example
cp .env.example .env

# Edit .env with your settings
notepad .env  # Windows
# or
nano .env     # Linux/Mac
```

For testing (no auth, no TLS):
```bash
ENVIRONMENT=test
MQTT_HOST=mosquitto
MQTT_PORT=1883
MQTT_USERNAME=
MQTT_PASSWORD=
MQTT_USE_TLS=false
MQTT_CA_CERT=/certs/ca.crt
GROUP_ID=g0X
```

### 3. Run the API

```bash
python -m uvicorn src.main:app --reload
```

The API will start on `http://localhost:8000`

### 4. Test the API

Visit http://localhost:8000/api/v1/health

You should see:
```json
{
  "status": "ok",
  "mqtt_connected": false
}
```

## Running Tests

### Install Test Dependencies

```bash
pip install -r requirement.txt
```

### Run All Tests

```bash
pytest
```

### Run Tests with Verbose Output

```bash
pytest -v
```

### Run Specific Test File

```bash
pytest tests/test_mqtt_handlers.py
```

### Run Specific Test

```bash
pytest tests/test_mqtt_handlers.py::TestHandleTelemetry::test_valid_telemetry
```

## Troubleshooting

### pytest not found

If you get "pytest: command not found", install it:

```bash
pip install pytest pytest-asyncio
```

Or use Python module syntax:

```bash
python -m pytest
```

### Import errors

Make sure you're in the `api` directory:

```bash
cd api
python -m pytest
```

### MQTT connection errors

- Ensure Mosquitto broker is running
- Check MQTT_HOST and MQTT_PORT in .env
- For testing, use MQTT_USE_TLS=false
