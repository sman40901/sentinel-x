import os
from unittest.mock import MagicMock, patch

import pytest
from fastapi.testclient import TestClient


# ============================================================
# PREPARATION DU TEST
# ============================================================
# Ce fichier teste le vrai backend utilise par Docker : api/main.py.
#
# DATABASE_URL est lu quand main.py est importe.
# Ici, on donne une fausse URL sans danger.
# Dans les tests unitaires, les appels a PostgreSQL sont remplaces
# par des mocks. On ne contacte donc pas une vraie base de donnees.
#
# API_KEY est aussi configuree pour tester les routes protegees.
os.environ.setdefault(
    "DATABASE_URL",
    "postgresql://test:test@localhost:5432/test",
)
os.environ.setdefault("API_KEY", "test-api-key")

import main


# TestClient permet d'appeler FastAPI comme un client HTTP,
# mais sans demarrer un vrai serveur web.
client = TestClient(main.app)


# ============================================================
# HEALTH - ETAT DE L'API
# ============================================================
class TestHealth:
    @patch("main.db")
    def test_health_all_ok(self, mock_db):
        # CE QU'ON FAIT :
        # On simule une base disponible et MQTT connecte.
        # Puis on appelle GET /api/v1/health.
        #
        # ATTENDU :
        # HTTP 200 et JSON : status=ok, db=true, mqtt=true.
        #
        # REEL :
        # response.status_code = code HTTP vraiment retourne par FastAPI.
        # response.json() = JSON vraiment retourne par FastAPI.
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
        # CE QU'ON FAIT : la base leve une erreur, mais MQTT est connecte.
        # ATTENDU : HTTP 200, status=degrade, db=false, mqtt=true.
        # REEL : response.json() contient l'etat calcule par l'API.
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
        # CE QU'ON FAIT : la base marche, mais MQTT est deconnecte.
        # ATTENDU : HTTP 200, status=degrade, db=true, mqtt=false.
        # REEL : response.json() est la reponse reelle de FastAPI.
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
# GET /api/v1/mesures - LECTURE DES MESURES
# ============================================================
class TestMeasurements:
    @patch("main.db")
    def test_get_measurements(self, mock_db):
        # CE QU'ON FAIT :
        # On simule deux lignes retournees par PostgreSQL.
        #
        # ATTENDU :
        # HTTP 200, deux mesures, ordre ancien -> nouveau pour le graphique.
        #
        # REEL :
        # data contient le JSON vraiment retourne par la route.
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
        data = response.json()

        assert response.status_code == 200
        assert len(data) == 2
        assert data[0]["t"] == 24.0
        assert data[1]["t"] == 24.5
        mock_db.assert_called_once()

    def test_measurement_limit_too_low(self):
        # ENTREE : limit=0.
        # ATTENDU : HTTP 422 car la limite est trop petite.
        # REEL : response.status_code.
        response = client.get("/api/v1/mesures?limit=0")
        assert response.status_code == 422

    def test_measurement_limit_too_high(self):
        # ENTREE : limit=5001.
        # ATTENDU : HTTP 422 car le maximum est 5000.
        # REEL : response.status_code.
        response = client.get("/api/v1/mesures?limit=5001")
        assert response.status_code == 422

    @patch("main.db")
    @pytest.mark.parametrize("limit", [1, 5000])
    def test_measurement_valid_boundaries(self, mock_db, limit):
        # CE QU'ON FAIT : on teste les deux limites valides : 1 et 5000.
        # ATTENDU : HTTP 200.
        # REEL : code HTTP retourne par FastAPI.
        mock_db.return_value = []
        response = client.get(f"/api/v1/mesures?limit={limit}")
        assert response.status_code == 200

    @patch("main.db")
    def test_measurements_empty_database(self, mock_db):
        # CE QU'ON FAIT : la base ne retourne aucune mesure.
        # ATTENDU : HTTP 200 et liste vide [].
        # REEL : response.json().
        mock_db.return_value = []
        response = client.get("/api/v1/mesures?limit=10")
        assert response.status_code == 200
        assert response.json() == []


