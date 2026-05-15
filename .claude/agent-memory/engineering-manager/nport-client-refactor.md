---
name: nport-client-refactor
description: NportClient refactored to use nport.exe directly, removed cloudflared direct invocation, added API retry on subdomain conflict
metadata:
  type: project
---

# NportClient Refactor (2026-05-15)

## Changement principal

`NportClient` a ete refactore pour lancer `nport.exe <port> -s moonlightweb-<subdomain>` au lieu d'appeler directement `cloudflared.exe tunnel run --token <token>`.

L'approche anterieure (POST API -> recuperer tunnelToken -> lancer cloudflared) est remplacee par :
1. Lancer `nport.exe` directement
2. Si echec "subdomain already in use" -> POST API pour ecraser -> relancer nport

**Pourquoi :** nport.exe encapsule cloudflared en interne et gere les appels API lui-meme. L'ancien code court-circuitait nport en allant chercher cloudflared directement.

## Fichiers modifies

- `backend/src/network/NportClient.h`
- `backend/src/network/NportClient.cpp`

## API publique inchangee

Tous les appels depuis `main.cpp` restent identiques : `start()`, `stop()`, `isAvailable()`, `isActive()`, `publicUrl()`, `subdomain()`, `lastError()`, `pauseRefresh()`, `resumeRefresh()`, les signaux `tunnelReady`/`tunnelError`/`tunnelStopped`.

## Details techniques

- **Recherche binaire :** `findNportBinary()` cherche dans `runtime/nport/node_modules/nport/bin/nport.exe` puis `.bin/nport.exe`, avec parent walk et fallback PATH
- **Args nport :** `nport.exe <targetPort> -s moonlightweb-<subdomain>`
- **Detection ready :** parsing stdout/stderr pour "ready"/"running" (case insensitive)
- **Timeout :** 15s avec QTimer annulable (`m_StartTimeoutTimer`) pour eviter les races lors d'un retry
- **Retry :** un seul retry possible (flag `m_Retried`), via `resetTunnelViaApi()` qui POST `{"subdomain":"..."}` a `https://api.nport.link`
- **Stop :** `taskkill /T /PID /F` conserve pour tuer l'arbre nport+cloudflared
- **URL :** construite par concatenation `https://moonlightweb-{subdomain}.nport.link`
- **Refresh :** mecanisme inchange (toutes les 3h30)
