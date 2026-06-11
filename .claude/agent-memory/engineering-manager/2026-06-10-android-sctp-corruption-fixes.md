---
name: android-sctp-corruption-fixes
description: 5 corrections pour corruption H.264 Android WebRTC DataChannel (payload size, zero-fill, IDR immediat, NAL validation, dirty mode)
metadata:
  type: project
---

## Contexte
Corruption video H.264 sur Android milieu de gamme en WebRTC DC. Cause racine : chunks SCTP de 64 KB se fragmentent en ~43 paquets IP, perte probable. PC et iOS non affectes.

## Corrections
1. **kMaxPayloadSize 64000->14000** (DataChannelRelay.h) : 14KB/chunk = ~10 paquets IP
2. **IDR immediat** (WebRtcDataChannel.js) : keyframe incomplete → IDR request immediat, plus d'attente cleanup 500ms
3. **Zero-fill delta** (WebRtcDataChannel.js) : chunks manquants remplis de 0x00 pour delta frames. Modifie _cleanupStaleFrames aussi.
4. **NAL validation** (StreamView.js) : _validateNalBuffer avant decodeFrame (start code, taille, IRAP)
5. **Dirty mode** (StreamView.js) : ignore delta frames jusqu'a 3 keyframes propres consecutives

## Fichiers
- backend/src/streaming/DataChannelRelay.h
- frontend/js/api/WebRtcDataChannel.js
- frontend/js/ui/StreamView.js

## Note technique
Le zero-fill dans _assembleFrame n'etait pas atteignable (appele seulement quand chunks complets). Fix : _cleanupStaleFrames appelle _assembleFrame pour les delta frames incompletes au lieu de les dropper.
