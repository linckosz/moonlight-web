# Prompt: Canvas Fullscreen pour StreamView

## Objectif
Remplacer l'actuel `document.documentElement.requestFullscreen()` par `this.canvas.requestFullscreen()` dans StreamView.js, et ajouter la gestion des dimensions HiDPI.

## Contexte

Le fullscreen existe déjà dans StreamView.js avec `toggleFullscreen()` (ligne 2757) :
```js
toggleFullscreen() {
    if (document.fullscreenElement) {
        document.exitFullscreen().catch(err => { ... });
    } else {
        document.documentElement.requestFullscreen().catch(err => { ... });
    }
}
```

Raccourci existant : `Ctrl+Alt+Shift+X` (Win) / `Cmd+Option+Ctrl+X` (Mac) — ligne 2466.

Le canvas est créé ligne 560 : `this.canvas = document.getElementById('stream-canvas')`
Le render loop redimensionne automatiquement le canvas à la résolution vidéo (lignes 1384-1389) :
```js
if (frame.displayWidth && frame.displayHeight &&
    (this.canvas.width !== frame.displayWidth ||
     this.canvas.height !== frame.displayHeight)) {
    this.canvas.width = frame.displayWidth;
    this.canvas.height = frame.displayHeight;
}
```

Fichier CSS : `frontend/css/stream.css` (lire avant de modifier)

## Changements à faire

### 1. StreamView.js — Constructeur

Ajouter ces variables d'instance :
- `this._fullscreenMode = false;` — flag pour savoir si on est en fullscreen
- `this._videoWidth = 0;` — résolution vidéo native (set à la première frame)
- `this._videoHeight = 0;`
- `this._onFullscreenChange = (e) => this.handleFullscreenChange(e);` — handler lié

Dans `_logPlatformInfo()` ou `_logPlatformInfo` sera appelé avant bindEvents, donc on peut initialiser là.

### 2. StreamView.js — toggleFullscreen() (ligne 2757)

Remplacer le contenu :
```js
toggleFullscreen() {
    if (document.fullscreenElement) {
        document.exitFullscreen().catch(err => {
            console.warn('[StreamView] exitFullscreen failed:', err.message);
        });
    } else {
        this.canvas.requestFullscreen().catch(err => {
            console.warn('[StreamView] canvas.requestFullscreen failed:', err.message);
        });
    }
}
```

### 3. StreamView.js — handleFullscreenChange() (nouvelle méthode)

Ajouter une méthode gérant le redimensionnement HiDPI :

```js
handleFullscreenChange() {
    if (document.fullscreenElement === this.canvas) {
        // Entering fullscreen on canvas
        this._fullscreenMode = true;
        
        // HiDPI: set canvas buffer to native screen resolution
        // so the rendered image is pixel-perfect (no blur on Retina displays)
        this.canvas.width = Math.round(window.innerWidth * (window.devicePixelRatio || 1));
        this.canvas.height = Math.round(window.innerHeight * (window.devicePixelRatio || 1));
        
        console.log('[StreamView] Fullscreen entered, canvas=' +
            this.canvas.width + 'x' + this.canvas.height +
            ' devicePixelRatio=' + (window.devicePixelRatio || 1) +
            ' screen=' + window.innerWidth + 'x' + window.innerHeight);
    } else {
        // Exiting fullscreen (Escape key or F11)
        this._fullscreenMode = false;
        
        // Restore canvas buffer to video resolution (if known)
        if (this._videoWidth > 0 && this._videoHeight > 0) {
            this.canvas.width = this._videoWidth;
            this.canvas.height = this._videoHeight;
        }
        
        console.log('[StreamView] Fullscreen exited, canvas restored to ' +
            this.canvas.width + 'x' + this.canvas.height);
    }
}
```

### 4. StreamView.js — Render loop (lignes 1384-1389)

Modifier le redimensionnement automatique du canvas pour ne PAS le faire quand on est en fullscreen :

