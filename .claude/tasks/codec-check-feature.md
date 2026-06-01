# Feature: Browser Codec Support Detection in SettingsView

## Contexte

Lors du démarrage de SettingsView, on veut détecter si le navigateur supporte H.264, HEVC et AV1 via `VideoDecoder.isConfigSupported()`. Les codecs non supportés sont grises (desactives) dans le dropdown de selection, avec un label indiquant l'indisponibilite.

## Fichier a modifier

- `d:\Code\moonlight-web-deepseek\frontend\js\ui\SettingsView.js`
- `d:\Code\moonlight-web-deepseek\frontend\css\style.css`

## Implementation

### 1. SettingsView.js — Ajouts

#### a) Dans le constructeur, apres les autres initialisations :

```javascript
/** Per-codec browser support map: { h264:bool, hevc:bool, av1:bool } or null */
this._codecSupport = null;
```

#### b) Nouvelle methode `_checkCodecSupport()` — a ajouter avant `destroy()` (ou apres `_applySettings`) :

```javascript
/**
 * Test browser codec support via VideoDecoder.isConfigSupported() for H.264,
 * HEVC and AV1. Uses minimal codec strings (no bitstream description needed).
 *
 * Each codec is tested with a list of fallback codec strings. The codec is
 * marked supported if ANY string in its list returns supported=true.
 *
 * If VideoDecoder.isConfigSupported is not available (old browser), all
 * codecs are assumed supported (graceful fallback).
 *
 * @returns {Promise<{h264:boolean, hevc:boolean, av1:boolean}>}
 */
async _checkCodecSupport() {
    const support = { h264: false, hevc: false, av1: false };

    if (typeof VideoDecoder?.isConfigSupported !== 'function') {
        console.warn('[Settings] VideoDecoder.isConfigSupported not available — ' +
            'assuming all codecs supported');
        support.h264 = true;
        support.hevc = true;
        support.av1 = true;
        return support;
    }

    // Test a list of codec strings, return true if ANY is supported.
    // We use multiple strings because some browsers support only specific
    // profiles/levels for a given codec.
    const testCodec = async (codecs) => {
        for (const codec of codecs) {
            try {
                const result = await VideoDecoder.isConfigSupported({ codec });
                if (result?.supported) return true;
            } catch (_) {
                // Individual codec string rejected — try next fallback
            }
        }
        return false;
    };

    support.h264 = await testCodec([
        'avc1.64002A',  // High 4.2
        'avc1.42001E',  // Baseline 3.0
        'avc1.64001E',  // High 3.0
    ]);
    support.hevc = await testCodec([
        'hvc1.1.6.L153.B0',  // Main, High tier, Level 5.1
        'hvc1.1.6.L120.B0',  // Main, High tier, Level 4.0
        'hvc1.1.2.L153.B0',  // Main, Main tier, Level 5.1
    ]);
    support.av1  = await testCodec([
        'av01.0.08M.08',    // Main, 1080p, 8-bit
        'av01.0.04M.08',    // Main, 720p, 8-bit
        'av01.0.01M.08',    // Main, 480p, 8-bit
    ]);

    console.log('[Settings] Browser codec support:', JSON.stringify(support));

    // If H.264 is not supported, this is a critical error — log prominently
    if (!support.h264) {
        console.error('[Settings] CRITICAL: H.264 not supported by this browser');
    }

    return support;
}
```

#### c) Modifier `start()` pour appeler `_checkCodecSupport()` :

```javascript
async start() {
    await this._loadState();
    this._codecSupport = await this._checkCodecSupport();
    this.render();
    this.bindEvents();
}
```

#### d) Nouvelle methode `_getEffectiveCodec()` — codec effectif tenant compte du support navigateur :

```javascript
/**
 * Return the effective codec to display in the dropdown, considering:
 * 1. MediaTrack transport forces H.264
 * 2. Browser codec support detection (preferred codec may be unsupported)
 * 3. Fallback chain: h264 > hevc > av1
 *
 * Does NOT modify this._videoCodec (user preference remains in storage).
 */
_getEffectiveCodec() {
    // MediaTrack transport only supports H.264
    if (this._mediaTrackOnlyH264) return 'h264';

    // If codec support not yet checked, use stored preference
    if (!this._codecSupport) return this._videoCodec;

    // If current preference is supported, keep it
    if (this._codecSupport[this._videoCodec]) return this._videoCodec;

    // Fallback to first supported codec in priority order
    if (this._codecSupport.h264) return 'h264';
    if (this._codecSupport.hevc) return 'hevc';
    if (this._codecSupport.av1) return 'av1';

    // No codec supported — keep current preference (unlikely)
    return this._videoCodec;
}
```

