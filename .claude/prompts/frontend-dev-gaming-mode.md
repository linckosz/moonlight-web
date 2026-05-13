# Frontend — Gaming Mode (souris)

## Tâche

Ajouter le support du mode Gaming (on/off) dans le frontend :
- SettingsView : checkbox "Gaming Mode"
- StreamView : 6 modifications pour gérer les deux modes souris
- app.js : transmettre `gamingMode` au constructeur de `StreamView`

## Lecture préalable obligatoire

Lis d'abord ces fichiers pour comprendre le pattern existant :

1. **frontend/js/ui/SettingsView.js** — voir comment les checkboxes de setting sont faites (ex: le codec selector, le dirty tracking)
2. **frontend/js/ui/StreamView.js** — voir le constructeur, la gestion actuelle de la souris (pointer lock, mouse events), handlePointerLockChange, les méthodes existantes
3. **frontend/js/app.js** — voir comment `StreamView` est instancié, comment `result.gamingMode` serait passé

## Modifications à faire

### D. SettingsView — Ajouter checkbox "Gaming Mode"

Dans le constructeur de `SettingsView`, après le codec selector, ajouter une checkbox "Gaming Mode" :

```html
<label class="setting-row">
  <span class="setting-label">
    Gaming Mode
    <span class="setting-description">Lock mouse pointer for seamless camera control in games</span>
  </span>
  <span class="setting-control">
    <input type="checkbox" id="gaming-mode" name="gaming_mode" checked />
  </span>
</label>
```

Pattern dirty tracking :
- Initialiser `this._originalGamingMode = true`
- Dans `_populateSettings(data)` : `this._originalGamingMode = data.gaming_mode`, set checkbox checked state
- Dans le handler `onchange` de la checkbox : déclencher `this._checkDirty()`
- Dans `_checkDirty()` : comparer `this._originalGamingMode !== this._getGamingMode()` avec les autres dirty checks
- Dans `_getSettingsPayload()` : ajouter `gaming_mode: this._getGamingMode()`
- Dans `_saveSettings()` : après sauvegarde, mettre à jour `this._originalGamingMode`

Ajouter une méthode `_getGamingMode()` qui retourne `bool` depuis la checkbox.

### E. StreamView — 6 modifications

#### E1. Constructeur — ajouter `gamingMode` paramètre

```js
constructor(wsUrl, gamingMode = true) {
    // ...
    this._gamingMode = gamingMode;
    // ...
}
```

Si `gamingMode` est `false`, ne PAS appeler `this._setupGamingMouse()`. À la place, configurer le mode Normal.

#### E2. Pointer lock — uniquement en gaming mode

**Comportement actuel (Gaming Mode)** — conserver :
- Click sur le canvas → `canvas.requestPointerLock()`
- `document.addEventListener('pointerlockchange', this._handlePointerLockChange.bind(this))`

**Mode Normal (nouveau)** : ne PAS appeler `requestPointerLock`. Les événements souris sont traités normalement.

Structure proposée dans le constructeur :
```js
if (this._gamingMode) {
    this._setupGamingMouse();
} else {
    this._setupNormalMouse();
}
```

#### E3. Mouse move — Mode Normal (position absolue + dx/dy relatifs)

En mode Normal, on tracke la position absolue de la souris, et on calcule le déplacement relatif entre deux frames (comme le ferait un pointeur virtuel).

```js
_setupNormalMouse() {
    // Tracking state
    this._mouseX = 0;
    this._mouseY = 0;
    this._lastMoveTime = 0;
    
    this._canvas.addEventListener('mousemove', (e) => {
        const rect = this._canvas.getBoundingClientRect();
        const newX = e.clientX - rect.left;
        const newY = e.clientY - rect.top;
        
        // Calculate relative delta from last absolute position
        const dx = newX - this._mouseX;
        const dy = newY - this._mouseY;
        
        this._mouseX = newX;
        this._mouseY = newY;
        this._lastMoveTime = performance.now();
        
        // Send relative mouse move (same as gaming mode would)
        this._sendMouseMove(dx, dy);
    });
    
    this._setupNormalMouseLeave();
}
```

#### E4. Drag detection — curseur local visible pendant le drag, invisible sinon

En mode Normal, le curseur doit être :
- `cursor: 'none'` sur le canvas quand la souris bouge normalement (pour immersion)
- `cursor: 'default'` pendant un drag (pour que l'utilisateur voie le curseur système)

```js
_setupNormalMouse() {
    // ... (existing mousemove logic)
    
    // Show cursor during drag operations
    let dragCount = 0;
    this._canvas.addEventListener('mousedown', () => {
        dragCount++;
        this._canvas.style.cursor = 'default';
    });
    
    this._canvas.addEventListener('mouseup', () => {
        dragCount--;
        if (dragCount <= 0) {
            dragCount = 0;
            this._canvas.style.cursor = 'none';
        }
    });
    
    // Also reset cursor on mouse leave
    this._canvas.addEventListener('mouseleave', () => {
        dragCount = 0;
        this._canvas.style.cursor = 'default';
    });
}
```

**IMPORTANT** : Le canvas initial doit avoir `cursor: 'none'` en mode Normal (même pattern que gaming, mais pas via pointer lock — via CSS direct).

#### E5. Combo sortie — Shift+Ctrl+Alt+E (Windows) / Shift+Ctrl+Option+E (Mac)

**Au lieu de la touche Escape** pour quitter le streaming, utiliser la combinaison :
- Windows/Linux : `Shift` + `Ctrl` + `Alt` + `E`
- macOS : `Shift` + `Ctrl` + `Option` + `E`

Pattern :
```js
document.addEventListener('keydown', (e) => {
    if (e.shiftKey && e.ctrlKey && e.altKey && e.code === 'KeyE') {
        // Same logic as Escape would have done before
        this._quit();
        e.preventDefault();
    }
});
```

**Important** : Cette combinaison remplace TOUTE autre logique de sortie clavier (Escape n'est plus géré).

#### E6. Mouse enter/leave — reset position tracking en mode Normal

```js
_setupNormalMouseLeave() {
    this._canvas.addEventListener('mouseenter', () => {
        // Reset tracking when mouse re-enters canvas
        this._mouseX = -1;
        this._mouseY = -1;
    });
    
    this._canvas.addEventListener('mouseleave', () => {
        this._canvas.style.cursor = 'default';
    });
}
```

### F. app.js — passage de `gamingMode`

Dans le handler de `/start` qui crée `StreamView` :
```js
// Avant : new StreamView(result.wsUrl)
// Après :
new StreamView(result.wsUrl, result.gamingMode !== false);
```

## Résumé des événements/signaux

| Mode | Canvas cursor | Mouse move | Drag | Exit combo |
|---|---|---|---|---|
| Gaming | hidden (via pointer lock) | pointer lock delta | N/A (locked) | Shift+Ctrl+Alt+E |
| Normal | hidden (CSS `cursor: none`), visible during drag | absolute → relative dx/dy | cursor:default pendant drag | Shift+Ctrl+Alt+E |

## Fichiers modifiés

- `frontend/js/ui/SettingsView.js`
- `frontend/js/ui/StreamView.js`
- `frontend/js/app.js`

## Résultat attendu

Écris ton résumé dans `.claude/results/frontend-dev/{session}/Resume-2026-05-13.md`.
