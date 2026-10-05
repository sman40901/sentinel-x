# api/src/main.py

import logging
from contextlib import asynccontextmanager

from fastapi import FastAPI, Request

from .mqtt.client import start_mqtt, stop_mqtt


logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s | %(levelname)s | %(name)s | %(message)s",
)

logger = logging.getLogger("sentinelx.api")


# This variable will hold our MQTT client.
mqtt_client = None


@asynccontextmanager
async def lifespan(app: FastAPI):
    """
    Called when the API starts and stops.
    """

    global mqtt_client

    # -----------------------------------------
    # API startup
    # -----------------------------------------

    logger.info("Starting Sentinel-X API")

    mqtt_client = start_mqtt()

    # Make the MQTT client accessible to
    # REST routes later.
    app.state.mqtt_client = mqtt_client

    logger.info("MQTT client started")

    yield

    # -----------------------------------------
    # API shutdown
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
    Simple endpoint to verify that
    the REST API is running.
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