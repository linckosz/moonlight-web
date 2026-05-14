Tu es backend-dev, sous-agent de l'Engineering Manager.

## Tache

Explore le package npm `nport` et lis les fichiers existants pour preparer la migration cloudflared -> nport.

### 1. Verifier le package nport

Execute :
```bash
cd d:\Code\moonlight-web-deepseek
npm view nport
```

Note : si npm view nport echoue parce que npm n'est pas dans le PATH, essaie d'utiliser le npx disponible ou localise npm avec `where npm`.

L'objectif est de connaitre :
- L'entry point (champ "main" ou "bin" dans package.json)
- Les arguments CLI supportes
- La structure du package

### 2. Lire les fichiers existants

Lis les fichiers suivants :
- `d:\Code\moonlight-web-deepseek\backend\src\network\NportClient.h`
- `d:\Code\moonlight-web-deepseek\backend\src\network\NportClient.cpp`
- `d:\Code\moonlight-web-deepseek\backend\build_msvc.bat`
- `d:\Code\moonlight-web-deepseek\backend\tools\download_cloudflared.ps1` (s'il existe)

### 3. Verifier l'existence du dossier cloudflared

```bash
ls -la d:/Code/moonlight-web-deepseek/backend/tools/cloudflared/
```

### 4. Rapport

Redige un rapport concis avec :
- Les infos du package nport (entry point, CLI)
- Le contenu resume des fichiers lus
- Les chemins a supprimer

En fin de travail, ecris ton resume dans `.claude/results/backend-dev/2026-05-14-nport-migration/Resume-2026-05-14.md`.
