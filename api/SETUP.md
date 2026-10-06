# Guide d'installation de l'API

## Prérequis

- Python 3.8 ou supérieur
- pip (gestionnaire de paquets Python)

## Installation

### 1. Installer les dépendances

```bash
cd api
pip install -r requirement.txt
```

Cette commande installera :
- paho-mqtt (client MQTT)
- fastapi (framework Web)
- uvicorn (serveur ASGI)
- pytest (framework de tests)
- pytest-asyncio (support des tests asynchrones)

### 2. Définir les variables d'environnement

Créez un fichier `.env` dans le répertoire `api` :

```bash
# Copier le fichier d'exemple
cp .env.example .env

# Modifier le fichier .env avec vos paramètres
notepad .env  # Windows
# ou
nano .env     # Linux/Mac
```

Pour les tests (sans authentification et sans TLS) :

```bash
ENVIRONMENT=test
MQTT_HOST=mosquitto
MQTT_PORT=1883
MQTT_USERNAME=
MQTT_PASSWORD=
MQTT_USE_TLS=false
MQTT_CA_CERT=/certs/ca.crt
GROUP_ID=g0X
```

### 3. Démarrer l'API

```bash
python -m uvicorn src.main:app --reload
```

L'API démarrera sur `http://localhost:8000`

### 4. Tester l'API

Accédez à :

`http://localhost:8000/api/v1/health`

Vous devriez obtenir :

```json
{
  "status": "ok",
  "mqtt_connected": false
}
```

## Exécution des tests

### Installer les dépendances de test

```bash
pip install -r requirement.txt
```

### Exécuter tous les tests

```bash
pytest
```

### Exécuter les tests avec une sortie détaillée

```bash
pytest -v
```

### Exécuter un fichier de test spécifique

```bash
pytest tests/test_mqtt_handlers.py
```

### Exécuter un test spécifique

```bash
pytest tests/test_mqtt_handlers.py::TestHandleTelemetry::test_valid_telemetry
```

## Dépannage

### pytest introuvable

Si vous obtenez l'erreur `"pytest: command not found"`, installez-le :

```bash
pip install pytest pytest-asyncio
```

Vous pouvez également utiliser la syntaxe de module Python :

```bash
python -m pytest
```

### Erreurs d'importation

Assurez-vous d'être dans le répertoire `api` :

```bash
cd api
python -m pytest
```

### Erreurs de connexion MQTT

- Assurez-vous que le broker Mosquitto est en cours d'exécution
- Vérifiez `MQTT_HOST` et `MQTT_PORT` dans le fichier `.env`
- Pour les tests, utilisez `MQTT_USE_TLS=false`