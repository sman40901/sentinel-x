"""
Sentinel-X — API du Centre de commandement

- S'abonne au broker MQTT et enregistre mesures et alertes dans PostgreSQL
- Expose l'historique au dashboard (GET /api/v1/mesures, GET /api/v1/alerts)
- POST /api/v1/alerts : déclarer une alerte (IA, outils externes) -> publiée en MQTT puis enregistrée
- POST /api/v1/cmd    : piloter le buzzer et les LEDs du boîtier
Les routes POST exigent l'en-tête X-API-Key.
"""
import hmac
import json
import logging
import os
import threading
import time
from typing import Literal, Optional

import paho.mqtt.client as mqtt
import psycopg
from psycopg.rows import dict_row
from fastapi import FastAPI, Header, HTTPException, Query
from pydantic import BaseModel, Field

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
log = logging.getLogger("sentinelx-api")

# ----------------------------------------------------------------- configuration
GROUP = os.getenv("GROUP_ID", "g02")
DSN = os.environ["DATABASE_URL"]
MQTT_HOST = os.getenv("MQTT_HOST", "mosquitto")
MQTT_PORT = int(os.getenv("MQTT_PORT", "1883"))
MQTT_USER = os.getenv("MQTT_USER", "api")
MQTT_PASSWORD = os.getenv("MQTT_PASSWORD", "")
API_KEY = os.getenv("API_KEY", "")

T_TELEMETRY = f"sentinelx/{GROUP}/telemetry"
T_ALERTS = f"sentinelx/{GROUP}/alerts"
T_STATUS = f"sentinelx/{GROUP}/status"
T_CMD = f"sentinelx/{GROUP}/cmd"

SCHEMA = [
    """CREATE TABLE IF NOT EXISTS mesures (
           id    BIGSERIAL PRIMARY KEY,
           ts    TIMESTAMPTZ NOT NULL DEFAULT now(),
           temp  REAL,
           hum   REAL,
           gaz   REAL,
           pir   BOOLEAN,
           rssi  INTEGER
       )""",
    "CREATE INDEX IF NOT EXISTS mesures_ts ON mesures (ts DESC)",
    """CREATE TABLE IF NOT EXISTS alertes (
           id      BIGSERIAL PRIMARY KEY,
           ts      TIMESTAMPTZ NOT NULL DEFAULT now(),
           type    TEXT NOT NULL,
           niveau  TEXT NOT NULL,
           message TEXT,
           conf    REAL,
           source  TEXT
       )""",
    "CREATE INDEX IF NOT EXISTS alertes_ts ON alertes (ts DESC)",
]


# ----------------------------------------------------------------- base de données
def db(sql: str, params=None, fetch: bool = False):
    """Une connexion courte par requête : simple et robuste au faible débit (1 mesure / 2 s)."""
    with psycopg.connect(DSN, autocommit=True, connect_timeout=5) as conn:
        with conn.cursor(row_factory=dict_row) as cur:
            cur.execute(sql, params)
            return cur.fetchall() if fetch else None


def init_db():
    for attempt in range(30):
        try:
            for stmt in SCHEMA:
                db(stmt)
            log.info("Base prête")
            return
        except psycopg.OperationalError as exc:
            log.warning("Base indisponible (%s), nouvel essai dans 2 s", exc)
            time.sleep(2)
    raise RuntimeError("Impossible de joindre PostgreSQL")


def num(value):
    try:
        return None if value is None or value == "" else float(value)
    except (TypeError, ValueError):
        return None


def store_telemetry(payload: str):
    try:
        d = json.loads(payload)
    except json.JSONDecodeError:
        log.warning("Mesure illisible : %.80s", payload)
        return
    if not isinstance(d, dict):
        return
    pir = d.get("pir", d.get("motion"))
    db(
        "INSERT INTO mesures (temp, hum, gaz, pir, rssi) VALUES (%s, %s, %s, %s, %s)",
        (
            num(d.get("t", d.get("temp"))),
            num(d.get("h", d.get("hum"))),
            num(d.get("gaz", d.get("gas"))),
            None if pir is None else pir in (1, True, "1"),
            int(d["rssi"]) if isinstance(d.get("rssi"), (int, float)) else None,
        ),
    )


def store_alert(payload: str):
    try:
        a = json.loads(payload)
        if not isinstance(a, dict):
            raise ValueError
    except ValueError:
        a = {"type": "alerte", "msg": payload}
    raw_msg = a.get("msg", a.get("message"))
    msg = str(raw_msg)[:200] if raw_msg not in (None, "") else None
    db(
        "INSERT INTO alertes (type, niveau, message, conf, source) VALUES (%s, %s, %s, %s, %s)",
        (
            str(a.get("type", "alerte"))[:32],
            str(a.get("niveau", a.get("level", "critique")))[:16],
            msg,
            num(a.get("conf")),
            str(a.get("source", ""))[:32] or None,
        ),
    )


