# api/src/mqtt/handlers.py

import json
import logging


logger = logging.getLogger("sentinelx.mqtt.handlers")


def handle_telemetry(payload: str) -> None:
    """
    Traiter les données de télémétrie reçues du boîtier ESP8266.

    Charge utile attendue :
    {
        "t": 24.1,          # null si le DHT22 ne répond pas
        "h": 48,            # null si le DHT22 ne répond pas
        "gaz": 312,         # ADC brut 0-1023 (ESP8266), PAS 0-4095
        "pir": 0,
        # champs optionnels ajoutés par le firmware ESP8266
        "gaz_v": 1.98,      # volts à la sortie AO du capteur
        "gaz_d": 0.12,      # écart en volts par rapport à la baseline
        "pres": 4,          # score de présence WiFi
        "assoc": 1,         # appareils associés au point d'accès du boîtier
        "sniff": 3,         # MAC uniques vues en sniff (gonflé par la
                            # randomisation : niveau d'activité, pas un décompte)
        "tilt": 0,          # boîtier incliné/choqué
        "state": "green",   # green | yellow | red
        "heap": 14200
    }

    't' et 'h' peuvent valoir null : le DHT22 rate régulièrement une lecture, et
    rejeter tout le message dans ce cas jetterait aussi les données de gaz et de
    mouvement du même coup.
    """

    try:
        data = json.loads(payload)

        required_fields = ("t", "h", "gaz", "pir")

        for field in required_fields:
            if field not in data:
                raise ValueError(
                    f"Missing telemetry field: {field}"
                )

        # Validation de base des types.
        # None est accepté pour t et h (capteur indisponible), mais une chaîne
        # ou un booléen reste une erreur de format.
        for field in ("t", "h"):
            value = data[field]
            if value is not None and not isinstance(value, (int, float)):
                raise ValueError(f"'{field}' must be a number or null")
            if isinstance(value, bool):
                raise ValueError(f"'{field}' must be a number or null")

        if not isinstance(data["gaz"], (int, float)) or isinstance(data["gaz"], bool):
            raise ValueError("'gaz' must be a number")

        if data["pir"] not in (0, 1):
            raise ValueError("'pir' must be 0 or 1")

        telemetry = {
            "temperature": data["t"],
            "humidity": data["h"],
            "gas": data["gaz"],
            "motion_detected": bool(data["pir"]),
            # Optionnels : absents si le firmware est compilé sans ces modules.
            "gas_volts": data.get("gaz_v"),
            "gas_delta": data.get("gaz_d"),
            "presence_score": data.get("pres"),
            "devices_associated": data.get("assoc"),
            "devices_sniffed": data.get("sniff"),
            "tampered": bool(data["tilt"]) if "tilt" in data else None,
            "threat_state": data.get("state"),
            "free_heap": data.get("heap"),
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