Tu es backend-dev.

Objectif : Analyser comment les codecs vidéo (H.264, HEVC, AV1) sont sélectionnés/configurés dans le backend C++ de moonlight-web-deepseek.

Recherche à effectuer :
1. Lis `d:\Code\moonlight-web-deepseek\backend\src\stream\StreamSession.cpp` — cherche comment la liste des codecs supportés est construite dans la vidéo RTSP handshake (SDP). Regarde les préférences de codec, l'ordre, et si AV1 apparaît.
2. Lis `d:\Code\moonlight-web-deepseek\backend\src\stream\StreamConfig.h` ou équivalent — vérifie s'il y a une structure de configuration qui stocke le codec préféré.
3. Cherche si settings.json (chemin = `c:\Users\Minis\AppData\Roaming\Moonlight-Web\Moonlight-Web\settings.json`) est lu par le backend, et si un paramètre de codec y est défini.
4. Lis les fichiers pertinents de `backend/src/` liés à la configuration/settings.

Rapporte :
- Quel fichier contient la liste des codecs supportés et dans quel ordre ?
- Y a-t-il déjà du code pour AV1 dans le backend ?
- Y a-t-il un mécanisme pour choisir le codec côté backend (settings, env var, flag) ?
- Si AV1 n'est pas activé, qu'est-ce qui le bloque exactement ?

En fin de travail, écris ton résumé dans `.claude/results/backend-dev/2026-05-12-av1-testing/Resume-2026-05-12.md`. Inclus uniquement tes résultats, pas la réflexion intermédiaire.
