"""
Camera, detection et reconnaissance faciale.

Deux modeles OpenCV, embarques dans l'image :
  YuNet  - detection de visages, 230 ko
  SFace  - empreinte de 128 valeurs par visage, 38 Mo

Volontairement PAS dlib ni PyTorch. face_recognition exige dlib, souvent
compile depuis les sources, et ultralytics tire PyTorch : plusieurs centaines
de Mo pour une tache que ces deux ONNX font tres bien sur un CPU de portable.
"""
import os
import threading
import time

import cv2
import numpy as np

import liveness as liveness_mod
from liveness import Liveness

CAMERA_INDEX = int(os.getenv("CAMERA_INDEX", "0"))
WIDTH = int(os.getenv("FRAME_WIDTH", "640"))
HEIGHT = int(os.getenv("FRAME_HEIGHT", "480"))
QUALITY = int(os.getenv("STREAM_QUALITY", "75"))

MODEL_DIR = os.getenv("MODEL_DIR", "/models")
DETECTOR = os.path.join(MODEL_DIR, "yunet.onnx")
RECOGNIZER = os.path.join(MODEL_DIR, "sface.onnx")

# 0.60 et pas 0.90 : mesure sur cette camera, les visages reellement presents
# dans les captures ne passaient 0.80 que par intermittence (0 image sur 4 a
# 0.80, 3 sur 4 a 0.50). Un seuil trop severe ne rend pas la detection plus
# sure - il la rend capricieuse, et l'enrolement echoue sans cesse sur
# "aucun visage detecte". Reglable par FACE_CONFIDENCE.
# Reglages MODIFIABLES A CHAUD depuis le dashboard. Les variables d'environnement
# ne servent que de valeurs de depart : tout se regle ensuite sans reconstruire
# ni redemarrer quoi que ce soit.
SETTINGS = {
    # Seuil de DETECTION d'un visage. Trop haut, les visages de profil ou mal
    # eclaires sont manques ; trop bas, des formes quelconques passent pour des
    # visages.
    "detect_confidence": float(os.getenv("FACE_CONFIDENCE", "0.60")),
    # Seuil de RECONNAISSANCE. Au-dessus, c'est la personne enregistree.
    "match_threshold": float(os.getenv("FACE_MATCH_THRESHOLD", "0.363")),
    # Apres une reconnaissance reussie, la personne reste consideree presente
    # pendant ce temps meme si son visage n'est plus reconnu image par image.
    # C'est ce qui evite qu'un simple mouvement de tete - un profil ne
    # ressemble pas a une vue de face - la fasse passer pour une inconnue et
    # declenche un decompte d'alarme alors qu'elle est assise la.
    "known_grace_seconds": float(os.getenv("KNOWN_GRACE_SECONDS", "20")),
    # Nombre de releves consecutifs sans aucun visage connu avant de declarer
    # un inconnu. Lisse les images ratees.
    "unknown_confirm": int(os.getenv("UNKNOWN_CONFIRM", "3")),
    # Une image sur N est analysee.
    "check_every_n": int(os.getenv("FACE_CHECK_EVERY_N_FRAMES", "5")),
}

LIMITS = {
    "detect_confidence":   (0.05, 0.99),
    # Plancher 0.30, PAS 0.10. En dessous du seuil recommande par OpenCV
    # (0.363) la similarite cosinus de deux inconnus suffit a correspondre :
    # a 0.10 n'importe quel visage est reconnu comme la premiere personne
    # enrolee. C'etait un "tout valider" en un seul reglage, verifie en direct.
    "match_threshold":     (0.30, 0.90),
    "known_grace_seconds": (0.0, 300.0),
    "unknown_confirm":     (1, 20),
    "check_every_n":       (1, 30),
}

DETECT_CONF = SETTINGS["detect_confidence"]
# Seuil cosinus recommande par OpenCV pour SFace. Plus haut = plus severe.


# Sans image pendant ce temps, la camera est rouverte.
WATCHDOG_SECONDS = float(os.getenv("CAMERA_WATCHDOG_SECONDS", "5"))


