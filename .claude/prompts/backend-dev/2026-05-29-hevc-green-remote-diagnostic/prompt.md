Tu es backend-dev. Voici ta mission de diagnostic.

## Session
`2026-05-29-hevc-green-remote-diagnostic`

## Contexte
L'utilisateur a un bug d'image verte en HEVC WebRTC DataChannel UDP, MAIS UNIQUEMENT en remote (https://brunoocto.moonlightweb.top/). En localhost (https://localhost) tout fonctionne parfaitement.

Les logs montrent une différence clé : en remote, la keyframe est **bufferisée** dans DataChannelRelay parce que les DataChannels ne sont pas encore ouverts. En localhost, la keyframe arrive APRES l'ouverture des DCs, donc pas de buffering.

## Fichiers à lire et analyser

### 1. `backend/src/streaming/DataChannelRelay.cpp`
Lis ce fichier en entier. Analyse avec attention :
- `onVideoFrame()` — comment les frames sont bufferisées quand DC pas prêts
- `sendBufferedKeyframe()` — comment la keyframe bufferisée est débufferisée
- `sendFragmented()` — comment les fragments sont envoyés
- Le stale keyframe check (`m_FramesSentCount` vs `m_FramesSentAtBufferTime`)
- Comment les données sont copiées dans le buffer (memcpy ? pointeur ? risque de dangling pointer ?)
- La gestion de `m_BufferedKeyframeData`

### 2. `backend/src/streaming/DataChannelRelay.h`
Lis le header pour voir les membres et leur typage.

### 3. `backend/src/streaming/Session.cpp`
Lis le fichier. Cherche :
- Comment le bitrate est configuré (20000 vs 15000 — est-ce que c'est un paramètre de session ?)
- Comment `DataChannelRelay` est instancié
- Si y a une différence de comportement entre localhost et remote dans la session

### 4. `backend/src/streaming/StreamRelay.cpp` (si existe)

## Ce que tu dois chercher spécifiquement

1. **Corruption de la keyframe bufferisée** : est-ce que le buffer est copié correctement ? Y a-t-il un risque que le buffer soit modifié entre le moment où il est stocké et le moment où il est envoyé ?
2. **Race condition** : est-ce que le thread vidéo continue d'écrire dans le buffer pendant que le DC thread envoie ?
3. **Stale keyframe check** : est-ce que `m_FramesSentAtBufferTime` pourrait faire qu'une keyframe légitime soit ignorée ou mal envoyée ?
4. **sendFragmented** : est-ce que la fragmentation pourrait être différente pour les frames bufferisées ?
5. **Bitrate** : la différence 20000 vs 15000 pourrait-elle causer un encodage différent qui expose un bug ?

En fin de travail, écris ton résumé dans `.claude/results/backend-dev/2026-05-29-hevc-green-remote-diagnostic/Resume-2026-05-29.md`.
Inclus uniquement tes résultats/conclusions (pas la réflexion intermédiaire).
Format : ce que tu as lu, l'analyse de chaque point, les découvertes, les causes potentielles identifiées.