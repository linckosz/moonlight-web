# Mission : Analyser le code actuel de chargement des certificats SSL

Tu es `backend-dev` (Sonnet). L'Engineering Manager a besoin que tu lises et analyses les fichiers suivants pour comprendre la logique actuelle de chargement des certificats SSL.

## Fichiers à lire

1. `d:\Code\moonlight-web-deepseek\backend\src\streaming\HttpServer.cpp` — en particulier :
   - La fonction `findCertDir()` — comment elle scanne les dossiers, quel est le critère de sélection
   - `loadCert()` — comment le certificat est chargé
   - Toute autre fonction liée aux certificats

2. `d:\Code\moonlight-web-deepseek\backend\src\streaming\HttpServer.h` — signatures des fonctions de certificat

3. `d:\Code\moonlight-web-deepseek\backend\src\streaming\InternetAccessManager.cpp` — en particulier :
   - `checkCertificate()` — comment elle vérifie l'expiration
   - Comment le sous-domaine (unique_id) est stocké/accessible

4. `d:\Code\moonlight-web-deepseek\backend\src\streaming\InternetAccessManager.h` — interface

5. `d:\Code\moonlight-web-deepseek\backend\src\streaming\Session.cpp` et `Session.h` — comment le cert_path est passé/utilisé (si pertinent)

## Ce que je dois comprendre

1. **Flux actuel** : Qui appelle `findCertDir()` ? Avec quels paramètres ? Où le `cert_path` est-il stocké ?
2. **Scan actuel** : Comment `findCertDir()` scanne-t-elle les dossiers ? Quel est le premier fichier trouvé ?
3. **Vérification CN** : Où et comment le CN du certificat est-il vérifié (ou pas) ?
4. **Accès au domaine** : Comment récupérer le sous-domaine actuel (ex: `brunoocto.moonlightweb.top`) depuis le code ? Est-ce que `InternetAccessManager` ou `AppSettings` contiennent cette info ?
5. **ACME** : Où est déclenché le renouvellement ACME / Let's Encrypt ?

## Résultat attendu

Retourne-moi un résumé structuré de :
- Chaque fonction pertinente et son rôle
- Les paramètres et signatures
- Le flux actuel de décision (diagramme textuel si pertinent)
- Les points de blocage / problèmes identifiés
- Où et comment le domaine actuel est accessible

En fin de travail, écris ton résumé dans
`.claude/results/backend-dev/2026-05-27-ssl-cert-logic/Resume-2026-05-27.md`.
Inclus uniquement tes résultats/conclusions (pas la réflexion intermédiaire).
Format : tâche accomplie, fichiers modifiés, décisions prises, points bloquants.
