Analyse du code existant pour comprendre comment les streaming settings sont geres.

## Contexte

L'utilisateur demande que les valeurs des Streaming Settings (bitrate, resolution, FPS, codec, gaming mode, etc.) soient stockees en localStorage dans le navigateur, avec les valeurs par defaut provenant du settings.json serveur.

## Backend

Pour backend-dev (backend-dev) :
1. Lis `d:\Code\moonlight-web-deepseek\backend\src\AppSettings.h` - pour voir les declarations des settings de streaming
2. Lis `d:\Code\moonlight-web-deepseek\backend\src\AppSettings.cpp` - pour voir l'implementation (chargement/sauvegarde, valeurs par defaut)
3. Lis `d:\Code\moonlight-web-deepseek\backend\src\HttpServer.cpp` - cherche les endpoints API lies aux settings de streaming (GET/PUT /api/settings)
4. Resume les endpoints API, leur signature et comment ils utilisent AppSettings

## Frontend

Pour frontend-dev (frontend-dev) :
1. Lis `d:\Code\moonlight-web-deepseek\frontend\js\ui\SettingsView.js` - pour voir comment les streaming settings sont affiches, charges et sauvegardes
2. Cherche comment les appels API sont faits (BackendClient.js probablement)
3. Resume le flow complet: chargement initial -> affichage -> modification -> sauvegarde

Chaque agent retourne un resume dans :
`.claude/results/{agent_name}/settings-read-2026-05-29/Resume-2026-05-29.md`
