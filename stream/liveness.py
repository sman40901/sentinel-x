"""
Vivacite : distinguer un vrai visage d'une photo de ce visage.

------------------------------------------------------------------------
POURQUOI IL N'Y A PLUS DE "TOURNEZ LA TETE"
------------------------------------------------------------------------

La version precedente demandait de tourner la tete a gauche puis a droite, et
mesurait le decalage lateral du nez. Elle echouait instantanement en usage, et
la cause est structurelle : **tourner la tete reduit la distance apparente
entre les yeux**, de cos(angle). Un visage de 55 px vu de face n'en fait plus
que 39 a 45 degres, ce qui passe sous la taille minimale exigee - donc les
releves etaient jetes exactement au moment ou le signal etait le plus fort,
pendant que le chronometre du defi continuait. Le defi se battait contre sa
propre mesure.

Et sur le fond, exiger une consigne pose un probleme d'usage insoluble ici :
la personne devant la camera ne voit pas le dashboard.

------------------------------------------------------------------------
CE QUI REMPLACE : LE CHANGEMENT D'APPARENCE, SANS RIEN DEMANDER
------------------------------------------------------------------------

Un vrai visage CHANGE tout seul : on cligne des yeux toutes les 3 a 4
secondes, la bouche bouge, les traits se deforment. Une photo ne change
jamais, quoi qu'on en fasse.

On redresse donc chaque visage dans le repere canonique d'alignCrop - celui que
SFace utilise deja pour la reconnaissance, donc sans calcul supplementaire -
puis on regarde OU ca change :

    rapport = variation(yeux + bouche) / variation(front + joues)

Le front et les joues servent de TEMOIN. Sur une photo, ce qu'on mesure n'est
que du bruit capteur et de l'erreur de recalage, repartis uniformement : les
deux regions varient pareil, le rapport reste bas. Sur un vrai visage, les
yeux et la bouche varient en plus du reste.

L'interet du temoin est qu'il normalise tout seul. Mesure sur une photo reelle
bougee a la main, du simple tremblement a l'agitation violente :

    immobile          0.46        agitee            0.49
    leger tremblement 0.54        tres agitee       0.53

Le rapport ne bouge quasiment pas malgre des amplitudes tres differentes. Les
tentatives precedentes echouaient precisement la : leur mesure suivait
l'amplitude du mouvement au lieu de la nature de l'objet.

------------------------------------------------------------------------
ETAT REEL : CINQ METHODES, AUCUNE FIABLE SUR CE MATERIEL
------------------------------------------------------------------------

Toutes ont ete mesurees, aucune ne tient :

  1. parallaxe affine sur les 5 points de YuNet - le bruit reel des points
     (1.9 a 3.1 px) noie un signal du meme ordre ;
  2. ajustement par homographie - 8 degres de liberte pour 5 points, mal
     conditionne, une photo a sorti un residu de 2787 ;
  3. flot optique dense, fraction hors du plan dominant - suit le DEPLACEMENT
     et non la geometrie : une photo approchee a donne 48.9 % contre 8.7 %
     pour un vrai visage ;
  4. defi de rotation de tete - casse par construction, tourner la tete REDUIT
     la distance inter-oculaire de cos(angle), donc les releves etaient jetes
     au moment ou le signal etait le plus fort ; et une photo gauchie a
     l'extreme atteint 0.92 contre un seuil de 0.60 ;
  5. rapport de changement d'apparence, ci-dessous - depend de la duree
     d'observation et non de l'objet.

Le point commun : une seule webcam 640x480 grand angle, un visage de 30 a
42 px entre les yeux a distance normale, pas de profondeur, pas
d'infrarouge, et aucun jeu d'attaques enregistrees pour calibrer un seuil.
A cette resolution, le signal qui distingue un visage d'une photo est du
meme ordre que le bruit.

Ce qui changerait la donne, par ordre d'efficacite : une camera avec
profondeur ou infrarouge ; a defaut une camera de meilleure resolution ou
montee plus pres, pour que le visage fasse 100 px et non 35 ; a defaut un
modele anti-spoofing entraine, avec un vrai jeu d'attaques pour l'evaluer
sur CETTE camera.

------------------------------------------------------------------------
LA MESURE QUI RESTE, A TITRE INDICATIF
------------------------------------------------------------------------

La marge mesurable aujourd'hui est MINCE : pire photo 0.54, plus bas vrai
visage 0.59. Huit pour cent d'ecart, ce qui ne suffit pas pour bloquer
quelqu'un.

La raison est une limite des donnees, pas de la methode : faute de sujet devant
la camera, le cote "vrai visage" a ete estime sur des captures INDEPENDANTES,
prises a vingt secondes d'intervalle, avec de gros changements de pose. Le
recalage ne les normalise pas entierement, donc le temoin varie beaucoup et le
rapport est dilue. Sur une vraie sequence courte, ou la pose est stable et ou
seuls les clignements bougent, le rapport devrait etre nettement plus haut -
mais "devrait" n'est pas une mesure.

D'ou le choix par defaut : **require_liveness = 0**. Le rapport est calcule,
publie et affiche en direct dans le dashboard, mais il ne bloque personne. Il
suffit de rester quelques secondes devant la camera, de lire le chiffre, puis
de montrer une photo et de lire le chiffre : l'ecart reel apparait, le seuil se
pose dessus, et on active le blocage en un clic.

Ce qu'aucun reglage ne reglera : une VIDEO rejouee sur un ecran cligne des yeux
comme un vrai visage. Voir docs/AUDIT-SECURITE.md.
"""
import math
import os
import time
from collections import deque

