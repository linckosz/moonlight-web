---
name: Android GPU Acceleration Corrections
description: 4 correctifs pour forcer le decodeur hardware GPU Android (H.264) : prefer-hardware, drawImage direct, avc3 fallback, desynchronized canvas
metadata:
  type: project
---

## Corrections GPU Android (2026-06-09)

4 corrections chirurgicales dans `frontend/js/ui/StreamView.js` pour reduire la latence H.264 sur Android (decodage software -> hardware).

**Pourquoi:** L'analyse Android a revele 500ms-1s de latence en 720p60 H.264 car le decodeur software etait utilise par defaut.

### Correction #1 — hardwareAcceleration: "prefer-hardware"
- `applyConfig()` refactore en `_doConfigure(config, hwAccel)`
- Phase 1: `{ ...config, hardwareAcceleration: 'prefer-hardware' }`
- Phase 2: fallback software si `NotSupportedError`

### Correction #2 — drawImage(VideoFrame) direct pour H.264
- H.264: `ctx.drawImage(frame, ...)` au lieu de `createImageBitmap` + `drawImage(bitmap)`
- Evite une copie GPU->CPU->GPU intermediaire
- HEVC NV12 preserve avec son early return existant (createImageBitmap + 'copy' composite)

### Correction #3 — Fallback avc3.42E01E
- Apres epuisement des configs `avc1.*` (derivees du SPS), essai `avc3.42E01E` (in-band)
- Certains decodeurs hardware Android preferent avc3

### Correction #4 — desynchronized: true sur canvas context
- `getContext('2d', { desynchronized: true })` aux 2 endroits (initial + context loss)
- Reduit la latence de composition en evitant une copie intermediaire (vsync bypass)

**Fichier modifie:** `frontend/js/ui/StreamView.js` uniquement
**Workarounds HEVC preserves:** ghost pixels, green screen, x4 stretch, Annex B — aucun modifie
