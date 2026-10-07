import os
from unittest.mock import MagicMock, patch

import pytest
from fastapi.testclient import TestClient


# main.py reads DATABASE_URL immediately when imported.
# Give it a harmless value because DB access is mocked in unit tests.
os.environ.setdefault(
    "DATABASE_URL",
    "postgresql://test:test@localhost:5432/test",
)
os.environ.setdefault("API_KEY", "test-api-key")

import main


client = TestClient(main.app)


# ============================================================
# HEALTH
# ============================================================

class TestHealth:

    @patch("main.db")
    def test_health_all_ok(self, mock_db):
        mock_db.return_value = None
        main.mqtt_ok.set()

        response = client.get("/api/v1/health")

        assert response.status_code == 200
        assert response.json() == {
            "status": "ok",
            "db": True,
            "mqtt": True,
        }

        main.mqtt_ok.clear()

    @patch("main.db")
    def test_health_database_down(self, mock_db):
        mock_db.side_effect = Exception("Database unavailable")
        main.mqtt_ok.set()

        response = client.get("/api/v1/health")

        assert response.status_code == 200
        assert response.json() == {
            "status": "degrade",
            "db": False,
            "mqtt": True,
        }

        main.mqtt_ok.clear()

    @patch("main.db")
    def test_health_mqtt_down(self, mock_db):
        mock_db.return_value = None
        main.mqtt_ok.clear()

        response = client.get("/api/v1/health")

        assert response.status_code == 200
        assert response.json() == {
            "status": "degrade",
            "db": True,
            "mqtt": False,
        }


# ============================================================
# GET MEASUREMENTS
# ============================================================

class TestMeasurements:

    @patch("main.db")
    def test_get_measurements(self, mock_db):
        mock_db.return_value = [
            {
                "ts": "2026-10-06T10:00:02Z",
                "t": 24.5,
                "h": 60.0,
                "gaz": 350.0,
                "pir": False,
                "rssi": -50,
            },
            {
                "ts": "2026-10-06T10:00:00Z",
                "t": 24.0,
                "h": 61.0,
                "gaz": 340.0,
                "pir": True,
                "rssi": -51,
            },
        ]

        response = client.get("/api/v1/mesures?limit=2")

        assert response.status_code == 200

        data = response.json()

        assert len(data) == 2

        # main.py reverses DB results for graph chronological order.
        assert data[0]["t"] == 24.0
        assert data[1]["t"] == 24.5

        mock_db.assert_called_once()

    def test_measurement_limit_too_low(self):
        response = client.get("/api/v1/mesures?limit=0")

        assert response.status_code == 422

    def test_measurement_limit_too_high(self):
        response = client.get("/api/v1/mesures?limit=5001")

        assert response.status_code == 422


# ============================================================
# GET ALERTS
# ============================================================

class TestAlerts:

    @patch("main.db")
    def test_get_alerts(self, mock_db):
        mock_db.return_value = [
            {
                "ts": "2026-10-06T10:00:00Z",
                "type": "intrus",
                "niveau": "critique",
                "msg": "Detection",
                "conf": 0.95,
                "source": "ia-webcam",
            }
        ]

        response = client.get("/api/v1/alerts?limit=20")

        assert response.status_code == 200

        data = response.json()

        assert len(data) == 1
        assert data[0]["type"] == "intrus"
        assert data[0]["niveau"] == "critique"

        mock_db.assert_called_once()

    def test_alert_limit_too_low(self):
        response = client.get("/api/v1/alerts?limit=0")

        assert response.status_code == 422

    def test_alert_limit_too_high(self):
        response = client.get("/api/v1/alerts?limit=1001")

        assert response.status_code == 422


# ============================================================
# POST ALERT
# ============================================================

