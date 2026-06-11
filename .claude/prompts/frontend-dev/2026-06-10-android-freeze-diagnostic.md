# Session: 2026-06-10-android-freeze-diagnostic — frontend-dev

## Tâche : Diagnostic du freeze Android WebRTC DC

Tu travailles pour l'Engineering Manager. Tu dois lire et analyser les fichiers suivants pour diagnostiquer un freeze Android.

### Contexte

Les corrections Android "A-E" ont introduit une régression : le stream WebRTC DC (HEVC et H264) freeze après ~1 seconde sur Android.

Séquence observée :
1. 30 frames reçues/décodées, 12 rendues
2. `[Dirty] entering dirty mode (frameId gap 7 (max=32))`
3. `dropped=98` pour `received=30` — ratio aberrant
4. `received` gelé à 30, `dropped` grimpe à 202
5. Des frames avec frameId #163, #463, #777 arrivent (delta frames)
6. Le dirty mode les droppe toutes → freeze permanent

### Fichiers à lire et analyser

1. **`d:\Code\moonlight-web-deepseek\frontend\js\ui\StreamView.js`** — tout lire
2. **`d:\Code\moonlight-web-deepseek\frontend\js\api\WebRtcDataChannel.js`** — tout lire

### Questions à répondre

**Q1 : Est-ce que le dirty mode sort correctement ?**
Vérifie la logique de sortie : `_decoderDirtyKeyframes`, `DIRTY_CLEAN_THRESHOLD`, et comment les keyframes sont détectées. Est-ce que le compteur de keyframes peut atteindre le seuil ?

**Q2 : Est-ce que le seuil de gap (frameId gap > X) est trop bas avec kMaxPayloadSize=14000 ?**
Quel est le seuil exact ? Regarde comment le frameId gap est calculé.

**Q3 : Est-ce que le zero-fill produit des frames que le dirty mode droppe immédiatement ?**
Regarde `_assembleFrame()` et le zero-fill. Est-ce que l'assemblage partiel peut produire des frames avec un frameId qui trigger le dirty mode ?

**Q4 : Est-ce que `_maxFrameId` / `_lastDisplayedFrameId` est mis à jour correctement ?**
Peut-il sauter à cause du reordering SCTP avec kMaxPayloadSize=14000 ?

### Instructions

Lis chaque fichier avec Read (fichier entier). Pour chaque question, cite les lignes exactes du code.

En fin de travail, écris ton résumé dans :
`.claude/results/frontend-dev/2026-06-10-android-freeze-diagnostic/Resume-2026-06-10.md`

Inclus : lignes exactes des mécanismes critiques, valeurs des constantes, réponse détaillée aux 4 questions.