# ----------------------------------------------------------------- MQTT
mqtt_client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"api-{GROUP}")
mqtt_client.username_pw_set(MQTT_USER, MQTT_PASSWORD)
mqtt_client.reconnect_delay_set(min_delay=1, max_delay=30)
mqtt_ok = threading.Event()


def on_connect(client, userdata, flags, reason_code, properties):
    if reason_code.is_failure:
        log.error("MQTT refusé : %s", reason_code)
        return
    mqtt_ok.set()
    client.subscribe([(T_TELEMETRY, 0), (T_ALERTS, 1), (T_STATUS, 1)])
    log.info("MQTT connecté à %s:%s", MQTT_HOST, MQTT_PORT)


def on_disconnect(client, userdata, flags, reason_code, properties):
    mqtt_ok.clear()
    log.warning("MQTT déconnecté (%s)", reason_code)


def on_message(client, userdata, msg):
    payload = msg.payload.decode("utf-8", errors="replace")
    try:
        if msg.topic == T_TELEMETRY:
            store_telemetry(payload)
        elif msg.topic == T_ALERTS:
            store_alert(payload)
        elif msg.topic == T_STATUS and not msg.retain:
            store_alert(json.dumps({"type": "boitier", "niveau": "info", "msg": f"État : {payload}", "source": "status"}))
    except Exception:  # une erreur base ne doit jamais tuer l'abonnement
        log.exception("Échec d'enregistrement (%s)", msg.topic)


mqtt_client.on_connect = on_connect
mqtt_client.on_disconnect = on_disconnect
mqtt_client.on_message = on_message


# ----------------------------------------------------------------- API
app = FastAPI(
    title="Sentinel-X API",
    version="1.0",
    docs_url=None,
    redoc_url=None,
    openapi_url="/api/openapi.json",
)


@app.on_event("startup")
def startup():
    init_db()
    mqtt_client.connect_async(MQTT_HOST, MQTT_PORT, keepalive=30)
    mqtt_client.loop_start()


@app.on_event("shutdown")
def shutdown():
    mqtt_client.loop_stop()
    mqtt_client.disconnect()


def require_key(x_api_key: Optional[str]):
    if not API_KEY:
        raise HTTPException(503, "API_KEY non configurée sur le serveur")
    if not x_api_key or not hmac.compare_digest(x_api_key, API_KEY):
        raise HTTPException(401, "Clé API invalide (en-tête X-API-Key)")


def publish(topic: str, body: dict, qos: int = 1):
    if not mqtt_ok.is_set():
        raise HTTPException(503, "Broker MQTT indisponible")
    info = mqtt_client.publish(topic, json.dumps(body, ensure_ascii=False), qos=qos)
    if info.rc != mqtt.MQTT_ERR_SUCCESS:
        raise HTTPException(503, f"Publication MQTT impossible (code {info.rc})")


class AlertIn(BaseModel):
    type: str = Field(..., min_length=1, max_length=32, examples=["intrus"])
    niveau: Literal["info", "attention", "critique"] = "critique"
    msg: Optional[str] = Field(None, max_length=200)
    conf: Optional[float] = Field(None, ge=0, le=1)
    source: Optional[str] = Field(None, max_length=32, examples=["ia-webcam"])


class CmdIn(BaseModel):
    buzzer: Optional[Literal[0, 1]] = None
    led: Optional[Literal["rouge", "jaune", "vert", "off"]] = None


@app.get("/api/v1/health")
def health():
    try:
        db("SELECT 1")
        db_ok = True
    except Exception:
        db_ok = False
    return {"status": "ok" if db_ok and mqtt_ok.is_set() else "degrade", "db": db_ok, "mqtt": mqtt_ok.is_set()}


@app.get("/api/v1/mesures")
def mesures(limit: int = Query(150, ge=1, le=5000)):
    rows = db(
        "SELECT ts, temp AS t, hum AS h, gaz, pir, rssi FROM mesures ORDER BY ts DESC LIMIT %s",
        (limit,),
        fetch=True,
    )
    return list(reversed(rows))          # du plus ancien au plus récent (pour les graphiques)


@app.get("/api/v1/alerts")
def alerts(limit: int = Query(50, ge=1, le=1000)):
    return db(
        "SELECT ts, type, niveau, message AS msg, conf, source FROM alertes ORDER BY ts DESC LIMIT %s",
        (limit,),
        fetch=True,
    )


@app.post("/api/v1/alerts", status_code=202)
def post_alert(alert: AlertIn, x_api_key: Optional[str] = Header(None)):
    require_key(x_api_key)
    publish(T_ALERTS, alert.model_dump(exclude_none=True))   # le dashboard la voit en direct, l'abonnement l'enregistre
    return {"publie": True, "topic": T_ALERTS}


@app.post("/api/v1/cmd", status_code=202)
def post_cmd(cmd: CmdIn, x_api_key: Optional[str] = Header(None)):
    require_key(x_api_key)
    body = cmd.model_dump(exclude_none=True)
    if not body:
        raise HTTPException(422, "Rien à envoyer : préciser buzzer et/ou led")
    publish(T_CMD, body)
    return {"publie": True, "commande": body}
