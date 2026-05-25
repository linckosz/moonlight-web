# Tâche : Analyser les headers HTTP dans le backend

Dans le projet moonlight-web-deepseek, je dois analyser comment les headers HTTP sont définis dans le backend C++/Qt.

## Ce que tu dois trouver

1. Où sont définis les headers de réponse HTTP (Content-Type, CORS, etc.)
2. Le fichier principal qui gère les réponses HTTP (probablement `backend/src/server/HttpServer.cpp` ou similaire)
3. Comment les réponses sont construites et envoyées

## Fichiers à lire

Cherche dans ces fichiers :
- `backend/src/server/HttpServer.cpp`
- `backend/src/server/HttpServer.h`
- Tout autre fichier gérant les réponses HTTP (response, reply, header)

## Rapporte

Pour chaque fichier pertinent :
1. Le chemin absolu
2. Comment les headers sont actuellement définis (structure, méthode)
3. Quels headers de sécurité sont déjà présents ou absents

Ne modifie rien. Lis seulement les fichiers pertinents et rapporte les résultats.
