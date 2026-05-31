Tu es frontend-dev. Tu dois analyser l'état actuel du AdminView côté frontend.

Tâche : Lis les fichiers suivants et donne-moi un résumé structuré :

1. `d:\Code\moonlight-web-deepseek\frontend\js\ui\AdminView.js` — comprends la structure complète :
   - Comment la classe est structurée (constructeur, render, bindEvents, etc.)
   - Quels endpoints API elle appelle et à quels moments
   - Affiche-t-elle déjà une liste de sessions actives ?
   - Y a-t-il déjà un polling ou un intervalle existant ?
   - Comment les données sont-elles affichées dans le DOM ?
   - Y a-t-il un mécanisme de rafraîchissement ?

2. Vérifie s'il y a d'autres fichiers dans `frontend/js/` qui gèrent des sessions ou utilisent un polling.

3. Regarde le fichier HTML de l'admin (`frontend/admin.html` ou équivalent) pour voir la structure.

Résumé attendu :
- Structure de AdminView, méthodes principales
- Endpoints API appelés
- État d'affichage des sessions actives
- Polling existant ou non
- Recommandation pour ajouter l'auto-refresh

En fin de travail, écris ton résumé dans `.claude/results/frontend-dev/{session}/Resume-YYYY-MM-DD.md`. Utilise l'identifiant de session `2026-05-30-adminview-autorefresh`.

Session ID: 2026-05-30-adminview-autorefresh