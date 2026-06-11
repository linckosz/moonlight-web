# Session: 2026-06-10-android-freeze-fix — frontend-dev

## Tache : Corrections du freeze Android dans StreamView.js

### Contexte

Le stream Android WebRTC DC freeze apres ~1 seconde. J'ai identifie la cause racine dans `frontend/js/ui/StreamView.js` :

1. **Stale check (ligne 2109) droppe les keyframes aussi quand decoderConfigured=true** :
   ```javascript
   if (this.decoderConfigured || !isKeyframe) {
       this.stats.dropped++;
       return;
   }
   ```
   Quand `decoderConfigured=true`, MEME les keyframes sont droppees si leur backend timestamp est stale (arrivee tardive due au reordering SCTP). Dirty mode ne recoit jamais de keyframe → ne sort jamais → freeze permanent.

2. **Pas d'auto-exit du dirty mode** : si les keyframes ne peuvent pas sortir du stale check, le dirty mode est permanent.

### Fichier a modifier

**`d:\Code\moonlight-web-deepseek\frontend\js\ui\StreamView.js`**

### Changement 1 : Ajouter _dirtyEnterTime dans le constructeur

Vers la fin du constructeur (apres la ligne 320 `this._pendingKeyframeTimestamps = new Set();`), ajoute :

```javascript
// Timestamp (performance.now()) when dirty mode was entered.
// Used for auto-exit timeout to prevent permanent freeze.
this._dirtyEnterTime = 0;
```

### Changement 2 : Modifier _enterDirtyMode (ligne 2060-2067)

Ajoute la ligne `this._dirtyEnterTime = performance.now();` :

```javascript
_enterDirtyMode(reason) {
    if (this._decoderDirty) return;
    this._decoderDirty = true;
    this._decoderDirtyKeyframes = 0;
    this._decoderDirtyLogged = true;
    this._dirtyEnterTime = performance.now();  // <-- AJOUT
    console.warn('[Dirty] entering dirty mode (' + reason + ')');
    this._requestIdr();
}
```

### Changement 3 : Modifier le stale check dans handleVideoFrame (ligne 2104-2117)

Modifie le bloc stale pour laisser passer les keyframes en dirty mode :

```javascript
if (backendTs !== undefined && backendTs > 0) {
    if (this._maxBackendTs === undefined || backendTs > this._maxBackendTs) {
        this._maxBackendTs = backendTs;
    } else if (backendTs < this._maxBackendTs) {
        // Out-of-order: frame is older than the newest seen.
        // In dirty mode: keyframes MUST pass through to recover decoder
        // state, even if stale (SCTP reordering causes late arrival).
        // Without this exemption, dirty mode never receives keyframes
        // to clean the decoder → permanent freeze.
        if (this._decoderDirty && isKeyframe) {
            // Let keyframe through in dirty mode to enable recovery
        } else if (this.decoderConfigured || !isKeyframe) {
            this.stats.dropped++;
            return;
        }
        // Keyframe before decoder config: let through to bootstrap decoder
    }
    // Equal timestamps: pass through (same-ms frames)
}
```

### Changement 4 : Ajouter auto-exit dirty mode dans _processVideoFrame (ligne 2261-2264)

Modifie le bloc dirty mode pour ajouter un auto-exit apres 5 secondes :

```javascript
// Correction E: dirty mode — skip delta frames when decoder state uncertain
if (this._decoderDirty && !isKeyframe) {
    // Auto-exit dirty mode after 5s to prevent permanent freeze.
    // SCTP unordered delivery can create false frameId gaps that enter
    // dirty mode. If no keyframe arrives to cleanly exit dirty mode
    // within 5s, the stream is likely healthy — exit and let frames
    // flow again. The decoder error handler re-enters dirty mode if
    // there is real corruption.
    if (this._dirtyEnterTime > 0 &&
        (performance.now() - this._dirtyEnterTime) > 5000) {
        this._decoderDirty = false;
        this._decoderDirtyKeyframes = 0;
        this._decoderDirtyLogged = false;
        this._dirtyEnterTime = 0;
        console.log('[Dirty] auto-exit after 5s timeout');
    } else {
        this.stats.dropped++;
        return;
    }
}
```

### Verification

Apres les modifications, verifie que :
1. `_dirtyEnterTime` est bien initialise a 0
2. `_enterDirtyMode` enregistre le timestamp
3. Le stale check a la ligne 2109-2117 ne droppe plus les keyframes en dirty mode
4. L'auto-exit a 5s est place AVANT le `this.stats.dropped++`

### Instructions

1. Lis les lignes concernees dans StreamView.js (autour de 2060, 2104, 2261)
2. Fais les 4 modifications avec Edit
3. Verifie que le code compile (syntaxe JS)
4. Ecris ton resume dans `.claude/results/frontend-dev/2026-06-10-android-freeze-fix/Resume-2026-06-10.md`
