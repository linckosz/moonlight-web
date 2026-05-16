## Tache : Comprendre comment moonlight-qt gere les couleurs H.264 (VUI, color space)

### Contexte
Sur moonlight-web, le streaming H.264 donne des couleurs correctes en LAN mais incorrectes en UPnP. Je soupconne un probleme de parsing VUI (color primaries, transfer characteristics, matrix coefficients, video_full_range_flag) ou de configuration du decoder.

### Fichiers a examiner dans `D:\Code\moonlight-qt\app`

1. **Fichiers de streaming video** : chercher comment la video H.264 est decodee et affichee
   - Suggestion : regarde dans `streaming/video/` pour les fichiers comme `VideoDecodeManager.cpp`, `VideoRenderer.cpp`, etc.
   - Cherche comment les VUI parameters du SPS sont lus ou extraits
   - Cherche tout handling de `AVCOL_RANGE_JPEG` vs `AVCOL_RANGE_MPEG` (full range vs limited range)

2. **FFmpeg/avutil** : moonlight-qt utilise FFmpeg. Comment configure-t-il le sws_scale ou l'espace colorimetrique ?
   - Cherche `SwsContext`, `sws_setColorspaceDetails`, `AVColorSpace`, `AVColorRange`
   - Cherche `color_range`, `color_primaries`, `color_trc`, `color_space`

3. **Vulkan/OpenGL rendering** : comment les frames sont-elles rendues a l'ecran ? Y a-t-il une transformation YUV->RGB ?
   - Cherche des shaders GLSL ou SPIR-V qui font la conversion
   - Cherche `YUV`, `BT709`, `BT601`, `fullRange`, `limitedRange`

4. **MoonlightCommon-c** : dans `D:\Code\moonlight-qt\moonlight-common-c\`
   - Cherche comment les paquets video sont marques (type de NAL, SPS parsing)
   - Cherche `VUI`, `color`, `h264` dans `src/` ou `include/`

### Questions specifiques
1. Comment moonlight-qt determine-t-il si la video est "full range" (0-255) ou "limited range" (16-235) ?
2. Comment moonlight-qt determine-t-il BT.601 vs BT.709 ?
3. Est-ce que moonlight-qt parse le SPS pour extraire les VUI, ou se fie-t-il a des heuristiques ?
4. Est-ce que le decoder (DXVA2, VAAPI, etc.) configure automatiquement l'espace colorimetrique, ou moonlight-qt le force-t-il ?

### Rapport
Ecrit ton resume dans `.claude/results/expert-moonlight-qt/2026-05-16-color-investigation/Resume-2026-05-16.md`.
Inclus les fichiers et fonctions pertinents avec leurs extraits de code.
