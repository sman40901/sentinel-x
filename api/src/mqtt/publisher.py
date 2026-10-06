# api/src/mqtt/publisher.py

import json
import logging

import paho.mqtt.client as mqtt


logger = logging.getLogger(
    "sentinelx.mqtt.publisher"
)


def publish_command(
    client: mqtt.Client,
    command_topic: str,
    buzzer: int,
    led: str,
) -> bool:
    """
    Publier une commande vers l'ESP32.

    Exemple de charge utile :

    {
        "buzzer": 1,
        "led": "rouge"
    }
    """

    # -------------------------
    # Valider la commande
    # -------------------------

    if buzzer not in (0, 1):
        raise ValueError(
            "buzzer must be 0 or 1"
        )

    if not isinstance(led, str):
        raise ValueError(
            "led must be a string"
        )

    valid_led_colors = (
        "rouge",
        "vert",
        "bleu",
        "jaune",
        "blanc",
        "eteint",
    )

    if led not in valid_led_colors:
        raise ValueError(
            f"led must be one of: {', '.join(valid_led_colors)}"
        )

    command = {
        "buzzer": buzzer,
        "led": led,
    }

    payload = json.dumps(command)

    # -------------------------
    # Publier le message MQTT
    # -------------------------

    result = client.publish(
        topic=command_topic,
        payload=payload,
        qos=1,
        retain=False,
    )

    if result.rc != mqtt.MQTT_ERR_SUCCESS:
        logger.error(
            "Failed to publish command. MQTT rc=%s",
            result.rc
        )

        return False

    logger.info(
        "Command published | topic=%s | payload=%s",
        command_topic,
        payload,
    )

    return True