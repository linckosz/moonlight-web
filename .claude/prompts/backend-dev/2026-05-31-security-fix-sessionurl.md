Tu travailles sur le backend C++/Qt de Moonlight-Web.

Tache : Securite — exposition d'IP interne via `sessionUrl` dans la reponse JSON de l'endpoint de lancement.

Contexte : La reponse de `/start` ou `/launch` contient un champ `sessionUrl` du type `rtspenc://192.168.1.9:48010` qui expose l'IP interne du reseau (entre le serveur MW et Sunshine) au navigateur client. Cela ne devrait jamais etre visible par le frontend.

Instructions :
1. Cherche dans les fichiers du backend (`backend/src/`) ou est genere le champ `sessionUrl` dans la reponse JSON des endpoints `/launch` ou `/start` ou tout endpoint lie au streaming. Cherche les patterns comme `sessionUrl`, `rtspenc://`, `launchApp`, `launchResult`, etc.
2. Analyse comment cette URL est construite et renvoyee au frontend.
3. Soit supprime completement le champ `sessionUrl` de la reponse JSON (le frontend n'en a pas besoin — il utilise `signalingUrl` et `signalingPort` pour se connecter), soit le masque en remplacant l'IP par une valeur neutre comme `0.0.0.0` ou `localhost`.
4. Ne modifie que le strict minimum — ne casse pas d'autre fonctionnalite.

Fichiers probables a examiner : `StreamSession.cpp`, `StreamSession.h`, `HttpServer.cpp`, `HttpServer.h`, ou tout fichier gerant la reponse de `/start` ou `/launch`.

En fin de travail, ecris ton resume dans `.claude/results/backend-dev/2026-05-31-security-fix-sessionurl/Resume-2026-05-31.md`. Inclus uniquement les resultats : fichiers modifies, modifications apportees, decisions prises, points bloquants eventuels.