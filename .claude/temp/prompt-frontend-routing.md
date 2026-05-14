Analyse le système de routing/navigation du frontend de Moonlight-Web.

Le projet est à d:\Code\moonlight-web-deepseek.
Le frontend est dans d:\Code\moonlight-web-deepseek\frontend\js.

Objectif : Comprendre comment les différentes pages sont affichées (liste des hosts, admin/settings, stream view). Le routing est probablement basé sur des hash fragments d'URL (genre `#admin`, `#settings`, etc.) ou sur des fragments de path.

Cherche spécifiquement :
1. Où est géré le routing / la navigation (fichier JS principal ?)
2. Quels fragments d'URL / hash sont utilisés pour chaque vue
3. Comment on navigue vers la page "Server Settings" (admin/settings)
4. Quelle URL exacte permet d'atterrir directement sur la page des paramètres serveur

Lis les fichiers pertinents (app.js, les différents modules dans js/ui/, js/api/, etc.) pour trouver ces informations.

En fin de travail, écris ton résumé dans
`.claude/results/frontend-dev/2026-05-14-tray-settings-routing/Resume-YYYY-MM-DD.md`.
Inclus uniquement tes résultats/conclusions (pas la réflexion intermédiaire).
Format : tâche accomplie, fichiers modifiés, décisions prises, points bloquants.
