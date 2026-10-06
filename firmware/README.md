<<<<<<< HEAD
# Firmware ESP32 Sentinel-X

Firmware pour le système de surveillance IoT Sentinel-X utilisant un ESP32, des capteurs et MQTT.

## Composants matériels

- **ESP32 DevKit** - Microcontrôleur
- **DHT22** - Capteur de température et d'humidité
- **MQ-2** - Capteur de gaz et de fumée (analogique)
- **PIR HC-SR501** - Détecteur de mouvement
- **Buzzer actif** - Alarme sonore
- **LED** - Indicateurs d'état rouge et jaune

## Câblage

### DHT22
- VCC → 3.3V
- DATA → GPIO 4 (configurable dans `config.h`)
- GND → GND

### MQ-2
- VCC → 5V (VIN)
- GND → GND
- AO → GPIO 34 (ADC1_CH6, configurable dans `config.h`)
- DO → Non utilisé (nous utilisons la sortie analogique)

**Important** : le MQ-2 peut fournir une tension de sortie allant jusqu'à 5V. Assurez-vous donc d'utiliser un diviseur de tension adapté si nécessaire.

### PIR HC-SR501
- VCC → 5V (VIN)
- OUT → GPIO 27 (configurable dans `config.h`)
- GND → GND

### Buzzer actif
- S → GPIO 26 (configurable dans `config.h`)
- VCC → 5V
- GND → GND

### LED
- Anode de la LED rouge → GPIO 25 avec une résistance de 220Ω → GND
- Anode de la LED jaune → GPIO 33 avec une résistance de 220Ω → GND

## Installation

### Prérequis

