"""
Detection de vivacite : distinguer un vrai visage d'une photo de ce visage.

------------------------------------------------------------------------
CE QUI A ETE ESSAYE, MESURE, ET REJETE
------------------------------------------------------------------------

L'idee seduisante est passive : une photo est un objet PLAN, donc son
mouvement dans l'image s'explique entierement par une transformation plane,
alors qu'un vrai visage a du relief - le nez est ~2 cm devant le plan des yeux
- et produit une parallaxe qu'aucun plan ne peut absorber. Mesure sur ce
materiel, en simulation puis sur de vraies images :

  * la premisse est juste : le relief donne 5 a 6 fois le residu d'un plan ;
  * mais le bruit des points de YuNet sur cette camera vaut 1.86 a 3.06 px
    (mesure, visage de 54 px entre les yeux). Or la separation ne tient qu'en
    dessous de ~1 px de bruit : a 2 px les deux populations se chevauchent
    completement ;
  * ajuster une homographie au lieu d'une affine est parfait sans bruit (residu
    exactement nul pour un plan) et INUTILISABLE avec : 8 degres de liberte
    pour 5 points devient mal conditionne, et une photo a pu sortir un residu
    de 2787 ;
  * le flot optique dense (~50 points suivis) devait regler le bruit par la
    moyenne. En pratique la fraction de points hors du plan dominant suit le
    DEPLACEMENT, pas la geometrie : derive accumulee de Lucas-Kanade. Une photo
    qu'on approche a donne 48.9 % de points hors-plan, bien plus qu'un vrai
    visage (8.7 % a 20 deg). Mesure inutilisable telle quelle.

Conclusion : la vivacite PASSIVE fiable n'est pas atteignable ici - une seule
webcam 640x480, pas de profondeur, pas d'infrarouge, et aucun jeu d'attaques
pour calibrer un seuil. Pretendre le contraire donnerait un faux sentiment de
securite, ce qui est pire que de ne rien avoir.

------------------------------------------------------------------------
CE QUI MARCHE : LE DEFI
------------------------------------------------------------------------

On arrete de chercher un effet subtil sous le bruit, et on EXIGE un mouvement
ample qu'une photo ne peut pas produire : tourner la tete d'un cote puis de
l'autre.

Indicateur, l'asymetrie du nez entre les deux yeux :

    a = ((nez.x - oeil_droit.x) + (nez.x - oeil_gauche.x)) / inter_oculaire

Tete droite, le nez est a mi-chemin : a = 0. Tete tournee, le nez se decale du
cote vise. Et c'est la que photo et visage divergent franchement : une photo
qu'on PIVOTE se raccourcit symetriquement autour de son propre axe, donc son
asymetrie bouge a peine ; un vrai nez, lui, balaye lateralement.

Mesure, 300 tirages avec 2.5 px de bruit, a 30 degres :

    photo  : -0.013 +/- 0.117
    visage : -0.397 +/- 0.116

30 fois l'ecart, pour un bruit de 0.117 : 3.3 sigma sur UN SEUL releve, et on
en obtient une dizaine pendant le defi. L'amplitude ("swing") entre les deux
extremes du balayage est encore plus nette, et elle a l'avantage de ne pas
dependre du cote vers lequel la personne tourne - donc aucun risque de se
tromper de signe.

------------------------------------------------------------------------
CE QUE CA N'ARRETE PAS
------------------------------------------------------------------------

Une VIDEO rejouee sur un ecran, montrant la personne en train de tourner la
tete, satisfait le defi : l'ecran est plan, mais le contenu encode un vrai
mouvement 3D. C'est la limite de fond de toute verification monoculaire sans
profondeur. Deux choses la reduisent sans la supprimer :

  * le defi est TIRE AU HASARD parmi plusieurs et doit etre satisfait dans la
    fenetre d'identification : l'attaquant doit posseder la bonne sequence et
    la jouer au bon moment ;
  * les indicateurs de texture (nettete, moire d'ecran) sont calcules et
    publies. Ils ne DECIDENT rien : faute de vraies attaques enregistrees pour
    les calibrer, aucun seuil ne serait honnete. Ils sont la pour etre
    etalonnes plus tard, et pour qu'un ecran evident se voie dans le dashboard.

Voir docs/AUDIT-SECURITE.md.
"""
import math
import os
import random
import time
from collections import deque

