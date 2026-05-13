# Tâche : Changements cosmétiques admin

## Contexte

L'utilisateur veut deux petites modifications cosmétiques dans le frontend.

## Fichiers à modifier

1. `frontend/index.html` — ligne du bouton "Admin" (id="btn-admin")
2. `frontend/js/ui/AdminView.js` — titre de la vue admin

## Changement 1 — Icône dans index.html

Localise le bouton admin (id="btn-admin") dans index.html. Il contient actuellement une icône SVG d'engrenage. Remplace-la par une icône "terminal" style `</>` :

```svg
<svg viewBox="0 0 24 24" width="20" height="20">
  <path d="M4 6l6 6-6 6" stroke="currentColor" fill="none" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/>
  <path d="M12 18h8" stroke="currentColor" fill="none" stroke-width="2" stroke-linecap="round"/>
</svg>
```

## Changement 2 — Titre dans AdminView.js

Localise le titre "Admin Control Panel" dans AdminView.js et remplace-le par "Server Settings".

## Instructions

1. Lis les deux fichiers pour voir l'état actuel
2. Fais les modifications
3. Écris ton résumé dans `.claude/results/frontend-dev/2026-05-12-admin-cosmetic/Resume-2026-05-12.md`
