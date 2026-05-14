---
name: cloudflared-direct
description: NportClient migre vers cloudflared direct, plus de Node.js/npx, API nport reserve subdomain
metadata:
  type: project
---

# Cloudflared Direct (plus de Node.js/npx)

NportClient a ete migre de `npx nport <port> -s <subdomain> --language en` vers un appel direct a cloudflared.

## Nouveau flux

1. `reserveSubdomain()` — HTTP POST a `POST https://api.nport.io/v1/tunnels`
   - Body: `{"subdomain": "moonlightweb-xxxx", "target_port": 48001, "target_host": "localhost"}`
   - Fallback quick tunnel trycloudflare.com si API inaccessible
2. `launchCloudflared()` — lance `cloudflared tunnel --url http://localhost:<port> [--hostname <sub>.nport.link]`
3. Parsing stdout/stderr pour URL du tunnel

## Fichiers modifies

- `backend/src/network/NportClient.h` — rewrite complet (supprime Node.js, ajoute QNetworkAccessManager + cloudflared)
- `backend/src/network/NportClient.cpp` — rewrite complet (reserveSubdomain, findCloudflared, launchCloudflared)
- `backend/src/main.cpp` — message d'erreur mis a jour
- `frontend/js/ui/AdminView.js` — messages "Node.js >= 20" remplaces par "cloudflared not found"
- `frontend/js/api/BackendClient.js` — commentaire mis a jour
- `backend/tools/download_cloudflared.ps1` — nouveau script de telechargement

## Comportement supprime

- `checkNodeVersion()`, `findNpx()`, `m_NpxPath`, `m_NodeVersion`, `m_NodeAvailable`
- `--language en` flag (etait specifique a nport)
- `LANG=en_US.UTF-8` env var (etait pour nport)

## API publique inchangee

Start/stop/isAvailable/isActive/publicUrl/lastError/pauseRefresh/resumeRefresh — tout pareil.

## Structure outils

```
backend/tools/cloudflared/
  windows/cloudflared.exe
  linux/cloudflared
  macos/cloudflared
backend/tools/download_cloudflared.ps1
```

**Why:** Elimination de la dependance Node.js instable. Le binaire cloudflared (~25 Mo) est bundle directement dans l'application, rendant le tunnel auto-contenu.

**How to apply:** Si l'API nport endpoint est different, modifier `kApiBaseUrl` dans `NportClient.cpp`. Le fallback quick tunnel (`*.trycloudflare.com`) fonctionne sans API.
