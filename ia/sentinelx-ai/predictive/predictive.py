import time
from typing import Callable, Optional

from config import CONFIG
from predictive.features import FeatureBuffer, Telemetry
from predictive.model import PredictiveModel


# Le firmware publie "t" et "h" à null quand le DHT22 ne répond pas, exprès,
# pour ne pas jeter le gaz et le mouvement du même message. Ça peut durer des
# heures : le journal est donc limité à une ligne par minute.
DEGRADED_LOG_INTERVAL_SECONDS = 60.0


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

        # Suivi des échantillons sans température/humidité.
        self.last_degraded_log = 0.0
        self.degraded_samples = 0

    @staticmethod
    def _optional_float(payload: dict, key: str) -> Optional[float]:
        """
        Renvoie None quand le capteur n'a rien donné (JSON null), sinon un float.

        Lève KeyError si le champ est absent et TypeError/ValueError s'il est
        d'un type inattendu : un champ manquant reste une erreur de format, une
        valeur nulle non.
        """
        value = payload[key]
        if value is None:
            return None
        # isinstance(True, int) vaut True en Python : sans ce garde-fou, un
        # booléen passerait silencieusement pour 1.0.
        if isinstance(value, bool):
            raise TypeError(f"'{key}' ne peut pas être un booléen")
        return float(value)

    def process(self, payload: dict):
        try:
            temperature = self._optional_float(payload, "t")
            humidity = self._optional_float(payload, "h")
            gas = float(payload["gaz"])
            pir = int(payload.get("pir", 0))
        except (KeyError, TypeError, ValueError) as exc:
            print(f"[PREDICTIVE] Télémétrie invalide : {exc}")
            return

        # Le PIR ne dépend pas du capteur climatique, donc il est traité avant
        # tout retour anticipé. Il était auparavant émis APRÈS la porte
        # "modèle prêt" : aucun événement PIR ne sortait donc d'ici pendant les
        # PREDICTIVE_MIN_SAMPLES échantillons d'apprentissage, soit ~5 min par
        # défaut. La présence physique ne doit pas attendre l'entraînement.
        if pir:
            self._emit_event(
                {
                    "event_type": "pir",
                    "score": 0.70,
                    "niveau": "info",
                    "message": "Présence physique détectée par PIR",
                }
            )

        # t ou h à null : le DHT22 n'a pas répondu.
        #
        # On NE remplit PAS la valeur manquante. Le vecteur de l'Isolation
        # Forest a 10 dimensions fixes et contient la température courante, son
        # delta, sa moyenne et son écart-type : entraîner ou prédire sur une
        # valeur recopiée fabriquerait une stabilité qui n'existe pas, puis
        # ferait passer le retour à la normale du capteur pour une anomalie.
        # Le volet environnemental est donc sauté pour cet échantillon, sans
        # jeter le gaz ni le mouvement comme le faisait la version précédente.
        #
        # Conséquence assumée : la fenêtre de features ne contient que des
        # échantillons complets, elle peut donc couvrir une durée réelle plus
        # longue quand le capteur a des trous.
        if temperature is None or humidity is None:
            self._log_degraded()
            return

        telemetry = Telemetry(
            temperature=temperature,
            humidity=humidity,
            gas=gas,
            pir=pir,
        )

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

    def _log_degraded(self):
        """Une ligne par minute au plus, sinon le journal est illisible."""
        self.degraded_samples += 1
        now = time.time()
        if now - self.last_degraded_log < DEGRADED_LOG_INTERVAL_SECONDS:
            return
        self.last_degraded_log = now
        print(
            f"[PREDICTIVE] DHT22 muet : {self.degraded_samples} échantillon(s) "
            f"sans température/humidité. Détection d'anomalie environnementale "
            f"en pause ; PIR et vision continuent de fonctionner."
        )

    def _emit_event(self, event: dict):
        if self.on_event:
            self.on_event(event)