import cv2
import numpy as np

SETTINGS = {
    # 1 : un visage reconnu ne compte comme autorise qu'apres un defi reussi.
    # 0 : on MESURE et on publie, sans rien bloquer.
    #
    # Par defaut 1, parce qu'un controle de securite desactive par defaut n'en
    # est pas un. Consequence a connaitre : a chaque approche, une personne
    # reconnue doit tourner la tete dans la fenetre d'identification, sinon
    # elle compte comme inconnue et le decompte d'alarme part. La consigne
    # s'affiche SUR l'image de la camera, et un bouton du dashboard repasse en
    # mesure-seule si c'est penible.
    "require_liveness": int(os.getenv("REQUIRE_LIVENESS", "1")),
    # Asymetrie SOUTENUE a atteindre de chaque cote, sur la mediane glissante.
    #
    # 0.30, releve depuis 0.25 quand la fenetre d'identification du boitier est
    # passee de 5 a 10 s. Ce n'est pas un reglage de confort : une fenetre deux
    # fois plus longue donne deux fois plus de tirages a un attaquant qui agite
    # une photo, et il faut relever la barre pour compenser. Mesure a 75 px,
    # fenetre de 10 s, 800 tirages :
    #
    #   seuil 0.25 : 9.0 % d'attaques acceptees, 99.9 % des vrais visages a 30 deg
    #   seuil 0.30 : 0.5 %                       96.2 %
    #   seuil 0.35 : 0.1 %                       71.6 %
    #
    # 0.30 est le compromis : la rotation doit etre FRANCHE (un vrai visage a
    # 40 deg passe a 100 %), mais une photo ne passe plus.
    "turn_required": float(os.getenv("LIVE_TURN", "0.30")),
    # Duree d'UN essai de defi - a ne pas confondre avec la fenetre
    # d'identification du boitier, qui est plus longue (10 s).
    #
    # Les deux ont ete deliberement separees, chiffres a l'appui : allonger
    # l'essai lui-meme est desastreux, parce que chaque releve de plus est une
    # chance de plus qu'un extreme de bruit franchisse le seuil. Mesure a 75 px,
    # 800 essais, attaque = photo agitee dans tous les sens :
    #
    #    essai de  5 s :  0.0 % d'attaques acceptees
    #    essai de  8 s :  4.4 %
    #    essai de 10 s : 18.6 %
    #    essai de 15 s : 60.8 %
    #
    # On garde donc des essais COURTS, et on en autorise plusieurs dans la
    # fenetre d'identification : deux essais de 5 s valent bien mieux qu'un
    # seul de 10 s, pour l'attaquant comme pour l'utilisateur.
    "challenge_seconds": float(os.getenv("LIVE_CHALLENGE_SECONDS", "5.0")),
    # Delai avant de proposer un nouvel essai apres un echec. Court, pour que
    # un deuxieme essai tienne dans la fenetre d'identification de 10 s.
    "retry_seconds": float(os.getenv("LIVE_RETRY_SECONDS", "1.5")),
    # Vivacite acquise : tient ce temps avant qu'un nouveau defi soit exige.
    # Sinon la personne devrait rejouer le defi en permanence.
    "hold_seconds": float(os.getenv("LIVE_HOLD_SECONDS", "90")),
    # Distance inter-oculaire minimale pour que le defi soit juge.
    #
    # C'est LE reglage qui ameliore les deux taux a la fois : le bruit de
    # l'asymetrie vaut ~2*bruit_px/inter_oculaire, donc un visage plus gros est
    # moins bruite. Mesure, 600 essais par taille, bruit 2.5 px :
    #
    #   45 px (0.84 m) : 5.0 % d'attaques acceptees  -  inacceptable
    #   60 px (0.63 m) : 0.5 %
    #   75 px (0.50 m) : 0.2 %, et 100 % des vrais visages a 30 deg
    #  110 px (0.34 m) : 0.0 %
    #
    # 75 px, soit environ 50 cm de la camera. Plus loin, on ne tranche pas :
    # on affiche "approchez-vous" plutot que de deviner.
    "min_iod_px": float(os.getenv("LIVE_MIN_IOD", "75")),
    # Lissage : mediane sur N releves avant de retenir un extreme, pour qu'un
    # seul point aberrant ne valide pas un defi.
    # Taille de la mediane glissante. 5 releves a ~5 Hz = 1 s par cote.
    "smooth": int(os.getenv("LIVE_SMOOTH", "5")),
    # Releves CONSECUTIFS que la mediane doit tenir au-dela du seuil.
    #
    # Defaut 1, c'est-a-dire desactive, APRES mesure : porte a 3, les faux
    # refuses d'un vrai visage tourne a 30 deg passaient de 0.6 % a 6.0 %
    # tandis que l'attaque la plus agressive ne baissait pas (0.6 -> 1.0 %, du
    # bruit d'echantillonnage). La raison est geometrique : une tete qui
    # balaye ne passe qu'un instant par ses extremes, donc exiger une DUREE a
    # l'extreme penalise surtout l'utilisateur legitime. Le reglage reste
    # disponible pour qui voudrait durcir.
    "sustain": int(os.getenv("LIVE_SUSTAIN", "1")),
}

