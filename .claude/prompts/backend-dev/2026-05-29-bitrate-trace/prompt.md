Tu dois tracer le chemin du bitrate depuis l'API backend jusqu'à moonlight-common-c (LiStartConnection / RTSP handshake).

## Contexte
L'utilisateur a observé que dans les logs Moonlight-Web on voit :
```
[Session] Stream settings: 1920 x 1080 @ 60 fps, bitrate: 40000 kbps
[Session] Launching app ...
```
Mais AUCUN log "Video bitrate: 40000 kbps" comme dans moonlight-qt. Il suspecte que le bitrate n'est pas passé correctement à moonlight-common-c pendant le handshake RTSP.

## Fichiers à lire

1. `d:\Code\moonlight-web-deepseek\backend\src\streaming\Session.cpp` — cherche :
   - Où `LiStartConnection` (ou la fonction qui démarre le stream) est appelée
   - Quels paramètres de stream sont configurés avant l'appel (bitrate, resolution, fps)
   - Comment les VIDEO_CONFIG ou flags sont positionnés
   - Où est loggé `[Session] Stream settings: ...` et comment bitrate est extrait

2. `d:\Code\moonlight-web-deepseek\backend\src\settings\AppSettings.h` et `AppSettings.cpp` — cherche :
   - Comment le bitrate est stocké (clé, type)
   - Les getter/setter

3. `d:\Code\moonlight-web-deepseek\backend\src\api\StreamController.cpp` (ou tout fichier API qui gère le lancement) — cherche :
   - Comment le bitrate est récupéré des settings et passé à Session

4. `d:\Code\moonlight-web-deepseek\backend\src\streaming\NvHTTP.cpp` ou similaire — cherche :
   - La construction de l'URL de launch (même si le bitrate n'est pas dans l'URL, vérifie)

5. Tout fichier qui appelle `LiStartConnection`, `LiCreateVideoCapabilities`, ou manipule `VIDEO_INFO`/`VIDEO_CONFIG`

## Questions précises

1. Quelle fonction de moonlight-common-c est appelée pour démarrer le handshake RTSP ? (`LiStartConnection` ou autre ?)
2. Quels paramètres sont passés à cette fonction ? Le bitrate fait-il partie des paramètres ?
3. Comment `LI_VIDEO_BITRATE` ou équivalent est-il configuré ?
4. Est-ce que `CONFIG.caps` inclut `CAP_SRGB` ou d'autres flags video ?
5. Est-ce que le bitrate est passé dans la structure `VIDEO_INFO` ou `PV_VIDEO_STATS` ou autre ?

## Livrable
Rapporte les résultats avec les fichiers, numéros de ligne, et extraits de code pertinents.

En fin de travail, écris ton résumé dans `.claude/results/backend-dev/2026-05-29-bitrate-trace/Resume-2026-05-29.md`.