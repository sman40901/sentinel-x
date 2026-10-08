"""
Service vision de Sentinel-X.

  - diffuse la webcam en MJPEG pour le dashboard
  - detecte et reconnait les visages (YuNet + SFace)
  - enregistre un instantane a chaque evenement de l'alarme
  - signale un visage inconnu au broker
  - expose l'API que le dashboard utilise pour gerer les personnes autorisees

Il parle au reste du systeme uniquement par MQTT, comme le boitier.
"""
import json
import os
import ssl
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

import paho.mqtt.client as mqtt

import vision as vision_mod
from store import FaceStore, EventStore
from vision import Vision

PORT = int(os.getenv("STREAM_PORT", "8080"))
GROUP = os.getenv("GROUP_ID", "g02")
MQTT_HOST = os.getenv("MQTT_HOST", "mosquitto")
MQTT_PORT = int(os.getenv("MQTT_PORT", "8883"))
MQTT_USER = os.getenv("MQTT_USERNAME", "ia")
MQTT_PASS = os.getenv("MQTT_PASSWORD", "")
MQTT_CA = os.getenv("MQTT_CA_CERT", "/certs/ca.crt")

T_STATE = f"sentinelx/{GROUP}/state"
T_ALERTS = f"sentinelx/{GROUP}/alerts"
T_VISION = f"sentinelx/{GROUP}/vision"

# Rythme auquel on dit au boitier ce qu'on voit. Assez rapide pour que le
# decompte de 10 s soit reactif, assez lent pour ne pas inonder le broker.
VISION_PERIOD = float(os.getenv("VISION_PUBLISH_SECONDS", "1.0"))

# Un visage inconnu au plus tous les N secondes : sans ca, une personne non
# enregistree qui reste dans le champ genere une alerte par image analysee.
UNKNOWN_COOLDOWN = float(os.getenv("UNKNOWN_FACE_COOLDOWN", "20"))

faces = FaceStore()
events = EventStore()
vision = Vision(faces)

_last_unknown = 0.0
_last_state = None
_mqtt = None
# Fenetre pendant laquelle une personne reconnue reste consideree presente.
_grace = {"until": 0.0, "name": ""}

# Etat de maintenance du boitier, lu sur le topic state.
#
# C'est le verrou de TOUTES les modifications de ce service : enroler une
# personne, en retirer une, changer un seuil. Sans lui, n'importe qui joignant
# le dashboard pouvait s'enroler comme autorise d'un simple POST, ou abaisser
# le seuil de reconnaissance pour que tout le monde corresponde. Verifie : les
# deux marchaient sans la moindre authentification.
#
# Le boitier n'entre en maintenance que sur presentation du code (defi-reponse,
# verrouillage apres 5 echecs), donc cet etat vaut deja preuve d'autorisation.
# Il expire de lui-meme, ce qui ferme la fenetre sans intervention.
_board = {"maint": False, "seen": 0.0}

# Sans nouvelles du boitier depuis ce delai, on REFUSE les modifications.
# Fermeture par defaut : un boitier injoignable ne doit pas ouvrir les vannes.
BOARD_STALE_SECONDS = float(os.getenv("BOARD_STALE_SECONDS", "30"))


def board_unlocked():
    """Le boitier est-il en maintenance, et l'information est-elle fraiche ?"""
    if time.time() - _board["seen"] > BOARD_STALE_SECONDS:
        return False, ("boitier injoignable - impossible de verifier "
                       "l'autorisation (modification refusee)")
    if not _board["maint"]:
        return False, "passez le boitier en maintenance (code requis) pour modifier"
    return True, ""


# =========================================================
# MQTT
# =========================================================

def publish_alert(payload):
    if _mqtt is None:
        return
    body = dict(payload)
    body.setdefault("source", "ia-vision")
    try:
        _mqtt.publish(T_ALERTS, json.dumps(body), qos=1)
    except Exception as exc:          # le flux ne doit jamais tomber pour ca
        print(f"[mqtt] publication impossible : {exc}", flush=True)


def snapshot(reason, detail="", prefer_face=False):
    """Capture et archive une image.

    prefer_face : prendre la derniere image ou un visage a ete vu plutot que
    l'image courante. Entre la detection et la capture, la personne a souvent
    deja bouge, et on archivait une piece vide.
    """
    jpg = vision.latest_with_face() if prefer_face else vision.latest()
    if jpg is None:
        return None
    meta = events.save(jpg, reason, detail)
    print(f"[snap] {reason} : {meta['file']} ({detail})", flush=True)
    return meta


