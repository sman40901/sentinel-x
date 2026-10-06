import pytest
from unittest.mock import Mock, patch
import paho.mqtt.client as mqtt

from src.mqtt.publisher import publish_command


class TestPublishCommand:
    """Tests for publish_command function."""

    def test_valid_command(self):
        """Test valid command publication."""
        mock_client = Mock()
        mock_client.publish.return_value = Mock(rc=mqtt.MQTT_ERR_SUCCESS)

        result = publish_command(
            client=mock_client,
            command_topic="sentinelx/g0X/cmd",
            buzzer=1,
            led="rouge"
        )

        assert result is True
        mock_client.publish.assert_called_once()
        call_args = mock_client.publish.call_args
        assert call_args[1]['topic'] == "sentinelx/g0X/cmd"
        assert '"buzzer": 1' in call_args[1]['payload']
        assert '"led": "rouge"' in call_args[1]['payload']

    def test_invalid_buzzer_value(self):
        """Test command with invalid buzzer value."""
        mock_client = Mock()

        with pytest.raises(ValueError, match="buzzer must be 0 or 1"):
            publish_command(
                client=mock_client,
                command_topic="sentinelx/g0X/cmd",
                buzzer=2,
                led="rouge"
            )

    def test_invalid_led_type(self):
        """Test command with invalid LED type."""
        mock_client = Mock()

        with pytest.raises(ValueError, match="led must be a string"):
            publish_command(
                client=mock_client,
                command_topic="sentinelx/g0X/cmd",
                buzzer=1,
                led=123
            )

    def test_invalid_led_color(self):
        """Test command with invalid LED color."""
        mock_client = Mock()

        with pytest.raises(ValueError, match="led must be one of"):
            publish_command(
                client=mock_client,
                command_topic="sentinelx/g0X/cmd",
                buzzer=1,
                led="purple"
            )

    def test_valid_led_colors(self):
        """Test all valid LED colors."""
        mock_client = Mock()
        mock_client.publish.return_value = Mock(rc=mqtt.MQTT_ERR_SUCCESS)

        valid_colors = ["rouge", "vert", "bleu", "jaune", "blanc", "eteint"]

        for color in valid_colors:
            result = publish_command(
                client=mock_client,
                command_topic="sentinelx/g0X/cmd",
                buzzer=1,
                led=color
            )
            assert result is True

    def test_mqtt_publish_failure(self):
        """Test MQTT publish failure."""
        mock_client = Mock()
        mock_client.publish.return_value = Mock(rc=mqtt.MQTT_ERR_NO_CONN)

        result = publish_command(
            client=mock_client,
            command_topic="sentinelx/g0X/cmd",
            buzzer=1,
            led="rouge"
        )

        assert result is False

    def test_buzzer_off(self):
        """Test command with buzzer off."""
        mock_client = Mock()
        mock_client.publish.return_value = Mock(rc=mqtt.MQTT_ERR_SUCCESS)

        result = publish_command(
            client=mock_client,
            command_topic="sentinelx/g0X/cmd",
            buzzer=0,
            led="eteint"
        )

        assert result is True
        call_args = mock_client.publish.call_args
        assert '"buzzer": 0' in call_args[1]['payload']
        assert '"led": "eteint"' in call_args[1]['payload']

    def test_led_blanc(self):
        """Test command with blanc LED (both red and yellow)."""
        mock_client = Mock()
        mock_client.publish.return_value = Mock(rc=mqtt.MQTT_ERR_SUCCESS)

        result = publish_command(
            client=mock_client,
            command_topic="sentinelx/g0X/cmd",
            buzzer=0,
            led="blanc"
        )

        assert result is True
        call_args = mock_client.publish.call_args
        assert '"led": "blanc"' in call_args[1]['payload']

    @patch('src.mqtt.publisher.logger')
    def test_publish_success_logging(self, mock_logger):
        """Test logging on successful publish."""
        mock_client = Mock()
        mock_client.publish.return_value = Mock(rc=mqtt.MQTT_ERR_SUCCESS)

        publish_command(
            client=mock_client,
            command_topic="sentinelx/g0X/cmd",
            buzzer=1,
            led="rouge"
        )

        mock_logger.info.assert_called_once()
        assert "Command published" in str(mock_logger.info.call_args)

    @patch('src.mqtt.publisher.logger')
    def test_publish_failure_logging(self, mock_logger):
        """Test logging on failed publish."""
        mock_client = Mock()
        mock_client.publish.return_value = Mock(rc=mqtt.MQTT_ERR_NO_CONN)

        publish_command(
            client=mock_client,
            command_topic="sentinelx/g0X/cmd",
            buzzer=1,
            led="rouge"
        )

        mock_logger.error.assert_called_once()
        assert "Failed to publish command" in str(mock_logger.error.call_args)
