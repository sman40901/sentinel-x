import pytest
from unittest.mock import Mock, patch, MagicMock
import paho.mqtt.client as mqtt

from src.mqtt.client import (
    MQTT_HOST,
    MQTT_PORT,
    MQTT_USERNAME,
    MQTT_PASSWORD,
    MQTT_USE_TLS,
    GROUP_ID,
    TOPIC_TELEMETRY,
    TOPIC_ALERTS,
    TOPIC_STATUS,
    TOPIC_COMMAND,
    create_mqtt_client,
    on_connect,
    on_disconnect,
    on_message,
)


class TestConfiguration:
    """Tests for MQTT configuration."""

    def test_mqtt_host_default(self):
        """Test default MQTT host."""
        assert MQTT_HOST == "192.168.1.50"

    def test_group_id_default(self):
        """Test default GROUP_ID."""
        assert GROUP_ID == "g01"

    def test_topic_format(self):
        """Test MQTT topic format."""
        assert GROUP_ID in TOPIC_TELEMETRY
        assert "telemetry" in TOPIC_TELEMETRY
        assert "alerts" in TOPIC_ALERTS
        assert "status" in TOPIC_STATUS
        assert "cmd" in TOPIC_COMMAND


class TestOnConnect:
    """Tests for on_connect callback."""

    @patch("src.mqtt.client.logger")
    def test_on_connect_success(self, mock_logger):
        """Test successful connection callback."""

        mock_client = Mock()

        # Paho subscribe() returns:
        # (result_code, message_id)
        mock_client.subscribe.return_value = (
            mqtt.MQTT_ERR_SUCCESS,
            1
        )

        mock_userdata = {}
        mock_flags = {}
        mock_properties = {}

        reason_code = 0

        on_connect(
            mock_client,
            mock_userdata,
            mock_flags,
            reason_code,
            mock_properties
        )

        mock_client.subscribe.assert_called_once()

        log_messages = str(
            mock_logger.info.call_args_list
        )

        assert "Connected to Mosquitto" in log_messages
        assert "Subscribed to telemetry" in log_messages

    @patch("src.mqtt.client.logger")
    def test_on_connect_failure(self, mock_logger):
        """Test failed connection callback."""

        mock_client = Mock()
        mock_userdata = {}
        mock_flags = {}
        mock_properties = {}

        reason_code = 1

        on_connect(
            mock_client,
            mock_userdata,
            mock_flags,
            reason_code,
            mock_properties
        )

        mock_client.subscribe.assert_not_called()

        assert (
            "MQTT connection failed"
            in str(mock_logger.error.call_args)
        )

    @patch("src.mqtt.client.logger")
    def test_on_connect_subscribe_failure(self, mock_logger):
        """Test subscribe failure in on_connect."""

        mock_client = Mock()

        # Use a valid Paho MQTT non-success code.
        # MQTT_ERR_NO_CONN means the client is not connected.
        mock_client.subscribe.return_value = (
            mqtt.MQTT_ERR_NO_CONN,
            0
        )

        mock_userdata = {}
        mock_flags = {}
        mock_properties = {}

        reason_code = 0

        on_connect(
            mock_client,
            mock_userdata,
            mock_flags,
            reason_code,
            mock_properties
        )

        assert (
            "Failed to subscribe to MQTT topics"
            in str(mock_logger.error.call_args)
        )


class TestOnDisconnect:
    """Tests for on_disconnect callback."""

    @patch("src.mqtt.client.logger")
    def test_on_disconnect_normal(self, mock_logger):
        """Test normal disconnection."""

        mock_client = Mock()
        mock_userdata = {}
        mock_disconnect_flags = {}
        mock_properties = {}

        reason_code = 0

        on_disconnect(
            mock_client,
            mock_userdata,
            mock_disconnect_flags,
            reason_code,
            mock_properties
        )

        assert (
            "Disconnected from Mosquitto"
            in str(mock_logger.info.call_args)
        )

    @patch("src.mqtt.client.logger")
    def test_on_disconnect_unexpected(self, mock_logger):
        """Test unexpected disconnection."""

        mock_client = Mock()
        mock_userdata = {}
        mock_disconnect_flags = {}
        mock_properties = {}

        reason_code = 1

        on_disconnect(
            mock_client,
            mock_userdata,
            mock_disconnect_flags,
            reason_code,
            mock_properties
        )

        assert (
            "Unexpected MQTT disconnect"
            in str(mock_logger.warning.call_args)
        )


