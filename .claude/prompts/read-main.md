Analyse les fichiers sources suivants et fournis un résumé de leur contenu :

1. `d:\Code\moonlight-web-deepseek\backend\src\main.cpp` — Point d'entrée, comment le serveur démarre
2. `d:\Code\moonlight-web-deepseek\backend\src\server\HttpServer.h` — Interface du serveur HTTP
3. `d:\Code\moonlight-web-deepseek\backend\src\server\HttpServer.cpp` — Implémentation du serveur HTTP
4. `d:\Code\moonlight-web-deepseek\backend\src\streaming\Session.h` — Interface de la session de streaming
5. `d:\Code\moonlight-web-deepseek\backend\src\streaming\Session.cpp` — Implémentation de la session

Pour chaque fichier, donne :
- Les classes/fonctions principales
- Le cycle de vie (comment ça démarre, s'arrête)
- Les threads utilisés
- Comment le port est configuré
- Où le settings.json est lu (si pertinent)

Ne modifie rien, lis seulement.

En fin de travail, écris ton résumé dans `.claude/results/backend-dev/{session}/Resume-YYYY-MM-DD.md` (remplace {session} par '2026-05-12-installer-plan' et YYYY-MM-DD par la date du jour).
