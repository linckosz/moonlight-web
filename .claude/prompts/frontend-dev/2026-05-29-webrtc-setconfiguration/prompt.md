Objectif : Analyser l'erreur WebRTC `Failed to execute 'setConfiguration' on 'RTCPeerConnection': Attempted to modify the PeerConnection's configuration in an unsupported way.` qui apparaît ligne 685 de WebRtcDataChannel.js.

Tâches :
1. Lis le fichier `d:\Code\moonlight-web-deepseek\frontend\js\streaming\WebRtcDataChannel.js`, en particulier autour de la ligne 685.
2. Identifie la fonction qui appelle `setConfiguration` et le contexte (pourquoi on appelle setConfiguration, à quel moment du cycle de vie WebRTC, quelles propriétés sont modifiées).
3. Regarde ce qui a changé dans le git status — le fichier est modifié (M frontend/js/streaming/WebRtcDataChannel.js) d'après le statut.
4. Explique **en français** (pour que je puisse transmettre à l'utilisateur) :
   a. Pourquoi cette erreur se produit (cause racine)
   b. Si l'erreur est bénigne ou bloquante
   c. Comment la corriger si nécessaire

Note : `setConfiguration` sur une `RTCPeerConnection` ne peut modifier que `iceServers` (et encore, pas dans tous les navigateurs). Les autres champs (iceTransportPolicy, bundlePolicy, rtcpMuxPolicy, certificates) sont en lecture seule après la création de la connection.

En fin de travail, écris ton résumé dans `.claude/results/frontend-dev/2026-05-29-webrtc-setconfiguration/Resume-2026-05-29.md`.