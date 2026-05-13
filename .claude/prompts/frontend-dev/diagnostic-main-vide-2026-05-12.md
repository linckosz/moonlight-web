# Diagnostic : main-content vide

## Contexte
L'application affiche une page blanche. Le `<main id="main-content">` dans `index.html` est vide — HostListView n'est pas rendu.

## Tâche

1. **Lire** les fichiers suivants :
   - `frontend/js/app.js` (fichier principal — initialisation, machine d'état, `showHostList()`)
   - `frontend/js/ui/HostListView.js`
   - `frontend/index.html`

2. **Diagnostiquer** pourquoi `#main-content` reste vide :
   - Vérifie le flux d'initialisation dans `app.js` — `showHostList()` est-il appelé ? `transition('host_list')` fonctionne-t-il ?
   - Vérifie `HostListView.js` — le constructeur est-il bien appelé ? `render()` produit-il du HTML ?
   - Vérifie qu'il n'y a pas d'erreur JS silencieuse (try/catch qui avale, propriété undefined, etc.)
   - Vérifie les modifications récentes : icônes Admin/Settings, transition d'état, changements CSS

3. **Corriger** le bug identifié.

4. **Écrire** ton résumé dans `.claude/results/frontend-dev/diagnostic-main-vide-2026-05-12/Resume-2026-05-12.md`

Instruction importante :
- Ta réponse finale doit inclure : la cause racine, les fichiers modifiés, et le correctif appliqué.
- Ne te contente pas de dire "j'ai trouvé" — explique le flux exact qui cassait.
