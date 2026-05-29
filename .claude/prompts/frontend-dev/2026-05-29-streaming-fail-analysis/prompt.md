Analyse les fichiers frontend suivants pour comprendre un bug de streaming.

**Session ID**: `2026-05-29-streaming-fail-analysis`

**Contexte** : Apres ICE timeout (UDP bloque), le fallback WS s'active. Le backend envoie "fallback-ws" au navigateur, mais la WebSocket se ferme immediatement apres avec code 1000.

**Sequence observee** :
1. Backend envoie "fallback-ws" au navigateur
2. Backend log "DataChannels are open" (SCTP etabli)
3. Backend log "WS FALLBACK ACTIVE"
4. Backend log "Video frames will be sent as binary WS messages"  
5. Backend log "WS closed: code=1000" (onWsDisconnected)

Le code 1000 est une fermeture normale (CLOSE_NORMAL), ce qui suggere que c'est le navigateur QUI fermE la WebSocket, pas une erreur reseau.

**Fichiers a examiner** :

1. `frontend/js/api/WebRtcDataChannel.js` — en entier. Concentre-toi sur :
   - Le handler de message "fallback-ws" 
   - Comment la WebSocket de signaling est creee et fermee
   - La logique de nettoyage/arret quand le fallback est demande
   - Tout endroit qui appelle `ws.close()` ou `signalingWs.close()`
   - La gestion du message "session-ended"

2. `frontend/js/streaming/StreamView.js` — en entier (ou au moins les parties pertinentes). Concentre-toi sur :
   - Comment le streaming demarre et s'arrete
   - La gestion des evenements de session (sessionEnded, clientDisconnected)
   - Tout endroit qui pourrait fermer la WebSocket

Pour chaque fichier, lis-le en entier et identifie TOUS les chemins de code qui pourraient fermer la WebSocket de signaling.

Hypothese principale : le navigateur, en recevant "fallback-ws", fait un cleanup qui ferme la WebSocket de signaling, ce qui coupe le fallback avant meme qu'il commence.

Ecris ton analyse detaillee dans `.claude/results/frontend-dev/2026-05-29-streaming-fail-analysis/Resume-2026-05-29.md`.
