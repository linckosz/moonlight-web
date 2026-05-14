## Mission : Explorer le code du System Tray

Je dois ajouter une action "Control Panel" dans le menu du system tray qui ouvre le navigateur vers l'URL du frontend.

### Tâches

1. Cherche dans `d:\Code\moonlight-web-deepseek\backend\src\` les fichiers qui implémentent le System Tray (QSystemTrayIcon, QMenu, tray icon, etc.)
2. Lis le(s) fichier(s) concerné(s) pour comprendre :
   - Comment le tray est créé
   - Comment le menu contextuel est construit
   - Quelles actions existent déjà
3. Vérifie aussi `main.cpp` pour voir comment le tray est initialisé
4. Cherche comment l'URL du serveur HTTP est accessible (port, host) pour l'ouvrir dans le navigateur

### Résultat attendu

Un résumé complet de :
- Le(s) fichier(s) exact(s) et numéro(s) de ligne où le tray et son menu sont implémentés
- La structure actuelle du menu (actions existantes)
- Comment l'URL du serveur (host:port) est stockée ou accessible
- Comment les autres actions (comme "Quit") sont connectées

N'écris PAS de code, contente-toi d'explorer et de rapporter.
