import json
import ssl
import threading
from typing import Callable, Optional

import paho.mqtt.client as mqtt

from config import CONFIG


class MQTTClient:
    def __init__(
        self,
        on_telemetry: Optional[Callable[[dict], None]] = None,
    ):
        self.on_telemetry = on_telemetry
        self._connected = threading.Event()

        self.client = mqtt.Client(
            callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
            client_id=f"sentinelx-ia-{CONFIG.group_id}",
            protocol=mqtt.MQTTv5,
        )

        if CONFIG.mqtt_username:
            self.client.username_pw_set(
                CONFIG.mqtt_username,
                CONFIG.mqtt_password,
            )

        self._configure_tls()

        self.client.on_connect = self._on_connect
        self.client.on_disconnect = self._on_disconnect
        self.client.on_message = self._on_message

    def _configure_tls(self):
        ca = CONFIG.mqtt_ca_cert
        cert = CONFIG.mqtt_client_cert
        key = CONFIG.mqtt_client_key

        if CONFIG.mqtt_tls_verify:
            self.client.tls_set(
                ca_certs=ca,
                certfile=cert,
                keyfile=key,
                tls_version=ssl.PROTOCOL_TLS_CLIENT,
                cert_reqs=ssl.CERT_REQUIRED,
            )
        else:
            # Only for local debugging. Do not use in the final demo.
            self.client.tls_set(
                ca_certs=None,
                certfile=cert if cert else None,
                keyfile=key if key else None,
                tls_version=ssl.PROTOCOL_TLS_CLIENT,
                cert_reqs=ssl.CERT_NONE,
            )
            self.client.tls_insecure_set(True)

    def _on_connect(self, client, userdata, flags, reason_code, properties):
        if reason_code == 0:
            self._connected.set()
            client.subscribe(CONFIG.telemetry, qos=1)
            print(f"[MQTT] Connecté à {CONFIG.mqtt_host}:{CONFIG.mqtt_port}")
            print(f"[MQTT] Abonné à {CONFIG.telemetry}")
        else:
            print(f"[MQTT] Échec connexion : {reason_code}")

    def _on_disconnect(self, client, userdata, disconnect_flags, reason_code, properties):
        self._connected.clear()
        print(f"[MQTT] Déconnexion : {reason_code}")

    def _on_message(self, client, userdata, message):
        if message.topic != CONFIG.telemetry:
            return

        try:
            payload = json.loads(message.payload.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            print(f"[MQTT] Payload invalide : {exc}")
            return

        if self.on_telemetry:
            self.on_telemetry(payload)

    def connect(self):
        self.client.connect(
            CONFIG.mqtt_host,
            CONFIG.mqtt_port,
            keepalive=60,
        )
        self.client.loop_start()

    def publish_alert(self, payload: dict):
        data = json.dumps(payload, ensure_ascii=False)
        result = self.client.publish(
            CONFIG.alerts,
            data,
            qos=1,
            retain=False,
        )

        if result.rc != mqtt.MQTT_ERR_SUCCESS:
            print(f"[MQTT] Échec publication : {result.rc}")
        else:
            print(f"[ALERT] {data}")

    def stop(self):
        self.client.loop_stop()
        self.client.disconnect()