LIMITS = {
    "require_liveness":  (0, 1),
    # Plancher 0.15 : en dessous on s'approche du bruit (0.117 mesure) et une
    # photo agitee finirait par passer. C'est le reglage sensible.
    # Plancher 0.15 : en dessous on entre dans le bruit de la mediane et une
    # photo agitee finit par passer, par pur hasard d'extremes.
    "turn_required":     (0.15, 1.50),
    "challenge_seconds": (2.0, 60.0),
    "retry_seconds":     (0.0, 30.0),
    "hold_seconds":      (0.0, 3600.0),
    "min_iod_px":        (15.0, 200.0),
    "smooth":            (1, 15),
    "sustain":           (1, 15),
}

# Un seul type de defi pour l'instant, volontairement : il est sans ambiguite
# de signe et donc sans risque de se tromper de cote. Les variantes sont la
# pour que le tirage au hasard ait un sens face a une video rejouee.
CHALLENGES = {
    "tete-gauche-droite": "Tournez la tete a gauche, puis a droite",
    "tete-droite-gauche": "Tournez la tete a droite, puis a gauche",
}

UNKNOWN, LIVE, FAILED = "inconnu", "vivant", "echec"


def landmarks(det):
    """Les 5 points de YuNet : oeil droit, oeil gauche, nez, bouche x2."""
    return np.array(det[4:14], dtype=np.float64).reshape(5, 2)


def iod(pts):
    return float(np.linalg.norm(pts[0] - pts[1]))


def asymmetry(pts):
    """Decalage lateral du nez, dans le repere du visage. 0 = tete droite.

    INVARIANT AU ROULIS, et ce n'est pas un detail. La version naive prenait
    les x de l'image brute. Or le nez est ~3.5 cm sous la ligne des yeux, donc
    PENCHER la tete - ou la photo - de l'angle phi deplace son x d'environ
    1.11 * sin(phi) en unites normalisees : 0.38 a seulement 20 degres, au-dela
    du seuil de 0.35. Autrement dit il suffisait de bercer une photo imprimee
    d'un cote a l'autre pour satisfaire le defi. Verifie par le test.

    On redresse donc la ligne des yeux avant de mesurer : le roulis sort de
    l'equation, et il ne reste que ce qu'on veut vraiment voir, le nez qui
    balaye lateralement parce qu'il est EN RELIEF.
    """
    d = iod(pts)
    if d < 1e-6:
        return 0.0
    eye = pts[1] - pts[0]
    phi = math.atan2(eye[1], eye[0])
    c, sn = math.cos(-phi), math.sin(-phi)
    rot = np.array([[c, -sn], [sn, c]])
    q = (pts - pts[:2].mean(axis=0)) @ rot.T      # origine au milieu des yeux
    return float(2.0 * q[2, 0] / d)


