"""
Persistance : visages autorises et instantanes d'evenements.

Tout tient dans un volume Docker, en JSON + JPEG. Pas de base de donnees : il
s'agit d'une poignee de visages et de quelques centaines d'images, et un
fichier qu'on peut lire et sauvegarder a la main vaut mieux ici qu'un schema.
"""
import json
import os
import threading
import time
import uuid

import numpy as np

DATA = os.getenv("DATA_DIR", "/data")
FACES_DIR = os.path.join(DATA, "faces")
EVENTS_DIR = os.path.join(DATA, "events")
FACES_DB = os.path.join(FACES_DIR, "faces.json")

MAX_EVENTS = int(os.getenv("MAX_EVENTS", "200"))


def _ensure():
    os.makedirs(FACES_DIR, exist_ok=True)
    os.makedirs(EVENTS_DIR, exist_ok=True)


class FaceStore:
    """Les personnes autorisees, avec leur empreinte faciale."""

    def __init__(self):
        _ensure()
        self.lock = threading.Lock()
        self.people = []
        self._load()

    def _load(self):
        try:
            with open(FACES_DB, encoding="utf-8") as f:
                self.people = json.load(f)
        except (FileNotFoundError, json.JSONDecodeError):
            self.people = []

    def _save(self):
        # Ecriture atomique : une coupure de courant en pleine sauvegarde ne
        # doit pas laisser un fichier tronque qui ferait tout perdre.
        tmp = FACES_DB + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(self.people, f, ensure_ascii=False, indent=1)
        os.replace(tmp, FACES_DB)

    def list(self):
        with self.lock:
            return [
                {"id": p["id"], "name": p["name"], "added": p["added"],
                 "samples": len(p["embeddings"])}
                for p in self.people
            ]

    def add(self, name, embedding, thumbnail_jpg):
        """Ajoute une personne, ou une empreinte de plus a une personne connue.

        Plusieurs empreintes par personne : un seul angle de prise de vue donne
        une reconnaissance fragile des que la personne tourne la tete.
        """
        with self.lock:
            existing = next((p for p in self.people
                             if p["name"].lower() == name.lower()), None)
            if existing:
                existing["embeddings"].append(embedding.tolist())
                pid = existing["id"]
            else:
                pid = uuid.uuid4().hex[:12]
                self.people.append({
                    "id": pid,
                    "name": name,
                    "added": time.time(),
                    "embeddings": [embedding.tolist()],
                })
            with open(os.path.join(FACES_DIR, f"{pid}.jpg"), "wb") as f:
                f.write(thumbnail_jpg)
            self._save()
            return pid

    def remove(self, pid):
        with self.lock:
            before = len(self.people)
            self.people = [p for p in self.people if p["id"] != pid]
            if len(self.people) == before:
                return False
            self._save()
        try:
            os.remove(os.path.join(FACES_DIR, f"{pid}.jpg"))
        except FileNotFoundError:
            pass
        return True

    def thumbnail(self, pid):
        # Meme garde que EventStore.image(). Sans elle, ?id=../events/<nom>
        # renvoyait n'importe quel .jpg du disque - verifie en direct, HTTP 200
        # sur une photo d'evenement. Le ".jpg" ajoute plus bas empechait de lire
        # /etc/passwd, mais c'est une contrainte heureuse, pas une protection.
        if "/" in pid or ".." in pid:
            return None
        try:
            with open(os.path.join(FACES_DIR, f"{pid}.jpg"), "rb") as f:
                return f.read()
        except FileNotFoundError:
            return None

    def match(self, embedding, threshold):
        """Renvoie (nom, score) de la meilleure correspondance, ou (None, score).

        Similarite cosinus : les empreintes SFace sont comparees par angle, pas
        par distance euclidienne.
        """
        best_name, best = None, 0.0
        with self.lock:
            for p in self.people:
                for e in p["embeddings"]:
                    ref = np.asarray(e, dtype=np.float32)
                    denom = np.linalg.norm(ref) * np.linalg.norm(embedding)
                    if denom == 0:
                        continue
                    score = float(np.dot(ref, embedding) / denom)
                    if score > best:
                        best, best_name = score, p["name"]
        return (best_name, best) if best >= threshold else (None, best)


class EventStore:
    """Instantanes pris lors des evenements de l'alarme."""

    def __init__(self):
        _ensure()
        self.lock = threading.Lock()

    def save(self, jpg, reason, detail=""):
        ts = time.time()
        name = f"{int(ts * 1000)}.jpg"
        with self.lock:
            with open(os.path.join(EVENTS_DIR, name), "wb") as f:
                f.write(jpg)
            meta = {"file": name, "ts": ts, "reason": reason, "detail": detail}
            with open(os.path.join(EVENTS_DIR, name + ".json"), "w",
                      encoding="utf-8") as f:
                json.dump(meta, f, ensure_ascii=False)
            self._trim()
        return meta

    def _trim(self):
        """Garde les MAX_EVENTS plus recents : le volume ne doit pas gonfler
        sans fin sur une machine de demo."""
        files = sorted(f for f in os.listdir(EVENTS_DIR) if f.endswith(".jpg"))
        for old in files[:-MAX_EVENTS]:
            for path in (old, old + ".json"):
                try:
                    os.remove(os.path.join(EVENTS_DIR, path))
                except FileNotFoundError:
                    pass

    def list(self, limit=40):
        with self.lock:
            files = sorted((f for f in os.listdir(EVENTS_DIR) if f.endswith(".json")),
                           reverse=True)[:limit]
            out = []
            for f in files:
                try:
                    with open(os.path.join(EVENTS_DIR, f), encoding="utf-8") as fh:
                        out.append(json.load(fh))
                except (OSError, json.JSONDecodeError):
                    continue
            return out

    def image(self, name):
        # Un nom venant du reseau ne doit pas pouvoir sortir du dossier.
        if "/" in name or ".." in name:
            return None
        try:
            with open(os.path.join(EVENTS_DIR, name), "rb") as f:
                return f.read()
        except (FileNotFoundError, IsADirectoryError):
            return None
