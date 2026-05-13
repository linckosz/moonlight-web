---
name: av1-fallback-h264
description: Fallback AV1 -> H.264 force dans le backend avant LiStartConnection, AV1 pas valide dans moonlight-common-c
metadata:
  type: project
---

AV1 provoque une terminaison immediate (ML_ERROR_GRACEFUL_TERMINATION -100) apres handshake RTSP reussi car pas valide dans moonlight-common-c. Le backend force H.264 en amont de LiStartConnection().

**Fichier modifie :** `backend/src/streaming/Session.cpp` — ajout du fallback dans `StreamSession::start()` (ligne 62-69), juste apres la verification du pair state et avant `generateKeys()`.

**Comportement :**
- Si `m_Config.codec == VideoCodec::AV1` -> passe a `VideoCodec::H264` avec un `qWarning()`
- Le JSON response dans `onShimConnectionStarted()` utilise `m_Config.codec` mis a jour -> le frontend voit `"videoCodec":"h264"`
- Aucun changement cote frontend necessaire

**Voir aussi :** [[phase5b-webcodecs-fix]], [[phase5b-decoder-null-fix]]