class TestOnMessage:
    """Tests for on_message callback."""

    @patch("src.mqtt.client.handle_telemetry")
    @patch("src.mqtt.client.logger")
    def test_on_message_telemetry(
        self,
        mock_logger,
        mock_handle_telemetry
    ):
        """Test telemetry message routing."""

        mock_client = Mock()
        mock_userdata = {}
        mock_message = Mock()

        mock_message.topic = TOPIC_TELEMETRY

        mock_message.payload.decode.return_value = (
            '{"t": 24.1, "h": 48, "gaz": 312, "pir": 0}'
        )

        on_message(
            mock_client,
            mock_userdata,
            mock_message
        )

        mock_handle_telemetry.assert_called_once_with(
            '{"t": 24.1, "h": 48, "gaz": 312, "pir": 0}'
        )

    @patch("src.mqtt.client.handle_alert")
    @patch("src.mqtt.client.logger")
    def test_on_message_alert(
        self,
        mock_logger,
        mock_handle_alert
    ):
        """Test alert message routing."""

        mock_client = Mock()
        mock_userdata = {}
        mock_message = Mock()

        mock_message.topic = TOPIC_ALERTS

        mock_message.payload.decode.return_value = (
            '{"type": "gaz", "niveau": "critique"}'
        )

        on_message(
            mock_client,
            mock_userdata,
            mock_message
        )

        mock_handle_alert.assert_called_once_with(
            '{"type": "gaz", "niveau": "critique"}'
        )

    @patch("src.mqtt.client.handle_status")
    @patch("src.mqtt.client.logger")
    def test_on_message_status(
        self,
        mock_logger,
        mock_handle_status
    ):
        """Test status message routing."""

        mock_client = Mock()
        mock_userdata = {}
        mock_message = Mock()

        mock_message.topic = TOPIC_STATUS
        mock_message.payload.decode.return_value = "online"

        on_message(
            mock_client,
            mock_userdata,
            mock_message
        )

        mock_handle_status.assert_called_once_with(
            "online"
        )

    @patch("src.mqtt.client.logger")
    def test_on_message_invalid_utf8(self, mock_logger):
        """Test message with invalid UTF-8."""

        mock_client = Mock()
        mock_userdata = {}
        mock_message = Mock()

        mock_message.topic = TOPIC_TELEMETRY

        mock_message.payload.decode.side_effect = (
            UnicodeDecodeError(
                "utf-8",
                b"",
                0,
                1,
                ""
            )
        )

        on_message(
            mock_client,
            mock_userdata,
            mock_message
        )

        assert (
            "Invalid UTF-8 message"
            in str(mock_logger.error.call_args)
        )

    @patch("src.mqtt.client.logger")
    def test_on_message_unhandled_topic(self, mock_logger):
        """Test message with unhandled topic."""

        mock_client = Mock()
        mock_userdata = {}
        mock_message = Mock()

        mock_message.topic = "some/unknown/topic"
        mock_message.payload.decode.return_value = "{}"

        on_message(
            mock_client,
            mock_userdata,
            mock_message
        )

        assert (
            "Unhandled MQTT topic"
            in str(mock_logger.warning.call_args)
        )


class TestCreateMqttClient:
    """Tests for create_mqtt_client function."""

    @patch("src.mqtt.client.mqtt.Client")
    @patch("src.mqtt.client.MQTT_USE_TLS", False)
    def test_create_client_without_tls(
        self,
        mock_mqtt_client
    ):
        """Test client creation without TLS."""

        mock_client_instance = Mock()
        mock_mqtt_client.return_value = (
            mock_client_instance
        )

        client = create_mqtt_client()

        mock_client_instance.tls_set.assert_not_called()
        mock_client_instance.tls_insecure_set.assert_not_called()

        assert client == mock_client_instance

    @patch("src.mqtt.client.mqtt.Client")
    @patch("src.mqtt.client.MQTT_USE_TLS", True)
    @patch(
        "src.mqtt.client.MQTT_CA_CERT",
        "/certs/ca.crt"
    )
    def test_create_client_with_tls(
        self,
        mock_mqtt_client
    ):
        """Test client creation with TLS."""

        mock_client_instance = Mock()
        mock_mqtt_client.return_value = (
            mock_client_instance
        )

        client = create_mqtt_client()

        mock_client_instance.tls_set.assert_called_once()

        mock_client_instance.tls_insecure_set.assert_called_once_with(
            False
        )

        assert client == mock_client_instance

    @patch("src.mqtt.client.mqtt.Client")
    @patch(
        "src.mqtt.client.MQTT_USERNAME",
        "test_user"
    )
    @patch(
        "src.mqtt.client.MQTT_PASSWORD",
        "test_pass"
    )
    def test_client_authentication(
        self,
        mock_mqtt_client
    ):
        """Test client with authentication."""

        mock_client_instance = Mock()
        mock_mqtt_client.return_value = (
            mock_client_instance
        )

        create_mqtt_client()

        mock_client_instance.username_pw_set.assert_called_once_with(
            username="test_user",
            password="test_pass"
        )

    @patch("src.mqtt.client.mqtt.Client")
    def test_client_callbacks_set(
        self,
        mock_mqtt_client
    ):
        """Test that callbacks are set."""

        mock_client_instance = Mock()
        mock_mqtt_client.return_value = (
            mock_client_instance
        )

        create_mqtt_client()

        assert mock_client_instance.on_connect is not None
        assert mock_client_instance.on_disconnect is not None
        assert mock_client_instance.on_message is not None

    @patch("src.mqtt.client.mqtt.Client")
    def test_client_reconnect_delay(
        self,
        mock_mqtt_client
    ):
        """Test reconnect delay configuration."""

        mock_client_instance = Mock()
        mock_mqtt_client.return_value = (
            mock_client_instance
        )

        create_mqtt_client()

        mock_client_instance.reconnect_delay_set.assert_called_once()