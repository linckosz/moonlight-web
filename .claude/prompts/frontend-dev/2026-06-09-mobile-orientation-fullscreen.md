## Tâche : Auto-fullscreen mobile selon l'orientation

**Session** : `2026-06-09-mobile-orientation-fullscreen`

### Contexte

Le projet Moonlight-Web a un mode webrtc-media (MediaTrack) qui utilise un élément `<video>` natif pour le rendu vidéo. En mode DataChannel SCTP, le rendu se fait sur un `<canvas>` (pas de `<video>`). On veut ajouter un comportement automatique sur mobile :

- **Landscape** → appeler `videoEl.webkitEnterFullscreen()` (fullscreen natif iOS/Android)
- **Portrait** → sortir du fullscreen via `document.exitFullscreen()`
- Comportement automatique, sans bouton UI

### Ce que tu dois faire

1. **Lire `frontend/js/stream/StreamView.js`** — comprendre comment le `<video>` est utilisé (propriété `this.videoEl`, dans quel mode il existe, comment il est créé)
2. **Lire `frontend/js/stream/WebRtcMedia.js`** — comprendre le flux webrtc-media
3. **Lire `frontend/js/app.js` et/ou `frontend/js/utils.js`** si besoin de helpers de détection mobile

### Implémentation

Dans `StreamView.js`, ajoute :

1. **Détection mobile** : une helper `_isMobile()` basée sur `'ontouchstart' in window` ET `navigator.maxTouchPoints > 0` (évite les faux positifs desktop). Optionnellement vérifie que `HTMLVideoElement.prototype.webkitEnterFullscreen` existe.

2. **Écouteur d'orientation** : dans la méthode qui configure le mode webrtc-media (probablement `_initWebRtcMedia()` ou `_startWebRtcMedia()`), si mobile :
   - Ajoute un listener sur `matchMedia('(orientation: landscape)')` via `MediaQueryList.addEventListener('change', ...)`
   - En landscape → `videoEl.webkitEnterFullscreen()` avec `.catch()` silencieux
   - En portrait → `document.exitFullscreen()` avec `.catch()` silencieux
   - Déclenche immédiatement le handler au setup (pour entrer en fullscreen si déjà landscape au démarrage)

3. **Nettoyage** : dans `stop()` ou `destroy()`, supprime le listener d'orientation pour éviter les leaks.

4. **Guard** : ne rien faire si `this.videoEl` n'existe pas (mode DataChannel SCTP).

### Règles

- Pas de code mort, pas de console.log de debug
- Utilise `webkitEnterFullscreen` ET `requestFullscreen` avec fallback (pour compat Android Chrome aussi)
- Les `.catch()` sont silencieux (les navigateurs reject parfois le fullscreen si pas d'interaction utilisateur)
- Commentaires en anglais, concis

### Résultat

En fin de travail, écris ton résumé dans `.claude/results/frontend-dev/2026-06-09-mobile-orientation-fullscreen/Resume-2026-06-09.md`.

Inclus :
- Les fichiers lus
- Les décisions prises (où et comment tu as ajouté le code)
- Le code exact ajouté (méthodes, listeners)
- Les fichiers modifiés