# ============================================================
# GET /api/v1/alerts - LECTURE DES ALERTES
# ============================================================
class TestAlerts:
    @patch("main.db")
    def test_get_alerts(self, mock_db):
        # CE QU'ON FAIT : on simule une alerte dans PostgreSQL.
        # ATTENDU : HTTP 200 et une alerte critique de type intrus.
        # REEL : data contient le JSON retourne par l'API.
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
        data = response.json()

        assert response.status_code == 200
        assert len(data) == 1
        assert data[0]["type"] == "intrus"
        assert data[0]["niveau"] == "critique"
        mock_db.assert_called_once()

    def test_alert_limit_too_low(self):
        # ENTREE : limit=0. ATTENDU : HTTP 422. REEL : status_code.
        response = client.get("/api/v1/alerts?limit=0")
        assert response.status_code == 422

    def test_alert_limit_too_high(self):
        # ENTREE : limit=1001. ATTENDU : HTTP 422. REEL : status_code.
        response = client.get("/api/v1/alerts?limit=1001")
        assert response.status_code == 422

    @patch("main.db")
    @pytest.mark.parametrize("limit", [1, 1000])
    def test_alert_valid_boundaries(self, mock_db, limit):
        # CE QU'ON FAIT : on teste les limites valides 1 et 1000.
        # ATTENDU : HTTP 200. REEL : code HTTP retourne.
        mock_db.return_value = []
        response = client.get(f"/api/v1/alerts?limit={limit}")
        assert response.status_code == 200

    @patch("main.db")
    def test_alerts_empty_database(self, mock_db):
        # ATTENDU : une base vide donne HTTP 200 et [].
        # REEL : response.json().
        mock_db.return_value = []
        response = client.get("/api/v1/alerts?limit=20")
        assert response.status_code == 200
        assert response.json() == []


# ============================================================
# POST /api/v1/alerts - CREATION D'UNE ALERTE
# ============================================================
class TestPostAlert:
    @patch("main.publish")
    def test_post_alert_success(self, mock_publish):
        # CE QU'ON FAIT : on envoie une alerte avec une bonne cle API.
        # ATTENDU : HTTP 202 et publie=true.
        # REEL : reponse FastAPI + appel reel observe sur le mock publish.
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
        # ENTREE : mauvaise cle API.
        # ATTENDU : HTTP 401 et aucun message MQTT publie.
        # REEL : status_code et mock_publish.call_count.
        with patch.object(main, "API_KEY", "test-api-key"):
            response = client.post(
                "/api/v1/alerts",
                headers={"X-API-Key": "wrong-key"},
                json={"type": "intrus", "niveau": "critique"},
            )
        assert response.status_code == 401
        mock_publish.assert_not_called()

    @patch("main.publish")
    def test_post_alert_missing_api_key(self, mock_publish):
        # ENTREE : aucune cle API.
        # ATTENDU : HTTP 401 et aucun publish MQTT.
        with patch.object(main, "API_KEY", "test-api-key"):
            response = client.post(
                "/api/v1/alerts",
                json={"type": "intrus", "niveau": "critique"},
            )
        assert response.status_code == 401
        mock_publish.assert_not_called()

    def test_post_alert_invalid_confidence(self):
        # ENTREE : conf=1.5, valeur hors plage.
        # ATTENDU : HTTP 422. REEL : status_code.
        with patch.object(main, "API_KEY", "test-api-key"):
            response = client.post(
                "/api/v1/alerts",
                headers={"X-API-Key": "test-api-key"},
                json={"type": "intrus", "niveau": "critique", "conf": 1.5},
            )
        assert response.status_code == 422

    @patch("main.publish")
    def test_post_alert_exact_mqtt_contract(self, mock_publish):
        # CE QU'ON FAIT : on verifie le contrat API -> MQTT.
        # ATTENDU : publish recoit le topic ALERTS et exactement le JSON envoye.
        # REEL : mock_publish.call_args contient les arguments vraiment utilises.
        payload = {
            "type": "intrus",
            "niveau": "critique",
            "msg": "Personne detectee",
            "conf": 0.95,
            "source": "ia-webcam",
        }
        with patch.object(main, "API_KEY", "test-api-key"):
            response = client.post(
                "/api/v1/alerts",
                headers={"X-API-Key": "test-api-key"},
                json=payload,
            )
        assert response.status_code == 202
        mock_publish.assert_called_once_with(main.T_ALERTS, payload)


