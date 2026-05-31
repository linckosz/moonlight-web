Tu es backend-dev. Tu dois analyser l'état actuel du code backend concernant les sessions d'authentification.

Tâche : Lis les fichiers suivants et donne-moi un résumé structuré :

1. `d:\Code\moonlight-web-deepseek\backend\src\server\AuthManager.h` — structure de la classe, quels signaux existent (Q_SIGNALS), quelles méthodes
2. `d:\Code\moonlight-web-deepseek\backend\src\server\AuthManager.cpp` — implémentation, notamment quand les sessions sont créées/révoquées
3. Cherche le fichier de route qui expose `/api/auth/sessions` — regarde dans `backend/src/server/` pour le routeur HTTP. Dis-moi quel fichier contient la route et comment elle est implémentée.
4. Vérifie s'il existe déjà un endpoint SSE (`/api/auth/sessions/subscribe` ou similaire) ou tout mécanisme de notification existant.
5. Vérifie si le AuthManager émet déjà un signal (ou devrait émettre un signal) lors de la création/suppression d'une session.

Résumé attendu :
- Les fichiers lus et leur contenu pertinent
- Les signaux existants dans AuthManager
- La route `/api/auth/sessions` existante (ou non)
- Tout mécanisme de notification existant
- Recommandation pour ajouter un signal sessionChanged (ou équivalent) dans AuthManager

En fin de travail, écris ton résumé dans `.claude/results/backend-dev/{session}/Resume-YYYY-MM-DD.md`. Utilise l'identifiant de session `2026-05-30-adminview-autorefresh`.

Session ID: 2026-05-30-adminview-autorefresh