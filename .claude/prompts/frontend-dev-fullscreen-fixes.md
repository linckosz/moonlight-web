# Fullscreen Fixes — Instructions pour frontend-dev

Fichiers à modifier :
- `d:\Code\moonlight-web-deepseek\frontend\js\ui\StreamView.js`
- `d:\Code\moonlight-web-deepseek\frontend\css\stream.css`

Session : `2026-06-09-fullscreen-fixes`

## Fix 1 — iOS Safari: pas de fullscreen auto en portrait

**Problème** : Dans `_initMobileFullscreen()` (ligne 2701), `this._requestMobileFullscreen()` est appelé inconditionnellement au démarrage du stream. Sur iOS Safari en portrait, ça passe immédiatement en fullscreen natif du `<video>`, ce qui est indésirable.

**Correctif** : Ligne 2701, avant d'appeler `this._requestMobileFullscreen()`, vérifier l'orientation actuelle avec `window.matchMedia('(orientation: landscape)').matches`. Si portrait → ne rien faire. Si landscape → appeler `_requestMobileFullscreen()`.

```javascript
// Ne garder que l'appel conditionnel
if (window.matchMedia('(orientation: landscape)').matches) {
    this._requestMobileFullscreen();
}
// Remplacer l'appel inconditionnel actuel
```

## Fix 2 — Bouton fullscreen dans la header bar (partout, desktop aussi)

**Problème** : Actuellement le bouton fullscreen mobile (`#stream-mobile-fs-btn`) est en bas au centre, visible seulement sur mobile/tablet.

**Correctif demandé** :

### 2a. Dans la méthode `render()` (StreamView.js)
- Remplacer la création du bouton mobile actuel (lignes 658-672, bloc `if (IS_MOBILE_OR_TABLET)`)
- Créer un nouveau bouton dans le `.stream-header` (entre le bouton Stop et le bord, centré dans le header), toujours présent (pas de condition mobile)
- Le bouton doit avoir l'id `btn-stream-fs`
- Texte/icône : utiliser `⛶` (icône fullscreen) comme icône, pas de texte
- Classe CSS : `btn-stream-fs` (small, subtle)
- Caché par défaut si déjà en fullscreen au moment de la création

### 2b. Nouvelle méthode `_initFullscreenButton()`
Extraire la logique de création du bouton fullscreen dans une méthode dédiée.

### 2c. Modifier `_requestMobileFullscreen()` existante
Renommer ou adapter : le bouton doit fonctionner pour **les deux modes** :
- **Mode `<video>`** (webrtc-media) : `webkitEnterFullscreen()` puis fallback `requestFullscreen()`
- **Mode `<canvas>`** (DataChannel / WebCodecs) : `document.documentElement.requestFullscreen()`

### 2d. Nouveau comportement de visibilité
Le bouton est visible quand :
- Le stream est actif (toujours, desktop comme mobile)
- On n'est PAS en fullscreen (`document.fullscreenElement === null`)
- On cache quand le fullscreen est actif

### 2e. Nettoyer `_exitMobileFullscreen()`
La méthode `_exitMobileFullscreen()` existante (ligne 2774) est déjà correcte pour les deux modes.

### 2f. CSS dans stream.css
Remplacer le style `.stream-mobile-fs-btn` (lignes 369-401) par le nouveau style `.btn-stream-fs` :
- Petit, discret
- Positionné dans le header (pas absolute)
- Taille : ~28x28px, padding réduit, bordure subtile
- Icône centrée

## Fix 3 — Canvas CSS fallback fullscreen (iOS)

**Problème** : Sur iOS Safari et Chrome avec `<canvas>` (mode DataChannel), l'API Fullscreen ne fonctionne pas. `requestFullscreen()` échoue silencieusement.

**Correctif** : Quand `requestFullscreen()` échoue (catch), activer un fallback CSS "faux fullscreen".

### 3a. Nouvel état `_cssFullscreen` (booléen, initial false)
Dans le constructeur, ajouter `this._cssFullscreen = false;`

