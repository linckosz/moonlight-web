# Prompt pour frontend-dev

**Session ID:** 2026-06-09-auto-fullscreen-orientation

## Tache : Auto-fullscreen orientation sur mobile dans StreamView.js

Modifie le fichier `d:\Code\moonlight-web-deepseek\frontend\js\ui\StreamView.js` pour implementer l'entree/sortie automatique du fullscreen selon l'orientation de l'ecran (landscape/portrait) sur les appareils mobiles/tactiles.

### Contexte existant

- `IS_TOUCH_DEVICE` est deja defini ligne 32-33 (constante module-level)
- `toggleFullscreen()` ligne 2779 : deja implemente avec `canvas.requestFullscreen()`
- `handleFullscreenChange()` ligne 2796 : deja implemente, ajuste les dimensions du canvas
- `bindEvents()` ligne 2165 : ajoute deja `fullscreenchange` listener
- `unbindEvents()` ligne 2302 : retire deja `fullscreenchange` listener
- Le constructeur (ligne 111-349) est la section ou ajouter les nouvelles props
- `quit()` ligne 3008 et `destroy()` ligne 3114 sont les methodes de cleanup

### Comportement attendu

1. **Mobile/touch uniquement** (utiliser `IS_TOUCH_DEVICE`)
2. **Landscape** → auto-fullscreen via `canvas.requestFullscreen()`
3. **Portrait** → auto-sortie du fullscreen via `document.exitFullscreen()`
4. **Si l'utilisateur sort manuellement du fullscreen en landscape** (Escape, swipe) : ne PAS le forcer a revenir jusqu'au prochain cycle portrait→landscape
5. **Si l'utilisateur entre manuellement en fullscreen en portrait** : respecter son choix

### Modifications

#### A. Constructeur (apres la ligne ~310)

Apres `this._onFullscreenChange = () => this.handleFullscreenChange();`, ajouter :

```js
// Touch auto-fullscreen orientation state
this._autoFsBlocked = false;   // true: user manually exited fullscreen in landscape
this._autoFsExiting = false;   // true: programmatic exitFullscreen due to portrait
this._onOrientationChange = () => this._handleOrientationChange();
```

#### B. Nouvelle methode `_setupAutoFullscreen()`

A appeler dans le constructeur, apres `this.bindEvents();` (vers ligne 345).

```js
_setupAutoFullscreen() {
  if (!IS_TOUCH_DEVICE) return;

  // matchMedia is widely supported and requires no permissions
  this._orientationMql = window.matchMedia('(orientation: landscape)');
  this._orientationMql.addEventListener('change', this._onOrientationChange);

  // Auto-enter fullscreen if already in landscape at stream start
  if (!this._autoFsBlocked && this._orientationMql && this._orientationMql.matches) {
    this.canvas.requestFullscreen().catch(err => {
      console.warn('[StreamView] Initial auto-fullscreen (landscape) failed:', err.message);
    });
  }
}
```

#### C. Nouvelle methode `_handleOrientationChange()`

```js
_handleOrientationChange() {
  if (!IS_TOUCH_DEVICE || this._quitting || !this._orientationMql) return;

  const landscape = this._orientationMql.matches;

  if (landscape) {
    // Landscape: auto-enter fullscreen (unless user blocked it)
    if (!this._autoFsBlocked && !document.fullscreenElement) {
      console.log('[StreamView] Auto-fullscreen on landscape orientation');
      this.canvas.requestFullscreen().catch(err => {
        console.warn('[StreamView] Auto-fullscreen on landscape failed:', err.message);
      });
    }
  } else {
    // Portrait: auto-exit fullscreen
    if (document.fullscreenElement) {
      console.log('[StreamView] Auto-exit fullscreen on portrait orientation');
      this._autoFsExiting = true;
      document.exitFullscreen().catch(err => {
        console.warn('[StreamView] Auto-exit fullscreen on portrait failed:', err.message);
        this._autoFsExiting = false;  // Reset on failure
      });
    } else {
      // Not fullscreen — reset block so next landscape triggers auto-enter
      this._autoFsBlocked = false;
    }
  }
}
```

#### D. Modifier `handleFullscreenChange()` (dans le `else`, apres ligne 2816)

Apres la ligne `console.log('[StreamView] Fullscreen exited, canvas restored to ' + this.canvas.width + 'x' + this.canvas.height);` (ligne 2816), AJOUTER :

```js
// ── Touch auto-fullscreen state tracking ──────────────────────────────
if (IS_TOUCH_DEVICE) {
  if (this._autoFsExiting) {
    // Programmatic exit due to portrait — reset block for next landscape
    this._autoFsBlocked = false;
    this._autoFsExiting = false;
    console.log('[StreamView] Auto-fullscreen block reset (portrait cycle)');
  } else if (this._orientationMql && this._orientationMql.matches) {
    // User manually exited fullscreen while in landscape
    this._autoFsBlocked = true;
    console.log('[StreamView] User denied auto-fullscreen (manual exit in landscape)');
  }
}
```

Note : verifie les numeros de ligne exacts apres avoir lu le fichier, car ils peuvent avoir change depuis cette analyse.

#### E. Cleanup dans `quit()` (apres l'appel exitFullscreen existant, vers ligne 3012-3013)

Apres `document.exitFullscreen().catch(() => {});` et avant `this.stopRenderLoop();` :

```js
// Cleanup orientation listener
if (this._orientationMql) {
  this._orientationMql.removeEventListener('change', this._onOrientationChange);
  this._orientationMql = null;
}
```

#### F. Cleanup dans `destroy()` (vers ligne 3114)

Ajouter en debut de methode (apres `this.stopDiagnostics();`) :

```js
if (this._orientationMql) {
  this._orientationMql.removeEventListener('change', this._onOrientationChange);
  this._orientationMql = null;
}
```

### Fichier a modifier

- `d:\Code\moonlight-web-deepseek\frontend\js\ui\StreamView.js` — seul fichier

### Logs a ajouter (en anglais, suivant le pattern `[StreamView]`)

- `[StreamView] Auto-fullscreen on landscape orientation`
- `[StreamView] Auto-exit fullscreen on portrait orientation`
- `[StreamView] User denied auto-fullscreen (manual exit in landscape)`
- `[StreamView] Auto-fullscreen block reset (portrait cycle)`
- `[StreamView] Initial auto-fullscreen (landscape) failed: ...` (warn)

### Ce qui NE doit PAS etre modifie

- `toggleFullscreen()` — garder tel quel
- `bindEvents()` / `unbindEvents()` — ne pas ajouter l'ecouteur orientation ici (on utilise `matchMedia.addEventListener`)
- Les methodes de rendu, decodeur, audio, input, etc.

### Contraintes

- `_autoFsExiting` doit etre remis a `false` si `exitFullscreen()` echoue (catch)
- Ne pas dupliquer la cleanup — la mettre dans `quit()` ET `destroy()` (tous les deux peuvent etre appeles)
- Les logs doivent etre en anglais, les commentaires en anglais

---

En fin de travail, ecris ton resume dans
`.claude/results/frontend-dev/2026-06-09-auto-fullscreen-orientation/Resume-2026-06-09.md`.
Inclus uniquement tes resultats/conclusions (pas la reflexion intermediaire).
Format : tache accomplie, fichiers modifies, decisions prises, points bloquants.
