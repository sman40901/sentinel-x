from dataclasses import dataclass
from pathlib import Path
import os

from dotenv import load_dotenv

ROOT = Path(__file__).resolve().parent
load_dotenv(ROOT / ".env")


def env_bool(name: str, default: bool) -> bool:
    value = os.getenv(name)
    if value is None:
        return default
    return value.lower() in {"1", "true", "yes", "on"}


@dataclass(frozen=True)
class Config:
    group_id: str = os.getenv("GROUP_ID", "G0X")

    mqtt_host: str = os.getenv("MQTT_HOST", "127.0.0.1")
    mqtt_port: int = int(os.getenv("MQTT_PORT", "8883"))
    mqtt_username: str = os.getenv("MQTT_USERNAME", "ia")
    mqtt_password: str = os.getenv("MQTT_PASSWORD", "")

    mqtt_ca_cert: str = os.getenv("MQTT_CA_CERT", "certs/ca.crt")
    mqtt_client_cert: str = os.getenv("MQTT_CLIENT_CERT", "certs/ia.crt")
    mqtt_client_key: str = os.getenv("MQTT_CLIENT_KEY", "certs/ia.key")
    mqtt_tls_verify: bool = env_bool("MQTT_TLS_VERIFY", True)

    telemetry_topic: str = os.getenv(
        "MQTT_TELEMETRY_TOPIC",
        "sentinelx/{group}/telemetry",
    )
    alert_topic: str = os.getenv(
        "MQTT_ALERT_TOPIC",
        "sentinelx/{group}/alerts",
    )

    camera_index: int = int(os.getenv("CAMERA_INDEX", "0"))
    frame_width: int = int(os.getenv("FRAME_WIDTH", "640"))
    frame_height: int = int(os.getenv("FRAME_HEIGHT", "480"))
    yolo_model: str = os.getenv("YOLO_MODEL", "yolov8n.pt")
    yolo_confidence: float = float(os.getenv("YOLO_CONFIDENCE", "0.45"))

    face_tolerance: float = float(os.getenv("FACE_TOLERANCE", "0.48"))
    face_check_every_n_frames: int = int(
        os.getenv("FACE_CHECK_EVERY_N_FRAMES", "3")
    )
    intrusion_cooldown_seconds: float = float(
        os.getenv("INTRUSION_COOLDOWN_SECONDS", "10")
    )

    predictive_min_samples: int = int(
        os.getenv("PREDICTIVE_MIN_SAMPLES", "60")
    )
    predictive_window: int = int(os.getenv("PREDICTIVE_WINDOW", "10"))
    predictive_cooldown_seconds: float = float(
        os.getenv("PREDICTIVE_COOLDOWN_SECONDS", "15")
    )
    anomaly_warning_threshold: float = float(
        os.getenv("ANOMALY_WARNING_THRESHOLD", "0.65")
    )
    anomaly_critical_threshold: float = float(
        os.getenv("ANOMALY_CRITICAL_THRESHOLD", "0.82")
    )

    fusion_window_seconds: float = float(
        os.getenv("FUSION_WINDOW_SECONDS", "8")
    )

    @property
    def telemetry(self) -> str:
        return self.telemetry_topic.format(group=self.group_id)

    @property
    def alerts(self) -> str:
        return self.alert_topic.format(group=self.group_id)


CONFIG = Config()
