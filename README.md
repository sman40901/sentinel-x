# Sentinel-X — Infra serveur (Docker)

Stack du PC Serveur Local (option B) : **Mosquitto** (MQTT/MQTTS), **PostgreSQL**, **nginx** (HTTPS) et l'**API** des DEV.
Adresses conformes au schéma réseau : LAN de table `192.168.10.0/24`, serveur `192.168.10.1`, réseau Docker `172.28.0.0/24`.

| Conteneur | IP interne | Port exposé sur le PC |
|---|---|---|
| mosquitto | 172.20.0.10 | `8883` (TLS, Wi-Fi de table) · `1883` (clair, **127.0.0.1 seulement**) |
| api | 172.20.0.20 | aucun (via nginx `/api/`) |
| db | 172.20.0.30 | aucun |
| nginx | 172.20.0.40 | `443` (HTTPS) |

## Prérequis
- Docker + Docker Compose (Docker Desktop sous Windows)
- `openssl` et un terminal bash (Linux, ou **Git Bash / WSL** sous Windows)

## Démarrage (dans l'ordre)

```bash
# 1. Secrets : copier le modèle puis changer TOUS les mots de passe
cp .env.example .env

# 2. Certificats TLS (CA + serveur 192.168.10.1)
./scripts/gen-certs.sh

# 3. Comptes MQTT (esp32, ia, api, admin)
./scripts/add-mqtt-users.sh

# 4. Remplacer g0X par votre numéro de groupe dans mosquitto/config/acl

# 5. Lancer la stack
docker compose up -d
docker compose ps
```

Quand les DEV ont mis leur API dans `./api/` (avec un `Dockerfile`, écoute sur le port 8000) :
```bash
docker compose --profile full up -d --build
```

## Tests

**MQTT en clair (depuis le PC uniquement)** — terminal 1, écouter :
```bash
docker exec -it sentinelx-mosquitto mosquitto_sub -h localhost -p 1883 -u admin -P 'MOT_DE_PASSE_ADMIN' -t 'sentinelx/#' -v
```
Terminal 2, envoyer un faux message ESP32 :
```bash
docker exec sentinelx-mosquitto mosquitto_pub -h localhost -p 1883 -u esp32 -P 'MOT_DE_PASSE_ESP32' -t 'sentinelx/g0X/telemetry' -m '{"t":24.1,"h":48,"gaz":312,"pir":0}'
```
→ le message doit apparaître dans le terminal 1.

**MQTTS chiffré (comme l'ESP32)** :
```bash
docker exec sentinelx-mosquitto mosquitto_pub -h localhost -p 8883 --cafile /mosquitto/certs/ca.crt -u esp32 -P 'MOT_DE_PASSE_ESP32' -t 'sentinelx/g0X/telemetry' -m 'test TLS'
```

**Test des droits (ACL)** : l'ESP32 ne doit pas pouvoir écrire dans `cmd` → le message n'arrive pas chez l'abonné.

**HTTPS** : ouvrir `https://localhost` (puis `https://192.168.10.1` une fois le Wi-Fi monté). Le navigateur prévient car le certificat est auto-signé : importer `certs/ca.crt` comme autorité de confiance pour supprimer l'alerte.

**Logs / MCO** :
```bash
docker compose logs -f mosquitto
docker stats
```

## À donner aux autres filières
- **DEV (firmware)** : `certs/ca.crt`, IP `192.168.10.1`, port `8883`, compte `esp32`, topics de `mosquitto/config/acl`.
- **DEV (API)** : MQTT `mosquitto:1883` compte `api` ; base `postgresql://…@db:5432/…` (variables déjà passées par le compose).
- **IA** : MQTT `127.0.0.1:1883` (ou `8883` + `ca.crt`), compte `ia`, topic `sentinelx/g0X/alerts`.
- **CYBER** : sécurité déjà en place listée ci-dessous.

## Sécurité déjà en place
- MQTT chiffré TLS 1.2+ sur 8883 ; le port clair 1883 n'est accessible que depuis le PC
- Pas de client anonyme, un compte par appareil, ACL par topic
- API et base de données sans port publié
- `no-new-privileges`, limites mémoire, API en utilisateur non-root
- Limites anti-DoS : 50 connexions max, messages de 10 Ko max
- Rotation des logs Docker (3 × 10 Mo par conteneur)
- Secrets dans `.env`, clés et mots de passe exclus de Git (`.gitignore`)

## Pare-feu du PC (Linux, UFW)
```bash
sudo ufw default deny incoming
sudo ufw allow from 192.168.10.0/24 to any port 8883 proto tcp
sudo ufw allow from 192.168.10.0/24 to any port 443 proto tcp
sudo ufw allow from 192.168.10.20 to any port 22 proto tcp
sudo ufw enable
```
⚠️ Docker publie ses ports en contournant UFW : pour filtrer vraiment, ajouter les règles dans la chaîne `DOCKER-USER` d'iptables (à voir avec les CYBER).