# ============================================================
# POST /api/v1/cmd - COMMANDES ESP32
# ============================================================
class TestCommands:
    @patch("main.publish")
    def test_send_buzzer_command(self, mock_publish):
        # ENTREE : buzzer=1 avec bonne cle.
        # ATTENDU : HTTP 202, publie=true, commande.buzzer=1.
        # REEL : response.json() et l'appel du mock publish.
        with patch.object(main, "API_KEY", "test-api-key"):
            response = client.post(
                "/api/v1/cmd",
                headers={"X-API-Key": "test-api-key"},
                json={"buzzer": 1},
            )
        assert response.status_code == 202
        data = response.json()
        assert data["publie"] is True
        assert data["commande"]["buzzer"] == 1
        mock_publish.assert_called_once()

    @patch("main.publish")
    def test_send_led_command(self, mock_publish):
        # ENTREE : led=rouge.
        # ATTENDU : HTTP 202 et commande.led=rouge.
        with patch.object(main, "API_KEY", "test-api-key"):
            response = client.post(
                "/api/v1/cmd",
                headers={"X-API-Key": "test-api-key"},
                json={"led": "rouge"},
            )
        assert response.status_code == 202
        assert response.json()["commande"]["led"] == "rouge"
        mock_publish.assert_called_once()

    @patch("main.publish")
    def test_empty_command(self, mock_publish):
        # ENTREE : JSON vide {}.
        # ATTENDU : HTTP 422 et aucun message MQTT.
        with patch.object(main, "API_KEY", "test-api-key"):
            response = client.post(
                "/api/v1/cmd",
                headers={"X-API-Key": "test-api-key"},
                json={},
            )
        assert response.status_code == 422
        mock_publish.assert_not_called()

    def test_invalid_led(self):
        # ENTREE : led=blue, valeur non autorisee.
        # ATTENDU : HTTP 422.
        with patch.object(main, "API_KEY", "test-api-key"):
            response = client.post(
                "/api/v1/cmd",
                headers={"X-API-Key": "test-api-key"},
                json={"led": "blue"},
            )
        assert response.status_code == 422

    @patch("main.publish")
    def test_combined_command_exact_mqtt_contract(self, mock_publish):
        # CE QU'ON FAIT : LED + buzzer dans la meme commande.
        # ATTENDU : HTTP 202 et publish(topic CMD, payload exact).
        # REEL : mock_publish.call_args.
        payload = {"buzzer": 1, "led": "rouge"}
        with patch.object(main, "API_KEY", "test-api-key"):
            response = client.post(
                "/api/v1/cmd",
                headers={"X-API-Key": "test-api-key"},
                json=payload,
            )
        assert response.status_code == 202
        mock_publish.assert_called_once_with(main.T_CMD, payload)

    @pytest.mark.parametrize("bad_value", [-1, 2])
    def test_invalid_buzzer_values(self, bad_value):
        # ENTREE : buzzer=-1 ou buzzer=2.
        # ATTENDU : HTTP 422 car seules les valeurs valides sont acceptees.
        with patch.object(main, "API_KEY", "test-api-key"):
            response = client.post(
                "/api/v1/cmd",
                headers={"X-API-Key": "test-api-key"},
                json={"buzzer": bad_value},
            )
        assert response.status_code == 422


