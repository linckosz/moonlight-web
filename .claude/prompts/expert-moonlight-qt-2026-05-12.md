Tu es expert-moonlight-qt.

Objectif : Expliquer comment moonlight-qt (code de référence C++/Qt) permet à l'utilisateur de choisir/sélectionner le codec vidéo (H.264, HEVC, AV1).

Recherche à effectuer dans `D:\Code\moonlight-qt\app` :
1. Cherche comment le codec vidéo est sélectionné dans l'interface utilisateur (settings UI, dropdown, radio buttons).
2. Cherche comment la préférence de codec est stockée (QSettings, config file).
3. Cherche comment la liste des codecs supportés est construite (VideoDecoderSupport, etc.) et envoyée dans le SDP RTSP.
4. Cherche spécifiquement comment AV1 est géré : est-ce optionnel ? Y a-t-il des flags de compilation ?
5. Cherche le fichier de settings (probablement dans `src/settings/` ou `src/streaming/`).

Rapporte :
- Dans quel fichier/menu l'utilisateur choisit-il le codec dans moonlight-qt ?
- Quel est le nom exact du setting (QSettings key) ?
- Comment la préférence est-elle transmise au RTSP handshake (VideoStream::getVideoCodecInfos, etc.) ?
- Y a-t-il des prérequis pour AV1 (matériel, compilation) ?

En fin de travail, écris ton résumé dans `.claude/results/expert-moonlight-qt/2026-05-12-av1-testing/Resume-2026-05-12.md`. Inclus uniquement tes résultats, pas la réflexion intermédiaire.
