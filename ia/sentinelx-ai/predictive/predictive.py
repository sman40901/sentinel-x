import time
from typing import Callable, Optional

from config import CONFIG
from predictive.features import FeatureBuffer, Telemetry
from predictive.model import PredictiveModel


class PredictiveAnalyzer:
    def __init__(
        self,
        on_event: Optional[Callable[[dict], None]] = None,
    ):
        self.features = FeatureBuffer(CONFIG.predictive_window)
        self.model = PredictiveModel(
            min_samples=CONFIG.predictive_min_samples,
            model_path="data/isolation_forest.joblib",
        )
        self.on_event = on_event
        self.last_alert_time = 0.0

    def process(self, payload: dict):
        try:
            telemetry = Telemetry(
                temperature=float(payload["t"]),
                humidity=float(payload["h"]),
                gas=float(payload["gaz"]),
                pir=int(payload.get("pir", 0)),
            )
        except (KeyError, TypeError, ValueError) as exc:
            print(f"[PREDICTIVE] Télémétrie invalide : {exc}")
            return

        self.features.add(telemetry)

        if not self.features.ready:
            return

        vector = self.features.vector()
        if vector is None:
            return

        if not self.model.ready:
            self.model.add_baseline_sample(vector)

            remaining = max(
                0,
                CONFIG.predictive_min_samples - self.model.sample_count,
            )
            print(
                f"[PREDICTIVE] Apprentissage : "
                f"{self.model.sample_count}/"
                f"{CONFIG.predictive_min_samples}"
            )
            if remaining == 0:
                print("[PREDICTIVE] Modèle prêt.")
            return

        result = self.model.predict(vector)

        # Le PIR n'est pas inclus dans l'Isolation Forest :
        # il sert ici à la corrélation comportementale.
        if telemetry.pir:
            self._emit_event(
                {
                    "event_type": "pir",
                    "score": 0.70,
                    "niveau": "info",
                    "message": "Présence physique détectée par PIR",
                }
            )

        if result["niveau"] == "normal":
            return

        now = time.time()
        if now - self.last_alert_time < CONFIG.predictive_cooldown_seconds:
            return

        self.last_alert_time = now

        level = result["niveau"]

        event = {
            "event_type": "environment_anomaly",
            "score": result["score"],
            "niveau": level,
            "message": "Anomalie environnementale détectée par le modèle prédictif",
            "features": {
                "temperature": telemetry.temperature,
                "humidity": telemetry.humidity,
                "gaz": telemetry.gas,
                "pir": telemetry.pir,
            },
        }

        self._emit_event(event)

    def _emit_event(self, event: dict):
        if self.on_event:
            self.on_event(event)
