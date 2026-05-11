---
name: test-stream
description: Test de streaming end-to-end — lance le serveur backend et vérifie les endpoints REST + WebSocket
---

# Skill — Test Streaming E2E

Lance le backend Moonlight-Web et vérifie la connectivité de base.

## Procédure

### 1. Arrêter toute instance existante
```bash
powershell -Command "Stop-Process -Name mw-server -Force" 2>/dev/null
```

### 2. Lancer le serveur en arrière-plan
```bash
cd d:/Code/moonlight-web-deepseek/backend/build/release && ./mw-server.exe &
```
Attendre 2 secondes que le serveur démarre.

### 3. Vérifier le health check HTTP
```bash
curl -k -s -o /dev/null -w "%{http_code}" http://127.0.0.1:48000/
```
Doit retourner `200`.

### 4. Vérifier l'endpoint hosts (API REST)
```bash
curl -k -s http://127.0.0.1:48000/api/hosts | head -c 200
```
Doit retourner un JSON valide `{"hosts": [...]}`.

### 5. Vérifier la route /start si un host est disponible
```bash
# Récupérer le premier host (si existant)
curl -k -s http://127.0.0.1:48000/api/hosts
# Si un host est listé, tester /start avec son UUID et un appId
```

### 6. Arrêter le serveur
```bash
curl -k -s http://127.0.0.1:48000/api/status  # si existe
powershell -Command "Stop-Process -Name mw-server -Force"
```

## Format de rapport

```
## Test Streaming E2E

| Test | Résultat |
|---|---|
| HTTP server (port 48000) | ✅ / ❌ |
| GET /api/hosts | ✅ / ❌ |
| GET / (index.html) | ✅ / ❌ |
| WebSocket endpoint | ✅ / ❌ (si testable) |

### Erreurs détectées
- [description + logs pertinents]
```

## Notes

- Le serveur doit avoir les DLLs Qt + OpenSSL dans le dossier de sortie
- Port par défaut : 48000 (HTTP), 48443 (HTTPS)
- Sunshine n'a pas besoin d'être en ligne pour ce test de base
- Pour tester le streaming réel, il faut un host Sunshine accessible
