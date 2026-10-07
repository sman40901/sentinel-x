import threading
import time

from config import CONFIG
from mqtt_client import MQTTClient
from predictive.predictive import PredictiveAnalyzer
from risk_engine import RiskEngine
from vision.vision import VisionAnalyzer


class SentinelAI:
    def __init__(self):
        self.mqtt = MQTTClient(on_telemetry=self.on_telemetry)

        self.risk = RiskEngine(
            window_seconds=CONFIG.fusion_window_seconds
        )

        self.predictive = PredictiveAnalyzer(
            on_event=self.on_ai_event
        )

        self.vision = VisionAnalyzer(
            on_event=self.on_ai_event
        )

    def on_telemetry(self, payload: dict):
        # Analyse prédictive.
        self.predictive.process(payload)

        # Le PIR sert aussi à la corrélation multimodale.
        try:
            if int(payload.get("pir", 0)) == 1:
                self.risk.add_event("pir", 0.70)
                self._publish_fused_if_needed()
        except (TypeError, ValueError):
            pass

    def on_ai_event(self, event: dict):
        event_type = event.get("event_type")

        if event_type == "unknown_person":
            self.risk.add_event(
                "unknown_person",
                float(event.get("score", 1.0)),
            )

        elif event_type == "environment_anomaly":
            self.risk.add_event(
                "environment_anomaly",
                float(event.get("score", 0.0)),
            )

        elif event_type == "pir":
            self.risk.add_event(
                "pir",
                float(event.get("score", 0.7)),
            )

        # L'événement brut est toujours envoyé au dashboard/API.
        self.publish_alert(
            {
                "source": "ia",
                **event,
            }
        )

        self._publish_fused_if_needed()

    def _publish_fused_if_needed(self):
        fused = self.risk.evaluate()

        if fused is None:
            return

        self.publish_alert(
            {
                "source": "ia-fusion",
                "event_type": "correlation",
                **fused,
            }
        )

    def publish_alert(self, payload: dict):
        self.mqtt.publish_alert(payload)

    def start(self):
        self.mqtt.connect()

        vision_thread = threading.Thread(
            target=self.vision.run,
            daemon=True,
            name="vision",
        )
        vision_thread.start()

        print("[SENTINEL-X IA] Système démarré.")

        try:
            while vision_thread.is_alive():
                time.sleep(1)
        except KeyboardInterrupt:
            print("\n[SENTINEL-X IA] Arrêt demandé.")
        finally:
            self.mqtt.stop()


if __name__ == "__main__":
    SentinelAI().start()
