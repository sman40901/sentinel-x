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

ENVIRONMENT = os.getenv(
    "ENVIRONMENT",
    "test",
).lower()

MQTT_HOST = os.getenv(
    "MQTT_HOST",
    "192.168.1.50",
)

# Définir les valeurs par défaut selon l'environnement
if ENVIRONMENT == "prod":
    MQTT_USE_TLS_DEFAULT = "true"
    MQTT_PORT_DEFAULT = "8883"
    MQTT_USERNAME_DEFAULT = "esp32"
    MQTT_PASSWORD_DEFAULT = "your_mqtt_password"
else:
    MQTT_USE_TLS_DEFAULT = "false"
    MQTT_PORT_DEFAULT = "1883"
    MQTT_USERNAME_DEFAULT = "esp32"
    MQTT_PASSWORD_DEFAULT = "your_mqtt_password"

MQTT_USE_TLS = os.getenv(
    "MQTT_USE_TLS",
    MQTT_USE_TLS_DEFAULT,
).lower() == "true"

MQTT_PORT = int(
    os.getenv(
        "MQTT_PORT",
        MQTT_PORT_DEFAULT,
    )
)

MQTT_USERNAME = os.getenv(
    "MQTT_USERNAME",
    MQTT_USERNAME_DEFAULT,
)

MQTT_PASSWORD = os.getenv(
    "MQTT_PASSWORD",
    MQTT_PASSWORD_DEFAULT,
)

MQTT_CA_CERT = os.getenv(
    "MQTT_CA_CERT",
    "/certs/ca.crt",
)

GROUP_ID = os.getenv(
    "GROUP_ID",
    "g02",
)


# =========================================================
# Topics MQTT
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
# Fonctions de rappel
# =========================================================

def on_connect(
    client,
    userdata,
    flags,
    reason_code,
    properties,
):
    """
    Appelée après la connexion à Mosquitto.
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

    # S'abonner aux entrées utilisées par notre API.

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
    Appelée si la connexion avec Mosquitto est perdue
    ou fermée.
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
    Appelée à chaque fois qu'un message MQTT
    provenant d'un topic souscrit est reçu.
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

    # Rediriger le message vers le gestionnaire approprié.

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
# Création du client
# =========================================================

def create_mqtt_client() -> mqtt.Client:
    """
    Créer et configurer le client MQTT
    de l'API Sentinel-X.
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
    # Authentification
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

        # Maintenir la vérification du certificat activée.
        client.tls_insecure_set(False)
    else:
        logger.warning(
            "TLS is disabled. Connection will not be encrypted."
        )

    # -------------------------
    # Fonctions de rappel
    # -------------------------

    client.on_connect = on_connect
    client.on_disconnect = on_disconnect
    client.on_message = on_message

    # -------------------------
    # Reconnexion
    # -------------------------

    client.reconnect_delay_set(
        min_delay=1,
        max_delay=30,
    )

    return client


# =========================================================
# Démarrage de MQTT
# =========================================================

def start_mqtt() -> mqtt.Client:
    """
    Se connecter à Mosquitto et démarrer
    la boucle réseau MQTT en arrière-plan.
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

    # Attendre que la connexion soit établie
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
# Arrêt de MQTT
# =========================================================

def stop_mqtt(
    client: mqtt.Client
) -> None:
    """
    Arrêter proprement le client MQTT.
    """

    logger.info(
        "Stopping MQTT client"
    )

    client.disconnect()
    client.loop_stop()