#### e) Modifier `render()` — la section de construction des options codec (lignes ~147-157) :

Remplacer le bloc actuel (depuis `const codecs = [` jusqu'a `codecOptions.map(...).join('')`) par :

```javascript
// Codec options (explicit, no "Auto")
const codecs = [
    { value: 'h264', label: 'H.264 (Wide compatibility)' },
    { value: 'hevc', label: 'HEVC (Efficient compression, recommended)' },
    { value: 'av1',  label: 'AV1 (Best compression for modern GPUs)' }
];
const effectiveCodec = this._getEffectiveCodec();
const codecOptions = codecs.map(c => {
    const browserDisabled = this._codecSupport && !this._codecSupport[c.value];
    const mediaTrackDisabled = this._mediaTrackOnlyH264 &&
        (c.value === 'hevc' || c.value === 'av1');
    const disabled = browserDisabled || mediaTrackDisabled;
    const selected = c.value === effectiveCodec ? ' selected' : '';

    let label = c.label;
    if (browserDisabled) {
        // Browser does not support this codec at all
        label = `${c.value.toUpperCase()} — not supported by this browser`;
    } else if (mediaTrackDisabled) {
        // Transport limitation (MediaTrack mode)
        label = `${c.value.toUpperCase()} (unavailable)`;
    }

    return `<option value="${c.value}"${selected}${disabled ? ' disabled' : ''}>${this.esc(label)}</option>`;
}).join('');

// ── Critical error if no codec is supported ──
const noCodecSupported = this._codecSupport &&
    !this._codecSupport.h264 && !this._codecSupport.hevc && !this._codecSupport.av1;
let warningHtml = '';
if (noCodecSupported) {
    warningHtml = `<div class="settings-status settings-status-pending" style="margin-bottom:20px">
        <strong>No codec supported.</strong> This browser does not support H.264, HEVC, or AV1 decoding.
        Streaming is not possible.
    </div>`;
} else if (this._codecSupport && !this._codecSupport.hevc && !this._codecSupport.av1) {
    warningHtml = `<div class="settings-hint" style="margin-bottom:16px">
        This browser only supports <strong>H.264</strong> decoding.
        HEVC and AV1 are not available on this platform.
    </div>`;
}
```

Puis injecter `warningHtml` dans le template HTML, juste avant la fermeture de `settings-section` (autour de la ligne 233). Plus precisement, ajouter `${warningHtml}` avant `</div> <!-- .settings-section -->` de Video.

### f) Si le codec effectif change par rapport a `_videoCodec`, afficher un petit hint :

Dans la section Video, apres le `<select>` du codec, on peut ajouter un hint si le codec prefere n'est pas disponible :

```javascript
// Dans render(), apres le select codec:
const codecChanged = this._codecSupport &&
    !this._codecSupport[this._videoCodec] &&
    this._codecSupport[effectiveCodec];
if (codecChanged) {
    hintHtml = `<div class="settings-note">
        ${this._videoCodec.toUpperCase()} was selected but is not supported by this browser.
        Falling back to ${effectiveCodec.toUpperCase()}.
    </div>`;
}
```

### 2. style.css — Ajout du style pour les options disabled

Apres le bloc `.settings-select option` actuel (ligne ~842), ajouter :

```css
.settings-select option:disabled {
    color: var(--text-secondary);
    opacity: 0.55;
    font-style: italic;
}
```

Ce style rend les options non supportees visuellement distinctes (grisees + italiques).

## Points d'attention

1. **`_videoCodec` n'est pas modifie** — seule l'affichage change via `_getEffectiveCodec()`. Quand l'utilisateur change la selection, l'auto-save persiste la nouvelle valeur.
2. **Si `_codecSupport` est null** (pas encore charge, ou erreur), tout reste disponible — comportement existant.
3. **H.264 non supporte** est un cas critique : tous les codecs sont desactives et un message d'erreur s'affiche.
4. **Le check est fait a chaque ouverture de SettingsView** — pas de cache entre les sessions (le support navigateur ne change pas dynamiquement).
5. **Les labels des options** changent dynamiquement : "HEVC — not supported by this browser" au lieu de "HEVC (Efficient compression, recommended)".
6. **L'ordre de priorite des fallbacks** : si le codec prefere n'est pas supporte, le dropdown affiche le premier codec supporte dans l'ordre h264 > hevc > av1.

## Apres l'implementation

Ecris ton resume dans `.claude/results/frontend-dev/{session}/Resume-YYYY-MM-DD.md` avec les fichiers modifies et les decisions prises.

Session ID: 2026-06-01-codec-check-feature