def texture_scores(frame, box):
    """Nettete et moire. RAPPORTES, jamais decisifs - voir l'en-tete."""
    x, y, w, h = (int(v) for v in box)
    fh, fw = frame.shape[:2]
    x, y = max(0, x), max(0, y)
    w, h = min(w, fw - x), min(h, fh - y)
    if w < 24 or h < 24:
        return None, None
    crop = cv2.cvtColor(frame[y:y + h, x:x + w], cv2.COLOR_BGR2GRAY)
    crop = cv2.resize(crop, (128, 128))
    sharp = float(cv2.Laplacian(crop, cv2.CV_64F).var())
    f = np.fft.fftshift(np.abs(np.fft.fft2(crop.astype(np.float64))))
    yy, xx = np.ogrid[:128, :128]
    r = np.sqrt((yy - 64) ** 2 + (xx - 64) ** 2)
    total = f[r > 2].sum()
    moire = float(f[(r > 40) & (r <= 60)].sum() / total) if total > 0 else 0.0
    return round(sharp, 1), round(moire, 5)


class Challenge:
    """Un defi en cours : ce qu'on demande, et ce qu'on a vu."""

    __slots__ = ("kind", "issued", "deadline", "neg", "pos", "samples",
                 "state", "reason", "run_neg", "run_pos", "held_neg", "held_pos")

    def __init__(self, kind, now):
        self.kind = kind
        self.issued = now
        self.deadline = now + SETTINGS["challenge_seconds"]
        # Le plus bas et le plus haut atteints par la MEDIANE GLISSANTE, pas
        # par les releves bruts - voir feed().
        self.neg = self.pos = 0.0
        self.samples = deque(maxlen=max(1, SETTINGS["smooth"]))
        self.state = UNKNOWN
        self.reason = ""
        # Longueur de la serie en cours de chaque cote, et si la duree exigee
        # a deja ete tenue au moins une fois.
        self.run_neg = self.run_pos = 0
        self.held_neg = self.held_pos = False

    @property
    def prompt(self):
        return CHALLENGES.get(self.kind, self.kind)

    def feed(self, a, now):
        """Un releve d'asymetrie. Met a jour le verdict.

        On exige une asymetrie SOUTENUE de chaque cote, mesuree sur la mediane
        glissante - pas l'amplitude entre le minimum et le maximum des releves
        bruts. La premiere version faisait exactement ca, et c'etait faux : le
        maximum moins le minimum de N tirages bruites grandit avec N, par pure
        statistique des extremes. Avec un bruit de 0.117 et 24 releves,
        l'amplitude attendue est de ~0.41 QUOI QU'IL Y AIT devant la camera.
        Mesure : un visage parfaitement immobile sortait 0.304, et trois
        mouvements de photo passaient le seuil de 0.35.

        Une mediane glissante qui se maintient d'un cote, elle, ne peut pas
        venir d'un bruit centre : il faudrait que 3 releves sur 5 derivent du
        meme cote, et qu'ensuite la meme chose arrive de l'autre cote.
        """
        if self.state != UNKNOWN:
            return
        self.samples.append(a)
        need = SETTINGS["turn_required"]
        sustain = max(1, SETTINGS["sustain"])
        if len(self.samples) >= self.samples.maxlen:
            m = float(np.median(self.samples))
            self.neg = min(self.neg, m)
            self.pos = max(self.pos, m)
            # Series consecutives : un franchissement isole ne compte pas.
            self.run_neg = self.run_neg + 1 if m <= -need else 0
            self.run_pos = self.run_pos + 1 if m >= need else 0
            if self.run_neg >= sustain:
                self.held_neg = True
            if self.run_pos >= sustain:
                self.held_pos = True

        if self.held_neg and self.held_pos:
            self.state = LIVE
            self.reason = f"tourne de part et d'autre ({self.neg:+.2f} / {self.pos:+.2f})"
        elif now > self.deadline:
            self.state = FAILED
            cotes = ("aucun cote" if not (self.held_neg or self.held_pos)
                     else "un seul cote")
            self.reason = (f"rotation insuffisante : {cotes} tenu "
                           f"({self.neg:+.2f} / {self.pos:+.2f}, il faut "
                           f"+/-{need:.2f} pendant {sustain} releves)")

    def expired(self, now):
        return self.state == UNKNOWN and now > self.deadline

    def detail(self):
        return {
            "defi": self.kind,
            "consigne": self.prompt,
            "etat": self.state,
            "atteint": [round(self.neg, 3), round(self.pos, 3)],
            "cotes_tenus": [self.held_neg, self.held_pos],
            "requis": SETTINGS["turn_required"],
            "restant": max(0.0, round(self.deadline - time.time(), 1)),
            "raison": self.reason,
        }


