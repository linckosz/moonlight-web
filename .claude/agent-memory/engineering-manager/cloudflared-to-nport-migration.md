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

## Point d'attention
Le package npm `nport` n'a pas ete verifie directement (npm view non disponible). L'implementation lit dynamiquement le `package.json` du package installe pour trouver le `bin` entry. Si le package nport a une structure non standard, un fallback vers `cli.js` ou `index.js` est fait.
