"""
Petit test local sans MQTT ni ESP32.

Lancer :
    python predictive/test_predictive.py
"""

import random

from predictive.predictive import PredictiveAnalyzer


def main():
    events = []

    analyzer = PredictiveAnalyzer(
        on_event=lambda event: events.append(event)
    )

    # Phase normale
    for _ in range(80):
        analyzer.process(
            {
                "t": 24.0 + random.uniform(-0.15, 0.15),
                "h": 48.0 + random.uniform(-1.0, 1.0),
                "gaz": 300 + random.uniform(-8, 8),
                "pir": 0,
            }
        )

    print("Modèle prêt :", analyzer.model.ready)

    # Simule une dérive progressive.
    for i in range(15):
        analyzer.process(
            {
                "t": 24.5 + i * 0.8,
                "h": 47 - i * 0.2,
                "gaz": 310 + i * 25,
                "pir": 1,
            }
        )

    print("\nÉvénements détectés :")
    for event in events:
        print(event)


if __name__ == "__main__":
    main()
