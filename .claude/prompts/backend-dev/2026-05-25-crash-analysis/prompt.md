# Analyse de crash — Serveur MW

## Contexte

L'utilisateur rapporte que le lancement du stream est cassé. Le serveur crash ("terminated abnormally") après la séquence suivante (logs) :

```
[Session] Transport: auto mode
[Auto] Codec 1 not supported by host, falling back to H.264
[Auto] Effective codec: 0
[Auto] Ordered transports after codec filter: QList("webrtc-media-udp", "webrtc-dc-udp", "webrtc-media-tcp", "webrtc-dc-tcp", "wss")
[Auto] Attempt 1 / 5 : "webrtc-media-udp" iceTcp= false
[Session] Video codec preference set to 1
[Session] Stream settings: 1920 x 1080 @ 60 fps, bitrate: 20000 kbps, gaming: on codec: 1
[Session] Pre-start: force-quitting any stale session on "780M" "192.168.1.9"
[moonlight] Control stream received unexpected disconnect event
[MoonlightShim] Connection terminated, code=-1 socketErr=-1
[MediaTrackRelay] Shim connection terminated, code= -1
[MediaTrackRelay::stop] Already stopping, skip
[Auto] "MediaTrackRelay" sessionEnded — responded= false
[MediaTrackRelay] Destructor
[MediaTrackRelay::stop] Already stopping, skip
[SignalingServer] Destructor
[SignalingServer::stop] ENTER, signalingComplete= false dataChannelsOpen= false
[SignalingServer] Closing WS server
[SignalingServer::stop] EXIT
[MoonlightShim::finishCleanup] Calling LiStopConnection() ...
[moonlight] Stopping input stream...[moonlight] done
[moonlight] Stopping audio stream...[moonlight] No audio traffic was ever received from the host!
[moonlight] done
[moonlight] Stopping video stream...[moonlight] Failed to send ENet control packet
[moonlight] Loss Stats: Transaction failed: 10035
[moonlight] done
[moonlight] Stopping control stream...[moonlight] ENet peer is already disconnected
[moonlight] done
[moonlight] Cleaning up input stream...[moonlight] done
[moonlight] Cleaning up video stream...[moonlight] done
[moonlight] Cleaning up control stream...[moonlight] done
[moonlight] Cleaning up audio stream...[moonlight] done
[moonlight] Cleaning up platform...[moonlight] done
[MoonlightShim::finishCleanup] LiStopConnection() returned OK
[Auto] Trying next transport after session ended
22:53:57: The command terminated abnormally.
```

## Tâches

1. **Lire les fichiers suivants** (affiche-moi le contenu complet ou les sections critiques) :
   - `backend/src/streaming/Session.cpp`
   - `backend/src/streaming/MediaTrackRelay.cpp` (et MediaTrackRelay.h)
   - `backend/src/streaming/SignalingServer.cpp` (et SignalingServer.h)

2. **Analyser les changements récents** : Exécute `git log --oneline -20` et `git diff HEAD~5` pour voir ce qui a changé récemment.

3. **Identifier la cause racine du crash** spécifiquement :
   - Pourquoi le serveur crash après "[Auto] Trying next transport after session ended" ?
   - Y a-t-il un use-after-free, un dangling pointer, ou un double delete dans le fallback de transport ?
   - Le destructeur de MediaTrackRelay est appelé, puis SignalingServer, puis MoonlightShim::finishCleanup. Ensuite "Trying next transport" → crash. Que se passe-t-il entre les deux ?
   - Vérifie si l'objet Session existe toujours après que le transport a été détruit.
   - Vérifie les callbacks (sessionEnded, connectionTerminated) : est-ce qu'ils référencent des objets déjà détruits ?

4. **Propose un fix** : Explique précisément quoi modifier (quel fichier, quelle ligne, quel correctif).

## Fichiers de résultat

Écris ton résumé d'analyse dans :
`.claude/results/backend-dev/2026-05-25-crash-analysis/Resume-2026-05-25.md`

Inclus : fichiers lus, git log/diff pertinent, cause racine identifiée, proposition de fix.
