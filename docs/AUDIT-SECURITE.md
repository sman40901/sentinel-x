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

- **Aucune detection de vivacite.** SFace compare des visages, il ne distingue
  pas un visage d'une photo de ce visage. Une photo imprimee d'une personne
  autorisee passera tres probablement. *Non teste faute de materiel* — mais
  c'est inherent au modele, pas un reglage. Pour un projet d'ecole c'est
  acceptable ; en vrai il faudrait un capteur de profondeur ou de l'infrarouge.
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
