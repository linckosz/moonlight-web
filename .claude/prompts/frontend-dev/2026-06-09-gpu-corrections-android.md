# Prompt pour frontend-dev

## Session
ID: `2026-06-09-gpu-corrections-android`

## Contexte

Tu travailles sur le projet moonlight-web-deepseek, un client de streaming GameStream compatible Sunshine. Le backend C++/Qt proxy le stream vidéo H.264 vers le navigateur via WebRTC (DataChannels pour l'ancien transport, media tracks pour le transport webrtc-media).

L'utilisateur a 500ms-1s de latence sur Android 720p60 H.264. L'analyse a révélé que le décodage se fait en software (CPU) au lieu de hardware (GPU).

Tu dois appliquer 4 corrections chirurgicales dans les fichiers frontend JavaScript.

## Fichiers concernés

1. `frontend/js/ui/StreamView.js` — corrections #1, #2, #4
2. `frontend/js/util/Mp4Muxer.js` (ou là où le codec string H.264 est construit) — correction #3

## Étapes préalables

### 1. État actuel des fichiers

Lis les fichiers suivants pour comprendre l'état actuel :

- `d:\Code\moonlight-web-deepseek\frontend\js\ui\StreamView.js`
- `d:\Code\moonlight-web-deepseek\frontend\js\util\Mp4Muxer.js`

Lis-les ENTIÈREMENT (ne t'arrête pas à un extrait). Tu as besoin de voir tout le code pour faire des modifications chirurgicales sans rien casser.

### 2. Vérifie aussi comment le codec string H.264 est construit

Cherche dans tout `frontend/js/` où le codec string H.264 (`avc1.XXXXXX`) est construit ou utilisé. Il pourrait être dans `frontend/js/util/` ou `frontend/js/ui/` ou ailleurs.

---

## Corrections à implémenter

### Correction #1 : hardwareAcceleration: "prefer-hardware" dans VideoDecoder.configure()

**Fichier** : `frontend/js/ui/StreamView.js`
**Méthode cible** : `configureDecoder()` ou l'endroit où `videoDecoder.configure()` est appelé.

**Changement** :
Ajoute `hardwareAcceleration: "prefer-hardware"` dans l'objet de configuration passé à `videoDecoder.configure()`.

Protège par un try/catch :
- Si `prefer-hardware` échoue (NotSupportedError), rattrape l'erreur et réessaie SANS le flag (fallback software).
- Loggue un avertissement en console lors du fallback.

**Code pattern attendu** :
```js
try {
    await this._videoDecoder.configure({
        codec: codecString,
        hardwareAcceleration: "prefer-hardware",
        codedWidth: width,
        codedHeight: height,
        ...
    });
    this._decoderConfigured = true;
} catch (e) {
    if (e.name === 'NotSupportedError') {
        console.warn('[GPU] prefer-hardware rejected, falling back to software:', e.message);
        // Retry without hardware acceleration flag
        await this._videoDecoder.configure({
            codec: codecString,
            codedWidth: width,
            codedHeight: height,
            ...
        });
        this._decoderConfigured = true;
    } else {
        throw e;
    }
}
```

### Correction #2 : Remplacer createImageBitmap(VideoFrame) par drawImage(VideoFrame) direct pour H.264

**Fichier** : `frontend/js/ui/StreamView.js`
**Méthode cible** : `_drawFrameWithBitmap()` ou la méthode équivalente qui fait le rendu.

**Changement** :
Pour H.264 uniquement : utiliser `ctx.drawImage(videoFrame, ...)` directement au lieu de `createImageBitmap` + `drawImage(bitmap)`.

Pour HEVC : conserver le chemin actuel (createImageBitmap) intacts. Les fixes HEVC existants (ghost pixels, green screen, x4 stretch) ne doivent PAS être impactés.

**Logique** :
```js
// StreamView.js — dans la méthode de rendu
if (this._codecProfile === 'h264' || !this._codecProfile?.startsWith('hev')) {
    // H.264: direct drawImage, plus performant
    ctx.drawImage(videoFrame, 0, 0, canvas.width, canvas.height);
    videoFrame.close();
} else {
    // HEVC: keep existing path with createImageBitmap workarounds
    // [existing code unchanged]
}
```

Note : `drawImage(VideoFrame)` est natif dans la spec WebCodecs — pas de création d'ImageBitmap intermédiaire, donc moins de copies GPU/CPU.

### Correction #3 : Fallback avc3.42E01E dans le codec string

**Fichier** : là où le codec string H.264 est construit (probablement `Mp4Muxer.js` ou `StreamView.js`).

**Changement** :
Quand le codec string dynamique `avc1.XXXXXX` (dérivé du SPS) est rejeté par `isConfigSupported()`, essayer `avc3.42E01E` (High Profile 4.1, in-band SPS/PPS).

`avc3` est le codec string avec SPS/PPS in-band (transmis dans chaque IDR/IDR slice), contrairement à `avc1` où les SPS/PPS sont dans le conteneur. Certains décodeurs hardware Android préfèrent `avc3`.

**Logique** :
```js
// Là où le codec string est testé avec isConfigSupported
if (configSupported === false && h264CodecString.startsWith('avc1.')) {
    // Fallback to avc3 with common High 4.1 profile
    const fallbackCodec = 'avc3.42E01E';
    console.warn(`[GPU] ${h264CodecString} rejected, trying fallback ${fallbackCodec}`);
    // Retry isConfigSupported with avc3.42E01E
    // If accepted, use it for VideoDecoder.configure
}
```

### Correction #4 : desynchronized: true sur le contexte canvas

**Fichier** : `frontend/js/ui/StreamView.js`
**Endroit** : là où `canvas.getContext('2d')` est appelé.

**Changement** :
Ajouter `{ desynchronized: true }` au contexte canvas 2D.

`desynchronized: true` réduit la latence d'affichage en mode défilement vertical (vsync) sur mobile en évitant une copie intermédiaire.

**Important** : `desynchronized` est incompatible avec `willReadFrequently: true`. Vérifie qu'il n'y a PAS de `willReadFrequently: true` dans le même contexte. Si c'est le cas, supprime-le ou ajuste.

**Code** :
```js
const ctx = canvas.getContext('2d', { desynchronized: true });
```

---

## Règles importantes

1. **Modifications chirurgicales** : ne change que ce qui est nécessaire. Pas de refactoring.
2. **Préserve les workarounds HEVC** : ne touche PAS au code HEVC. Les longs chemins de compatibilité HEVC (ghost pixels, green screen, etc.) doivent rester intacts.
3. **Fallbacks** : chaque correction doit avoir un fallback en cas d'échec (sauf pour `desynchronized` qui est optionnel).
4. **Console logs** : ajoute des logs `[GPU]` pour chaque décision prise (hardware vs software, avc1 vs avc3, desynchronized enabled).
5. **Lis les fichiers ENTIER** avant de modifier. Tu dois voir le contexte complet pour ne rien casser.

## Résultat attendu

Après les 4 corrections, le pipeline vidéo Android (H.264) devrait :
- Utiliser le décodeur hardware GPU si disponible
- Éviter l'étape `createImageBitmap` (réduit les copies mémoire GPU→CPU)
- Proposer `avc3.42E01E` comme fallback si `avc1` est rejeté
- Avoir un canvas en mode `desynchronized` pour réduire la latence d'affichage

## Fin de travail

En fin de travail, écris ton résumé dans :
`.claude/results/frontend-dev/2026-06-09-gpu-corrections-android/Resume-2026-06-09.md`

Inclus dans ton résumé :
- Quels fichiers ont été lus
- Quelles modifications exactes ont été apportées (numéro de correction, ligne, changement)
- Les décisions prises (notamment pour préserver les workarounds HEVC)
- Tout point bloquant ou question
