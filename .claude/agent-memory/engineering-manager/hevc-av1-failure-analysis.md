---
name: hevc-av1-failure-analysis
description: Analyse des echecs HEVC/AV1 sur machine multi-GPU (iGPU + 2x RTX 5060) -- Sunshine utilise probablement le mauvais GPU pour l'encodage
metadata:
  type: project
---

## Contexte

Deux machines :
- 192.168.1.9 : moonlight-web server, tous les codecs marchent (H.264, HEVC, AV1)
- 192.168.1.66 : Sunshine host avec iGPU + 2x RTX 5060. H.264 marche, mais HEVC et AV1 echouent.

Le materiel (RTX 5060) supporte HEVC et AV1 en hardware, pourtant le streaming echoue.

## Flux du codec dans moonlight-web

1. **Choix** : SettingsView -> `/api/settings/streaming` -> `AppSettings` (persiste serveur)
2. **Lancement** : `POST /api/hosts/:id/start` -> `StreamSession` cree avec `appSettings.videoCodec()`
3. **Fallback AV1** : `Session::start()` force `AV1 -> H.264` (ligne 62-69 de Session.cpp) car moonlight-common-c ne supporte pas AV1 correctement (termine avec ML_ERROR_GRACEFUL_TERMINATION -100)
4. **Appel Sunshine** : `NvHTTP::launchAppAsync()` -- **aucun parametre codec passe a `/launch`**
5. **Negociation RTSP** : `StreamConfig::computeVideoFormats()` construit le bitmask -> `LiStartConnection()` -> RTSP ANNOUNCE
6. **Pas de fallback HEVC -> H.264** : si HEVC echoue, le stream echoue sans fallback

## Infos exposees par Sunshine

- `/serverinfo` XML contient `<gputype>`, `<ServerCodecModeSupport>` (bitmask codecs), `<MaxLumaPixelsHEVC>`
- Deja parse dans `NvComputer` et expose dans `/api/hosts` JSON via `toJson()`
- GPU selection par Sunshine = probe d'encodeur au demarrage (`active_hevc_mode`, `active_av1_mode`)

## Cause probable

Sunshine sur 192.168.1.66 utilise l'iGPU (pas les RTX 5060) pour l'encodage, ou le probe d'encodeur echoue pour HEVC/AV1 sur les RTX 5060 (driver issue, GPU non selectionne).

## Verifications possibles

1. Lire `serverCodecModeSupport` dans la reponse `/api/hosts` pour 192.168.1.66
2. Lire les logs de demarrage Sunshine sur 192.168.1.66
3. Verifier `adapter_name` dans la config Sunshine

## Limitations de moonlight-web

- Aucune verification croisee entre `serverCodecModeSupport` et le codec demande
- Pas de fallback automatique HEVC -> H.264
- GPU model expose dans l'API mais pas affiche dans l'UI

**Fichiers cles :**
- `backend/src/streaming/Session.cpp` (fallback AV1)
- `backend/src/streaming/StreamConfig.cpp` (computeVideoFormats)
- `backend/src/backend/NvComputer.cpp` (parsing serverCodecModeSupport, gpuModel)
- `backend/src/main.cpp` (routes /start, /settings/streaming)
- `frontend/js/ui/SettingsView.js` (choix codec)
- `frontend/js/ui/StreamView.js` (WebCodecs VideoDecoder)
