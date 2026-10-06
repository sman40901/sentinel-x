from pathlib import Path
from typing import Optional

import joblib
import numpy as np
from sklearn.ensemble import IsolationForest
from sklearn.preprocessing import StandardScaler


class PredictiveModel:
    """
    Détection d'anomalies non supervisée.

    Le modèle apprend le comportement habituel des capteurs puis attribue
    un score d'anomalie à chaque nouvelle observation.
    """

    def __init__(
        self,
        min_samples: int = 60,
        model_path: str = "data/isolation_forest.joblib",
    ):
        self.min_samples = min_samples
        self.model_path = Path(model_path)
        self.samples = []
        self.scaler: Optional[StandardScaler] = None
        self.model: Optional[IsolationForest] = None
        self._load()

    @property
    def ready(self) -> bool:
        return self.model is not None and self.scaler is not None

    @property
    def sample_count(self) -> int:
        return len(self.samples)

    def add_baseline_sample(self, vector: np.ndarray):
        if self.ready:
            return

        self.samples.append(vector)

        if len(self.samples) >= self.min_samples:
            self.fit()

    def fit(self):
        X = np.asarray(self.samples, dtype=float)

        self.scaler = StandardScaler()
        X_scaled = self.scaler.fit_transform(X)

        self.model = IsolationForest(
            n_estimators=150,
            contamination="auto",
            random_state=42,
            n_jobs=-1,
        )
        self.model.fit(X_scaled)

        self.model_path.parent.mkdir(parents=True, exist_ok=True)
        joblib.dump(
            {
                "scaler": self.scaler,
                "model": self.model,
            },
            self.model_path,
        )

        print(
            f"[IA] Isolation Forest entraîné avec "
            f"{len(self.samples)} observations."
        )

    def _load(self):
        if not self.model_path.exists():
            return

        data = joblib.load(self.model_path)
        self.scaler = data["scaler"]
        self.model = data["model"]
        print(f"[IA] Modèle chargé : {self.model_path}")

    def anomaly_score(self, vector: np.ndarray) -> float:
        if not self.ready:
            raise RuntimeError("Le modèle n'est pas encore entraîné.")

        X = self.scaler.transform([vector])

        # decision_function : valeurs élevées = plus normal.
        raw = float(self.model.decision_function(X)[0])

        # Transformation empirique en score 0..1.
        # Plus raw est faible, plus l'anomalie est forte.
        score = 1.0 / (1.0 + np.exp(5.0 * raw))
        return float(np.clip(score, 0.0, 1.0))

    def predict(self, vector: np.ndarray) -> dict:
        score = self.anomaly_score(vector)

        if score >= 0.82:
            level = "critique"
        elif score >= 0.65:
            level = "warning"
        else:
            level = "normal"

        return {
            "score": round(score, 3),
            "niveau": level,
        }
