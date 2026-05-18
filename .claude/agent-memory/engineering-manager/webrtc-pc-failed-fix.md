---
name: webrtc-pc-failed-fix
description: Fix PeerConnection Failed sur localhost — forceMediaTransport=true causait DTLS-SRTP mismatch avec DataChannel-only
metadata:
  type: project
---

**Root cause** : `config.forceMediaTransport = true` dans `SignalingServer::buildIceConfig()` forçait DTLS-SRTP
même pour les connexions DataChannel-only. Le navigateur génère une SDP answer sans media tracks,
donc sans paramètres SRTP. Le handshake DTLS échoue → PC state=4 (Failed).

**Pourquoi :**
- Le bug arrive seulement en mode DataChannelRelay (pas de media tracks)
- Le browser voit ICE connected (connectivité UDP OK)
- Le backend voit PC Failed (handshake DTLS échoue à cause du mismatch SRTP)

**How to apply:**
- `forceMediaTransport` ne doit JAMAIS être activé dans `buildIceConfig()`
- Pour MediaTrackRelay, libdatachannel active SRTP automatiquement quand `m=video` est présent
- Pour DataChannelRelay, DTLS pour SCTP est créé automatiquement sans ce flag
- Conséquence : ICE-TCP déplacé dans le bloc `isInternet` uniquement (inutile en LAN)

**Fichiers modifiés:**
- `backend/src/streaming/SignalingServer.cpp` : retiré `forceMediaTransport=true`, ICE-TCP désactivé en LAN
- `frontend/js/api/WebRtcDataChannel.js` : `maxRetransmits: 1` → `maxRetransmits: 0` pour match backend
