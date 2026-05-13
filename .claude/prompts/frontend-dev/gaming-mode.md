# Frontend — Gaming Mode (souris)

## Tache

Ajouter le support du mode Gaming (on/off) dans le frontend :
- SettingsView : checkbox "Gaming Mode"
- StreamView : 6 modifications pour gerer les deux modes souris
- app.js : transmettre `gamingMode` au constructeur de `StreamView`

## Lecture prealable obligatoire

Lis d'abord ces fichiers pour comprendre le pattern existant :

1. **frontend/js/ui/SettingsView.js** — voir comment les checkboxes de setting sont faites (ex: le codec selector, le dirty tracking)
2. **frontend/js/ui/StreamView.js** — voir le constructeur, la gestion actuelle de la souris (pointer lock, mouse events), handlePointerLockChange, les methodes existantes
3. **frontend/js/app.js** — voir comment `StreamView` est instancie, comment `result.gamingMode` serait passe

## Modifications a faire

### D. SettingsView — Ajouter checkbox "Gaming Mode"

Dans le constructeur de `SettingsView`, apres le codec selector, ajouter une checkbox "Gaming Mode" avec dirty tracking.

Regle du dirty tracking :
- Initialiser `this._originalGamingMode = true`
- Dans `_populateSettings(data)` : `this._originalGamingMode = data.gaming_mode`, set checkbox checked state
- Dans le handler `onchange` de la checkbox : declencher `this._checkDirty()`
- Dans `_checkDirty()` : comparer `this._originalGamingMode !== this._getGamingMode()` avec les autres dirty checks
- Dans `_getSettingsPayload()` : ajouter `gaming_mode: this._getGamingMode()`
- Dans `_saveSettings()` : apres sauvegarde, mettre a jour `this._originalGamingMode`

Ajouter une methode `_getGamingMode()` qui retourne `bool` depuis la checkbox.

### E. StreamView — 6 modifications

**E1. Constructeur — ajouter `gamingMode` parametre**
```js
constructor(wsUrl, gamingMode = true) {
    this._gamingMode = gamingMode;
    // ...
}
```
Si `gamingMode` est `true`, initialiser le mode Gaming (pointer lock, comportement actuel).
Si `gamingMode` est `false`, initialiser le mode Normal.

**E2. Pointer lock — uniquement en gaming mode**
- Gaming mode : conserver `requestPointerLock`, `pointerlockchange` listener (comportement actuel)
- Normal mode : PAS de pointer lock. Les evenements souris sont traites normalement via `mousemove` sur le canvas.

**E3. Mode Normal — mouse move avec tracking position absolue + dx/dy relatifs**
```js
_setupNormalMouse() {
    this._mouseX = 0;
    this._mouseY = 0;
    this._lastMoveTime = 0;
    
    this._canvas.addEventListener('mousemove', (e) => {
        const rect = this._canvas.getBoundingClientRect();
        const newX = e.clientX - rect.left;
        const newY = e.clientY - rect.top;
        
        const dx = newX - this._mouseX;
        const dy = newY - this._mouseY;
        
        this._mouseX = newX;
        this._mouseY = newY;
        
        this._sendMouseMove(dx, dy);
    });
}
```

**E4. Mode Normal — curseur visible pendant le drag, invisible sinon**
```js
this._canvas.style.cursor = 'none';

// Show cursor during drag
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
this._canvas.addEventListener('mouseleave', () => {
    dragCount = 0;
    this._canvas.style.cursor = 'default';
});
```

**E5. Combo sortie universelle — Shift+Ctrl+Alt+E**
Remplacer la gestion de la touche Escape par :
```js
document.addEventListener('keydown', (e) => {
    if (e.shiftKey && e.ctrlKey && e.altKey && e.code === 'KeyE') {
        this._quit();
        e.preventDefault();
    }
});
```
Valable pour les DEUX modes (Gaming ET Normal). Plus de sortie par Escape.

**E6. Mode Normal — reset tracking sur mouse enter/leave**
```js
this._canvas.addEventListener('mouseenter', () => {
    this._mouseX = -1;
    this._mouseY = -1;
});
this._canvas.addEventListener('mouseleave', () => {
    this._canvas.style.cursor = 'default';
});
```

### F. app.js — passage de `gamingMode`

Dans le handler de `/start` qui cree `StreamView` :
```js
// Avant : new StreamView(result.wsUrl)
// Apres :
new StreamView(result.wsUrl, result.gamingMode !== false);
```

## Resume des evenements/signaux

| Mode | Canvas cursor | Mouse move | Drag | Exit combo |
|---|---|---|---|---|
| Gaming | hidden (via pointer lock) | pointer lock delta | N/A (locked) | Shift+Ctrl+Alt+E |
| Normal | hidden (CSS `cursor: none`), visible during drag | absolute to relative dx/dy | cursor:default pendant drag | Shift+Ctrl+Alt+E |

## Fichiers modifies

- `frontend/js/ui/SettingsView.js`
- `frontend/js/ui/StreamView.js`
- `frontend/js/app.js`

## Resultat attendu

Ecris ton resume dans `.claude/results/frontend-dev/{session}/Resume-2026-05-13.md`.
