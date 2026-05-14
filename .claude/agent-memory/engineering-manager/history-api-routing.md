---
name: history-api-routing
description: Remplacement du hash-based routing par HTML5 History API (pushState/popstate) avec SPA fallback backend
metadata:
  type: project
---

Remplacement du hash-based routing (`window.location.hash`) par le History API (`history.pushState` + `popstate`).

## Fichiers modifies

### Backend : `backend/src/server/HttpServer.cpp`
- **processRequest()** : quand une requete non-API retourne 404 (fichier introuvable), le backend sert `index.html` en fallback (SPA fallback). Cela permet au frontend de gerer son propre routing pour les URLs comme `/admin` ou `/settings`.

### Frontend : `frontend/js/app.js`
- `_initRouter()` :
  - Lit `window.location.pathname` pour la vue initiale
  - Initialise `history.replaceState()` avec la vue courante
  - Ecoute `popstate` pour les boutons back/forward du navigateur
- `_navigateByView(view, state)` : render une vue sans toucher a l'URL (utilise par popstate et init)
  - Accepte un second parametre `state` pour la restoration de vue (ex: `{ hostUuid, hostDisplayName }` pour `/apps`)
- Navigation methods (`showHostList`, `showAdmin`, `showSettings`, `showAppList`, `launchApp`) : `history.pushState()` + appel de rendu

### `showAppList(host)` — ajout de pushState
- `history.pushState({ view: 'apps', hostUuid, hostDisplayName }, '', '/apps')`
- Stocke le contexte du host (uuid + displayName) dans l'etat pour la restoration via popstate

### `launchApp()` — ajout de pushState
- `history.pushState({ view: 'streaming' }, '', '/streaming')` dans le bloc `result.status === 'streaming'`

### Route `/apps` dans `_navigateByView()`
- Si `state.hostUuid` est present : reconstruit un objet `host` minimal (uuid + displayName) et cree un `AppListView`
- Si `state` vide (URL directe / bookmark) : redirige vers la liste des hosts
- Le `_updateNavHighlight` reste sur 'hosts' (pas de nav specifique pour apps)

### Route `/streaming` dans `_navigateByView()`
- L'etat de streaming est ephemere (depend de la session active) → impossible a restaurer
- Redirige vers la liste des hosts

### Tray : `backend/src/TrayManager.cpp`
- `onOpenSettings()` : l'URL passe de `https://localhost:<port>#admin` a `https://localhost:<port>/admin`

## Comportement

| URL | Vue affichee | Restoration possible |
|-----|--------------|---------------------|
| `/` | Liste des hosts (accueil) | Oui |
| `/admin` | Admin/Server settings | Oui |
| `/settings` | Streaming settings | Oui |
| `/apps` | Liste des apps du host | Oui (avec `hostUuid` dans le state) |
| `/streaming` | Vue streaming | Non (redirige vers `/`) |

Les boutons back/forward du navigateur fonctionnent correctement. Les signets (`/admin`, `/settings`) sont persistants et rechargeables.

**Note** : `/apps` en URL directe (bookmark, nouvelle fenetre) redirige vers la liste des hosts car le contexte du host (`hostUuid`) n'est pas disponible. C'est un comportement attendu.
