# Tâche : Analyser le code source pour le partage du port 443

Lis les fichiers suivants et résume pour chaque fichier : le rôle du composant, comment il gère les connexions réseau, et les détails pertinents pour un partage de port entre HTTPS et WebSocket.

## Fichiers à lire

1. `d:\Code\moonlight-web-deepseek\backend\src\server\HttpServer.h`
2. `d:\Code\moonlight-web-deepseek\backend\src\server\HttpServer.cpp`
3. `d:\Code\moonlight-web-deepseek\backend\src\signaling\SignalingServer.h`
4. `d:\Code\moonlight-web-deepseek\backend\src\signaling\SignalingServer.cpp`
5. `d:\Code\moonlight-web-deepseek\backend\src\main.cpp`
6. `d:\Code\moonlight-web-deepseek\backend\src\network\NportClient.h`
7. `d:\Code\moonlight-web-deepseek\backend\src\network\NportClient.cpp`

## Questions auxquelles répondre pour chaque fichier

### HttpServer
- Quel type est `m_HttpsServer` ? (QTcpServer? QSslServer? autre?)
- Comment est créé le serveur HTTPS ? (port, listen, mode SSL)
- Existe-t-il déjà un getter pour exposer le QTcpServer ?
- Comment sont gérées les connexions entrantes ? (incomingConnection, signal newConnection, etc.)

### SignalingServer
- Comment est créé le QWebSocketServer actuellement ?
- Sur quel port écoute-t-il ?
- Comment gère-t-il start() et stop() ?
- Quel est son constructeur actuel ?
- Sa méthode `parsePublicUrl()` — comment construit-elle l'URL ?

### main.cpp
- Comment sont instanciés HttpServer et SignalingServer ?
- Quel port est passé à nportClient.setTargetPort() ?
- Comment est défini le signalingPort ?

### NportClient
- Quelle est la valeur par défaut de m_TargetPort ?
- Comment est utilisé setTargetPort() ?

## Livrable

Un résumé concis de chaque fichier, avec les extraits de code pertinents (lignes exactes). Structure le résumé pour permettre la prise de décision sur une fusion des serveurs.

En fin de travail, écris ton résumé dans
`.claude/results/backend-dev/2026-05-14-shared-port/Resume-2026-05-14.md`.
Inclus uniquement tes résultats/conclusions (pas la réflexion intermédiaire).
Format : fichiers lus, extraits clés, réponses aux questions ci-dessus.
