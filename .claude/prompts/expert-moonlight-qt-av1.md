Analyse le support du codec AV1 dans moonlight-qt (D:\Code\moonlight-qt\app). Je dois comprendre tous les aspects de l'implémentation AV1 pour pouvoir le porter dans moonlight-web-deepseek.

Ce qui m'intéresse spécifiquement :

1. **StreamConfig** : Comment AV1 est déclaré dans l'enum VideoCodec, dans supportedVideoCodecs(), videoCodecToFlag(), et tout codec mapping. Quel flag est utilisé pour AV1 (valeur hexadécimale) ?

2. **RTSP Handshake** : Comment AV1 est négocié pendant le handshake RTSP. Quelles sont les capabilities déclarées pour AV1 ? Y a-t-il un échange spécifique ?

3. **Launch/Stream** : Comment le paramètre `videoCodec` est passé à Sunshine pendant le launch. Quelle valeur est utilisée pour AV1 ?

4. **Decode & Render** : Comment le décodage AV1 est configuré côté renderer (VAAPI, DXVA, etc.) ? Quels setup sont spécifiques à AV1 ?

5. **Tout header/source** pertinent : explore les fichiers liés à AV1 dans moonlight-qt (peut-être SdlVideo.cpp, VideoDecoder, etc.)

6. **NAL/OBU parsing** : Est-ce que moonlight-qt a un parsing spécifique pour les OBUs AV1 (similaire au parsing NAL H.264/HEVC) ?

7. **Capabilities flags** : Dans moonlight-common-c, quels sont les flags exacts pour AV1 (VLC_AVC, VLC_HEVC, VLC_AV1, etc.) ?

Pour chaque point, fournis les extraits de code pertinents, leurs chemins de fichier, et une explication concise.

En fin de travail, écris ton résumé dans `.claude/results/expert-moonlight-qt/{session}/Resume-2026-05-12.md`.
Inclus uniquement tes résultats/conclusions (pas la réflexion intermédiaire).
Format : tâche accomplie, fichiers analysés, découvertes principales.
