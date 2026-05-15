---
name: nport-graceful-shutdown-fix
description: Fix NportClient: graceful Ctrl+C shutdown au lieu de taskkill, pre-creation tunnel API pour tunnelId, HTTP origin fix
metadata:
  type: project
---

# Nport Graceful Shutdown Fix

## Problemes corriges

1. **DELETE 400** : `releaseSubdomain()` appelait DELETE API sans `tunnelId` → API rejetait (400 Bad Request). Solution : supprimee, nport gere le DELETE via son cleanup() sur SIGINT.

2. **taskkill /T /F** : tuait nport brutalement → son handler SIGINT (cleanup avec DELETE) n'etait jamais appele → tunnel orphelin. Solution : `sendCtrlC()` utilisant `GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0)` sur Windows, declenche le cleanup() de nport.

3. **cloudflared EOF** : nport spawne cloudflared avec `--url http://localhost:443` mais notre serveur attend TLS sur 443. Solution double :
   - `HttpServer::onHttpConnection()` ne fait plus de redirect 307 → sert le full app en HTTP
   - `NportClient::m_TargetPort` pointe sur le port HTTP (48000) au lieu du HTTPS (443)

## Fichiers modifies

- `backend/src/network/NportClient.h` — Nouveaux membres : `m_TunnelId`, methodes `createTunnelViaApi()`, `sendCtrlC()`, `forceKill()`. Supprime `releaseSubdomain()`. Default port 48000.
- `backend/src/network/NportClient.cpp` — Implementation complete du shutdown gracieux, pre-creation tunnel API, retry preserve.
- `backend/src/server/HttpServer.cpp` — `onHttpConnection()` sert les requetes HTTP directement (plus de redirect).
- `backend/src/main.cpp` — `setTargetPort()` utilise `server.httpPort()` au lieu de `server.activeHttpsPort()`.

## Architecture

### Shutdown flow
```
stop() → sendCtrlC() → GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0)
                          ↓
                     nport recoit SIGINT
                          ↓
                     nport.cleanup() → kill cloudflared → DELETE API avec tunnelId → exit(0)
                          ↓
                     waitForFinished(10s) → OK → clean
                          ↓ (timeout)
                     forceKill() → taskkill /T /F
```

### Pre-creation tunnel
```
doStart() → createTunnelViaApi() → POST {subdomain} → {tunnelId, url}
                                                         ↓
                                                    store m_TunnelId, m_PublicUrl
                                                         ↓
                                                    launchNport() (nport cree son propre tunnel aussi)
```

**Why:** Le tunnelId est stocke pour reference/monitoring. L'URL vient directement de l'API (fiable).

### HTTP origin fix
**Why:** cloudflared utilise `http://localhost:<port>` comme origin. Avant, le port etait 443 (HTTPS) et le serveur HTTP (48000) ne faisait que du redirect. Maintenant le serveur HTTP sert le full app, et nport cible le port HTTP.

## Pre-requis

- Windows only pour le Ctrl+C (`GenerateConsoleCtrlEvent`), fallback `terminate()` sur Unix.
- Serveur HTTP sur port 48000 (ou autre, configure via --port) doit fonctionner.
