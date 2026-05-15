---
name: cloudflared-to-nport-migration
description: Migration de cloudflared vers nport + Node.js runtime integre (Phase 6)
metadata:
  type: project
---

# Migration cloudflared -> nport

Remplacement de cloudflared par le package npm nport + Node.js runtime embarqué pour le tunnel Internet.

## Nouvelle architecture

- `runtime/node/` — Node.js v26.1.0 executable (telecharge par `prepare_node_nport.ps1`)
- `runtime/nport/` — Wrapper npm avec dependance nport
- `NportClient` — Lance `node <nport-cli> <port> -s moonlightweb-<hex>` au lieu de `cloudflared tunnel`
- `prepare_node_nport.ps1` — Script PowerShell appele par le build, telecharge Node.js et installe nport

## Changements cles

- `findCloudflared()` -> `findNodeRuntime()` cherche `runtime/node/node.exe`
- Nouvelle methode `findNportScript()` lit `runtime/nport/node_modules/nport/package.json` pour trouver l'entry point (bin/main field)
- `launchCloudflared()` -> `launchNport()` lance `node <script> <port> -s moonlightweb-<hex>`
- Plus d'appel API REST de reservation (nport CLI gere lui-meme)
- `isAvailable()` necessite Node.js + nport script + subdomain (3 conditions)
- `parsePublicUrl()` simplifiee (plus de trycloudflare.com generic URL)

## Fichiers supprimes
- `backend/tools/download_cloudflared.ps1` (remplace par prepare_node_nport.ps1)

## Point d'attention — Detection du binaire nport

Le package npm `nport` est un outil Node.js CLI. npm cree un wrapper `.cmd` sur Windows (dans `node_modules/.bin/nport.cmd`), pas un `.exe`.

### Correction (2026-05-15)

Le `findNportBinary()` cherchait uniquement `nport.exe` sur Windows, donc ne trouvait jamais le wrapper `.cmd`. Fix :
- Cherche d'abord `nport.cmd` (npm wrapper), puis `nport.exe`, puis `nport`
- `where nport` (sans extension) au lieu de `where nport.exe`
- QProcess execute les `.cmd` nativement via cmd.exe, aucun changement dans `launchNport()`

### Messages stale corriges
- `AdminView.js` : "Node.js runtime not found" -> "nport binary not found"
- `main.cpp` : "Node.js/nport not found" -> "binary not found"

### Fichiers modifies
- `backend/src/network/NportClient.cpp` — `findNportBinary()` multi-binary search
- `backend/src/network/NportClient.h` — commentaire mis a jour
- `frontend/js/ui/AdminView.js` — message stale supprime
- `backend/src/main.cpp` — message stale supprime
