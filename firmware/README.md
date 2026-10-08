# Firmware Sentinel-X — NodeMCU LoLin V3 / ESP8266MOD (ESP-12F)

> **Ce firmware cible un ESP8266, pas un ESP32.** Les versions précédentes de ce
> dossier visaient un ESP32 DevKit (GPIO 34, `setCACert()`, ADC 0-4095). Rien de
> tout cela ne compile ni ne fonctionne sur la carte réellement utilisée. Si vous
> voyez une erreur sur `WiFiClientSecure::setCACert` ou `GPIO34`, vous avez un
> reste de l'ancienne cible.

Le boîtier :

- lit 4 capteurs (température/humidité, mouvement, gaz, inclinaison),
- détecte les appareils WiFi autour de lui, **y compris à travers les murs**,
- résume tout ça sur 3 LEDs (verte / jaune / rouge),
- **héberge son propre point d'accès et son propre tableau de bord**,
- publie sa télémétrie en MQTT vers la stack Docker du PC serveur.

Tous les réglages sont dans un seul fichier : **`include/config.h`**. Rien
d'autre ne devrait avoir besoin d'être modifié pour changer un seuil, une
broche ou un intervalle.

---

## Matériel

| | |
|---|---|
| Carte | NodeMCU LoLin V3, module **ESP8266MOD (ESP-12F)**, USB-série CH340 |
| Carte PlatformIO | `nodemcuv2` (`platform = espressif8266`) |
| Moniteur série | 115200 bauds |

### Brochage

| Rôle | Broche | Alim | Remarque |
|---|---|---|---|
| DHT22 `DATA` | `D2` (GPIO4) | **3.3 V** | boîtier blanc = DHT22, bleu = DHT11 |
| PIR HC-SR501 `OUT` | `D1` (GPIO5) | **5 V** (`VU`) | `OUT` sort en 3,3 V : branchement direct OK |
| MQ gaz `AO` | `A0` | **5 V** (`VU`) | **via pont diviseur 68k+68k** |
| MQ gaz `DO` | *rien* | — | monte à 5 V, détruirait l'ESP8266 |
| MPU-6500 `SDA` / `SCL` | `D6` / `D7` | **3.3 V** | I2C `0x68`, **100 kHz obligatoire** |
| 🟢 LED verte (validation) | `D5` (GPIO14) | — | `D5 → 220 Ω → LED → GND` |
| 🟡 LED jaune (pré-alerte) | `D0` (GPIO16) | — | idem |
| 🔴 LED rouge (intrusion) | `D8` (GPIO15) | — | idem — **sens imposé, voir ci-dessous** |
| Buzzer actif | `D4` (GPIO2) | — | **actif à l'état HAUT** : `D4 → 100 Ω → buzzer → GND`, voir ci-dessous |

#### Les deux pièges de câblage qui empêchent la carte de démarrer

Ce ne sont pas des recommandations, c'est le strap de boot du chip :

- **`D8` (GPIO15) doit être actif à l'état haut.** Il a un pull-down interne et
  le chip refuse de démarrer si GPIO15 est haut au reset. Câbler la LED rouge
  dans l'autre sens (`3V3 → LED → D8`) bloque le boot jusqu'à ce que vous la
  débranchiez.
