Tu es backend-dev pour le projet Moonlight-Web.

## Contexte : Crash du serveur apres timeout de liste d'applications

Les logs montrent ceci juste avant le crash :
```
[2026-05-29 18:36:35.679] [WARN] App list fetch failed for 780M: Operation timed out
18:36:38: The command "...mw-server.exe" terminated abnormally.
```

Le crash semble se produire immediatement apres qu'un timeout de fetch de la liste d'applications se decline.

## Investigation demandee

1. **Trouve le code qui logge "App list fetch failed"** — cherche dans tout le backend C++.
2. **Analyse le chemin de code** depuis ce log jusqu'au point de crash probable :
   - Comment le timeout est gere ?
   - Qu'est-ce qui se passe apres le timeout ?
   - Y a-t-il un callback, un deletion, un acces a un pointeur invalide ?
3. **Cherche les vulnerabilites** dans la gestion des timeouts :
   - Double deletion ?
   - Use-after-free ?
   - Callback appele sur un objet deja detruit ?
   - Exception non capturee ?
4. **Regarde aussi le HttpServer::onDisconnected** : le log montre "socket had pending async request" — est-ce lie ?
5. **Regarde tous les appels** a `ComputerManager`, `HttpServer`, `ResponseCallback` et async HTTP pour trouver le pattern de crash.

## Fichiers a examiner (liste non exhaustive)
- `backend/src/http/HttpServer.cpp` — gestion des connexions, onDisconnected
- `backend/src/http/ComputerManager.cpp` — app list fetch
- `backend/src/http/AsyncHttp.cpp` (ou equivalent) — gestion des requetes async
- `backend/src/streaming/Session.cpp` — eventuellement le code de lancement
- Tout fichier contenant "App list fetch" ou "Operation timed out" ou "timeout"

## Instructions
1. Lis les fichiers pertinents
2. Identifie exactement la ligne et le fichier du log "App list fetch failed"
3. Trace le chemin de code complet du timeout jusqu'au crash potentiel
4. Explique la cause racine
5. Propose une correction

En fin de travail, ecris ton resume dans `.claude/results/backend-dev/2026-05-29-crash-applist-timeout/Resume-2026-05-29.md`.

Commence par chercher le log "App list fetch" dans tous les fichiers source.