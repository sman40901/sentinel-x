# api/src/main.py

import logging
import os
from contextlib import asynccontextmanager

from fastapi import FastAPI, Request

from .mqtt.client import start_mqtt, stop_mqtt


logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s | %(levelname)s | %(name)s | %(message)s",
)

logger = logging.getLogger("sentinelx.api")

ENVIRONMENT = os.getenv("ENVIRONMENT", "test").lower()
logger.info(f"Environment: {ENVIRONMENT.upper()}")


# Cette variable contiendra notre client MQTT.
mqtt_client = None


@asynccontextmanager
async def lifespan(app: FastAPI):
    """
    Appelée lorsque l'API démarre et s'arrête.
    """

    global mqtt_client

    # -----------------------------------------
    # Démarrage de l'API
    # -----------------------------------------

    logger.info("Starting Sentinel-X API")

    mqtt_client = start_mqtt()

    # Rendre le client MQTT accessible
    # aux routes REST ultérieurement.
    app.state.mqtt_client = mqtt_client

    logger.info("MQTT client started")

    yield

    # -----------------------------------------
    # Arrêt de l'API
    # -----------------------------------------

    logger.info("Stopping Sentinel-X API")

    if mqtt_client is not None:
        stop_mqtt(mqtt_client)

    logger.info("Sentinel-X API stopped")


app = FastAPI(
    title="Sentinel-X API",
    version="1.0.0",
    lifespan=lifespan,
)


@app.get("/api/v1/health")
def health(request: Request):
    """
    Endpoint simple permettant de vérifier
    que l'API REST fonctionne.
    """

    mqtt_connected = (
        request.app.state.mqtt_client.is_connected()
        if request.app.state.mqtt_client
        else False
    )

    return {
        "status": "ok",
        "mqtt_connected": mqtt_connected
    }