- **`D4` (GPIO2) doit être HAUT au reset**, ce qui impose deux choses au
  buzzer. Se tromper sur l'une ou l'autre empêche la carte de démarrer :
  1. le buzzer est **actif à l'état bas** (broche à LOW = il sonne) ;
  2. l'interrupteur doit être un **PNP côté haut**, pas un NPN côté bas. Un NPN
     est actif à l'état haut, et sa résistance de base de 1 kΩ clouerait GPIO2
     vers 0,7 V au reset — lu comme LOW, et le chip ne démarre jamais.

  ```
            3V3
             │
        E ───┴───  PNP  (2N3906 / BC557 / S8550)
  GPIO2 ─[1k]─ B
        C ───┬───
             │
         [buzzer]
             │
            GND
  ```

  **Pourquoi 1 kΩ et pas ~100 Ω ?** Ce ne sont pas deux variantes du même
  montage. Le 1 kΩ est sur la **base** : `(3,3 − 0,7) / 1k ≈ 2,6 mA`, largement
  assez pour saturer le transistor sur un buzzer de 25-30 mA, pendant que la
  broche elle-même ne porte que ces 2,6 mA. Avec 100 Ω à cet endroit, la broche
  encaisserait ~26 mA, au-delà de sa limite — ce qui annule tout l'intérêt du
  transistor. Une résistance de ~100 Ω aurait un sens **en série avec le buzzer
  sans transistor**, mais un buzzer actif a une tension minimale de
  fonctionnement (souvent 2-2,5 V) : lui enlever ~1,4 V le rend muet, pas plus
  discret.

  **Alimenter le buzzer en 3,3 V, pas en 5 V.** Un émetteur à 5 V se trouve
  1,7 V au-dessus d'une broche à 3,3 V : le Vbe n'atteindrait jamais 0 et le
  buzzer ne pourrait plus être coupé.

  GPIO2 porte aussi la LED bleue du module (également active à l'état bas) :
  elle clignote donc avec le buzzer. Pratique — tant que le buzzer n'est pas
  câblé, cette LED permet de valider le rythme.

#### Montage réellement utilisé : attaque directe, actif à l'état HAUT

Le PNP ci-dessus reste la solution propre, mais le boîtier est câblé en direct,
sans transistor, et **dans l'autre sens** :

```
D4 (GPIO2) ──[100 Ω]──[buzzer]── GND
```

`BUZZER_ACTIVE_LOW 0` dans `config.h` : **HIGH = il sonne**. La résistance de
100 Ω limite le courant que la broche doit fournir — et sert aussi de shunt de
mesure, voir plus bas.

Deux conséquences de ce câblage, aucune des deux n'étant un bug logiciel :

- **GPIO2 est une broche de strap qui veut être HAUTE au reset**, et une charge
  vers GND tire dans l'autre sens. Ça fonctionne ici parce que l'impédance
  continue d'un buzzer actif est assez élevée, mais ça marche *malgré* le
  câblage, pas grâce à lui. Un démarrage qui échoue par intermittence, ou un
  buzzer remplacé, doit ramener directement ici.
- **Le buzzer émet un bip court à chaque démarrage** : le pull-up de strap tient
  GPIO2 haut avant que `setup()` ne tourne. `setup()` le coupe en toute première
  action pour que ce soit le plus bref possible. C'est attendu, pas un défaut.

Compromis à connaître si l'on baisse encore la résistance : un buzzer actif a
une tension minimale de fonctionnement (souvent 2-2,5 V), donc trop de
résistance le rend muet plutôt que discret. Si le volume s'écroule, descendre à
47 Ω ou 68 Ω. Pour le plein volume *et* une broche protégée, repasser au PNP
actif à l'état bas (`BUZZER_ACTIVE_LOW 1`).

**La résistance sert aussi de shunt de mesure**, et 100 Ω rend le calcul
immédiat. Multimètre aux bornes de la résistance pendant que le buzzer sonne :

```
I (mA) = V (aux bornes des 100 Ω) × 10
```

1,0 V = 10 mA, 1,4 V = 14 mA. C'est la mesure qui tranche pour de bon : sous
~12 mA on est dans les specs, nettement au-dessus de 20 mA le PNP se justifie.

`D5`, `D0` et `D8` ont été choisis pour que **les 3 LEDs soient toutes actives à
l'état haut**, donc toutes câblées pareil. `D5` était la broche prévue pour le
joystick, qui n'a jamais été câblé.

#### Pont diviseur du MQ

La sortie `AO` monte jusqu'à 5 V, et aucune broche de l'ESP8266 ne tolère plus
de 3,3 V :

```
MQ AO ──[ 68k ]──┬──[ 68k ]── GND
                 │
                A0
```

Deux résistances **identiques** : `A0` voit la moitié de la tension. Le firmware
remultiplie par `GAS_DIVIDER_RATIO` pour afficher la vraie tension du capteur.

---

## Choses qui coûtent une après-midi à découvrir

Toutes vérifiées sur le matériel.

- **Alimenter le rail 5 V depuis `VU`, pas `VIN`.** Sur la LoLin V3, `VIN` est
  une *entrée* du régulateur, pas une sortie : il mesure 0 V quand la carte est
  alimentée par USB.
- **Le 3,3 V est un îlot de trous isolé, jamais un rail.** C'est volontaire :
  il ne peut pas toucher le rail 5 V.
- **L'ADC de l'ESP8266 est en 10 bits : `0-1023`.** Pas 4095. Les seuils de
  l'ancien dashboard ESP32 ne déclenchaient jamais à cause de ça.
- **Le capteur vendu comme MPU-9250 est un MPU-6500.** `WHO_AM_I` (`0x75`)
  renvoie `0x70`. Pas de magnétomètre : tangage et roulis OK, cap impossible.
  `imu.h` attaque les registres directement, parce que les bibliothèques
  MPU9250 exigent `0x71` et refusent de s'initialiser sur `0x70`.
- **L'I2C doit tourner à 100 kHz.** `Wire.setClock(400000)` et le MPU ne répond
  plus du tout : l'ESP8266 fait l'I2C en logiciel.
- **Une entrée ESP8266 en l'air lit un `1` stable**, indiscernable d'un capteur
  déclenché en permanence. Le vrai défaut du PIR était `VCC`/`GND` inversés : le
  module n'a aucune sérigraphie et `OUT` est la broche **du milieu**.
- **La valeur de gaz n'est pas des ppm.** Sans calibrer `R0` à l'air libre (et
  24-48 h de rodage pour un capteur neuf), aucune valeur absolue n'a de sens. Le
  firmware publie les volts et surtout **l'écart par rapport à une baseline**
  mesurée après 3 min de chauffe. C'est le seul signal honnête.

---

## Installation

```sh
cp include/secrets.example.h include/secrets.h
$EDITOR include/secrets.h          # SSID, mots de passe, ca.crt
cd firmware && pio run -t upload
pio device monitor                 # 115200
```

Le bandeau de boot de la ROM sort à 74880 bauds et ressemble à du bruit à
115200. C'est normal ; votre programme parle juste après.

### Environnements de compilation

| Environnement | Pour quoi |
|---|---|
| `pio run -e nodemcuv2` | tout activé : capteurs, LEDs, dashboard, présence, MQTT+TLS |
| `pio run -e nodemcuv2_standalone` | aucun broker, aucun TLS, aucun Docker. Capteurs + LEDs + dashboard local |
| `pio run -e nodemcuv2_lowheap` | garde MQTT+TLS, coupe le sniffer et l'IMU. À essayer si la carte redémarre au hasard |

---

## Les 3 LEDs et le buzzer

Les états et leurs rythmes sont décrits dans « Alarme, maintenance et
auto-test » plus bas. Ce qui compte au câblage :

Au démarrage, les 3 LEDs s'allument l'une après l'autre. Une LED qui ne
s'allume **pas** à ce moment-là est mal câblée, pas inactive.

Un buzzer actif n'a qu'une hauteur et qu'un volume, donc **le rythme est le
seul moyen de distinguer les états à l'oreille**. Le rouge est une **rafale**
et non un simple rapport cyclique : à ~3 Hz, la tonalité propre du buzzer plus
le hachage donnent un bourdonnement continu — c'est exactement ce qu'a donné la
première version au banc. Il faut des bips d'environ 120 ms séparés par une
vraie pause. Garder `BUZZER_RED_BEEP_MS` nettement au-dessus de ~100 ms.

Tout est non bloquant : la boucle principale n'attend jamais le buzzer.

---

## Détection d'appareils WiFi

Capture en mode promiscuous des *probe requests* : les téléphones qui ne sont
connectés à rien chez nous demandent en permanence les réseaux qu'ils
connaissent. C'est le signal « à travers les murs ».

Chaque `SNIFF_PERIOD_MS`, la radio écoute `SNIFF_WINDOW_MS` en sautant les
canaux 1/6/11 et compte les appareils distincts entendus **au-dessus de
`SNIFF_MIN_RSSI`**. Ce compte est comparé à ce qui est **normal pour l'endroit** :

| Phase | Ce qui se passe |
|---|---|
| apprentissage | les `PRESENCE_LEARN_WINDOWS` premières fenêtres ne font que mesurer ; leur moyenne devient le niveau habituel |
| surveillance | un excès de `PRESENCE_WARN_EXCESS` appareils, **`PRESENCE_CONFIRM_WINDOWS` fenêtres de suite**, lève la pré-alerte. Une fenêtre calme l'efface |
| suivi | les fenêtres calmes font dériver le niveau habituel ; une alerte le **gèle**, donc une vraie affluence n'est jamais apprise comme normale |

### Pourquoi la première version était inutilisable

Elle comptait **toutes** les MAC vues dans les cinq dernières minutes, jusqu'à
-80 dBm, face à un seuil fixe de 3. Dans un bâtiment c'est toujours dépassé —
voisins, passants, votre propre téléphone — donc elle alertait dès le démarrage
et ne redescendait jamais. D'où les trois changements : fenêtre **courante**
seulement, portée réduite à **-65 dBm**, et comparaison à un niveau **appris**
plutôt qu'à un nombre fixe.

### Limites qu'aucun réglage ne supprime

1. **L'ESP8266 ne peut pas sniffer et rester connecté en même temps.** Chaque
   fenêtre coupe le lien WiFi ; MQTT se reconnecte ensuite. Le sniffer est donc
   suspendu pendant la maintenance et l'auto-test, où perdre le lien en pleine
   action serait pire que rater une fenêtre.
2. **Les téléphones randomisent leur adresse MAC** (iOS 8+, Android 10+). Un
   seul téléphone peut émettre une dizaine d'adresses en une minute : le compte
   est un **niveau d'activité, pas un décompte de personnes**. `randomized` et
   `stable` sont comptés séparément exprès.
3. **Un radio ne mesure pas une distance.** Le RSSI y est vaguement corrélé et
   massacré par les murs, les corps et l'orientation. `SNIFF_MIN_RSSI` est un
   réglage de rayon grossier, pas une distance en mètres.

La présence seule ne vaut jamais qu'une **pré-alerte** : c'est trop indirect
pour désigner un intrus.

Cela capte des identifiants diffusés en clair. Sans problème sur votre propre
banc ; réfléchissez avant de le pointer vers une salle publique.

---

## Réseau

Le boîtier est un **simple client WiFi**. Il n'héberge ni point d'accès ni page
web : c'est voulu, rien n'écoute sur la carte, donc il n'y a aucune surface à
attaquer dessus.

| | |
|---|---|
| Réseau rejoint | `WIFI_SSID` de `secrets.h` — le hotspot du PC serveur |
| Broker | `MQTT_HOST:MQTT_PORT` de `config.h` (8883, TLS) |
| Interface | `../dashboard`, servi par nginx **depuis le PC**, jamais depuis la carte |

La connexion est **non bloquante** : un hotspot absent ou un broker éteint ne
fige jamais la boucle. Les capteurs, les LED et la sirène continuent hors
ligne, et les tentatives de reconnexion sont espacées
(`MQTT_RETRY_MIN_MS` → `MQTT_RETRY_MAX_MS`).

`WiFi.begin()` **ne sait pas rejoindre un réseau WPA2-Enterprise**
(802.1X/PEAP), ce qu'est la plupart des WiFi de campus. Il faut du WPA2-PSK :
le hotspot du PC serveur, ou un partage de connexion depuis un téléphone.

**La caméra est une webcam USB branchée sur le PC** : elle ne touche jamais
l'ESP8266. Le flux est analysé par `../ia/sentinelx-ai` et proxifié par nginx.

---

## Alarme, maintenance et auto-test

Le boîtier n'héberge **plus de point d'accès ni de page web** : il rejoint le
hotspot du PC serveur comme un client ordinaire et se pilote entièrement
depuis le dashboard principal (`../dashboard`) via MQTT. Rien n'écoute sur la
carte, donc il n'y a aucune surface à attaquer dessus.

### La machine à états

| État | LED | Buzzer | Ce qui se passe |
|---|---|---|---|
| **Sortie** | 🟡 lente | bip/s | Vient d'être armé : le temps de quitter la pièce |
| **Armé** | 🟢 fixe | — | Surveille. 🟢 clignote si un capteur ne répond pas |
| **Pré-alerte** | 🟡 lente | silencieux | Présence WiFi inhabituelle ou gaz en hausse légère |
| **Entrée** | 🟡 rapide | bip/s | Détection : `ENTRY_DELAY_MS` pour s'identifier |
| **Alarme** | 🔴 rapide | rafales | Personne ne s'est identifié. Dure `ALARM_DURATION_MS` puis ré-arme |
| **Maintenance** | 🟢/🟡 alternées | silencieux | Déverrouillé : rien n'alarme, tout se pilote |

Seul l'état **Armé** déclenche une intrusion, et seule une détection **nouvelle**
démarre un cycle : un capteur bloqué en position haute provoque une alarme, pas
une sirène sans fin. Le **gaz** ne passe pas par la temporisation d'entrée —
c'est un risque de sécurité, pas une intrusion — et sonne même en maintenance
sauf si `MAINT_SILENCES_GAS` vaut 1.

### Maintenance : le seul état déverrouillé

Tout ce qui pilote le boîtier (mode manuel, prévisualisation, auto-test,
recalibrage, coupure du sniffer) est **refusé par la carte** hors maintenance —
pas simplement grisé sur la page.

Le code ne traverse jamais le réseau :

1. la carte publie un *nonce* aléatoire dans son état ;
2. le dashboard renvoie `sha256(nonce + ":" + code)` ;
3. la carte recompare en temps constant.

Le nonce est à usage unique — remplacé après **chaque** tentative, réussie ou
non — donc une réponse interceptée ne se rejoue pas. Seule la carte peut lire
le topic `cmd` (ACL Mosquitto), donc personne ne peut non plus collecter des
réponses pour les attaquer hors ligne. Après `MAINT_MAX_FAILS` échecs, verrouillage
`MAINT_LOCKOUT_MS`, et chaque tentative part en alerte de sécurité. La
maintenance s'arrête seule au bout de `MAINT_TIMEOUT_MS`, en annulant toutes
les dérogations.

> **À faire avant la démo** : définir `MAINT_PIN` dans `include/secrets.h`.
> Tant qu'il vaut `change-me`, la carte **refuse toute maintenance à distance**
> plutôt que d'exposer une serrure devinable. Le dashboard l'affiche clairement.

La **console série** est en revanche de confiance et n'exige pas de code : un
accès USB à la carte signifie déjà un accès physique à la carte.

### Auto-test

Seize étapes : relecture électrique des 3 LED et du buzzer, chaque LED seule,
tout éteint, tout allumé, les rythmes du buzzer, puis DHT22, MPU-6500, gaz et
PIR. Démarrage, pause, reprise, étape précédente/suivante, saut direct, boucle,
et un mode guidé qui attend un Oui/Non sur chaque étape visuelle.

C'est une **machine à états non bloquante** : bloquer ici figerait MQTT et le
dashboard perdrait la carte en plein test.

### Exercice

`Simuler une détection` et `Déclencher l'alarme` déroulent la vraie chaîne sans
intrusion. Les alertes émises portent le type `exercice`, donc elles ne peuvent
pas être confondues avec un événement réel dans le journal ou la base.

## Réglages des capteurs

Tout est dans `include/config.h`. Les valeurs actuelles viennent de mesures au
banc, pas d'estimations.

| Détection | Réglage | Pourquoi |
|---|---|---|
| **Gaz, hausse rapide** | `GAS_RISE_V` 0,40 V sur `GAS_RISE_WINDOW_MS` 5 s | Un briquet fait monter la mesure de ~1 V en deux secondes. Échantillonné à 4 Hz, sans baseline : marche 30 s après l'allumage au lieu d'attendre 3 min |
| **Gaz, fuite lente** | `GAS_WARN_DELTA_V` / `GAS_CRIT_DELTA_V` au-dessus d'une baseline qui suit la dérive mais **gèle** dès que la mesure s'en écarte | Une vraie fuite ne peut pas être apprise comme normale |
| **Sabotage** | `TILT_WARN_DEG` **8°** (était 25°), plus choc et rotation | 25° laissait passer un simple déplacement. Au repos la dérive mesurée est < 1° |
| **Mouvement** | `PIR_CONFIRM_MS` 1 s, puis **front montant seulement** | Le HC-SR501 reste haut aussi longtemps que son potentiomètre TIME le dit — jusqu'à ~5 min. Aucun firmware ne raccourcit ça : mettre le potentiomètre au minimum |
| **Présence WiFi** | apprend le niveau habituel sur 5 fenêtres, puis exige `PRESENCE_WARN_EXCESS` **confirmé 2 fenêtres de suite**, et seulement à `SNIFF_MIN_RSSI` **-65 dBm** | L'ancienne version comptait tout jusqu'à -80 dBm face à un seuil fixe : dans un bâtiment c'est toujours dépassé, donc elle alertait tout de suite et ne redescendait jamais |

La présence seule ne vaut jamais qu'une **pré-alerte** : une activité WiFi est
trop indirecte pour désigner un intrus. Il faut le PIR ou le sabotage.

## Topics MQTT

Noms imposés par `../mosquitto/config/acl`. Seul `GROUP_ID` devrait changer.

### Publication

| Topic | Contenu |
|---|---|
| `telemetry` | mesures, toutes les 5 s — forme inchangée pour l'API et l'IA |
| `state` | **état complet, retenu** : alarme, décompte, LED, buzzer, capteurs, maintenance |
| `test` | avancement de l'auto-test, retenu |
| `testmeta` | liste des étapes, retenu, publié une fois par connexion |
| `alerts` | alarmes et événements de sécurité |
| `status` | `online` / `offline` (*last will* : c'est le broker qui publie `offline`) |

`state` est **retenu**, donc un dashboard ouvert plus tard voit l'état courant
immédiatement au lieu d'une page vide. Le rythme passe à 1 s pendant un
décompte, pour que le compteur de la page suive celui de la carte.

### Abonnement : `cmd`

```json
{"maint":"<sha256 hex>"}   {"maint":"off"}   {"arm":"now"}
{"test":"start"}           {"test":"goto","step":5}   {"test":"confirm","step":5,"flag":1}
{"mode":"manual"}          {"force":"alarm"}          {"vert":1}
{"buzzer":1}  {"beep":300} {"alloff":1}  {"mute":1}   {"sniff":0}
{"rebase":"imu"}           {"drill":"entry"}
```

Bornes côté carte : `CMD_MAX_BYTES` (au-delà, rejeté sans être lu) et
`CMD_RATE_PER_S` (au-delà, ignoré — répondre à un flood l'alimente).

### Console série

Le même JSON, à 115200 bauds, sans rejoindre le réseau. `s` état, `r` mesures,
`t` / `m` auto-test, ou n'importe quelle commande JSON telle quelle. C'est ce
qui a servi à valider le firmware sur la carte.

## TLS sur ESP8266 : les deux vrais problèmes

`MQTT_USE_TLS 1` est le défaut, mais lisez ça avant le jour J.

1. **La RAM.** BearSSL veut un tampon de réception de 16 ko si le broker ne
   négocie pas MFLN — et Mosquitto (OpenSSL) ne le fait pas. Ça fait ~22 ko sur
   un tas d'environ 40 ko, en plus du serveur web et de la table de présence. Le
   firmware teste MFLN au démarrage et le dit sur le port série. **Surveillez
   `heap` sur le dashboard** : sous ~8 ko, attendez-vous à des redémarrages
   aléatoires. La sortie de secours est `MQTT_USE_TLS 0` (port 1883) ou
   l'environnement `nodemcuv2_lowheap`.

2. **L'horloge.** La validation X.509 vérifie `notBefore`/`notAfter`. La carte
   n'a pas d'horloge temps réel, et un point d'accès isolé n'a pas de NTP. Le
   firmware amorce donc l'horloge avec `BUILD_EPOCH` de `config.h`. Si les
   certificats commencent à être vus comme « pas encore valides » ou
   « expirés », c'est cette constante qu'il faut bouger.

---

## Dépannage

| Symptôme | Cause |
|---|---|
| la carte ne démarre pas, ou démarre en mode flash | LED rouge câblée à l'envers sur `D8`. Voir « les deux pièges » |
| démarrage qui échoue par intermittence | le buzzer sur `D4` tire GPIO2 vers le bas au reset. Augmenter la résistance série, ou repasser au montage actif à l'état bas |
| bip court du buzzer à chaque démarrage | **normal** : pull-up de strap sur GPIO2 avant que `setup()` ne tourne |
| le buzzer sonne pendant les pauses, se tait pendant les bips | polarité inversée : `BUZZER_ACTIVE_LOW` ne correspond pas au câblage |
| reste sur `[sta] joining ...` | point d'accès en 5 GHz, ou réseau WPA2-Enterprise, ou SSID/mot de passe faux |
| `state=-2` + erreur TLS | mauvaise IP, port 8883 bloqué par le pare-feu Windows, `ca.crt` mal collé, dates du certificat contre `BUILD_EPOCH`, ou plus assez de tas |
| `state=4` ou `5` | utilisateur/mot de passe MQTT faux, ou mauvais groupe dans l'ACL |
| `"t":null` en permanence | DHT22 : `DATA` sur `D2`, `VCC` sur l'îlot 3,3 V |
| `gaz` bloqué à 0 ou 1023 | pont diviseur absent ou mal câblé |
| PIR toujours à `1` | `VCC`/`GND` inversés (`OUT` = broche du milieu), ou broche en l'air |
| `imu : not responding` | `SDA`/`SCL` inversés entre `D6` et `D7`, ou I2C poussé à 400 kHz |
| redémarrages aléatoires | tas épuisé. `nodemcuv2_lowheap`, ou `MQTT_USE_TLS 0` |
| le dashboard se déconnecte toutes les minutes | **normal** : c'est la fenêtre de sniff. Allonger `SNIFF_PERIOD_MS` ou `PRESENCE_SNIFFER 0` |

---

## Licence

Projet pédagogique réalisé dans le cadre du Workshop Sentinel-X d'EPSI.
