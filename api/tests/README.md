# MQTT Unit Tests

This directory contains unit tests for the MQTT module.

## Test Structure

- `test_mqtt_handlers.py` - Tests for message handlers (telemetry, alerts, status)
- `test_mqtt_publisher.py` - Tests for command publishing
- `test_mqtt_client.py` - Tests for MQTT client creation and callbacks

## Running Tests

### Run all tests:
```bash
cd api
pytest
```

### Run specific test file:
```bash
pytest tests/test_mqtt_handlers.py
```

### Run specific test class:
```bash
pytest tests/test_mqtt_handlers.py::TestHandleTelemetry
```

### Run specific test:
```bash
pytest tests/test_mqtt_handlers.py::TestHandleTelemetry::test_valid_telemetry
```

### Run with verbose output:
```bash
pytest -v
```

### Run with coverage (optional):
```bash
pip install pytest-cov
pytest --cov=src --cov-report=html
```

## Test Coverage

### Handlers (test_mqtt_handlers.py)
- ✅ Valid telemetry parsing
- ✅ Missing field detection
- ✅ Invalid JSON handling
- ✅ Type validation (temperature, PIR)
- ✅ Valid alert parsing
- ✅ Missing alert fields
- ✅ Online/offline status
- ✅ Case-insensitive status
- ✅ Whitespace handling

### Publisher (test_mqtt_publisher.py)
- ✅ Valid command publication
- ✅ Buzzer validation (0 or 1)
- ✅ LED type validation (string)
- ✅ LED color validation (rouge, vert, bleu, jaune, blanc, eteint)
- ✅ MQTT publish failure handling
- ✅ Success/failure logging

### Client (test_mqtt_client.py)
- ✅ Configuration defaults
- ✅ Topic format validation
- ✅ Connection callback (success/failure)
- ✅ Disconnection callback (normal/unexpected)
- ✅ Message routing (telemetry, alerts, status)
- ✅ Invalid UTF-8 handling
- ✅ Unhandled topic warning
- ✅ Client creation with/without TLS
- ✅ Authentication configuration
- ✅ Callback registration
- ✅ Reconnect delay configuration

## Mocking Strategy

All tests use `unittest.mock` to avoid:
- Real MQTT broker connections
- Real sensor hardware
- Real network operations

This ensures tests are:
- Fast (no network I/O)
- Reliable (no external dependencies)
- Isolated (test failures don't affect other components)

## Adding New Tests

1. Create a new test file or add to existing ones
2. Use the `@patch` decorator to mock external dependencies
3. Follow the naming convention: `test_<functionality>_<scenario>`
4. Add descriptive docstrings
5. Run tests to verify they pass

## Continuous Integration

These tests can be integrated into CI/CD pipelines to ensure code quality before deployment.
