# Task: Analyser la gestion d'input tactile existante dans le frontend

## Contexte

L'utilisateur veut ajouter un comportement tactile mobile dans StreamView.js :
1. Single finger drag = mouse move (trackpad-like)
2. Single finger tap = left click
3. Two fingers = right click
4. Two finger swipe = scroll

## Ce que tu dois faire

Lis et analyse les fichiers suivants pour comprendre l'architecture actuelle de gestion d'input :

1. `d:\Code\moonlight-web-deepseek\frontend\js\ui\StreamView.js` — cherche :
   - Les gestionnaires d'événements souris (mousedown, mousemove, mouseup, wheel)
   - Les éventuels gestionnaires d'événements tactiles déjà présents
   - Comment les événements souris sont convertis en messages pour le backend
   - La structure générale de la classe StreamView

2. `d:\Code\moonlight-web-deepseek\frontend\js\streaming\` — lis tous les fichiers JS de ce dossier pour comprendre :
   - Comment les événements souris/clavier sont envoyés au backend
   - Le format des messages (protocole WebSocket/DataChannel)

3. `d:\Code\moonlight-web-deepseek\frontend\js\streaming\InputProcessor.js` (si existe) ou tout fichier lié à l'input

4. `d:\Code\moonlight-web-deepseek\backend\src\streaming\input\*` — fichiers backend pour comprendre le format attendu des messages souris

## Ce que tu dois produire

Dans ton résumé (`.claude/results/frontend-dev/touch-input-2026-06-03/Resume-2026-06-03.md`), écris :

1. **Structure actuelle des événements souris** : quels gestionnaires existent, comment ils envoient les messages
2. **Format des messages souris** : le protocole exact (type, payload, coordonnées, boutons)
3. **Format des messages clavier/molette** : pour le scrolling
4. **Taille du viewport** : comment la vue streaming est dimensionnée, mapping canvas -> écran
5. **Déjà du tactile ?** : y a-t-il déjà des gestionnaires touchstart/touchmove/touchend ?
6. **Recommandation** : comment intégrer proprement le tactile sans casser l'existant

Lis TOUS ces fichiers et réponds avec précision sur le format des messages.