```js
// Resize canvas to match frame dimensions if needed
// Skip in fullscreen mode — canvas buffer is set to native screen resolution
if (!this._fullscreenMode) {
    if (frame.displayWidth && frame.displayHeight &&
        (this.canvas.width !== frame.displayWidth ||
         this.canvas.height !== frame.displayHeight)) {
        this.canvas.width = frame.displayWidth;
        this.canvas.height = frame.displayHeight;
    }
}

// Store video resolution for restore on fullscreen exit
if (frame.displayWidth && frame.displayHeight) {
    if (this._videoWidth === 0 && this._videoHeight === 0) {
        this._videoWidth = frame.displayWidth;
        this._videoHeight = frame.displayHeight;
    }
}
```

### 5. StreamView.js — bindEvents() (ligne 2145)

Ajouter l'écouteur `fullscreenchange` :
```js
document.addEventListener('fullscreenchange', this._onFullscreenChange);
```

### 6. StreamView.js — unbindEvents() (ligne 2282)

Ajouter le remove :
```js
document.removeEventListener('fullscreenchange', this._onFullscreenChange);
```

### 7. StreamView.js — quit() (ligne 2954)

Si le fullscreen est actif, en sortir avant de quitter :
```js
if (document.fullscreenElement) {
    document.exitFullscreen().catch(() => {});
}
```
À ajouter vers le début de `quit()`, après la ligne `this._quitting = true;`.

### 8. stream.css

Ajouter à la fin du fichier (après le bloc @keyframes startup-pulse) :

```css
/* ── Canvas fullscreen ───────────────────────────────────────────── */
.stream-canvas:fullscreen {
    width: 100vw;
    height: 100vh;
    object-fit: contain;
    background: #000;
}
```

Optionnel : pour Safari iOS qui ne supporte pas `:fullscreen`, ajouter aussi un fallback avec une classe CSS, mais on peut commencer sans.

## Points de vigilance

1. Le raccourci X existe déjà et appelle `toggleFullscreen()` — pas besoin de modifier handleKeyDown.
2. Le slide des raccourcis clavier mentionne déjà "Fullscreen" (ligne 2860) — inchangé.
3. `Escape` est intercepté ligne 2438-2442 et forwardé comme touche normale — NE PAS CHANGER cette logique. Le fullscreen API exit sur Escape est normal, mais on veut qu'Escape soit forwardé au jeu. C'est OK car `document.exitFullscreen()` est appelé automatiquement par le navigateur sur Escape, et notre `fullscreenchange` handler restaure la taille. Le keydown Escape continue d'être forwardé au host — c'est le comportement désiré (l'utilisateur veut qu'Escape aille au jeu, pas qu'il quitte le fullscreen).
4. **Attention** : `canvas:fullscreen` en CSS ne fonctionne que si le canvas est l'élément fullscreen. Vérifier que la syntaxe est bien supportée. Alternative : utiliser `.stream-overlay:fullscreen` sur le parent.
   En fait, le canvas est un enfant de `.stream-canvas-area` qui est dans `.stream-overlay`. Le CSS `:fullscreen` s'applique à l'élément fullscreen lui-même. Donc `.stream-canvas:fullscreen` ciblera le canvas quand il est en fullscreen — c'est correct.

## Tests

1. Cliquer sur le raccourci X → le canvas doit passer en plein écran (sans le header ni les overlays)
2. La résolution du canvas doit être à la résolution native de l'écran (vérifier via les stats overlay)
3. Appuyer sur Escape → le fullscreen se ferme, le canvas reprend la résolution vidéo
4. Le streaming continue normalement après sortie de fullscreen
5. Pas de régression sur le rendu vidéo normal (sans fullscreen)

## Livrable

Écris ton résumé dans `.claude/results/frontend-dev/2026-06-09-fullscreen-canvas/Resume-YYYY-MM-DD.md`.
