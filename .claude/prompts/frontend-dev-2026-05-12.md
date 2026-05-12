Tu es frontend-dev.

Objectif : Analyser comment le frontend (WebCodecs) configure et sélectionne les codecs vidéo (H.264, HEVC, AV1) dans moonlight-web-deepseek.

Recherche à effectuer :
1. Lis `d:\Code\moonlight-web-deepseek\frontend\js\video\VideoPipeline.js` — cherche comment VideoDecoder est configuré, quels codecs sont supportés ou détectés.
2. Lis `d:\Code\moonlight-web-deepseek\frontend\js\stream\StreamConnection.js` ou fichier équivalent — cherche comment les paramètres de codec sont transmis entre le serveur et le frontend.
3. Cherche dans tout le dossier `frontend/` les références à "av1", "vp9", "hevc", "h264", "codec" pour comprendre la détection de capacités.
4. Vérifie s'il y a un fichier de configuration frontend qui pourrait permettre de choisir le codec (settings.json côté navigateur, localStorage, config.js, etc.)

Rapporte :
- Comment le frontend détermine-t-il quel codec utiliser (négociation, auto-détection, hardcodé) ?
- Y a-t-il déjà du code pour AV1 dans le frontend (détection `VideoDecoder.isConfigSupported`, etc.) ?
- Y a-t-il un paramètre localStorage, URL query param, ou config qui permet de forcer un codec ?
- Le frontend supporte-t-il déjà la réception de flux vidéo AV1 dans WebCodecs ?

En fin de travail, écris ton résumé dans `.claude/results/frontend-dev/2026-05-12-av1-testing/Resume-2026-05-12.md`. Inclus uniquement tes résultats, pas la réflexion intermédiaire.