class Face:
    # recognised : l'empreinte correspond a une personne enregistree.
    # known      : ...ET la vivacite est confirmee, quand on l'exige.
    #
    # Les deux sont distincts exprès. C'est "known" qui dit au boitier
    # "personne autorisee en vue, n'alarme pas" ; une photo tendue devant la
    # camera est "recognised" mais ne doit jamais etre "known".
    __slots__ = ("box", "name", "score", "known", "recognised", "live",
                 "prompt", "live_detail")

    def __init__(self, box, name, score, recognised, known, live,
                 prompt="", live_detail=None):
        self.box, self.name, self.score = box, name, score
        self.recognised, self.known, self.live = recognised, known, live
        self.prompt, self.live_detail = prompt, live_detail or {}


class Vision:
    def __init__(self, store):
        self.store = store
        self.lock = threading.Lock()
        self.jpeg = None            # derniere image encodee, avec annotations
        self.clean_jpeg = None      # ...et sans, pour les instantanes
        # Derniere image ou un visage a REELLEMENT ete vu. Les instantanes de
        # visage inconnu s'en servent : prendre l'image courante donnait
        # souvent une piece vide, la personne ayant bouge entre la detection
        # et la capture.
        self.face_jpeg = None
        self.faces = []
        self.frames = 0
        self.camera_ok = False
        self.error = ""
        self.running = True
        self.last_frame_ms = 0.0     # chien de garde : derniere lecture reussie
        self.reopens = 0

        self.liveness = Liveness()

        self.detector = cv2.FaceDetectorYN.create(
            DETECTOR, "", (WIDTH, HEIGHT), DETECT_CONF, 0.3, 5000)
        self.recognizer = cv2.FaceRecognizerSF.create(RECOGNIZER, "")

        self.cap = None
        self._open()
        threading.Thread(target=self._loop, daemon=True).start()
        threading.Thread(target=self._watchdog, daemon=True).start()

    def _open(self):
        """(Re)ouvre le peripherique. Appele au demarrage et par le chien de garde."""
        if self.cap is not None:
            try:
                self.cap.release()
            except Exception:
                pass
        self.cap = cv2.VideoCapture(CAMERA_INDEX)
        self.cap.set(cv2.CAP_PROP_FRAME_WIDTH, WIDTH)
        self.cap.set(cv2.CAP_PROP_FRAME_HEIGHT, HEIGHT)
        if self.cap.isOpened():
            self.camera_ok = True
            self.error = ""
            self.last_frame_ms = time.time()
        else:
            self.camera_ok = False
            self.error = (f"camera {CAMERA_INDEX} introuvable - verifier "
                          f"CAMERA_DEVICE dans .env")

    def _watchdog(self):
        """Rouvre la camera si plus aucune image n'arrive.

        Sans ca, un debranchement, une reprise de veille ou un hoquet USB
        laissaient le flux fige indefiniment : le conteneur restait "Up", le
        processeur a 0 %, et l'image du dashboard ne bougeait plus. Constate
        en service.
        """
        while self.running:
            time.sleep(WATCHDOG_SECONDS)
            if not self.last_frame_ms:
                continue
            stale = time.time() - self.last_frame_ms
            if stale > WATCHDOG_SECONDS:
                self.reopens += 1
                print(f"[vision] flux fige depuis {stale:.0f} s - "
                      f"reouverture de la camera (#{self.reopens})", flush=True)
                self._open()

    # --- une seule capture, partagee ---------------------------------------
    # Ouvrir le peripherique deux fois echoue sur la plupart des webcams, donc
    # un seul thread lit en continu et tout le monde se sert de la derniere
    # image.
    def _loop(self):
        last = []
        while self.running:
            # Tout le corps est protege : une seule exception non rattrapee
            # (image corrompue, hoquet du modele) tuait ce thread en silence,
            # et le flux restait fige pour toujours sans que rien ne l'indique.
            try:
                if not self.camera_ok:
                    time.sleep(1)
                    continue
                ok, frame = self.cap.read()
                if not ok:
                    time.sleep(0.1)
                    continue
                self.frames += 1
                self.last_frame_ms = time.time()

                clean = self._encode(frame)
                had_face = False
                if self.frames % max(1, SETTINGS["check_every_n"]) == 0:
                    last = self._analyse(frame)
                    had_face = bool(last)

                annotated = self._encode(self._annotate(frame.copy(), last))
                with self.lock:
                    self.clean_jpeg = clean
                    self.jpeg = annotated
                    self.faces = last
                    if had_face:
                        self.face_jpeg = annotated
                time.sleep(1 / 25)
            except Exception as exc:
                print(f"[vision] image ignoree : {exc}", flush=True)
                time.sleep(0.2)

    def _encode(self, frame):
        ok, buf = cv2.imencode(".jpg", frame,
                               [int(cv2.IMWRITE_JPEG_QUALITY), QUALITY])
        return buf.tobytes() if ok else None

    def _analyse(self, frame):
        h, w = frame.shape[:2]
        self.detector.setInputSize((w, h))
        _, raw = self.detector.detect(frame)
        if raw is None:
            return []

        # Suivi de vivacite d'abord : il faut une piste par detection, dans le
        # MEME ordre, pour pouvoir les apparier ensuite par indice.
        now = time.time()
        tracks = self.liveness.update(frame, raw)

        require = bool(liveness_mod.SETTINGS["require_liveness"])
        out = []
        for i, det in enumerate(raw):
            try:
                # alignCrop redresse le visage avant l'empreinte : sans ca, une
                # tete penchee ne ressemble plus a elle-meme.
                aligned = self.recognizer.alignCrop(frame, det)
                emb = self.recognizer.feature(aligned).flatten()
            except cv2.error:
                continue
            name, score = self.store.match(emb, SETTINGS["match_threshold"])
            box = tuple(int(v) for v in det[:4])
            track = tracks[i] if i < len(tracks) else None
            live = bool(track and track.is_live(now))

            recognised = name is not None
            # LA porte : sans vivacite confirmee, une correspondance ne vaut
            # pas une autorisation. Une photo de la bonne personne s'arrete
            # ici.
            known = recognised and (live or not require)

            prompt = ""
            if track is not None and recognised and not live and require:
                if track.too_far:
                    prompt = "Approchez-vous de la camera"
                elif track.challenge is not None:
                    prompt = track.challenge.prompt
            out.append(Face(box, name or "Inconnu", score, recognised, known,
                            live, prompt,
                            track.detail(now) if track is not None else None))
        return out

    def _annotate(self, frame, faces):
        for f in faces:
            x, y, bw, bh = f.box
            if f.known:
                colour = (80, 200, 120)        # vert : autorise
            elif f.recognised:
                colour = (60, 190, 240)        # orange : reconnu, vivacite a prouver
            else:
                colour = (80, 80, 240)         # rouge : inconnu
            cv2.rectangle(frame, (x, y), (x + bw, y + bh), colour, 2)
            if f.known:
                label = f"{f.name} {f.score:.2f}"
            elif f.recognised:
                label = f"{f.name} ? vivacite"
            else:
                label = "Inconnu"
            cv2.rectangle(frame, (x, y - 20), (x + max(90, bw), y), colour, -1)
            cv2.putText(frame, label, (x + 4, y - 6),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.5, (20, 20, 20), 1, cv2.LINE_AA)
            # La consigne du defi, SUR l'image : la personne regarde la camera,
            # pas le dashboard.
            if f.prompt:
                cv2.rectangle(frame, (x, y + bh), (x + max(240, bw), y + bh + 24),
                              (30, 30, 30), -1)
                cv2.putText(frame, f.prompt, (x + 5, y + bh + 17),
                            cv2.FONT_HERSHEY_SIMPLEX, 0.45, (240, 240, 240), 1,
                            cv2.LINE_AA)
        return frame

    # --- lectures ----------------------------------------------------------
    def latest(self, clean=False):
        with self.lock:
            return self.clean_jpeg if clean else self.jpeg

    def latest_with_face(self):
        """L'image du dernier visage vu, sinon l'image courante."""
        with self.lock:
            return self.face_jpeg or self.jpeg

    def current_faces(self):
        with self.lock:
            return list(self.faces)

    def current_prompt(self):
        """La consigne a afficher, s'il y en a une."""
        with self.lock:
            for f in self.faces:
                if f.prompt:
                    return f.prompt
        return ""

    def liveness_state(self):
        now = time.time()
        return {
            "settings": dict(liveness_mod.SETTINGS),
            "limits": {k: list(v) for k, v in liveness_mod.LIMITS.items()},
            "prompt": self.current_prompt(),
            "tracks": [t.detail(now) for t in self.liveness.tracks],
        }

    def reset_liveness(self):
        """Redonne un defi a tout le monde."""
        self.liveness.reset_all()

    def apply_settings(self, changes):
        """Applique des reglages a chaud. Renvoie (appliques, erreurs)."""
        # Les reglages de vivacite vivent dans leur module : on les y renvoie
        # plutot que de dupliquer des bornes qui finiraient par divergier.
        mine = {k: v for k, v in changes.items() if k not in liveness_mod.SETTINGS}
        theirs = {k: v for k, v in changes.items() if k in liveness_mod.SETTINGS}
        applied, errors = liveness_mod.apply_settings(theirs) if theirs else ({}, {})

        for k, v in mine.items():
            if k not in SETTINGS:
                errors[k] = "reglage inconnu"
                continue
            try:
                v = int(v) if isinstance(SETTINGS[k], int) else float(v)
            except (TypeError, ValueError):
                errors[k] = "valeur non numerique"
                continue
            lo, hi = LIMITS[k]
            if not (lo <= v <= hi):
                errors[k] = f"hors limites ({lo} - {hi})"
                continue
            SETTINGS[k] = v
            applied[k] = v
        # Le seuil de detection vit dans le modele, pas seulement dans le dict.
        if "detect_confidence" in applied:
            self.detector.setScoreThreshold(SETTINGS["detect_confidence"])
        return applied, errors

    def status(self):
        with self.lock:
            faces = [{"name": f.name, "known": f.known,
                      "recognised": f.recognised, "live": f.live,
                      "prompt": f.prompt, "liveness": f.live_detail,
                      "score": round(f.score, 3)}
                     for f in self.faces]
        stale = time.time() - self.last_frame_ms if self.last_frame_ms else None
        return {
            "camera": self.camera_ok,
            "error": self.error,
            "frames": self.frames,
            "faces": faces,
            "settings": dict(SETTINGS),
            "liveness": dict(liveness_mod.SETTINGS),
            "stale_seconds": round(stale, 1) if stale is not None else None,
            "reopens": self.reopens,
        }

    # --- enrolement --------------------------------------------------------
    def enrol(self, name):
        """Apprend le visage actuellement devant la camera.

        Renvoie (id, None) ou (None, raison). On exige exactement un visage :
        avec deux personnes dans le champ, on ne peut pas savoir laquelle on
        est cense enregistrer.
        """
        if not self.camera_ok:
            return None, "camera indisponible"
        # On repart de la derniere image capturee plutot que de relire le
        # peripherique : une seconde lecture concurrente du meme /dev/video
        # echoue sur la plupart des webcams.
        with self.lock:
            jpg = self.clean_jpeg
        if jpg is None:
            return None, "pas encore d'image"
        frame = cv2.imdecode(np.frombuffer(jpg, np.uint8), cv2.IMREAD_COLOR)

        h, w = frame.shape[:2]
        self.detector.setInputSize((w, h))
        _, raw = self.detector.detect(frame)
        if raw is None or len(raw) == 0:
            return None, "aucun visage detecte - placez-vous face a la camera"
        if len(raw) > 1:
            return None, f"{len(raw)} visages dans le champ - une seule personne a la fois"

        # On n'enregistre pas une photo. Sans cette garde, un attaquant ayant
        # obtenu la maintenance pourrait enroler l'image imprimee de quelqu'un
        # d'autre, et le modele garderait pour toujours une empreinte qu'une
        # simple feuille de papier rejoue.
        if liveness_mod.SETTINGS["require_liveness"]:
            live = [t for t in self.liveness.tracks if t.is_live(time.time())]
            if not live:
                return None, ("vivacite non confirmee - "
                              + (self.current_prompt() or "tournez la tete"))

        det = raw[0]
        aligned = self.recognizer.alignCrop(frame, det)
        emb = self.recognizer.feature(aligned).flatten()

        x, y, bw, bh = (int(v) for v in det[:4])
        pad = int(0.25 * max(bw, bh))
        crop = frame[max(0, y - pad):min(h, y + bh + pad),
                     max(0, x - pad):min(w, x + bw + pad)]
        thumb = self._encode(cv2.resize(crop, (160, 160)))
        return self.store.add(name, emb, thumb), None
