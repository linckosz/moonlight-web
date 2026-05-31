Tu travailles sur le frontend Vanilla JS de Moonlight-Web.

Tache : Securite — le log `Launch result` dans la console navigateur expose l'IP interne du reseau (ex: `sessionUrl: 'rtspenc://192.168.1.9:48010'`).

Contexte : Un utilisateur a vu dans la console JS du navigateur le message suivant :
```
Launch result: { gamingMode: true, sessionUrl: 'rtspenc://192.168.1.9:48010', ... }
```
Cette IP interne (entre le serveur MW et Sunshine) ne devrait jamais etre visible par le navigateur. Meme si le backend finira par ne plus renvoyer cette IP, il faut aussi arreter de logger l'integralite de la reponse.

Instructions :
1. Cherche dans les fichiers JS du frontend (`frontend/js/`) ou apparait le log `Launch result` ou `launchResult` ou tout log qui affiche la reponse complete de l'API de lancement.
2. Cherche dans `StreamView.js` et `BackendClient.js` en priorite.
3. Modifie le log pour soit :
   - Logger uniquement les champs non-sensibles (ex: `gamingMode`, `videoCodec`, `status` — mais PAS `sessionUrl`)
   - Ou logger `[Launch result]` sans les details
4. Verifie aussi si d'autres logs dans le flux de streaming (start, stop, error) loggent des reponses brutes contenant potentiellement des IP internes.

En fin de travail, ecris ton resume dans `.claude/results/frontend-dev/2026-05-31-security-fix-sessionurl/Resume-2026-05-31.md`. Inclus uniquement les resultats : fichiers modifies, modifications apportees, decisions prises, points bloquants eventuels.