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
| Buzzer (option) | `D3` (GPIO0) | — | **actif à l'état bas**, voir ci-dessous |

#### Les deux pièges de câblage qui empêchent la carte de démarrer

Ce ne sont pas des recommandations, c'est le strap de boot du chip :

- **`D8` (GPIO15) doit être actif à l'état haut.** Il a un pull-down interne et
  le chip refuse de démarrer si GPIO15 est haut au reset. Câbler la LED rouge
  dans l'autre sens (`3V3 → LED → D8`) bloque le boot jusqu'à ce que vous la
  débranchiez.
- **`D3` (GPIO0) doit être actif à l'état bas** : `3V3 → buzzer → D3`, et on
  tire la broche à LOW pour l'activer. Une charge vers GND sur GPIO0 le tire bas
  au reset et la carte démarre en mode flash au lieu de lancer votre programme.

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

## Les 3 LEDs

| LED | État | Déclencheurs |
|---|---|---|
| 🟢 verte **fixe** | *validation* | tous les capteurs répondent, rien détecté |
| 🟢 verte **qui clignote** | dégradé | ça tourne, mais un capteur ne répond pas |
| 🟡 jaune | *pré-alerte* | score de présence WiFi ≥ `PRESENCE_WARN_SCORE`, gaz ≥ `GAS_WARN_DELTA_V`, ou boîtier incliné |
| 🔴 rouge (clignotement rapide) | *intrusion* | mouvement PIR confirmé, gaz ≥ `GAS_CRIT_DELTA_V`, ou présence ≥ `PRESENCE_CRIT_SCORE` |

Jaune et rouge sont maintenues `WARNING_HOLD_MS` / `INTRUDER_HOLD_MS` après le
dernier déclenchement, pour qu'un passage d'une fraction de seconde reste
visible. Au démarrage, les 3 LEDs s'allument l'une après l'autre : une LED qui
ne s'allume pas à ce moment-là est mal câblée, pas inactive.

---

## Détection d'appareils WiFi

Deux signaux indépendants, volontairement comptés séparément :

| Signal | Ce que c'est | Fiabilité |
|---|---|---|
| **associés** | appareils connectés au point d'accès du boîtier | exact et continu |
| **sniffés** | téléphones qui ne nous parlent pas du tout, vus via leurs *probe requests* en mode promiscuous | c'est le signal « à travers les murs » |

`score = associés × PRESENCE_ASSOC_WEIGHT + sniffés × PRESENCE_SNIFF_WEIGHT`

### Trois limites à connaître avant de régler les seuils

1. **L'ESP8266 ne peut pas sniffer et héberger le point d'accès en même temps.**
   Le mode promiscuous exige d'être en station seule, donc chaque fenêtre de
   sniff **coupe le point d'accès** : les clients du dashboard sont éjectés et
   le lien MQTT tombe le temps de la fenêtre. D'où `SNIFF_WINDOW_MS` court
   (3 s) et `SNIFF_PERIOD_MS` long (60 s). Le dashboard affiche le compte à
   rebours ; quelques requêtes ratées à ce moment-là sont normales.
2. **Les téléphones randomisent leur adresse MAC** (iOS 8+, Android 10+). Un
   seul téléphone peut émettre une dizaine d'adresses en une minute. Le compte
   de MAC uniques est donc un **niveau d'activité, pas un décompte de
   personnes**. Le firmware sépare `randomized` et `stable` exprès, pour que le
   chiffre puisse être lu honnêtement.
3. **Un radio ne mesure pas une distance.** Le RSSI y est vaguement corrélé et
   massacré par les murs, les corps et l'orientation. `SNIFF_MIN_RSSI` est un
   réglage de rayon grossier, pas une distance en mètres.

Le sniffer capture des identifiants diffusés en clair. C'est sans problème sur
votre propre banc ; réfléchissez avant de le pointer vers une salle publique.
`PRESENCE_SNIFFER 0` ne garde que le comptage des clients associés, sans jamais
couper le point d'accès.

---

## Réseau

Le boîtier est en **AP + station** simultanément :

| | |
|---|---|
| Point d'accès hébergé | `AP_SSID` de `secrets.h` (défaut `Sentinel-X`) |
| Dashboard embarqué | `http://192.168.4.1/` ou `http://sentinel-x.local/` |
| API JSON embarquée | `http://192.168.4.1/api/readings` |
| Réseau rejoint | `WIFI_SSID` de `secrets.h`, pour atteindre le broker |

Les deux radios partagent un seul canal (celui de la station gagne) : c'est
normal.

`WiFi.begin()` **ne sait pas rejoindre un réseau WPA2-Enterprise**
(802.1X/PEAP), ce qu'est la plupart des WiFi de campus. Il faut du WPA2-PSK :
le hotspot du PC serveur, ou un partage de connexion.

**Le dashboard embarqué n'a aucune dépendance externe, et ça doit rester le
cas.** Les clients du point d'accès n'ont pas d'accès Internet : une police ou
un script sur CDN ne se dégrade pas, il fait attendre le chargement de la page
jusqu'au timeout du navigateur.

Le gros dashboard de supervision avec le flux webcam est un autre programme :
`../dashboard`, servi par nginx depuis le PC serveur. **La caméra est une
webcam USB branchée sur le PC**, elle ne touche jamais l'ESP8266.

---

## Topics MQTT

Les noms sont imposés par `../mosquitto/config/acl`. Le compte MQTT s'appelle
toujours `esp32` pour que l'ACL et le `.env` du serveur restent valables, même
si la carte est un ESP8266.

### Publication

- **`sentinelx/g02/telemetry`** toutes les `TELEMETRY_INTERVAL_MS`

  ```json
  {"t":24.1,"h":48,"gaz":312,"pir":0,"rssi":-62,
   "gaz_v":1.98,"gaz_d":0.12,
   "pres":4,"assoc":1,"sniff":3,
   "tilt":0,"state":"green","heap":14200}
  ```

  `gaz` est l'ADC brut, **0-1023**. `t` et `h` valent `null` quand le DHT22 n'a
  pas donné de lecture valable — exprès, pour qu'un capteur mort ne fasse pas
  jeter les données de gaz et de mouvement avec lui.

- **`sentinelx/g02/alerts`** — `type` ∈ `gaz`, `mouvement`, `sabotage`,
  `presence` ; `niveau` ∈ `attention`, `critique`. Une alerte par type au plus
  toutes les `ALERT_COOLDOWN_MS`.

- **`sentinelx/g02/status`** — `online` / `offline`, en retained. Le `offline`
  est un *Last Will* : c'est le broker qui le publie si le boîtier disparaît.

### Abonnement

- **`sentinelx/g02/cmd`**
  - `{"buzzer":1}`
  - `{"led":"rouge"}` — aussi `vert`, `jaune`, `blanc`, `eteint`. Prend la main
    sur les LEDs pendant `LED_OVERRIDE_MS` puis **rend automatiquement le
    contrôle** à la machine à états : une commande de test oubliée ne doit pas
    laisser le boîtier mentir indéfiniment sur ce qu'il voit.
  - `{"auto":1}` — rend la main tout de suite.

---

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
| la carte ne démarre pas, ou démarre en mode flash | LED rouge câblée à l'envers sur `D8`, ou charge vers GND sur `D3`. Voir « les deux pièges » |
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