class TestPostAlert:

    @patch("main.publish")
    def test_post_alert_success(self, mock_publish):
        with patch.object(main, "API_KEY", "test-api-key"):

            response = client.post(
                "/api/v1/alerts",
                headers={"X-API-Key": "test-api-key"},
                json={
                    "type": "intrus",
                    "niveau": "critique",
                    "msg": "Personne detectee",
                    "conf": 0.95,
                    "source": "ia-webcam",
                },
            )

        assert response.status_code == 202
        assert response.json()["publie"] is True

        mock_publish.assert_called_once()

    @patch("main.publish")
    def test_post_alert_wrong_api_key(self, mock_publish):
        with patch.object(main, "API_KEY", "test-api-key"):

            response = client.post(
                "/api/v1/alerts",
                headers={"X-API-Key": "wrong-key"},
                json={
                    "type": "intrus",
                    "niveau": "critique",
                },
            )

        assert response.status_code == 401
        mock_publish.assert_not_called()

    @patch("main.publish")
    def test_post_alert_missing_api_key(self, mock_publish):
        with patch.object(main, "API_KEY", "test-api-key"):

            response = client.post(
                "/api/v1/alerts",
                json={
                    "type": "intrus",
                    "niveau": "critique",
                },
            )

        assert response.status_code == 401
        mock_publish.assert_not_called()

    def test_post_alert_invalid_confidence(self):
        with patch.object(main, "API_KEY", "test-api-key"):

            response = client.post(
                "/api/v1/alerts",
                headers={"X-API-Key": "test-api-key"},
                json={
                    "type": "intrus",
                    "niveau": "critique",
                    "conf": 1.5,
                },
            )

        assert response.status_code == 422


# ============================================================
# POST COMMAND
# ============================================================

class TestCommands:

    @patch("main.publish")
    def test_send_buzzer_command(self, mock_publish):
        with patch.object(main, "API_KEY", "test-api-key"):

            response = client.post(
                "/api/v1/cmd",
                headers={"X-API-Key": "test-api-key"},
                json={
                    "buzzer": 1,
                },
            )

        assert response.status_code == 202

        data = response.json()

        assert data["publie"] is True
        assert data["commande"]["buzzer"] == 1

        mock_publish.assert_called_once()

    @patch("main.publish")
    def test_send_led_command(self, mock_publish):
        with patch.object(main, "API_KEY", "test-api-key"):

            response = client.post(
                "/api/v1/cmd",
                headers={"X-API-Key": "test-api-key"},
                json={
                    "led": "rouge",
                },
            )

        assert response.status_code == 202

        data = response.json()

        assert data["commande"]["led"] == "rouge"

        mock_publish.assert_called_once()

    @patch("main.publish")
    def test_empty_command(self, mock_publish):
        with patch.object(main, "API_KEY", "test-api-key"):

            response = client.post(
                "/api/v1/cmd",
                headers={"X-API-Key": "test-api-key"},
                json={},
            )

        assert response.status_code == 422
        mock_publish.assert_not_called()

    def test_invalid_led(self):
        with patch.object(main, "API_KEY", "test-api-key"):

            response = client.post(
                "/api/v1/cmd",
                headers={"X-API-Key": "test-api-key"},
                json={
                    "led": "blue",
                },
            )

        assert response.status_code == 422


# ============================================================
# TELEMETRY STORAGE
# ============================================================

class TestStoreTelemetry:

    @patch("main.db")
    def test_store_valid_telemetry(self, mock_db):
        payload = """
        {
            "t": 24.5,
            "h": 60,
            "gaz": 350,
            "pir": 1,
            "rssi": -50
        }
        """

        main.store_telemetry(payload)

        mock_db.assert_called_once()

        _, params = mock_db.call_args.args

        assert params == (
            24.5,
            60.0,
            350.0,
            True,
            -50,
        )

    @patch("main.db")
    def test_invalid_json_not_stored(self, mock_db):
        main.store_telemetry("not-json")

        mock_db.assert_not_called()


# ============================================================
# ALERT STORAGE
# ============================================================

class TestStoreAlert:

    @patch("main.db")
    def test_store_valid_alert(self, mock_db):
        payload = """
        {
            "type": "intrus",
            "niveau": "critique",
            "msg": "Detection",
            "conf": 0.95,
            "source": "ia-webcam"
        }
        """

        main.store_alert(payload)

        mock_db.assert_called_once()

        _, params = mock_db.call_args.args

        assert params == (
            "intrus",
            "critique",
            "Detection",
            0.95,
            "ia-webcam",
        )