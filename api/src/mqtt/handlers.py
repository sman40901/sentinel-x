# api/src/mqtt/handlers.py

import json
import logging

logger = logging.getLogger("sentinelx.mqtt.handlers")


def handle_telemetry(payload: str) -> None:
    """
    Process telemetry received from the ESP32.

    Expected payload:
    {
        "t": 24.1,
        "h": 48,
        "gaz": 312,
        "pir": 0
    }
    """

    try:
        data = json.loads(payload)

        required_fields = ("t", "h", "gaz", "pir")

        for field in required_fields:
            if field not in data:
                raise ValueError(
                    f"Missing telemetry field: {field}"
                )

        # Basic type validation
        if not isinstance(data["t"], (int, float)):
            raise ValueError("'t' must be a number")

        if not isinstance(data["h"], (int, float)):
            raise ValueError("'h' must be a number")

        if not isinstance(data["gaz"], (int, float)):
            raise ValueError("'gaz' must be a number")

        if data["pir"] not in (0, 1):
            raise ValueError("'pir' must be 0 or 1")

        telemetry = {
            "temperature": data["t"],
            "humidity": data["h"],
            "gas": data["gaz"],
            "motion_detected": bool(data["pir"]),
        }

        logger.info("Telemetry received: %s", telemetry)

        # TODO:
        # Save telemetry into PostgreSQL.
        #
        # Example later:
        #
        # telemetry_service.save(telemetry)

    except json.JSONDecodeError:
        logger.error(
            "Invalid telemetry JSON: %s",
            payload
        )

    except (ValueError, TypeError) as exc:
        logger.error(
            "Invalid telemetry payload: %s",
            exc
        )


def handle_alert(payload: str) -> None:
    """
    Process an alert coming from ESP32 or AI.

    Expected example:
    {
        "type": "gaz",
        "niveau": "critique"
    }
    """

    try:
        data = json.loads(payload)

        if "type" not in data:
            raise ValueError(
                "Missing alert field: type"
            )

        if "niveau" not in data:
            raise ValueError(
                "Missing alert field: niveau"
            )

        if not isinstance(data["type"], str):
            raise ValueError(
                "'type' must be a string"
            )

        if not isinstance(data["niveau"], str):
            raise ValueError(
                "'niveau' must be a string"
            )

        alert = {
            "type": data["type"],
            "level": data["niveau"],
        }

        logger.warning(
            "Alert received: %s",
            alert
        )

        # TODO:
        # Save alert into PostgreSQL.
        #
        # alert_service.save(alert)

    except json.JSONDecodeError:
        logger.error(
            "Invalid alert JSON: %s",
            payload
        )

    except (ValueError, TypeError) as exc:
        logger.error(
            "Invalid alert payload: %s",
            exc
        )


def handle_status(payload: str) -> None:
    """
    Process ESP32 connection status.

    Expected:
        online

    or:
        offline
    """

    status = payload.strip().lower()

    if status not in ("online", "offline"):
        logger.error(
            "Invalid ESP32 status: %s",
            payload
        )
        return

    logger.info(
        "ESP32 status: %s",
        status
    )

    # TODO:
    # Update ESP32 status in PostgreSQL.
    #
    # device_service.update_status(status)