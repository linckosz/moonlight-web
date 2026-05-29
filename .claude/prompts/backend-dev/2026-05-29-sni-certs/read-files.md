Tâche : Lire les fichiers sources du backend C++/Qt pour analyser l'implémentation actuelle des certificats SSL et du serveur TLS.

Session ID: 2026-05-29-sni-certs

Contexte : Nous allons implémenter le support SNI (Server Name Indication) pour servir des certificats différents selon le hostname demandé par le client. Le but est d'avoir :
- `brunoocto2.moonlightweb.top` → certificat PositiveSSL/Let's Encrypt actuel
- `localhost`, `127.0.0.1`, IP LAN → certificat self-signed avec SANs appropriés

Fichiers à lire (contenu COMPLET de chaque fichier) :

1. `backend/src/server/HttpServer.h` — Interface de SslServer, HttpServer
2. `backend/src/server/HttpServer.cpp` — Implémentation complète (SslServer, generateSelfSignedCert, loadCert, etc.)
3. `backend/src/server/AppSettings.h` — Pour voir comment les chemins de certificats sont stockés
4. `backend/src/server/AppSettings.cpp` — Pour voir la gestion des chemins de certificats

Pour CHAQUE fichier, lis-le en entier avec l'outil Read et rapporte-moi :
- Le contenu complet (ou un résumé détaillé des parties pertinentes si le fichier est très long)
- Les signatures des classes/méthodes clés
- Les patterns utilisés

Ne modifie rien. Je veux juste un rapport complet du code existant pour concevoir l'approche SNI.
