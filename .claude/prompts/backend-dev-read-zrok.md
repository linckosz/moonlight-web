# Mission : lecture des fichiers backend avant migration DuckDNS → zrok

Session : 2026-05-14-zrok-signaling

## Contexte

Nous remplacons DuckDNS par zrok pour le signaling WebSocket. Le target : zrok fait un tunnel TLS → WS non-secure sur localhost:48001.

## Fichiers a lire

Lis ces fichiers et reporte leur contenu pertinent (structure des classes, methodes, signatures, references DuckDNS) :

1. `backend/src/main.cpp` — cherche toutes les references DuckDNS (DdnsClient, routes /api/ddns/*, checkDdnsBanner, connect consent/cert/error, etc.)
2. `backend/src/server/AppSettings.h` — cherche ddnsConsentGranted, ddnsToken, leurs accesseurs
3. `backend/src/server/AppSettings.cpp` — cherche load/save des ddns settings
4. `backend/src/network/DdnsClient.h` — structure complete
5. `backend/src/network/DdnsClient.cpp` — implementation complete
6. `backend/src/streaming/StreamRelay.h` — structure, surtout le QWebSocketServer (mode Secure/NonSecure, port)
7. `backend/src/streaming/StreamRelay.cpp` — comment le WS server est initialise (Secure mode ? port ?)
8. `backend/src/TrayManager.h` — references DuckDNS
9. `backend/src/TrayManager.cpp` — references DuckDNS
10. `backend/backend.pro` — liste des fichiers (DdnsClient entries)

## Format du rapport

Pour chaque fichier, indique :
- Les lignes exactes qui referencent DuckDNS
- Les signatures de methodes importantes
- Comment le WS server est configure (port, Secure/NonSecure)
- La structure de AppSettings (methodes, champs)

Ecris ton resume dans `.claude/results/backend-dev/2026-05-14-zrok-signaling/Read-results.md`.

En fin de travail, ecris ton resume dans
`.claude/results/backend-dev/2026-05-14-zrok-signaling/Resume-2026-05-14.md`.
Inclus uniquement tes resultats/conclusions (pas la reflexion intermediaire).
Format : tache accomplie, fichiers modifies, decisions prises, points bloquants.