### 3b. Modifier `_requestMobileFullscreen()` (ou une nouvelle méthode `_requestFullscreen()`)
Ajouter la logique suivante :
```javascript
_requestFullscreen() {
    // Si déjà en CSS fake fullscreen, ne rien faire
    if (this._cssFullscreen) return;
    
    // 1. Essayer webkitEnterFullscreen() pour <video>
    // 2. Essayer document.documentElement.requestFullscreen()
    // 3. Si tout échoue → _enterCssFallbackFullscreen()
    
    // Note : garder la logique existante pour webkitEnterFullscreen
    // et requestFullscreen(), mais AJOUTER un catch qui appelle
    // _enterCssFallbackFullscreen() si ça échoue
}
```

### 3c. Nouvelle méthode `_enterCssFallbackFullscreen()`
```javascript
_enterCssFallbackFullscreen() {
    this._cssFullscreen = true;
    // 1. Cacher le header
    const header = document.querySelector('.stream-header');
    if (header) header.style.display = 'none';
    
    // 2. Étendre le canvas (<canvas> ou <video>) à tout l'écran
    const canvasArea = document.querySelector('.stream-canvas-area');
    if (canvasArea) {
        canvasArea.style.position = 'fixed';
        canvasArea.style.inset = '0';
        canvasArea.style.zIndex = '9999';
    }
    
    // 3. Créer un petit bouton "Exit fullscreen"
    this._exitFsBtn = document.createElement('button');
    this._exitFsBtn.textContent = 'Exit FS';
    this._exitFsBtn.style.cssText = 'position:fixed;top:12px;right:12px;z-index:10000;...';
    this._exitFsBtn.onclick = () => this._exitCssFallbackFullscreen();
    document.body.appendChild(this._exitFsBtn);
}
```

### 3d. Nouvelle méthode `_exitCssFallbackFullscreen()`
```javascript
_exitCssFallbackFullscreen() {
    if (!this._cssFullscreen) return;
    this._cssFullscreen = false;
    
    // 1. Restaurer le header
    const header = document.querySelector('.stream-header');
    if (header) header.style.display = '';
    
    // 2. Restaurer le canvas area
    const canvasArea = document.querySelector('.stream-canvas-area');
    if (canvasArea) {
        canvasArea.style.position = '';
        canvasArea.style.inset = '';
        canvasArea.style.zIndex = '';
    }
    
    // 3. Supprimer le bouton exit
    if (this._exitFsBtn) {
        this._exitFsBtn.remove();
        this._exitFsBtn = null;
    }
}
```

### 3e. Dans `quit()` et `destroy()`
Ajouter un appel à `_exitCssFallbackFullscreen()` dans les deux méthodes pour nettoyer le fallback si la session se termine en mode CSS fullscreen.

### 3f. Écouter la touche Escape en CSS fullscreen
Quand `_cssFullscreen` est vrai, la touche Escape doit sortir du CSS fullscreen (pas forwardée à l'hôte). Dans `handleKeyDown()`, ajouter un check en début :
```javascript
if (this._cssFullscreen && e.key === 'Escape') {
    e.preventDefault();
    this._exitCssFallbackFullscreen();
    return;
}
```

## Résumé des modifications attendues

### StreamView.js :
1. Ligne 2701 : Rendre l'appel à `_requestMobileFullscreen()` conditionnel à l'orientation landscape
2. Constructeur : Ajouter `this._cssFullscreen = false;`
3. `render()` : Remplacer la création du bouton mobile (lignes 658-672) par un bouton dans le header
4. `_initMobileFullscreen()` : Adapter pour inclure la logique de visibilité du nouveau bouton
5. `_requestFullscreen()` (nouveau nom ou modifié) : Ajouter la logique de fallback CSS
6. `_enterCssFallbackFullscreen()` : Nouvelle méthode
7. `_exitCssFallbackFullscreen()` : Nouvelle méthode
8. `handleKeyDown()` : Intercepter Escape en CSS fullscreen
9. `quit()` et `destroy()` : Nettoyer le CSS fullscreen
10. Renommer `_requestMobileFullscreen()` → `_requestFullscreen()` ou créer une nouvelle méthode

### stream.css :
1. Remplacer `.stream-mobile-fs-btn` par `.btn-stream-fs` (style header button)

## En fin de travail
Écris ton résumé dans `.claude/results/frontend-dev/2026-06-09-fullscreen-fixes/Resume-2026-06-09.md`
