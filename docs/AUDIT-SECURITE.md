# Audit de securite — Sentinel-X

Audit mene le 2026-10-08 en **attaquant la pile en marche**, pas en relisant le
code : chaque faille ci-dessous a ete reproduite avec `curl` ou `mosquitto_pub`
contre les conteneurs en fonctionnement, et chaque correctif a ete reverifie par
la meme attaque.

Question posee : *qu'est-ce qui permettrait a quelqu'un de non autorise de
« tout valider » ?*

---

## 1. Ce qui etait exploitable

### CRITIQUE — l'API de la vision n'avait aucune authentification

Le gardiennage « maintenance » du dashboard etait **uniquement du JavaScript
cote navigateur**. Le service de vision, lui, acceptait tout.

    POST   /video/api/config?match_threshold=0.1  -> 200 {"ok": true}
    POST   /video/api/faces?name=ATTAQUANT        -> accepte (refuse seulement faute de visage)
    DELETE /video/api/faces?id=...                -> accepte

Deux chemins de « tout valider » en un appel, sans le moindre identifiant :

1. **S'enroler soi-meme** comme personne autorisee. Une fois dans la liste, la
   regle « une personne validee est visible => jamais d'alarme » fait le reste.
2. **Abaisser `match_threshold` a 0.1.** En dessous du seuil recommande par
   OpenCV (0.363), la similarite cosinus de deux inconnus suffit : tout visage
   est reconnu comme la premiere personne enrolee. Un seul reglage desarme la
   reconnaissance entiere.

**Correctifs.** Deux couches independantes :

- `nginx` exige desormais une authentification HTTP sur `/`, `/api/` et
  `/video/` (`nginx/default.conf`, `scripts/gen-htpasswd.sh`). `/mqtt` en est
  exempte : le CONNECT MQTT porte deja son propre couple identifiant/mot de
  passe, verifie par le broker contre son ACL.
- Toute **modification** (`POST`/`DELETE`) du service de vision exige en plus
  que le boitier soit **en maintenance** (`stream/app.py`, `_guard()`). La
  maintenance n'est accordee que sur defi-reponse avec le code, avec
  verrouillage apres 5 echecs et expiration automatique : cet etat vaut donc
  preuve d'autorisation, sans inventer un secret de plus. Les refus sont
  journalises et publies en alerte.
- Le plancher de `match_threshold` passe de 0.10 a **0.30**
  (`stream/vision.py`), pour qu'aucun reglage legitime ne puisse desarmer la
  reconnaissance.

Fermeture **par defaut** : boitier injoignable => modifications refusees.

### CRITIQUE — un etat MQTT retenu rouvrait les modifications, boitier eteint

Decouvert en verifiant le correctif precedent : il repondait `200` alors que le
boitier etait hors tension.

L'etat est publie **retenu**, pour que le dashboard affiche quelque chose des
l'ouverture. Mais le broker le rejoue a **chaque (re)connexion**, et il survit a
l'extinction du boitier. Un `maint.on: true` retenu de la veille rendait donc le
boitier « present et en maintenance » pendant 30 s a chaque redemarrage du
service de vision. Il suffit d'attendre un redemarrage.

    maint : {'on': True, 'nonce': '304580860c31338f', ...}   <- retenu, boitier eteint

**Correctif.** Le verrou ne se fie plus qu'aux messages **en direct**
(`msg.retain` ignore, `stream/app.py`). Un boitier vivant republie en quelques
secondes, donc l'usage normal ne change pas. L'etat retenu empoisonne a ete
purge du broker.

### CONFIRMEE — traversee de chemin sur les vignettes

`GET /video/api/faces/thumb?id=../events/<nom>` renvoyait **HTTP 200 et un vrai
JPEG** pris hors du dossier des visages. `EventStore.image()` avait la garde,
`FaceStore.thumbnail()` ne l'avait pas. Le `.jpg` ajoute par le code empechait
de lire `/etc/passwd` — une contrainte heureuse, pas une protection.

**Correctif.** Meme garde que son voisin (`stream/store.py`). Verifie : 404.

### ELEVEE — camera en direct et archive photo en acces libre

