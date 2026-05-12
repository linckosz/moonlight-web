Analyse le support du codec AV1 dans moonlight-web-stream (D:\Code\moonlight-web-stream). Je dois comprendre comment le frontend de streaming navigateur gère le codec AV1.

Ce qui m'intéresse spécifiquement :

1. **Codec string WebCodecs** : Quel est le codec string exact utilisé pour AV1 dans WebCodecs (ex: "av01.0.08M.08" ou similaire) ? Comment il est construit ?

2. **VideoDecoder configuration** : Comment le VideoDecoder WebCodecs est configuré pour l'AV1 ? Y a-t-il des paramètres spécifiques (avcC/annexB different de hvcC pour HEVC) ?

3. **OBU parsing** : Comment le format AV1 (OBU — Open Bitstream Units) est géré côté JS ? Y a-t-il un parsing spécifique ou les OBUs sont directement passées à WebCodecs ?

4. **NAL splitting** : Est-ce que moonlight-web-stream a un code pour découper/flagger les OBUs AV1 (similaire au AnnexB NAL splitting pour H.264/HEVC) ?

5. **EncodedVideoChunk** : Comment les frames AV1 sont encapsulées dans EncodedVideoChunk (type: key vs delta, timestamp, etc.) ?

6. **Capabilities / negotiation** : Comment la negotiation AV1 se fait-elle (via WebRTC SDP ? ou paramètre direct ?) ?

7. **Tout fichier JS/TS** pertinent : liste les fichiers qui gèrent l'AV1 ou le décodage vidéo en général.

Pour chaque point, fournis les extraits de code pertinents, leurs chemins de fichier, et une explication concise.

En fin de travail, écris ton résumé dans `.claude/results/expert-moonlight-web-stream/{session}/Resume-2026-05-12.md`.
Inclus uniquement tes résultats/conclusions (pas la réflexion intermédiaire).
Format : tâche accomplie, fichiers analysés, découvertes principales.
