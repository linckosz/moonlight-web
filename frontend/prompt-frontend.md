# Mission: Améliorer la barre des statistiques — StreamView.js

## Contexte

Tu travailles sur `frontend/js/ui/StreamView.js` et `frontend/css/stream.css` du projet MoonlightWeb.
Les commentaires dans le code doivent être en anglais.
Les fichiers concernés sont en UTF-8 sans BOM.

## Changements demandés

### 1) Header simplifié — supprimer le codec badge ET le status dot

**Fichier** : `frontend/js/ui/StreamView.js`, méthode `render()`, template HTML (lignes ~466-473)

Supprime les deux elements du stream-header :
- L'element `.stream-status` (ligne 467-470) — le dot + "Connecting..."
- L'element `.stream-codec-badge` (ligne 471) — l'affichage du codec

Le header ne doit PLUS contenir que le bouton "Stop Streaming".

**ATTENTION** : Ne pas supprimer la variable `this.statusEl` ni la méthode `setStatus()`. 
- `this.statusEl` est assigné ligne 511 et utilisé par `setStatus()` ligne 2327.
- `setStatus()` est appelée dans 7+ endroits du fichier.
- SOLUTION : Garder la déclaration `this.statusEl = document.getElementById('stream-status');` mais l'element n'existera plus dans le DOM. Pour éviter des erreurs, modifie `setStatus()` pour qu'elle soit no-op si `this.statusEl` est null :

```javascript
setStatus(state, text) {
    // No-op — replaced by stats overlay
}
```

### 2) Stats overlay — déplacer en haut-centre, avec responsive mobile

**Fichier** : `frontend/js/ui/StreamView.js`, méthode `render()`, création de `_overlayEl` (lignes ~516-527)

Remplacer le style inline par des classes CSS :

```javascript
this._overlayEl = document.createElement('div');
this._overlayEl.id = 'stream-stats-overlay';
this._overlayEl.className = 'stream-stats-overlay';
this._overlayEl.style.display = 'none';
this._overlayEl.innerHTML = '<div class="stats-waiting">Waiting...</div>';
document.getElementById('stream-view').appendChild(this._overlayEl);
```

### 3) Stats content — utiliser innerHTML avec structure riche

**Fichier** : `frontend/js/ui/StreamView.js`, méthode `_updateOverlay()` (lignes ~1506-1567)

Remplacer le `textContent = lines.join('\n')` par une construction innerHTML avec la structure suivante :

```html
<div class="stats-line stats-main">1920×1080 | 60 fps | 25.3 Mbps | H.264 | webrtc</div>
<div class="stats-line stats-sub">
  <span class="stats-label">Host RTT:</span>
  <span class="stats-value stats-rtt">5.2ms</span>
  <span class="stats-range">[3.1–8.7]</span>
</div>
<div class="stats-line stats-sub">
  <span class="stats-label">Browser RTT:</span>
  <span class="stats-value stats-rtt">2.1ms</span>
  <span class="stats-range">[1.0–4.2]</span>
</div>
<div class="stats-line stats-sub">
  <span class="stats-label">Decode:</span>
  <span class="stats-value stats-decode">4.5ms</span>
  <span class="stats-range">[2.1–9.8]</span>
</div>
<div class="stats-line stats-sub">
  <span class="stats-label">Frames:</span>
  <span class="stats-value">R:1234 D:1230 Dr:4</span>
</div>
```

La classe `stats-main` pour la première ligne, `stats-sub` pour les autres.
Le `stats-waiting` ne s'affiche que tant que `_firstFrameRendered` est false.

**Structure des données** : Garder exactement la même logique de calcul (FPS sliding 2s, bitrate cumulé, RTT sliding 5s, decode latency). Ne changer que le rendu visuel.

### 4) CSS — style élégant pour la stats card

**Fichier** : `frontend/css/stream.css`

Ajouter les styles suivants (après la section shortcuts slide) :

```css
/* ── Streaming stats overlay (top-center card) ───────────────────── */
.stream-stats-overlay {
    position: fixed;
    top: 12px;
    left: 50%;
    transform: translateX(-50%);
    z-index: 100;
    pointer-events: none;
    background: rgba(8, 8, 10, 0.82);
    backdrop-filter: blur(8px);
    -webkit-backdrop-filter: blur(8px);
    border: 1px solid rgba(255, 255, 255, 0.08);
    border-radius: 10px;
    padding: 10px 16px;
    font-family: 'SF Mono', 'Cascadia Code', 'Consolas', monospace;
    line-height: 1.6;
    color: rgba(255, 255, 255, 0.85);
    transition: opacity 0.3s ease;
}

/* Mobile: move to left to avoid overlapping the Stop button (top-right) */
@media (max-width: 768px) {
    .stream-stats-overlay {
        left: 10px;
        transform: none;
    }
}

.stats-waiting {
    font-size: 12px;
    color: rgba(255, 255, 255, 0.5);
    text-align: center;
}

.stats-line {
    white-space: nowrap;
}

.stats-main {
    font-size: 13px;
    font-weight: 600;
    color: #fff;
    letter-spacing: 0.3px;
    padding-bottom: 6px;
    border-bottom: 1px solid rgba(255, 255, 255, 0.06);
    margin-bottom: 4px;
}

.stats-sub {
    font-size: 11px;
    color: rgba(255, 255, 255, 0.65);
}

.stats-label {
    color: rgba(255, 255, 255, 0.45);
    margin-right: 4px;
}

.stats-value {
    font-weight: 500;
}

.stats-rtt {
    color: #f0c040;  /* amber for latency */
}

.stats-decode {
    color: #60c0f0;  /* blue for decode */
}

.stats-range {
    color: rgba(255, 255, 255, 0.35);
    font-size: 10px;
    margin-left: 2px;
}
```

### 5) Ajustement : premier affichage de l'overlay

Dans `onDecodedFrame()` (ligne ~1141), l'overlay est affiché au premier frame :
```javascript
if (this._overlayEl) this._overlayEl.style.display = '';
```

Remplacer le texte "Waiting..." par les vraies stats quand le premier frame arrive. Le plus simple : ne pas du tout afficher l'overlay pendant l'attente (garder `display:none`), puis l'afficher au premier frame avec les stats déjà calculées.

**Alternative** : Afficher l'overlay immédiatement avec `stats-waiting`, puis le remplacer par les vraies stats au premier frame. Choisis cette option pour donner un feedback visuel immédiat pendant la connexion.

### 6) Ne pas casser le reste

- Ne pas modifier le comportement de `setStatus()` en appel extérieur — juste la rendre no-op
- Ne pas modifier `_handleStatsMessage()`, `handleVideoFrame()`, ni aucune autre méthode
- Les stats (RTT, decode latency, FPS) doivent continuer à fonctionner exactement comme avant

## Résumé des fichiers à modifier

1. `frontend/js/ui/StreamView.js`
   - `render()` : supprimer stream-status + stream-codec-badge du template, changer création overlay
   - `_updateOverlay()` : utiliser innerHTML au lieu de textContent
   - `setStatus()` : rendre no-op
   - (optionnel) premier affichage overlay

2. `frontend/css/stream.css`
   - Ajouter styles `.stream-stats-overlay`, `.stats-main`, `.stats-sub`, `.stats-label`, etc.

## Fin du travail

Écris ton résumé dans `.claude/results/frontend-dev/2026-06-03-stats-overlay/Resume-2026-06-03.md`.
Inclus : fichiers modifiés, décisions prises, points bloquants éventuels.
