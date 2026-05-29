Tu dois tracer le chemin du bitrate depuis l'UI frontend jusqu'à l'API backend.

## Contexte
L'utilisateur veut vérifier que le bitrate (40 Mbps par défaut) est correctement transmis de Moonlight-Web à Sunshine. Il suspecte qu'il est perdu quelque part dans la pipeline.

## Fichiers à lire

1. `d:\Code\moonlight-web-deepseek\frontend\js\ui\SettingsView.js` — cherche comment le bitrate slider est lu et envoyé
2. `d:\Code\moonlight-web-deepseek\frontend\js\services\BackendClient.js` — cherche comment les settings sont envoyés au backend
3. `d:\Code\moonlight-web-deepseek\frontend\js\ui\StreamView.js` — vérifie si le bitrate est récupéré depuis les settings avant le lancement

## Questions précises

1. Dans SettingsView.js, quel est le champ/nom du contrôle de bitrate ? (ex: `bitrate`, `videoBitrate`, etc.)
2. Comment est-il envoyé au backend ? (quelle URL, quel verb HTTP, quel payload JSON ?)
3. Est-ce que le bitrate est stocké localement (localStorage) ou toujours récupéré du backend ?
4. Dans StreamView.js, est-ce que le bitrate est lu depuis les settings du backend avant de lancer le stream ?
5. Y a-t-il un endpoint `/api/settings/stream` ou similaire qui reçoit le bitrate ?

## Livrable
Rapporte les résultats avec les fichiers, numéros de ligne, et extraits de code pertinents.

En fin de travail, écris ton résumé dans `.claude/results/frontend-dev/2026-05-29-bitrate-trace/Resume-2026-05-29.md`.