def on_state(payload, retained=False):
    """Instantane aux moments qui comptent."""
    global _last_state
    try:
        st = json.loads(payload)
    except json.JSONDecodeError:
        return
    # Le verrou des modifications ne se fie QU'AUX messages en direct.
    #
    # L'etat est publie en retenu, pour que le dashboard affiche quelque chose
    # des l'ouverture. Mais le broker le rejoue a chaque (re)connexion, drapeau
    # retain leve - et il survit a l'extinction du boitier. Teste : avec le
    # boitier hors tension, un maint.on retenu de la veille rouvrait les
    # modifications pendant 30 s a chaque redemarrage de ce service. Un attaquant
    # n'a qu'a attendre un redemarrage. Un boitier vivant republie en quelques
    # secondes, donc ignorer le retenu ne coute rien en usage normal.
    if not retained:
        _board["maint"] = bool(st.get("maint", {}).get("on"))
        _board["seen"] = time.time()

    now_state = st.get("st")
    if now_state == _last_state:
        return
    previous, _last_state = _last_state, now_state
    if previous is None:
        return                        # premier message retenu : pas un evenement

    # Les trois moments ou l'on veut savoir qui etait devant la camera.
    reasons = {
        "entry": "detection",
        "alarm": "alarme",
        "maintenance": "identification",
    }
    if now_state in reasons:
        snapshot(reasons[now_state], st.get("cause", ""))


def mqtt_loop():
    global _mqtt
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                         client_id=f"vision-{GROUP}")
    client.username_pw_set(MQTT_USER, MQTT_PASS)
    # Pas de certificat client : le broker n'en exige pas, et gen-certs.sh
    # n'en produit pas pour les clients.
    client.tls_set(ca_certs=MQTT_CA, tls_version=ssl.PROTOCOL_TLS_CLIENT,
                   cert_reqs=ssl.CERT_REQUIRED)

    def on_connect(c, u, flags, rc, props=None):
        print(f"[mqtt] connecte ({rc})", flush=True)
        c.subscribe(T_STATE, qos=1)

    def on_message(c, u, msg):
        if msg.topic == T_STATE:
            # msg.retain : le broker rejoue le DERNIER etat connu a chaque
            # connexion. Il faut le savoir, sinon un boitier eteint depuis des
            # jours a l'air present - voir on_state().
            on_state(msg.payload.decode("utf-8", "replace"), msg.retain)

    client.on_connect = on_connect
    client.on_message = on_message
    _mqtt = client

    while True:
        try:
            client.connect(MQTT_HOST, MQTT_PORT, keepalive=30)
            client.loop_forever()
        except Exception as exc:
            print(f"[mqtt] {exc} - nouvel essai dans 5 s", flush=True)
            time.sleep(5)


def watch_faces():
    """Dit au boitier ce que la camera voit, et signale un visage inconnu.

    Le boitier applique les regles : une personne autorisee en vue annule
    toute alarme d'intrusion, un visage inconnu a VISION_IDENTIFY_MS pour se
    faire reconnaitre. Ici on se contente de rapporter, fidelement.
    """
    global _last_unknown
    last_sent = None
    unknown_streak = 0
    while True:
        time.sleep(VISION_PERIOD)
        seen = vision.current_faces()
        known = [f for f in seen if f.known]
        unknown = [f for f in seen if not f.known]
        now_s = time.time()

        # Une personne AUTORISEE l'emporte : si quelqu'un de reconnu est dans
        # le champ, la presence d'un inconnu a cote ne doit pas declencher.
        if known:
            face, name = "known", known[0].name
            _grace["until"] = now_s + vision_mod.SETTINGS["known_grace_seconds"]
            _grace["name"] = known[0].name
            unknown_streak = 0
        elif unknown:
            # DELAI DE GRACE. Un visage de profil ne ressemble pas a une vue de
            # face : la personne enregistree devient brievement "inconnue" des
            # qu'elle tourne la tete. Sans ce delai, quelqu'un d'autorise assis
            # a son bureau declencherait un decompte d'alarme a chaque fois
            # qu'il regarde ailleurs. Observe sur cette camera.
            if now_s < _grace["until"]:
                face, name = "known", _grace["name"]
                unknown_streak = 0
            else:
                unknown_streak += 1
                # Il faut plusieurs releves de suite : une seule image ratee ne
                # doit pas suffire a declarer un intrus.
                if unknown_streak >= vision_mod.SETTINGS["unknown_confirm"]:
                    face, name = "unknown", ""
                else:
                    face, name = "none", ""
        else:
            face, name = "none", ""
            unknown_streak = 0

        # Publie a chaque cycle : le boitier perime l'information au bout de
        # VISION_STALE_MS, donc le silence doit vouloir dire "service arrete",
        # pas "rien n'a change".
        if _mqtt is not None:
            try:
                _mqtt.publish(T_VISION, json.dumps({"face": face, "name": name}), qos=0)
            except Exception:
                pass
        if face != last_sent:
            print(f"[vision] {face}{' : ' + name if name else ''}", flush=True)
            last_sent = face

        if not unknown or known:
            continue
        now = time.time()
        if now - _last_unknown < UNKNOWN_COOLDOWN:
            continue
        _last_unknown = now
        detail = f"{len(unknown)} visage(s) non autorise(s)"
        snapshot("visage-inconnu", detail, prefer_face=True)
        publish_alert({"type": "intrus", "niveau": "attention",
                       "msg": f"Visage non autorise detecte ({len(unknown)})",
                       "event_type": "unknown_person", "score": 0.9})


