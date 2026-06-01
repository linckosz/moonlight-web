---
name: Codec Browser Support Check
description: Detection du support navigateur H.264/HEVC/AV1 dans SettingsView avec options grisees
metadata:
  type: project
---

# Codec Browser Support Check

**Feature** : Au chargement de SettingsView, detection du support des codecs H.264, HEVC et AV1 via `VideoDecoder.isConfigSupported()`. Les codecs non supportes sont grises dans le dropdown avec un label descriptif.

## Implementation

### Fichiers modifies
- `d:\Code\moonlight-web-deepseek\frontend\js\ui\SettingsView.js`
- `d:\Code\moonlight-web-deepseek\frontend\css\style.css`

### Nouvelles methodes dans SettingsView.js

1. **`_checkCodecSupport()`** — Teste chaque codec avec des codec strings minimales (pas de bitstream). Chaque codec est teste avec 3 codec strings de fallback. Si `isConfigSupported()` n'est pas disponible, tous les codecs sont marques supportes (graceful degradation).

2. **`_getEffectiveCodec()`** — Calcule le codec effectif a afficher en priorisant : MediaTrack > codec prefere si supporte > fallback h264/hevc/av1 selon support navigateur.

### Modifications dans `render()`

- Les options codec utilisent desormais `this._codecSupport` et `this._getEffectiveCodec()`
- Label modifie pour les codecs non supportes : `"HEVC — not supported by this browser"`
- Hint affiche quand le codec prefere est override par le fallback
- Message d'erreur critique si aucun codec n'est supporte

### CSS ajoute

```css
.settings-select option:disabled {
    color: var(--text-secondary);
    opacity: 0.55;
    font-style: italic;
}
```

### Design decisions

- `_videoCodec` n'est pas modifie par le fallback — seule la selection affichee change via `_getEffectiveCodec()`. Quand l'utilisateur sauvegarde, le nouveau codec est persiste normalement.
- Les test codec strings utilisees sont celles deja definies dans `Mp4Muxer.js` (H264_FALLBACK_CODEC_STRINGS / HEVC_FALLBACK_CODEC_STRINGS).
- La detection est refaite a chaque ouverture de SettingsView (le support navigateur ne change pas dynamiquement, mais le cout est negligeable).
- Si le check echoue ou n'est pas disponible, tous les codecs sont consideres supportes (maximum compatibility).
