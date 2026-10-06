import time
from collections import deque
from typing import Optional


class RiskEngine:
    """
    Fusion simple de plusieurs signaux IA/capteurs.
    Ce n'est pas le modèle prédictif : c'est la couche de décision.
    """

    def __init__(self, window_seconds: float = 8):
        self.window_seconds = window_seconds
        self.recent_events = deque()

    def add_event(self, event_type: str, score: float = 1.0):
        now = time.time()
        self.recent_events.append((now, event_type, score))
        self._cleanup(now)

    def _cleanup(self, now):
        while self.recent_events and now - self.recent_events[0][0] > self.window_seconds:
            self.recent_events.popleft()

    def evaluate(self) -> Optional[dict]:
        now = time.time()
        self._cleanup(now)

        types = {event[1] for event in self.recent_events}
        max_score = max((event[2] for event in self.recent_events), default=0.0)

        if "unknown_person" in types and "pir" in types:
            return {
                "niveau": "critique",
                "score": 1.0,
                "message": "Personne non autorisée + présence physique détectée",
            }

        if "unknown_person" in types:
            return {
                "niveau": "critique",
                "score": max(0.95, max_score),
                "message": "Personne non autorisée détectée",
            }

        if "environment_anomaly" in types and "pir" in types:
            return {
                "niveau": "warning",
                "score": max(0.80, max_score),
                "message": "Anomalie environnementale corrélée à une présence",
            }

        return None
