# Session: 2026-06-10-android-freeze-diagnostic

## Phase 1a — Diagnostic : Lecture des fichiers sources

Je suis l'Engineering Manager. Tu dois analyser les fichiers suivants pour diagnostiquer un freeze Android du stream WebRTC DC (HEVC et H264) au bout d'une seconde.

### Contexte

Les corrections Android "A-E" ont introduit une régression : le stream freeze après ~1 seconde. La séquence observée :
1. 30 frames reçues/décodées, 12 rendues
2. `[Dirty] entering dirty mode (frameId gap 7 (max=32))`
3. `dropped=98` pour `received=30` — ratio aberrant
4. `received` gelé à 30, `dropped` grimpe à 202
5. Des frames avec frameId #163, #463, #777 arrivent (delta frames)
6. Le dirty mode les droppe toutes → freeze permanent

### Fichier 1 : `frontend/js/ui/StreamView.js`

Lis le fichier en entier. Porte une attention particulière à :

1. **`_enterDirtyMode()`** (ou équivalent) — comment la détection de gap fonctionne
2. **`_decoderDirtyKeyframes`** et **`DIRTY_CLEAN_THRESHOLD`** — comment le dirty mode sort
3. **`handleVideoFrame()`** — comment les frames sont traitées
4. **`_validateNalBuffer()`** — validation des NAL units
5. La logique de frameId tracking : comment `_maxFrameId` est mis à jour
6. Le seuil de gap qui déclenche le dirty mode

### Fichier 2 : `frontend/js/api/WebRtcDataChannel.js`

Lis le fichier en entier. Porte une attention particulière à :

1. **`_assembleFrame()`** — comment les chunks SCTP sont assemblés
2. **Zero-fill (correction C)** — comment les frames partielles sont complétées
3. **`_cleanupStaleFrames()`** — nettoyage des frames en attente
4. La gestion des frameId et leur mise à jour

### Tâche

Pour chaque fichier, rapporte :
- Les lignes exactes des fonctions critiques
- Les constantes (seuil de gap, DIRTY_CLEAN_THRESHOLD, etc.)
- Les mécanismes de dirty mode (entrée, sortie, comportement pendant)
- Les interactions possibles avec kMaxPayloadSize=14000

### Format de réponse

```
## StreamView.js

### _enterDirtyMode (ligne X-Y)
[code pertinent]

### DIRTY_CLEAN_THRESHOLD (ligne X)
valeur = ...

### Réponse à la question 1: Est-ce que le dirty mode sort correctement ?
[analyse]

### Réponse à la question 2: Est-ce que le seuil de gap est trop bas ?
[analyse]

...

## WebRtcDataChannel.js

### _assembleFrame (ligne X-Y)
[code pertinent]

### Réponse à la question 3: Zero-fill produit-il des frames que le dirty mode droppe ?
[analyse]

### Réponse à la question 4: _maxFrameId correctement mis à jour ?
[analyse]
```
