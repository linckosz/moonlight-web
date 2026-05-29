Tu dois expliquer comment moonlight-qt passe le bitrate vidéo à moonlight-common-c lors du handshake RTSP.

## Contexte
On compare Moonlight-Web avec Moonlight-QT. Dans moonlight-qt, on voit le log :
```
SDL Info (0): Video bitrate: 40000 kbps
```
Dans Moonlight-Web, ce log n'apparaît pas. On veut comprendre où exactement dans le flux moonlight-qt le bitrate est configuré.

## Codebase à consulter
`D:\Code\moonlight-qt\app`

## Fichiers à lire (au minimum)

1. `streaming/Stream.cpp` — cherche :
   - Où `LiStartConnection` est appelée
   - Comment le bitrate est passé (via `LiVideoCallbacks` ? via `VIDEO_CONFIG` ? via `CONFIG` ?)
   - Le log "Video bitrate" — comment il est généré et où

2. `streaming/Stream.h` — les membres de classe liés au bitrate

3. Regarde `streaming/sdlinput.cpp`, `streaming/video/VideoCallbacks.cpp` ou tout fichier qui manipule `CONFIG` ou `LiCreateVideoCapabilities`

4. Regarde aussi les fichiers qui manipulent `PV_VIDEO_STATS` ou `VIDEO_INFO`

## Questions précises

1. Quelle est la séquence exacte d'appels pour configurer le bitrate avant handshake ?
2. Est-ce que le bitrate est passé dans une structure de type `VIDEO_CONFIG` ou directement dans le `CONFIG` global ?
3. Comment `SDL Info (0): Video bitrate: 40000 kbps` est-il généré ? Par moonlight-common-c ou par moonlight-qt ?
4. Où exactement dans moonlight-qt la fonction `LiStartConnection` reçoit-elle le bitrate ?
5. Y a-t-il un appel à `LiCreateVideoCapabilities` ou `LiSetVideoBitrate` ou équivalent ?

## Livrable
Rapporte les résultats avec les fichiers, numéros de ligne, et extraits de code exacts.

En fin de travail, écris ton résumé dans `.claude/results/expert-moonlight-qt/2026-05-29-bitrate-trace/Resume-2026-05-29.md`.