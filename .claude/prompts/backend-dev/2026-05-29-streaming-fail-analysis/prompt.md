Analyse les fichiers backend suivants pour comprendre un bug de streaming.

**Session ID**: `2026-05-29-streaming-fail-analysis`

**Contexte** : Le WS fallback s'active apres ICE timeout, mais la WebSocket se ferme immediatement (code 1000) juste apres l'envoi du message "fallback-ws".

**Sequence critique dans les logs** :
```
[SignalingServer] ICE timeout — WS fallback disabled (auto mode), emitting sessionEnded for fallback chain
[SignalingServer] Browser requested WS fallback (ICE disconnected/failed before connected)
[SignalingServer] === STARTING WS FALLBACK ===
[SignalingServer] ICE failed — routing video/audio over signaling WebSocket
[DataChannelRelay::stop] ENTER, frameCount= 0
[DataChannelRelay] Closing video DataChannel
...
[DataChannelRelay::stop] EXIT
[SignalingServer] Sent fallback-ws notification to browser
[SignalingServer] DataChannels are open (SCTP established)
[SignalingServer] === WS FALLBACK ACTIVE ===
[SignalingServer] Video frames and audio samples will now be sent as binary WebSocket messages
[SignalingServer::onWsDisconnected] WS closed: code= 1000 reason= "" error= "Unknown error"
```

**Fichiers a examiner** :

1. `backend/src/streaming/SignalingServer.cpp` — en entier. Concentre-toi sur :
   - La methode qui gere le fallback WS (fallback-ws notification)
   - Le callback `onWsDisconnected` 
   - L'etat `m_DataChannelsOpen` et quand il est mis a true
   - La logique qui decide d'emettre `sessionEnded` vs continuer
   - Comment la WebSocket est liee au cycle de vie du fallback

2. `backend/src/streaming/DataChannelRelay.cpp` — en entier. Concentre-toi sur :
   - La methode `stop()` et quand `m_Stopping` est mis a true
   - Le callback `onDataChannel` (ouvert/ferme)
   - La logique ICE timeout (3s)
   - Le callback `onStateChange` du PeerConnection
   - Comment `m_DataChannelsOpen` du SignalingServer est mis a jour

3. `backend/src/streaming/DataChannelRelay.h` — pour voir les declarations et flags

4. `backend/src/streaming/SignalingServer.h` — pour voir les declarations et flags

Pour chaque fichier, lis-le en entier et identifie TOUS les chemins de code qui pourraient expliquer pourquoi la WS se ferme immediatement apres le fallback-ws.

Hypotheses a verifier :
- Est-ce que `m_DataChannelsOpen` devient true APRES que `stop()` a ete appele ? (Ca semble etre le cas dans les logs)
- Est-ce qu'un callback async (`onDataChannel` ouvert) arrive apres le stop et re-ouvre ou change l'etat ?
- Est-ce que `onWsDisconnected` est declenche par le navigateur qui ferme la WS, ou par le backend ?

Ecris ton analyse detaillee dans `.claude/results/backend-dev/2026-05-29-streaming-fail-analysis/Resume-2026-05-29.md`.
