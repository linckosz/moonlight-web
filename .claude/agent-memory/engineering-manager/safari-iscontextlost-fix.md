---
name: safari-iscontextlost-fix
description: Safari/WebKit ne supporte pas CanvasRenderingContext2D.isContextLost() — feature detection necessaire dans la boucle de rendu
metadata:
  type: project
---

CanvasRenderingContext2D.isContextLost() n'est pas implemente sur Safari/WebKit (macOS et iOS). Appeler cette methode directement provoque un TypeError → crash de la boucle requestAnimationFrame → ecran noir.

**Correction** : remplacer `this.ctx.isContextLost()` par :
```js
typeof this.ctx.isContextLost === 'function' ? this.ctx.isContextLost() : false
```

**Detection** : tester `CanvasRenderingContext2D.prototype.isContextLost` dans `_logPlatformInfo()` pour emission d'un warning si absent.

**Fichier concerne** : `frontend/js/ui/StreamView.js` (ligne ~1009 et ~357)