`GET /video/snapshot`, `/video/api/events` (33 photos d'evenements) et
`GET /api/v1/mesures` repondaient **200 sans identifiant**. Ce n'est pas une
validation, c'est une fuite : n'importe qui sur le reseau pouvait regarder la
camera et relire l'historique. Couvert par l'authentification nginx ci-dessus.

---

## 2. Ce qui a tenu

- **L'ACL Mosquitto.** Publier `{"face":"known","name":"ATTAQUANT"}` sur le
  topic `vision` avec les identifiants `dashboard` a ete **silencieusement
  rejete par le broker** (0 occurrence chez un abonne admin). Seul le compte
  `ia` ecrit ce topic, et seul `esp32` ecrit `state` — personne ne peut donc
  forger ni « visage connu », ni `maint.on`. C'est ce qui rend le verrou de la
  section 1 solide plutot que circulaire.
- **`POST /api/v1/cmd`** refusait deja sans cle API (401).
- **L'auto-test** se garde lui-meme : `testAction()` refuse hors maintenance
  des sa premiere ligne, avant toute action.
- **`EventStore.image()`** refusait deja `/` et `..`.
- **La maintenance du boitier** : defi-reponse `sha256(nonce + ":" + code)`,
  comparaison a temps constant, verrouillage 5 min apres 5 echecs, expiration
  automatique. `cmd` n'est lisible que par le boitier, donc les reponses
  d'authentification ne peuvent pas etre collectees au passage.

---

## 3. Limites connues, non corrigees

- **La vivacite est maintenant verifiee** (voir la section 5), mais une VIDEO
  rejouee sur un ecran satisfait le defi. C'est la limite de fond d'une
  verification monoculaire sans profondeur.
- **`{"maint":"off"}` et `{"arm":...}` ne demandent pas le code.** Ils sont dans
  le sens « on reverrouille », donc ce n'est pas une elevation de privilege.
  Mais maintenant que l'enrolement exige la maintenance, qui possede les
  identifiants MQTT `dashboard` peut **empecher** un enrolement en spammant
  `{"arm":"now"}`. C'est une nuisance, pas un contournement.
- **Les reglages de la vision ne survivent pas a un redemarrage** du conteneur :
  ils retombent aux valeurs par defaut.
- **Le jeton GitHub communique dans la conversation doit etre considere comme
  compromis** et revoque.
- La personne enrolee (« Test ») a ete **perdue** pendant l'audit :
  `faces.json` est revenu a `[]`. Cause non etablie — peut-etre deux instances
  de `FaceStore` ecrivant en concurrence pendant un test. **A reenroler.**

---

## 4. Verification

Sans identifiant, tout doit repondre 401 :

    for p in /video/snapshot /video/api/events /api/v1/mesures /; do
      echo "$p -> $(curl -sk -o /dev/null -w '%{http_code}' https://localhost$p)"
    done

Authentifie mais boitier hors maintenance, les modifications doivent etre 403 :

    curl -sk -u dashboard:MOT_DE_PASSE -X POST \
      'https://localhost/video/api/config?match_threshold=0.1'

---

## 5. Detection de vivacite

Ajoutee apres l'audit, en reponse directe a la limite relevee en section 3 :
sans elle, une photo imprimee d'une personne autorisee suffisait a obtenir
« personne validee en vue », ce qui annule toute alarme d'intrusion.

### Ce qui a ete essaye, mesure, et rejete

L'approche passive semblait evidente : une photo est un objet PLAN, donc son
mouvement s'explique entierement par une transformation plane, alors que le
relief d'un vrai visage (le nez est ~2 cm devant le plan des yeux) produit une
parallaxe qu'aucun plan ne peut absorber. La premisse est juste — le relief
donne 5 a 6 fois le residu d'un plan — mais elle ne survit pas au materiel :

| essai | resultat mesure |
|---|---|
| residu affine sur les 5 points de YuNet | separe a 0.5 et 1.0 px de bruit, **chevauche completement a 2.0 px** |
| bruit reel des points sur cette camera | **1.86 a 3.06 px** (visage de 54 px entre les yeux) — donc dans le regime qui ne separe pas |
| ajustement par homographie | parfait sans bruit (residu exactement nul pour un plan), **inutilisable avec** : 8 degres de liberte pour 5 points, mal conditionne, une photo a sorti un residu de 2787 |
| auto-calibration sur le bruit (fort mouvement / faible mouvement) | **chevauche a tous les niveaux de bruit** |
| flot optique dense, fraction de points hors du plan dominant | suit le DEPLACEMENT et non la geometrie (derive de Lucas-Kanade) : une photo qu'on approche a donne **48.9 % hors-plan**, contre 8.7 % pour un vrai visage a 20 deg |

Conclusion : la vivacite **passive** fiable n'est pas atteignable ici — une
seule webcam 640x480, pas de profondeur, pas d'infrarouge, et aucun jeu
d'attaques enregistrees pour calibrer un seuil. Pretendre le contraire aurait
donne un faux sentiment de securite, ce qui est pire que rien.

### Ce qui marche : le defi

On cesse de chercher un effet subtil sous le bruit, et on EXIGE un mouvement
ample qu'une photo ne peut pas produire : tourner la tete d'un cote puis de
l'autre. L'indicateur est le decalage lateral du nez entre les deux yeux,
normalise et **redresse pour etre invariant au roulis**.

Cette invariance n'est pas cosmetique : la premiere version lisait les x bruts
de l'image, et comme le nez est sous la ligne des yeux, **pencher** la photo de
20 deg deplacait l'indicateur de 0.38 — au-dela du seuil. Il suffisait de
bercer une feuille de papier. Apres correction, l'asymetrie est strictement
independante du roulis (verifie de 0 a 60 deg), et un plan ne depasse jamais
0.045 meme a 60 deg de lacet, contre 0.403 pour un visage a 30 deg.

La statistique a elle aussi du etre refaite. Prendre l'amplitude entre le
minimum et le maximum des releves etait faux : le maximum moins le minimum de
N tirages bruites grandit avec N par pure statistique des extremes. Un visage
**parfaitement immobile** sortait 0.304, et trois mouvements de photo passaient
le seuil. Remplacee par une exigence d'asymetrie **soutenue** de chaque cote,
mesuree sur une mediane glissante : un bruit centre ne peut pas la produire.

### Performance mesuree, aux reglages livres

Seuil +/-0.30 soutenu, mediane de 5 releves, essais de 5 s, 600 essais par
case, bruit 2.5 px :

| taille du visage | distance | attaques acceptees | vrai visage, 30 deg |
|---|---|---|---|
| 45 px | 0.84 m | 2.0 % | 93.5 % |
| 60 px | 0.63 m | 0.3 % | 97.0 % |
| **75 px** | **0.50 m** | **0.2 %** | **99.7 %** |
| 110 px | 0.34 m | 0.0 % | 100 % |

D'ou `min_iod_px = 75` : en dessous, on n'affirme rien, on affiche
« approchez-vous ». C'est le seul reglage qui ameliore securite ET confort en
meme temps, puisque le bruit de l'indicateur vaut ~2 x bruit_px / inter-oculaire.

Neuf mouvements de photo ont ete joues contre le detecteur — immobile, pivotee
jusqu'a 45 deg, penchee jusqu'a 60 deg, inclinee, translatee, approchee,
secouee, et tout a la fois : **tous refuses**. Et en integration, sur le vrai
pipeline, la photo d'une personne enrolee ressort `recognised = true`,
`known = false` : reconnue, mais pas autorisee.

Un reglage essaye puis abandonne, mesure a l'appui : exiger que la rotation
tienne 3 releves consecutifs faisait passer les faux refus de 0.6 % a 6.0 %
sans reduire les attaques — une tete qui balaye ne passe qu'un instant par ses
extremes. Le reglage `sustain` reste disponible, a 1 par defaut.

### Ce que ca n'arrete pas

- **Une video rejouee sur un ecran**, montrant la personne en train de tourner
  la tete : l'ecran est plan, mais le contenu encode un vrai mouvement 3D. Le
  tirage au hasard du defi oblige l'attaquant a posseder la bonne sequence et a
  la jouer au bon moment, ce qui releve la barre sans la fermer.
- Les indicateurs de **texture** (nettete, moire d'ecran) sont calcules et
  affiches, mais ne **decident rien** : sans vraies attaques enregistrees pour
  les calibrer, aucun seuil ne serait honnete.
- **Non teste sur un vrai visage humain.** Toute la chaine a ete validee sur
  des attaques (vraies images de cette camera, warpees par de vraies
  homographies de plan) et sur un modele geometrique pour le cote legitime.
  Le taux de reussite d'un vrai visage reste donc une prediction, pas une
  mesure. Le panneau « Preuve de vivacite » du dashboard affiche les chiffres
  en direct pour le verifier en vingt secondes, et `require_liveness` repasse
  en mesure-seule en un clic si le defi gene.

### Le prix d'une fenetre d'identification plus longue

Le delai d'identification du boitier est passe de 5 a 10 s. Ce n'est pas
neutre pour ce controle, et la mesure le montre : chaque releve de plus est une
chance de plus qu'un extreme de bruit franchisse le seuil des deux cotes. A
75 px, 800 tirages, attaque = photo agitee dans tous les sens :

| duree d'UN essai | attaques acceptees | vrai visage, 30 deg |
|---|---|---|
| 5 s | 0.0 % | 97.5 % |
| 8 s | 4.4 % | 100 % |
| 10 s | 18.6 % | 99.9 % |
| 15 s | 60.8 % | 100 % |

Deux consequences, toutes deux appliquees :

1. **La duree d'un essai reste a 5 s**, decouplee de la fenetre
   d'identification. Un echec donne droit a un nouvel essai au bout de 1.5 s,
   donc deux essais tiennent dans les 10 s. Mesure sur la fenetre complete de
   10 s : un seul essai de 10 s donne 19.8 % d'attaques acceptees, deux essais
   de 5 s en donnent 6.2 % pour le meme temps d'observation.
2. **Le seuil est passe de 0.25 a 0.30** pour absorber le reste. Sur la
   fenetre de 10 s : 0.25 laissait passer 9.0 % des attaques, 0.30 en laisse
   0.5 %, et 0.35 tombe a 0.1 % mais refuse 28 % des vrais visages a 30 deg.

Le resultat net : 0.5 % d'attaques acceptees au lieu de 0.0 %, et la rotation
doit etre FRANCHE - un vrai visage a 40 deg passe a 100 %, a 30 deg a 96 %, a
25 deg seulement a 59 %. C'est le prix explicite des 10 secondes.
