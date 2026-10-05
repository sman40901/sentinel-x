# api/src/mqtt/client.py

import logging
import os
import ssl
import time

import paho.mqtt.client as mqtt

from .handlers import (
    handle_alert,
    handle_status,
    handle_telemetry,
)


logger = logging.getLogger(
    "sentinelx.mqtt.client"
)


# =========================================================
# Configuration
# =========================================================

MQTT_HOST = os.getenv(
    "MQTT_HOST",
    "mosquitto",
)

MQTT_USE_TLS = os.getenv(
    "MQTT_USE_TLS",
    "true",
).lower() == "true"

MQTT_PORT = int(
    os.getenv(
        "MQTT_PORT",
        "8883" if MQTT_USE_TLS else "1883",
    )
)

MQTT_USERNAME = os.getenv(
    "MQTT_USERNAME",
    "api",
)

MQTT_PASSWORD = os.getenv(
    "MQTT_PASSWORD"
)

MQTT_CA_CERT = os.getenv(
    "MQTT_CA_CERT",
    "/certs/ca.crt",
)

GROUP_ID = os.getenv(
    "GROUP_ID",
    "g0X",
)


# =========================================================
# MQTT topics
# =========================================================

TOPIC_TELEMETRY = (
    f"sentinelx/{GROUP_ID}/telemetry"
)

TOPIC_ALERTS = (
    f"sentinelx/{GROUP_ID}/alerts"
)

TOPIC_STATUS = (
    f"sentinelx/{GROUP_ID}/status"
)

TOPIC_COMMAND = (
    f"sentinelx/{GROUP_ID}/cmd"
)


# =========================================================
# Callbacks
# =========================================================

def on_connect(
    client,
    userdata,
    flags,
    reason_code,
    properties,
):
    """
    Called after connecting to Mosquitto.
    """

    if reason_code != 0:
        logger.error(
            "MQTT connection failed: %s",
            reason_code,
        )
        return

    logger.info(
        "Connected to Mosquitto at %s:%s",
        MQTT_HOST,
        MQTT_PORT,
    )

    # Subscribe to inputs used by our API.

    subscriptions = [
        (TOPIC_TELEMETRY, 1),
        (TOPIC_ALERTS, 1),
        (TOPIC_STATUS, 1),
    ]

    result, message_id = client.subscribe(
        subscriptions
    )

    if result != mqtt.MQTT_ERR_SUCCESS:
        logger.error(
            "Failed to subscribe to MQTT topics"
        )

        return

    logger.info(
        "Subscribed to telemetry, alerts and status"
    )


def on_disconnect(
    client,
    userdata,
    disconnect_flags,
    reason_code,
    properties,
):
    """
    Called if connection with Mosquitto is lost
    or closed.
    """

    if reason_code == 0:
        logger.info(
            "Disconnected from Mosquitto"
        )

    else:
        logger.warning(
            "Unexpected MQTT disconnect: %s",
            reason_code,
        )


def on_message(
    client,
    userdata,
    message,
):
    """
    Called whenever a subscribed MQTT message arrives.
    """

    try:
        payload = message.payload.decode(
            "utf-8"
        )

    except UnicodeDecodeError:
        logger.error(
            "Invalid UTF-8 message | topic=%s",
            message.topic,
        )
        return

    logger.info(
        "MQTT message | topic=%s",
        message.topic,
    )

    # Route the message to the correct handler.

    if message.topic == TOPIC_TELEMETRY:

        handle_telemetry(payload)

    elif message.topic == TOPIC_ALERTS:

        handle_alert(payload)

    elif message.topic == TOPIC_STATUS:

        handle_status(payload)

    else:

        logger.warning(
            "Unhandled MQTT topic: %s",
            message.topic,
        )


# =========================================================
# Create client
# =========================================================

def create_mqtt_client() -> mqtt.Client:
    """
    Create and configure the Sentinel-X
    MQTT API client.
    """

    client = mqtt.Client(
        callback_api_version=(
            mqtt.CallbackAPIVersion.VERSION2
        ),
        client_id=(
            f"sentinelx-{GROUP_ID}-api"
        ),
        protocol=mqtt.MQTTv311,
    )

    # -------------------------
    # Authentication
    # -------------------------

    if MQTT_USERNAME:
        if not MQTT_PASSWORD:
            logger.warning(
                "MQTT_USERNAME is set but MQTT_PASSWORD is not. "
                "Authentication may fail."
            )
        client.username_pw_set(
            username=MQTT_USERNAME,
            password=MQTT_PASSWORD or "",
        )

    # -------------------------
    # TLS
    # -------------------------

    if MQTT_USE_TLS:
        client.tls_set(
            ca_certs=MQTT_CA_CERT,
            tls_version=ssl.PROTOCOL_TLS,
        )

        # Keep certificate verification enabled.
        client.tls_insecure_set(False)
    else:
        logger.warning(
            "TLS is disabled. Connection will not be encrypted."
        )

    # -------------------------
    # Callbacks
    # -------------------------

    client.on_connect = on_connect
    client.on_disconnect = on_disconnect
    client.on_message = on_message

    # -------------------------
    # Reconnection
    # -------------------------

    client.reconnect_delay_set(
        min_delay=1,
        max_delay=30,
    )

    return client


# =========================================================
# Start MQTT
# =========================================================

def start_mqtt() -> mqtt.Client:
    """
    Connect to Mosquitto and start the
    MQTT network loop in the background.
    """

    client = create_mqtt_client()

    logger.info(
        "Connecting to MQTT broker %s:%s",
        MQTT_HOST,
        MQTT_PORT,
    )

    client.connect(
        host=MQTT_HOST,
        port=MQTT_PORT,
        keepalive=60,
    )

    client.loop_start()

    # Wait for connection to be established
    max_wait = 5
    waited = 0
    while not client.is_connected() and waited < max_wait:
        time.sleep(0.5)
        waited += 0.5

    if not client.is_connected():
        logger.error(
            "Failed to connect to MQTT broker after %s seconds",
            max_wait
        )
        client.loop_stop()
        raise RuntimeError(
            f"MQTT connection failed to {MQTT_HOST}:{MQTT_PORT}"
        )

    return client


# =========================================================
# Stop MQTT
# =========================================================

def stop_mqtt(
    client: mqtt.Client
) -> None:
    """
    Gracefully stop the MQTT client.
    """

    logger.info(
        "Stopping MQTT client"
    )

    client.disconnect()
    client.loop_stop()