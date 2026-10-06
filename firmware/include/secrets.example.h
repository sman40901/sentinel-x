// =========================================================
// Copier ce fichier en "secrets.h" (même dossier include/) puis le remplir.
// secrets.h ne doit JAMAIS être commité (il est dans .gitignore).
// =========================================================
#pragma once

#define WIFI_SSID     "SENTINELX-G02"
#define WIFI_PASSWORD "DJJUJNCjndnvizjoi546"

#define MQTT_PASSWORD "nucidHFUIEJYUGF5483"   // = MQTT_ESP32_PASSWORD du .env serveur

// Contenu complet de certs/ca.crt du serveur
static const char CA_CERT[] = R"EOF(
-----BEGIN CERTIFICATE-----
MIIDVTCCAj2gAwIBAgIUWjXntNJIAgmfgNK+CJrPKxuJMNkwDQYJKoZIhvcNAQEL
BQAwOjELMAkGA1UEBhMCRlIxEzARBgNVBAoMClNlbnRpbmVsLVgxFjAUBgNVBAMM
DVNlbnRpbmVsLVggQ0EwHhcNMjYxMDA2MDgyMzEwWhcNMjcxMDA2MDgyMzEwWjA6
MQswCQYDVQQGEwJGUjETMBEGA1UECgwKU2VudGluZWwtWDEWMBQGA1UEAwwNU2Vu
dGluZWwtWCBDQTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBALqM2PdT
Y7+lboMnvevL1KDU0n6cX2NBnGuVwT9AA1CPjRjJskT4lNrCIxtl9i+3PJUwwrzX
E5l5nUCnMN8MirBasfYhPwHv3JDOYHAdlxI/RDA110SitkF4J/4MZdUpx7UtmPL4
ztVs5qG4AhbdutM+dSuKvisiKo9oxirHwVpPPUTQOneQ//6YLB6PhKh/DLtoiY6U
ia6hfMWT60o6wMg5q7vio62wRGpr2c8Fq7q6y6KMRuPenyrrohY4UehOVgYds6Cf
ScuTdPbm1HKOAJiD9remYhZpp4U2f3Me6+WEK++bwRzacrXv3r4LAj8TCjB49+FF
VrUYv2/XXDcZS30CAwEAAaNTMFEwHQYDVR0OBBYEFI7LPri1PhJZPEq0axMNv4a+
/WwNMB8GA1UdIwQYMBaAFI7LPri1PhJZPEq0axMNv4a+/WwNMA8GA1UdEwEB/wQF
MAMBAf8wDQYJKoZIhvcNAQELBQADggEBAFjFBUrnb9wmmfyQjkAbg4T4myTQsFQN
siaU0t8NxAnAbAT+e2jjKZ3yoOzgJuynmIJJi47qIgEdT+SfNVNh7sonCroR7lRn
WX+KdTvpIDWK6owyARmGEsqc/UEPZ3PpFvsqpA3lRW+RyqGGg0D22EOtcSMLyBbq
5pRsjPFkkJNfwEs+w4NQqr9OBfMy/NEVMdo0oZLSfBHHCo0pi3L2UDlQkHR2IFpR
yHazMQKhk/GAn7VaEYt13Vw/hT5+/lRdQXx9RsYTNKuI2kT2FFWaFizREPJDhVIH
u6Qxo+j4a6lm+IEptp3oLcR5NL6r/MoMs5EymCAGtJVd4tDTXRRPV1E=
-----END CERTIFICATE-----

