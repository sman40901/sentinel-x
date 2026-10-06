import pytest
from unittest.mock import patch

from src.mqtt.handlers import handle_telemetry, handle_alert, handle_status


class TestHandleTelemetry:
    """Tests for handle_telemetry function."""

    def test_valid_telemetry(self):
        """Test valid telemetry payload."""
        payload = '{"t": 24.1, "h": 48, "gaz": 312, "pir": 0}'

        with patch('src.mqtt.handlers.logger') as mock_logger:
            handle_telemetry(payload)
            mock_logger.info.assert_called_once()
            assert "Telemetry received" in str(mock_logger.info.call_args)

    def test_missing_field(self):
        """Test telemetry with missing field."""
        payload = '{"t": 24.1, "h": 48, "gaz": 312}'  # Missing 'pir'

        with patch('src.mqtt.handlers.logger') as mock_logger:
            handle_telemetry(payload)
            mock_logger.error.assert_called_once()
            assert "Missing telemetry field" in str(mock_logger.error.call_args)

    def test_invalid_json(self):
        """Test telemetry with invalid JSON."""
        payload = 'invalid json'

        with patch('src.mqtt.handlers.logger') as mock_logger:
            handle_telemetry(payload)
            mock_logger.error.assert_called_once()
            assert "Invalid telemetry JSON" in str(mock_logger.error.call_args)

    def test_invalid_temperature_type(self):
        """Test telemetry with invalid temperature type."""
        payload = '{"t": "not_a_number", "h": 48, "gaz": 312, "pir": 0}'

        with patch('src.mqtt.handlers.logger') as mock_logger:
            handle_telemetry(payload)
            mock_logger.error.assert_called_once()
            assert "'t' must be a number" in str(mock_logger.error.call_args)

    def test_invalid_pir_value(self):
        """Test telemetry with invalid PIR value."""
        payload = '{"t": 24.1, "h": 48, "gaz": 312, "pir": 2}'  # pir must be 0 or 1

        with patch('src.mqtt.handlers.logger') as mock_logger:
            handle_telemetry(payload)
            mock_logger.error.assert_called_once()
            assert "'pir' must be 0 or 1" in str(mock_logger.error.call_args)


class TestHandleAlert:
    """Tests for handle_alert function."""

    def test_valid_alert(self):
        """Test valid alert payload."""
        payload = '{"type": "gaz", "niveau": "critique"}'

        with patch('src.mqtt.handlers.logger') as mock_logger:
            handle_alert(payload)
            mock_logger.warning.assert_called_once()
            assert "Alert received" in str(mock_logger.warning.call_args)

    def test_missing_type(self):
        """Test alert with missing type field."""
        payload = '{"niveau": "critique"}'

        with patch('src.mqtt.handlers.logger') as mock_logger:
            handle_alert(payload)
            mock_logger.error.assert_called_once()
            assert "Missing alert field: type" in str(mock_logger.error.call_args)

    def test_missing_niveau(self):
        """Test alert with missing niveau field."""
        payload = '{"type": "gaz"}'

        with patch('src.mqtt.handlers.logger') as mock_logger:
            handle_alert(payload)
            mock_logger.error.assert_called_once()
            assert "Missing alert field: niveau" in str(mock_logger.error.call_args)

    def test_invalid_json(self):
        """Test alert with invalid JSON."""
        payload = 'invalid json'

        with patch('src.mqtt.handlers.logger') as mock_logger:
            handle_alert(payload)
            mock_logger.error.assert_called_once()
            assert "Invalid alert JSON" in str(mock_logger.error.call_args)

    def test_invalid_type_type(self):
        """Test alert with invalid type field type."""
        payload = '{"type": 123, "niveau": "critique"}'

        with patch('src.mqtt.handlers.logger') as mock_logger:
            handle_alert(payload)
            mock_logger.error.assert_called_once()
            assert "'type' must be a string" in str(mock_logger.error.call_args)


class TestHandleStatus:
    """Tests for handle_status function."""

    def test_online_status(self):
        """Test online status."""
        payload = 'online'

        with patch('src.mqtt.handlers.logger') as mock_logger:
            handle_status(payload)
            mock_logger.info.assert_called_once()
            assert "ESP32 status: online" in str(mock_logger.info.call_args)

    def test_offline_status(self):
        """Test offline status."""
        payload = 'offline'

        with patch('src.mqtt.handlers.logger') as mock_logger:
            handle_status(payload)
            mock_logger.info.assert_called_once()
            assert "ESP32 status: offline" in str(mock_logger.info.call_args)

    def test_invalid_status(self):
        """Test invalid status."""
        payload = 'invalid'

        with patch('src.mqtt.handlers.logger') as mock_logger:
            handle_status(payload)
            mock_logger.error.assert_called_once()
            assert "Invalid ESP32 status" in str(mock_logger.error.call_args)

    def test_status_with_whitespace(self):
        """Test status with extra whitespace."""
        payload = '  online  '

        with patch('src.mqtt.handlers.logger') as mock_logger:
            handle_status(payload)
            mock_logger.info.assert_called_once()
            assert "ESP32 status: online" in str(mock_logger.info.call_args)

    def test_status_case_insensitive(self):
        """Test status is case insensitive."""
        payload = 'ONLINE'

        with patch('src.mqtt.handlers.logger') as mock_logger:
            handle_status(payload)
            mock_logger.info.assert_called_once()
            assert "ESP32 status: online" in str(mock_logger.info.call_args)