# =========================================================
# HTTP
# =========================================================

class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def _send(self, code, body, ctype="application/json"):
        data = body.encode() if isinstance(body, str) else body
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def _json(self, obj, code=200):
        self._send(code, json.dumps(obj))

    def _err(self, code, msg):
        self._json({"ok": False, "err": msg}, code)

    def do_GET(self):
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        p = u.path

        if p in ("/stream", "/"):
            return self._stream()
        if p == "/snapshot":
            jpg = vision.latest()
            return self._send(200, jpg, "image/jpeg") if jpg else self._err(503, "pas d'image")
        if p == "/api/status":
            st = vision.status()
            unlocked, why = board_unlocked()
            st["unlocked"] = unlocked
            st["lock_reason"] = why
            st["grace_active"] = time.time() < _grace["until"]
            st["grace_name"] = _grace["name"]
            return self._json(st)
        if p == "/api/config":
            return self._json({"settings": dict(vision_mod.SETTINGS),
                               "limits": {k: list(v) for k, v in vision_mod.LIMITS.items()}})
        if p == "/api/faces":
            return self._json({"people": faces.list()})
        if p == "/api/faces/thumb":
            t = faces.thumbnail(q.get("id", ""))
            return self._send(200, t, "image/jpeg") if t else self._err(404, "inconnu")
        if p == "/api/events":
            return self._json({"events": events.list()})
        if p == "/api/events/img":
            img = events.image(q.get("file", ""))
            return self._send(200, img, "image/jpeg") if img else self._err(404, "inconnu")
        self._err(404, "not found")

    def _guard(self):
        """Refuse une modification si le boitier n'est pas en maintenance."""
        ok, why = board_unlocked()
        if not ok:
            # X-Real-IP : sans lui on journalise l'adresse de nginx, pas l'attaquant.
            client = self.headers.get("X-Real-IP") or self.client_address[0]
            print(f"[securite] modification refusee depuis {client} : {why}", flush=True)
            publish_alert({"type": "securite", "niveau": "attention",
                           "msg": f"Tentative de modification non autorisee depuis {client}"})
            self._err(403, why)
        return ok

    def do_POST(self):
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        if not self._guard():
            return
        if u.path == "/api/config":
            applied, errors = vision.apply_settings(q)
            if errors and not applied:
                return self._err(400, "; ".join(f"{k}: {v}" for k, v in errors.items()))
            print(f"[config] {applied}", flush=True)
            return self._json({"ok": True, "applied": applied, "errors": errors,
                               "settings": dict(vision_mod.SETTINGS)})
        if u.path == "/api/faces":
            name = (q.get("name") or "").strip()[:40]
            if not name:
                return self._err(400, "nom manquant")
            pid, err = vision.enrol(name)
            if err:
                return self._err(409, err)
            publish_alert({"type": "maintenance", "niveau": "info",
                           "msg": f"Personne autorisee ajoutee : {name}"})
            return self._json({"ok": True, "id": pid, "name": name})
        self._err(404, "not found")

    def do_DELETE(self):
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        if not self._guard():
            return
        if u.path == "/api/faces":
            pid = q.get("id", "")
            if not faces.remove(pid):
                return self._err(404, "inconnu")
            return self._json({"ok": True})
        self._err(404, "not found")

    def _stream(self):
        self.send_response(200)
        self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        try:
            while True:
                jpg = vision.latest()
                if jpg is None:
                    time.sleep(0.05)
                    continue
                self.wfile.write(b"--frame\r\nContent-Type: image/jpeg\r\n")
                self.wfile.write(f"Content-Length: {len(jpg)}\r\n\r\n".encode())
                self.wfile.write(jpg)
                self.wfile.write(b"\r\n")
                time.sleep(1 / 20)
        except (BrokenPipeError, ConnectionResetError):
            pass          # onglet ferme : normal


if __name__ == "__main__":
    threading.Thread(target=mqtt_loop, daemon=True).start()
    threading.Thread(target=watch_faces, daemon=True).start()
    st = vision.status()
    print(f"[vision] camera={'ok' if st['camera'] else st['error']} "
          f"| {len(faces.list())} personne(s) autorisee(s)", flush=True)
    print(f"[vision] http://0.0.0.0:{PORT}/stream", flush=True)
    ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()