# ============================================================
# SECURITE DE LA CLE API
# ============================================================
class TestAPIKeySecurity:
    @patch("main.publish")
    def test_empty_api_key_is_rejected(self, mock_publish):
        # ENTREE : en-tete X-API-Key vide.
        # ATTENDU : HTTP 401 et aucun publish.
        with patch.object(main, "API_KEY", "test-api-key"):
            response = client.post(
                "/api/v1/cmd",
                headers={"X-API-Key": ""},
                json={"buzzer": 1},
            )
        assert response.status_code == 401
        mock_publish.assert_not_called()

    @patch("main.publish")
    def test_api_key_is_case_sensitive(self, mock_publish):
        # ENTREE : cle proche mais avec mauvaises majuscules/minuscules.
        # ATTENDU : HTTP 401.
        with patch.object(main, "API_KEY", "Test-Key"):
            response = client.post(
                "/api/v1/cmd",
                headers={"X-API-Key": "test-key"},
                json={"buzzer": 1},
            )
        assert response.status_code == 401
        mock_publish.assert_not_called()

    @patch("main.publish")
    def test_server_without_api_key_is_unavailable(self, mock_publish):
        # CE QU'ON FAIT : on simule un serveur sans API_KEY configuree.
        # ATTENDU : HTTP 503. Une route protegee ne doit pas devenir ouverte.
        # REEL : response.status_code.
        with patch.object(main, "API_KEY", ""):
            response = client.post(
                "/api/v1/cmd",
                headers={"X-API-Key": "anything"},
                json={"buzzer": 1},
            )
        assert response.status_code == 503
        mock_publish.assert_not_called()


# ============================================================
# STOCKAGE TELEMETRIE MQTT -> POSTGRESQL
# ============================================================
class TestStoreTelemetry:
    @patch("main.db")
    def test_store_valid_telemetry(self, mock_db):
        # ENTREE : JSON MQTT valide.
        # ATTENDU : un appel DB avec (24.5, 60.0, 350.0, True, -50).
        # REEL : mock_db.call_args.args contient les valeurs preparees par main.py.
        payload = '''
        {
            "t": 24.5,
            "h": 60,
            "gaz": 350,
            "pir": 1,
            "rssi": -50
        }
        '''
        main.store_telemetry(payload)
        mock_db.assert_called_once()
        _, params = mock_db.call_args.args
        assert params == (24.5, 60.0, 350.0, True, -50)

    @patch("main.db")
    def test_invalid_json_not_stored(self, mock_db):
        # ENTREE : texte qui n'est pas du JSON.
        # ATTENDU : aucun INSERT, donc mock_db jamais appele.
        main.store_telemetry("not-json")
        mock_db.assert_not_called()

    @patch("main.db")
    def test_alternative_telemetry_names(self, mock_db):
        # CE QU'ON FAIT : on teste les alias temp/hum/gas/motion.
        # ATTENDU : memes valeurs normalisees pour la base.
        payload = '{"temp": 21.5, "hum": 45, "gas": 200, "motion": 1}'
        main.store_telemetry(payload)
        _, params = mock_db.call_args.args
        assert params == (21.5, 45.0, 200.0, True, None)

    @patch("main.db")
    def test_empty_telemetry_stores_null_values(self, mock_db):
        # ENTREE : objet JSON vide {}.
        # ATTENDU : les champs absents deviennent None.
        # REEL : params envoyes a db().
        main.store_telemetry("{}")
        _, params = mock_db.call_args.args
        assert params == (None, None, None, None, None)

    @patch("main.db")
    def test_non_object_telemetry_not_stored(self, mock_db):
        # ENTREE : JSON valide mais de type liste.
        # ATTENDU : aucun stockage car une telemetrie doit etre un objet.
        main.store_telemetry("[1, 2, 3]")
        mock_db.assert_not_called()


