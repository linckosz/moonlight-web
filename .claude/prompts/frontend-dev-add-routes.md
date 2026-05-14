# Frontend task: Add /apps and /streaming routes

## Contexte

Le projet Moonlight-Web utilise un systeme de routing History API (SPA) deja en place dans `frontend/js/app.js`. Les routes actuelles sont :
- `/` → liste des hosts
- `/admin` → admin/server settings
- `/settings` → streaming settings

Le SPA fallback backend est deja operationnel (HttpServer.cpp sert index.html pour toutes les URLs non-API).

## Objectif

Ajouter deux nouvelles routes :
- `/apps` → vue liste des applications (quand un host est selectionne)
- `/streaming` → vue de streaming (quand on streame)

## Taches

1. **Lire** `frontend/js/app.js` et analyser le systeme de routing :
   - Comment `_initRouter()` fonctionne (mapping URL → callback)
   - Comment `_navigateByView()` ou equivalent est implemente
   - Quelles methodes existent deja pour l'affichage (`showHostList`, `showAdmin`, `showSettings`, etc.)
   - Si des methodes `showApps` ou `showStreaming` existent deja

2. **Ajouter les routes** dans le mapping du router (`_initRouter`) :
   - `apps` → callback qui appelle la methode d'affichage des apps
   - `streaming` → callback qui appelle la methode d'affichage du streaming

3. **S'assurer que `pushState` est bien appele** quand l'utilisateur navigue vers ces vues :
   - Si une methode `showApps()` existe deja, verifier qu'elle appelle `pushState({view: 'apps'}, '', '/apps')`
   - Si une methode `showStreaming()` existe deja, verifier qu'elle appelle `pushState({view: 'streaming'}, '', '/streaming')`
   - Si ces methodes n'existent pas encore, ne pas les creer — seulement preparer le routing pour quand elles existeront (Phase 7)

4. **Verifier les liens de navigation** existants (menus, boutons) qui pointent deja vers `/apps` ou `/streaming` et qui doivent fonctionner avec le nouveau routing.

## Fichier a modifier

- `frontend/js/app.js` uniquement

## Convention existante (a respecter)

Le format du router mapping est :
```javascript
'apps': (params) => this.showApps(),
'streaming': (params) => this.showStreaming(),
```

`_navigateByView` utilise `pushState` avec un objet `{view: viewName}`.

En fin de travail, ecris ton resume dans `.claude/results/frontend-dev/2026-05-14-frontend-routes/Resume-2026-05-14.md`.
Inclus uniquement tes resultats/conclusions (pas la reflexion intermediaire).
Format : tache accomplie, fichiers modifies, decisions prises, points bloquants.
