from collections import deque
from dataclasses import dataclass
from typing import Optional

import numpy as np


@dataclass
class Telemetry:
    temperature: float
    humidity: float
    gas: float
    pir: int


class FeatureBuffer:
    def __init__(self, maxlen: int = 10):
        self.values = deque(maxlen=maxlen)

    def add(self, telemetry: Telemetry):
        self.values.append(telemetry)

    @property
    def ready(self) -> bool:
        return len(self.values) >= 2

    def vector(self) -> Optional[np.ndarray]:
        if not self.ready:
            return None

        current = self.values[-1]
        previous = self.values[-2]

        temps = np.array([x.temperature for x in self.values], dtype=float)
        gases = np.array([x.gas for x in self.values], dtype=float)
        humidity = np.array([x.humidity for x in self.values], dtype=float)

        delta_temp = current.temperature - previous.temperature
        delta_gas = current.gas - previous.gas

        return np.array(
            [
                current.temperature,
                current.humidity,
                current.gas,
                delta_temp,
                delta_gas,
                temps.mean(),
                gases.mean(),
                humidity.mean(),
                temps.std(),
                gases.std(),
            ],
            dtype=float,
        )
