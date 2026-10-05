# api/src/mqtt/handlers.py

import json
import logging


logger = logging.getLogger("sentinelx.mqtt.handlers")


def handle_telemetry(payload: str) -> None:
    """
    Traiter les données de télémétrie reçues de l'ESP32.

    Charge utile attendue :
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

        # Validation de base des types
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

        # À FAIRE :
        # Enregistrer la télémétrie dans PostgreSQL.
        #
        # Exemple pour plus tard :
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
    Traiter une alerte provenant de l'ESP32 ou de l'IA.

    Exemple attendu :
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

        # À FAIRE :
        # Enregistrer l'alerte dans PostgreSQL.
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
    Traiter l'état de connexion de l'ESP32.

    Valeur attendue :
        online

    ou :
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

    # À FAIRE :
    # Mettre à jour l'état de l'ESP32 dans PostgreSQL.
    #
    # device_service.update_status(status)