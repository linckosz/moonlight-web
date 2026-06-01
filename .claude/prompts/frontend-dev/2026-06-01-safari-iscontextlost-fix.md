# Tache : Fix Safari isContextLost — frontend-dev

## Problème

`CanvasRenderingContext2D.isContextLost()` n'est pas disponible sur Safari/WebKit (ni macOS, ni iOS). Cela cause un crash `TypeError: this.ctx.isContextLost is not a function` a la ligne 1009 de StreamView.js, dans la boucle `requestAnimationFrame` → ecran noir sur tous les navigateurs WebKit.

## Instructions

1. **Lire** `d:\Code\moonlight-web-deepseek\frontend\js\ui\StreamView.js`
2. **Trouver tous les appels** a `isContextLost()` dans ce fichier
3. **Remplacer chaque occurrence** par une feature detection robuste :
   ```js
   typeof this.ctx.isContextLost === 'function' ? this.ctx.isContextLost() : false
   ```
4. **Optionnel** : Ajouter un log dans `_logPlatformInfo()` pour avertir si `isContextLost` n'est pas disponible
5. **Ne pas modifier** d'autres fichiers que StreamView.js

## Criteres d'acceptation

- `typeof this.ctx.isContextLost === 'function' ? this.ctx.isContextLost() : false` partout dans StreamView.js
- La boucle de rendu ne crash plus sur Safari
- Les autres appels a `isContextLost()` (s'il y en a) sont corriges de la meme maniere

## Session

Identifiant de session : `2026-06-01-safari-iscontextlost-fix`

En fin de travail, ecris ton resume dans `.claude/results/frontend-dev/2026-06-01-safari-iscontextlost-fix/Resume-2026-06-01.md`.