# ============================================================
# STOCKAGE ALERTES MQTT -> POSTGRESQL
# ============================================================
class TestStoreAlert:
    @patch("main.db")
    def test_store_valid_alert(self, mock_db):
        # ENTREE : alerte MQTT complete.
        # ATTENDU : INSERT avec les 5 valeurs exactes.
        # REEL : params envoyes au mock DB.
        payload = '''
        {
            "type": "intrus",
            "niveau": "critique",
            "msg": "Detection",
            "conf": 0.95,
            "source": "ia-webcam"
        }
        '''
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

    @patch("main.db")
    def test_plain_text_alert_uses_safe_defaults(self, mock_db):
        # ENTREE : texte simple, pas du JSON.
        # ATTENDU : l'application garde l'alerte avec des valeurs par defaut.
        # REEL : params vraiment envoyes a la base.
        main.store_alert("plain text alert")
        _, params = mock_db.call_args.args
        assert params == (
            "alerte",
            "critique",
            "plain text alert",
            None,
            None,
        )

    @patch("main.db")
    def test_alert_aliases(self, mock_db):
        # ENTREE : level/message au lieu de niveau/msg.
        # ATTENDU : les alias sont compris par le backend.
        payload = (
            '{"type":"system","level":"attention",'
            '"message":"Test alias","conf":0.5,"source":"test"}'
        )
        main.store_alert(payload)
        _, params = mock_db.call_args.args
        assert params == (
            "system",
            "attention",
            "Test alias",
            0.5,
            "test",
        )


# ============================================================
# ROUTAGE DES MESSAGES MQTT
# ============================================================
class TestMQTTMessageRouting:
    @patch("main.store_telemetry")
    def test_telemetry_topic_calls_store_telemetry(self, mock_store):
        # CE QU'ON FAIT : on simule un message venant du topic telemetry.
        # ATTENDU : store_telemetry recoit exactement le payload decode.
        # REEL : mock_store.call_args.
        message = MagicMock()
        message.topic = main.T_TELEMETRY
        message.payload = b'{"t": 24.5}'
        message.retain = False
        main.on_message(None, None, message)
        mock_store.assert_called_once_with('{"t": 24.5}')

    @patch("main.store_alert")
    def test_alert_topic_calls_store_alert(self, mock_store):
        # ATTENDU : un message du topic alerts va vers store_alert.
        message = MagicMock()
        message.topic = main.T_ALERTS
        message.payload = b'{"type":"intrus"}'
        message.retain = False
        main.on_message(None, None, message)
        mock_store.assert_called_once_with('{"type":"intrus"}')

    @patch("main.store_alert")
    def test_retained_status_is_ignored(self, mock_store):
        # CE QU'ON FAIT : message status MQTT avec retain=true.
        # ATTENDU : pas de nouvelle alerte dans la base.
        message = MagicMock()
        message.topic = main.T_STATUS
        message.payload = b"online"
        message.retain = True
        main.on_message(None, None, message)
        mock_store.assert_not_called()

    @patch("main.store_telemetry")
    @patch("main.store_alert")
    def test_unknown_topic_is_ignored(self, mock_alert, mock_telemetry):
        # ENTREE : topic que Sentinel-X ne connait pas.
        # ATTENDU : aucun stockage telemetrie ou alerte.
        message = MagicMock()
        message.topic = "sentinelx/g02/unknown"
        message.payload = b"hello"
        message.retain = False
        main.on_message(None, None, message)
        mock_telemetry.assert_not_called()
        mock_alert.assert_not_called()

    @patch("main.store_telemetry", side_effect=Exception("Database down"))
    def test_storage_error_does_not_leave_on_message(self, mock_store):
        # CE QU'ON FAIT : la base tombe pendant un message MQTT.
        # ATTENDU : on_message gere l'erreur et ne la renvoie pas au broker.
        # REEL : si une exception sort de on_message, ce test echoue.
        message = MagicMock()
        message.topic = main.T_TELEMETRY
        message.payload = b'{"t":24}'
        message.retain = False
        main.on_message(None, None, message)
        mock_store.assert_called_once()


