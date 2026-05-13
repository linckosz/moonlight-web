Lit ces fichiers et réponds à chaque question précisément (pas de code, juste de l'analyse).

Fichiers à lire :
1. `backend/src/streaming/StreamRelay.h` — toute la classe, ses membres, son constructeur
2. `backend/src/streaming/StreamRelay.cpp` — tout le fichier (constructeur, start(), les callbacks, etc.)
3. `backend/src/streaming/Session.cpp` — cherche comment StreamRelay est créé et configuré (constructeur, start(), notamment où le wsPort est passé, et si une adresse d'écoute est spécifiée)
4. `backend/src/server/SslServer.h` et `SslServer.cpp` — comment le serveur SSL écoute (sur quelle adresse/interface)
5. `backend/src/server/HttpServer.h` et `HttpServer.cpp` — comment le serveur HTTP/WS principal écoute (pour comparaison)
6. Tout fichier qui crée un `QTcpServer` / `QSslServer` / `SslServer` dans le contexte streaming

Questions spécifiques :
1. Est-ce que `StreamRelay` spécifie une adresse d'écoute (QHostAddress::Any, QHostAddress::LocalHost, ou une IP spécifique) ? Si oui, laquelle ?
2. Est-ce que le `QSslServer` / `SslServer` utilisé par StreamRelay est configuré pour écouter sur une adresse particulière ?
3. Y a-t-il du `127.0.0.1` ou `localhost` hardcodé dans le code de streaming ?
4. Comment Session.cpp instancie-t-il StreamRelay ? Passe-t-il une adresse ?
5. Où sont les logs `[StreamRelay] Created` et `[StreamRelay] Starting WS server on port` générés ? Quelle est la séquence exacte ?
6. Comment le certificat SSL est-il partagé entre le serveur HTTP principal et le WebSocket relay ? Sont-ils indépendants ?

Donne la cause racine probable du problème et la solution précise (quel fichier, quelle ligne, quoi changer).

En fin de travail, écris ton résumé dans
`.claude/results/backend-dev/2026-05-13-streaming-network-analysis/Resume-2026-05-13.md`.
Inclus uniquement tes résultats/conclusions (pas la réflexion intermédiaire).
Format : tâche accomplie, fichiers modifiés, décisions prises, points bloquants.