import cv2
import numpy as np

SETTINGS = {
    # 0 - LE BLOCAGE EST DESACTIVE, ET C'EST UN CONSTAT D'ECHEC ASSUME.
    #
    # Aucune des methodes essayees ne separe de facon fiable sur ce materiel
    # (voir l'en-tete du fichier). La derniere en date, le rapport de
    # changement d'apparence, a donne sur LE MEME visage vivant en direct :
    #
    #   fenetre de 4 s, ~14 releves : 0.70 a 0.77   (au-dessus du seuil)
    #   fenetre de 6 s, ~21 releves : 0.21 a 0.50   (dans la plage d'une photo)
    #
    # Le rapport depend donc de la duree d'observation, pas de la nature de
    # l'objet observe. Bloquer sur une mesure comme celle-la refuserait
    # l'entree a de vraies personnes au hasard.
    #
    # Le rapport reste calcule, publie et affiche : il informe, il ne decide
    # pas. A passer a 1 seulement apres avoir etabli un seuil sur ses propres
    # mesures, et en sachant que la marge est faible.
    "require_liveness": int(os.getenv("REQUIRE_LIVENESS", "0")),
    # Rapport a atteindre.
    #
    # 0.60, et cette fois la marge est MESUREE des deux cotes, sur cette
    # camera :
    #
    #   photo reelle bougee a la main   : 0.46 a 0.54  (quelle que soit
    #                                     l'amplitude du mouvement)
    #   vrai visage vivant, en direct   : 0.70 a 0.77
    #
    # 0.60 est a mi-chemin, ~11 % au-dessus du pire cas photo et ~14 % sous le
    # vrai visage. C'est la premiere version de ce controle dont les deux cotes
    # viennent d'une mesure et non d'un modele.
    "change_ratio": float(os.getenv("LIVE_CHANGE_RATIO", "0.60")),
    # Profondeur de la fenetre d'observation. 6 s et non 4 : il faut qu'un
    # clignement ait le temps de s'y produire - on cligne toutes les 3 a 4
    # secondes - et la detection est intermittente, donc une fenetre courte se
    # retrouve souvent sous le minimum de releves.
    "window_seconds": float(os.getenv("LIVE_WINDOW_SECONDS", "6.0")),
    # Nombre de visages alignes avant de se prononcer. Abaisse de 8 a 5 :
    # observe en direct, le compte de releves retombait regulierement a 4-7
    # parce que le detecteur ne trouve pas le visage sur chaque image analysee,
    # et le rapport ne pouvait alors plus etre recalcule.
    "min_samples": int(os.getenv("LIVE_MIN_SAMPLES", "5")),
    # Taille minimale pour que le recalage soit propre.
    #
    # 25 px. Mesure en direct : une personne assise normalement devant cette
    # camera grand angle donne 30 a 38 px entre les yeux. Les valeurs
    # precedentes - 75 puis 45 puis 35 - coupaient donc tout ou partie d'un
    # usage normal. Il n'y a plus de rotation exigee, donc plus de
    # retrecissement a compenser, et le rapport d'une photo reste stable de
    # 44 a 107 px.
    "min_iod_px": float(os.getenv("LIVE_MIN_IOD", "25")),
    # Une fois la vivacite etablie, elle tient ce temps.
    "hold_seconds": float(os.getenv("LIVE_HOLD_SECONDS", "60")),
}

LIMITS = {
    "require_liveness": (0, 1),
    "change_ratio":     (0.20, 3.00),
    "window_seconds":   (1.0, 30.0),
    "min_samples":      (3, 60),
    "min_iod_px":       (15.0, 200.0),
    "hold_seconds":     (0.0, 3600.0),
}

UNKNOWN, LIVE, SUSPECT = "inconnu", "vivant", "suspect"

# Regions dans le repere canonique d'alignCrop en 112x112 (gabarit
# ArcFace/SFace : yeux vers y=51, nez y=72, bouche y=92).
R_YEUX   = (np.s_[40:63],  np.s_[26:86])
R_BOUCHE = (np.s_[82:104], np.s_[33:79])
R_FRONT  = (np.s_[6:34],   np.s_[26:86])
R_JOUES  = (np.s_[58:86],  np.s_[6:26])


def landmarks(det):
    return np.array(det[4:14], dtype=np.float64).reshape(5, 2)


def iod(pts):
    return float(np.linalg.norm(pts[0] - pts[1]))