# ============================================================
# CONNEXION MQTT
# ============================================================
class TestMQTTConnection:
    def test_on_connect_success_subscribes_topics(self):
        # CE QU'ON FAIT : on simule une connexion MQTT reussie.
        # ATTENDU : mqtt_ok=true et abonnement aux 3 topics attendus.
        # REEL : mqtt_ok + mqtt_client.subscribe.call_args.
        mqtt_client = MagicMock()
        reason_code = MagicMock()
        reason_code.is_failure = False
        main.mqtt_ok.clear()

        main.on_connect(mqtt_client, None, None, reason_code, None)

        assert main.mqtt_ok.is_set()
        mqtt_client.subscribe.assert_called_once_with([
            (main.T_TELEMETRY, 0),
            (main.T_ALERTS, 1),
            (main.T_STATUS, 1),
        ])
        main.mqtt_ok.clear()

    def test_on_connect_failure_does_not_subscribe(self):
        # CE QU'ON FAIT : on simule une connexion MQTT refusee.
        # ATTENDU : mqtt_ok=false et aucun abonnement.
        mqtt_client = MagicMock()
        reason_code = MagicMock()
        reason_code.is_failure = True
        main.mqtt_ok.clear()

        main.on_connect(mqtt_client, None, None, reason_code, None)

        assert not main.mqtt_ok.is_set()
        mqtt_client.subscribe.assert_not_called()

    def test_on_disconnect_clears_mqtt_state(self):
        # CE QU'ON FAIT : MQTT etait connecte puis se deconnecte.
        # ATTENDU : mqtt_ok devient false.
        main.mqtt_ok.set()
        main.on_disconnect(MagicMock(), None, None, MagicMock(), None)
        assert not main.mqtt_ok.is_set()


# ============================================================
# FONCTION publish() - ENVOI MQTT
# ============================================================
class TestPublish:
    def test_publish_when_mqtt_offline_returns_503(self):
        # CE QU'ON FAIT : on essaye de publier quand MQTT est hors ligne.
        # ATTENDU : erreur HTTP 503.
        # REEL : exc.value.status_code.
        main.mqtt_ok.clear()
        with pytest.raises(Exception) as exc:
            main.publish(main.T_CMD, {"buzzer": 1})
        assert getattr(exc.value, "status_code", None) == 503

    @patch.object(main.mqtt_client, "publish")
    def test_publish_success_uses_qos_1(self, mock_publish):
        # CE QU'ON FAIT : MQTT est connecte et publish reussit.
        # ATTENDU : bon topic, bon JSON et qos=1.
        # REEL : mock_publish.call_args.
        result = MagicMock()
        result.rc = main.mqtt.MQTT_ERR_SUCCESS
        mock_publish.return_value = result
        main.mqtt_ok.set()

        main.publish(main.T_CMD, {"buzzer": 1})

        args = mock_publish.call_args.args
        kwargs = mock_publish.call_args.kwargs
        assert args[0] == main.T_CMD
        assert main.json.loads(args[1]) == {"buzzer": 1}
        assert kwargs["qos"] == 1
        main.mqtt_ok.clear()

    @patch.object(main.mqtt_client, "publish")
    def test_publish_broker_error_returns_503(self, mock_publish):
        # CE QU'ON FAIT : le client MQTT retourne un code d'erreur.
        # ATTENDU : HTTP 503.
        result = MagicMock()
        result.rc = 99
        mock_publish.return_value = result
        main.mqtt_ok.set()

        with pytest.raises(Exception) as exc:
            main.publish(main.T_CMD, {"buzzer": 1})

        assert getattr(exc.value, "status_code", None) == 503
        main.mqtt_ok.clear()