1. Installer [PlatformIO](https://platformio.org/)
2. Installer VS Code avec l'extension PlatformIO (recommandé)

### Configuration

Modifier `include/config.h` :

```cpp
#define WIFI_SSID "your_wifi"
#define WIFI_PASSWORD "your_wifi_password"

#define MQTT_HOST "192.168.1.50"
#define GROUP_ID "g02"
```

### Sélection de l'environnement

Le firmware prend en charge deux environnements :

**Test/Développement (par défaut)** :
- Port MQTT : 1883 (MQTT non chiffré)
- TLS : Désactivé
- Authentification : Désactivée
- Compilation avec : `pio run -e esp32dev`

**Production** :
- Port MQTT : 8883 (MQTTS)
- TLS : Activé
- Authentification : Activée
- Compilation avec : `pio run -e esp32dev_prod`

Pour changer d'environnement, utilisez l'environnement approprié dans `platformio.ini` ou spécifiez-le lors de la compilation.

### Compilation et téléversement

Avec l'interface en ligne de commande de PlatformIO :

```bash
cd firmware
pio run
pio run --target upload
pio device monitor
```

Vous pouvez également utiliser les boutons de l'extension PlatformIO dans VS Code.

## Topics MQTT

### Publication

- **Télémétrie** : `sentinelx/{GROUP_ID}/telemetry`
  - Charge utile : `{"t":24.1,"h":48,"gaz":312,"pir":0}`
  - Publiée toutes les 2 secondes

- **Alertes** : `sentinelx/{GROUP_ID}/alerts`
  - Charge utile : `{"type":"gaz","niveau":"critique"}`
  - Publiées lorsque le seuil de gaz est dépassé

- **Statut** : `sentinelx/{GROUP_ID}/status`
  - Charge utile : `online` ou `offline`
  - Publié lors de la connexion/déconnexion

### Abonnement

- **Commandes** : `sentinelx/{GROUP_ID}/cmd`
  - Charge utile : `{"buzzer":1,"led":"rouge"}`
  - `buzzer` : 0 ou 1
  - `led` : `"rouge"`, `"jaune"`, `"blanc"` ou `"eteint"`

## Calibration

### Capteur de gaz MQ-2

Le MQ-2 nécessite une période de préchauffage de 1 à 2 minutes avant de fournir des mesures stables. Ajustez la valeur `GAS_ALERT_THRESHOLD` dans `config.h` en fonction de votre environnement.

### DHT22

Les mesures doivent être effectuées au maximum une fois toutes les 2 secondes. Le firmware respecte cette limitation.

## Dépannage

### Problèmes de connexion WiFi

- Vérifiez le SSID et le mot de passe dans `config.h`
- Assurez-vous que l'ESP32 se trouve à portée de votre routeur WiFi
- Consultez le moniteur série (115200 bauds) pour identifier les messages d'erreur

### Problèmes de connexion MQTT

- Vérifiez l'adresse et le port du broker MQTT
- Vérifiez si le broker nécessite une authentification
- Assurez-vous que le broker est en cours d'exécution et accessible

### Mesures des capteurs

- **DHT22** : si les mesures retournent `NaN`, vérifiez le câblage et essayez un autre GPIO
- **MQ-2** : si les valeurs sont toujours `0` ou `4095`, vérifiez l'alimentation et le diviseur de tension
- **PIR** : utilisez les potentiomètres intégrés pour régler la sensibilité et le délai

## Intégration avec l'API

Ce firmware est conçu pour fonctionner avec l'API Python Sentinel-X située dans le répertoire `../api`. L'API attend les données de télémétrie au format envoyé par ce firmware.

## Licence

Projet pédagogique réalisé dans le cadre du Workshop Sentinel-X d'EPSI.
=======
# Firmware ESP32 — boîtier Sentinel-X

## Câblage

| Composant | Broche du module | → ESP32 | Remarque |
|---|---|---|---|
| **DHT22** | VCC / DATA / GND | 3V3 / **GPIO 4** / GND | |
| **MQ-2** | VCC / GND | **VIN (5 V)** / GND | le MQ-2 a besoin de 5 V pour chauffer |
| **MQ-2** | AO | **GPIO 34** *via pont diviseur* | voir ci-dessous, ne jamais brancher AO en direct |
| **PIR HC-SR501** | VCC / OUT / GND | **VIN (5 V)** / **GPIO 27** / GND | la sortie OUT est en 3,3 V : OK |
| **Buzzer actif** | S (I/O) / VCC / GND | **GPIO 26** / 3V3 / GND | |
| **LED rouge** | patte longue (+) | **GPIO 25** via résistance ~220 Ω | patte courte → GND |
| **LED jaune** | patte longue (+) | **GPIO 33** via résistance ~220 Ω | patte courte → GND |
| OLED (option) | SDA / SCL / VCC / GND | GPIO 21 / GPIO 22 / 3V3 / GND | mettre `USE_OLED 1` |

**Pont diviseur du MQ-2** (la sortie AO monte jusqu'à 5 V, l'ESP32 accepte 3,3 V max) :

```
MQ-2 AO ──[ R1 ]──┬──[ R2 ]── GND
                  │
               GPIO 34
```
Avec **deux résistances identiques** (par ex. 2 × 68 kΩ de la bande), GPIO 34 reçoit au plus 2,5 V.

## Installation (Arduino IDE)
1. **Fichier → Préférences → URL de gestionnaire de cartes** : `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
2. **Outils → Carte → Gestionnaire de cartes** : installer **esp32 by Espressif** (version **3.1 ou plus**).
3. **Outils → Gérer les bibliothèques** : installer **PubSubClient**, **DHT sensor library** (+ Adafruit Unified Sensor), **ArduinoJson** (v7).
4. **Outils → Carte** : *ESP32 Dev Module*. **Outils → Port** : le port COM de la carte.

## Configuration
1. Copier `secrets.example.h` en **`secrets.h`** (même dossier).
2. Remplir : SSID/mot de passe du point d'accès, IP du PC serveur, mot de passe MQTT `esp32` (= `MQTT_ESP32_PASSWORD` du `.env`).
3. Coller le contenu complet de `certs/ca.crt` entre les lignes `R"EOF(` et `)EOF"`.

## Téléverser et vérifier
Téléverser (→), puis **Moniteur série à 115200 bauds**. Attendu :
```
[WiFi] connexion à SENTINELX-G02...
[MQTT] connexion TLS à 192.168.137.1:8883...
[MQTT] connecté
[TX] {"t":23.4,"h":45,"gaz":512,"pir":0,...}
```

## Dépannage
| Message | Cause |
|---|---|
| reste sur `[WiFi] connexion...` | point d'accès en 5 GHz (l'ESP32 ne voit que le **2,4 GHz**), SSID ou mot de passe faux |
| `état=-2` + erreur TLS | IP du serveur absente du certificat, `ca.crt` mal collé, ou port 8883 bloqué par le pare-feu Windows |
| `état=4` ou `5` | utilisateur/mot de passe MQTT faux, ou mauvais groupe dans l'ACL |
| `"t":null` | DHT22 mal branché (vérifier DATA sur GPIO 4) |
| `gaz` bloqué à 0 ou 4095 | pont diviseur absent ou mal câblé |
>>>>>>> 0be6474 (modif dossier)