def prepare(aligned_bgr):
    """Niveaux de gris, normalises pour enlever l'exposition globale.

    Sans cette normalisation, un simple changement d'eclairage ou de gain
    automatique de la webcam ferait varier tout le visage a la fois et
    gonflerait les deux regions - le rapport resterait juste, mais le signal
    absolu deviendrait illisible.
    """
    g = cv2.cvtColor(aligned_bgr, cv2.COLOR_BGR2GRAY).astype(np.float32)
    return (g - g.mean()) / (g.std() + 1e-6)


def _mean_sd(sd, region):
    ys, xs = region
    return float(sd[ys, xs].mean())


def change_ratio(stack):
    """(rapport, signal, temoin) sur une pile de visages alignes."""
    sd = np.stack(stack).std(axis=0)
    signal = (_mean_sd(sd, R_YEUX) + _mean_sd(sd, R_BOUCHE)) / 2.0
    temoin = (_mean_sd(sd, R_FRONT) + _mean_sd(sd, R_JOUES)) / 2.0
    return signal / (temoin + 1e-6), signal, temoin


class FaceTrack:
    """Historique d'apparence d'un visage suivi, et son verdict."""

    __slots__ = ("last_seen", "centre", "crops", "ratio", "signal", "temoin",
                 "live_until", "last_iod", "too_small", "samples")

    def __init__(self):
        self.last_seen = 0.0
        self.centre = None
        self.crops = deque()
        self.ratio = self.signal = self.temoin = 0.0
        self.live_until = 0.0
        self.last_iod = 0.0
        self.too_small = False
        self.samples = 0

    def is_live(self, now):
        return now < self.live_until

    def verdict(self, now):
        if self.is_live(now):
            return LIVE
        if self.samples >= SETTINGS["min_samples"] and not self.too_small:
            # Assez observe pour se prononcer, et le compte n'y est pas.
            return SUSPECT
        # Pas encore assez d'information. On ne devine pas : il n'y a PAS de
        # chronometre qui force un verdict, contrairement a l'ancien defi qui
        # echouait des que son delai passait.
        return UNKNOWN

    def observe(self, pts, aligned_bgr, now):
        self.last_seen = now
        self.centre = (float(pts[:, 0].mean()), float(pts[:, 1].mean()))
        self.last_iod = iod(pts)
        self.too_small = self.last_iod < SETTINGS["min_iod_px"]
        if self.too_small or aligned_bgr is None:
            return

        self.crops.append((now, prepare(aligned_bgr)))
        horizon = now - SETTINGS["window_seconds"]
        while self.crops and self.crops[0][0] < horizon:
            self.crops.popleft()
        self.samples = len(self.crops)
        if self.samples < SETTINGS["min_samples"]:
            return

        self.ratio, self.signal, self.temoin = change_ratio(
            [c for _, c in self.crops])
        if self.ratio >= SETTINGS["change_ratio"]:
            self.live_until = now + SETTINGS["hold_seconds"]

    def reset(self, now):
        self.crops.clear()
        self.samples = 0
        self.live_until = 0.0

    def detail(self, now):
        besoin = SETTINGS["min_samples"]
        if self.too_small:
            why = (f"visage trop petit ({self.last_iod:.0f} px, il en faut "
                   f"{SETTINGS['min_iod_px']:.0f}) - approchez-vous")
        elif self.samples < besoin:
            why = f"observation en cours ({self.samples}/{besoin})"
        elif self.is_live(now):
            why = f"changements du visage detectes (rapport {self.ratio:.2f})"
        else:
            why = (f"trop peu de changement (rapport {self.ratio:.2f}, il faut "
                   f"{SETTINGS['change_ratio']:.2f}) - une photo donne ~0.5")
        return {
            "etat": self.verdict(now),
            "rapport": round(self.ratio, 3),
            "requis": SETTINGS["change_ratio"],
            "signal": round(self.signal, 4),
            "temoin": round(self.temoin, 4),
            "releves": self.samples,
            "inter_oculaire": round(self.last_iod, 1),
            "trop_petit": self.too_small,
            "tient_jusqua": round(max(0.0, self.live_until - now), 1),
            "explication": why,
        }


class Liveness:
    MATCH_FRACTION = 0.8
    FORGET_SECONDS = 2.0

    def __init__(self):
        self.tracks = []

    def observe(self, det, aligned_bgr, now=None):
        """Un visage detecte, avec son recadrage aligne. Renvoie sa piste."""
        now = time.time() if now is None else now
        pts = landmarks(det)
        t = self._match(det[:4], pts)
        t.observe(pts, aligned_bgr, now)
        return t

    def sweep(self, now=None):
        """Oublie les visages disparus. A appeler une fois par image."""
        now = time.time() if now is None else now
        self.tracks = [t for t in self.tracks
                       if now - t.last_seen < self.FORGET_SECONDS]

    def _match(self, box, pts):
        cx, cy = float(pts[:, 0].mean()), float(pts[:, 1].mean())
        tol = max(30.0, float(box[2]) * self.MATCH_FRACTION)
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