class FaceTrack:
    __slots__ = ("last_seen", "centre", "challenge", "live_until",
                 "sharpness", "moire", "last_iod", "too_far", "last_asym")

    def __init__(self):
        self.last_seen = 0.0
        self.centre = None
        self.challenge = None
        self.live_until = 0.0
        self.sharpness = self.moire = None
        self.last_iod = 0.0
        self.too_far = False
        self.last_asym = 0.0

    def is_live(self, now):
        return now < self.live_until

    def verdict(self, now):
        if self.is_live(now):
            return LIVE
        if self.challenge is not None and self.challenge.state == FAILED:
            return FAILED
        return UNKNOWN

    def observe(self, pts, now):
        self.last_seen = now
        self.centre = (float(pts[:, 0].mean()), float(pts[:, 1].mean()))
        self.last_iod = iod(pts)
        self.too_far = self.last_iod < SETTINGS["min_iod_px"]
        if self.too_far:
            return
        self.last_asym = asymmetry(pts)
        if self.is_live(now):
            return
        if self.challenge is None or self.challenge.state == FAILED:
            if self.challenge is None:
                self.challenge = Challenge(random.choice(list(CHALLENGES)), now)
            else:
                return      # echec : il faut un nouveau cycle, voir reset()
        self.challenge.feed(self.last_asym, now)
        if self.challenge.state == LIVE:
            self.live_until = now + SETTINGS["hold_seconds"]

    def reset(self, now):
        """Relance un defi. Appele par le dashboard, ou apres un echec expire."""
        self.challenge = Challenge(random.choice(list(CHALLENGES)), now)
        self.live_until = 0.0

    def detail(self, now):
        d = {
            "etat": self.verdict(now),
            "inter_oculaire": round(self.last_iod, 1),
            "trop_loin": self.too_far,
            "asymetrie": round(self.last_asym, 3),
            "tient_jusqua": round(max(0.0, self.live_until - now), 1),
            "nettete": self.sharpness,
            "moire": self.moire,
        }
        if self.challenge is not None:
            d["challenge"] = self.challenge.detail()
        return d


class Liveness:
    MATCH_FRACTION = 0.6
    FORGET_SECONDS = 2.0

    def __init__(self):
        self.tracks = []

    def update(self, frame, dets):
        """dets : lignes brutes de YuNet. Renvoie une piste par detection."""
        now = time.time()
        out = []
        for det in dets:
            pts = landmarks(det)
            t = self._match(det[:4], now)
            # Apres un echec, on laisse un instant puis on redonne une chance :
            # quelqu'un de legitime qui n'a pas compris la consigne doit
            # pouvoir reessayer sans intervention.
            if (t.challenge is not None and t.challenge.state == FAILED
                    and now - t.challenge.deadline > SETTINGS["retry_seconds"]):
                t.reset(now)
            t.observe(pts, now)
            t.sharpness, t.moire = texture_scores(frame, det[:4])
            out.append(t)
        self.tracks = [t for t in self.tracks
                       if now - t.last_seen < self.FORGET_SECONDS]
        return out

    def _match(self, box, now):
        cx, cy = box[0] + box[2] / 2.0, box[1] + box[3] / 2.0
        tol = max(20.0, float(box[2]) * self.MATCH_FRACTION)
        best, best_d = None, tol
        for t in self.tracks:
            if t.centre is None:
                continue
            d = math.hypot(cx - t.centre[0], cy - t.centre[1])
            if d < best_d:
                best, best_d = t, d
        if best is None:
            best = FaceTrack()
            self.tracks.append(best)
        return best

    def reset_all(self):
        now = time.time()
        for t in self.tracks:
            t.reset(now)


def apply_settings(changes):
    applied, errors = {}, {}
    for k, v in changes.items():
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
    return applied, errors
