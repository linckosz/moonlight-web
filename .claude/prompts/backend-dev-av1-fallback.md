## Mission : Fallback AV1 → H.264 dans le backend

### Contexte

Le codec AV1 n'est pas encore validé dans moonlight-common-c. Quand l'utilisateur sélectionne AV1 dans les Settings, `LiStartConnection()` négocie AV1 avec Sunshine, la connexion RTSP réussit, mais Sunshine termine immédiatement avec `ML_ERROR_GRACEFUL_TERMINATION` (-100) sans envoyer aucun frame.

### Tâche

1. **Lire** `backend/src/server/Session.cpp` — identifie la méthode qui appelle `LiStartConnection()` (probablement `doLaunchApp()` ou une méthode dans le flow de `StreamSession::start()`).

2. **Ajouter un fallback** juste avant l'appel à `LiStartConnection()` :
   ```cpp
   // Si AV1 est sélectionné, forcer H.264 — AV1 pas encore validé
   if (m_Config.videoCodec == VideoCodec::AV1) {
       qWarning() << "[Session] AV1 not validated yet, falling back to H.264";
       m_Config.videoCodec = VideoCodec::H264;
   }
   ```

3. **Vérifier le retour JSON** — dans `StreamSession::start()` ou le handler de `/start`, le codec est renvoyé au frontend dans la réponse JSON. Assure-toi que :
   - Le codec dans la réponse correspond bien au codec réellement utilisé (H.264 en cas de fallback)
   - Si le fallback a eu lieu, on pourrait éventuellement ajouter un champ `"codecFallback": true` dans la réponse pour informer le frontend

4. **Vérifier les includes** — `VideoCodec` est probablement défini dans un header. Assure-toi que l'enum `VideoCodec::AV1` et `VideoCodec::H264` existent bien.

5. **Compiler** avec la skill `build` pour vérifier que ça compile.

### Fichiers concernés

- `backend/src/server/Session.cpp` — le fallback principal
- Éventuellement `backend/src/server/Session.h` ou `backend/src/streaming/StreamConfig.h` si l'enum VideoCodec est là
- Le handler HTTP qui sert la réponse `/start`

### Résultat attendu

- Quand l'utilisateur choisit AV1 dans Settings → le backend force H.264 avant de lancer la connexion
- Le frontend reçoit la bonne information (codec = H.264 dans la réponse JSON)
- Le fallback est loggé en warning

### En fin de travail

Écris ton résumé dans `.claude/results/backend-dev/av1-fallback-2026-05-13/Resume-2026-05-13.md`.
Inclus uniquement tes résultats/conclusions (pas la réflexion intermédiaire).
Format : tâche accomplie, fichiers modifiés (littéral + diff summary), décisions prises, points bloquants.
