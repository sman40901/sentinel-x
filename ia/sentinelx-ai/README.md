# Sentinel-X — IA locale

Cette brique IA est prévue pour l'architecture Sentinel-X :
- webcam USB sur le PC serveur local ;
- ESP32/ESP8266 -> Mosquitto en MQTTS ;
- script IA Python sur le PC serveur ;
- publication des alertes sur `sentinelx/<GROUP_ID>/alerts`.

## 1. Architecture

```text
Webcam USB
   |
   v
vision.py
   |-- YOLO -> personne
   |-- face_recognition -> visage connu/inconnu
   |
   +----------------------+
                          |
ESP32 -> MQTTS -> mqtt_client.py -> predictive.py
                          |              |
                          |              +-> Isolation Forest
                          |              +-> features temporelles
                          |
                          +-> risk_engine.py
                                   |
                                   v
                         sentinelx/<GROUP_ID>/alerts
```

## 2. Installation

Python 3.10/3.11 recommandé.

```bash
python -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

Sous Windows :
```powershell
.venv\Scripts\activate
pip install -r requirements.txt
```

Le modèle YOLO est téléchargé automatiquement par Ultralytics au premier lancement.

## 3. Configuration

Copier :

```bash
cp .env.example .env
```

Puis adapter `.env`.

IMPORTANT : ne jamais committer `.env`, les certificats privés ou les photos personnelles.

## 4. Visages autorisés

Placer une photo nette par personne dans :

```text
vision/known_faces/
    alice.jpg
    bob.jpg
```

Le nom du fichier devient le nom reconnu.

Puis lancer :

```bash
python vision/build_face_db.py
```

Cela crée `vision/data/encodings.pkl`.

## 5. Lancement

Dans un premier terminal :

```bash
python main.py
```

La webcam est analysée et les télémétries MQTT sont consommées par le même processus.

Pour tester uniquement la vision :

```bash
python vision/vision.py
```

Pour tester uniquement la partie prédictive :

```bash
python predictive/test_predictive.py
```

## 6. Logique prédictive

Le modèle Isolation Forest apprend le comportement normal pendant les premières mesures.

Il utilise :
- température ;
- humidité ;
- gaz ;
- variation température ;
- variation gaz ;
- moyennes glissantes.

Le modèle ne fait donc pas un simple seuil statique.

Pendant la phase d'apprentissage, les données sont collectées sans générer d'alerte prédictive.

## 7. Alertes

Exemple :

```json
{
  "type": "predictive",
  "niveau": "warning",
  "score": 0.84,
  "message": "Anomalie environnementale détectée",
  "features": {
    "temperature": 29.4,
    "humidity": 42.0,
    "gaz": 380.0
  }
}
```

Vision :

```json
{
  "type": "intrusion",
  "niveau": "critique",
  "message": "Personne non autorisée détectée",
  "person": "unknown"
}
```

## 8. Intégration

Le script IA publie sur :

```text
sentinelx/<GROUP_ID>/alerts
```

Le schéma du projet prévoit également :

```text
sentinelx/<GROUP_ID>/telemetry
sentinelx/<GROUP_ID>/cmd
sentinelx/<GROUP_ID>/status
```

L'API/dashboard peut donc consommer les alertes produites par cette brique.

## 9. Démonstration conseillée

1. Démarrer le serveur MQTT.
2. Démarrer `main.py`.
3. Montrer une personne autorisée : aucune alerte d'intrusion.
4. Montrer une personne non enregistrée : alerte intrusion.
5. Faire varier progressivement une source de chaleur ou la valeur de gaz.
6. Montrer le score d'anomalie avant un seuil critique.
7. Vérifier que l'alerte arrive sur le dashboard.

## 10. Sécurité

Les identifiants MQTT sont lus depuis `.env`.
Les certificats TLS sont lus depuis les chemins configurés.
Aucun mot de passe ou certificat privé ne doit être commité.

## Flux webcam vers le dashboard

`vision.py` affiche les images dans une fenetre OpenCV locale (`cv2.imshow`) :
il n'expose **rien** en HTTP. Or nginx proxifie `/video/` vers le port 8080 de
la machine hote, donc le panneau "Webcam" du dashboard ne pouvait jamais rien
afficher.

`stream.py` comble ce trou et ne depend que d'opencv — ni YOLO, ni dlib :

```sh
cd ia/sentinelx-ai
python3 stream.py            # CAMERA_INDEX=1 si la mauvaise camera est prise
```

Visible ensuite dans le dashboard, et directement sur
`http://127.0.0.1:8080/snapshot`.

### Analyse IA (optionnelle, lourde)

`main.py` ajoute YOLO et la reconnaissance faciale. Attention a l'installation :
`ultralytics` tire PyTorch (plusieurs centaines de Mo) et `face-recognition`
demande dlib, souvent compile depuis les sources.

```sh
pip install -r requirements.txt
cp .env.example .env          # puis remplir MQTT_PASSWORD depuis le .env racine
python3 main.py
```

**`MQTT_CLIENT_CERT` et `MQTT_CLIENT_KEY` doivent rester VIDES.** Le broker
n'exige pas de certificat client (`require_certificate` n'est pas active) et
`scripts/gen-certs.sh` ne genere pas `ia.crt`/`ia.key` : les laisser remplis
fait echouer `tls_set()` sur un fichier introuvable.
