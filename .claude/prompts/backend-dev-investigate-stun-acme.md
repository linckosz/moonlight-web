Tu dois investiguer le code source du backend moonlight-web pour comprendre trois erreurs remontees dans les logs :

1. `[InternetAccess] Setup complete, domain: "92b8d126.moonlightweb.dedyn.io" public IP: ""` — l'IP publique est vide
2. `[InternetAccess] lastError: "Failed to detect public IP via STUN"` — echec detection IP publique
3. `[AcmeClient] "Cannot start openssl for keygen"` — echec generation cle ACME

Fichiers a lire (dans `backend/src/server/`) :
- Fichiers lies a InternetAccess (InternetAccess.cpp/.h)
- Fichiers lies a AcmeClient (AcmeClient.cpp/.h)
- Tout fichier lie a STUN detection (cherche stun dans le dossier)
- Le fichier de configuration des chemins openssl si pertinent

Pour chaque fichier, lis-le integralement et reponds aux questions suivantes :

**STUN detection :**
- Comment la detection STUN est-elle implementee ? Quelle librairie/methode utilise-t-elle ?
- Quelle est la sequence d'appels ? Y a-t-il un timeout ?
- Qu'est-ce qui pourrait faire echouer la detection et retourner une IP vide ?
- Y a-t-il une gestion d'erreur adequate ?

**ACME openssl keygen :**
- Comment AcmeClient lance-t-il openssl ? (QProcess ?)
- Quel chemin/commande exacte est utilisee pour openssl ?
- Qu'est-ce qui pourrait empecher openssl de demarrer ?
- Y a-t-il une verification que openssl est installe et accessible ?

Lis tous les fichiers pertinents et fournis une analyse detaillee.

En fin de travail, ecris ton resume dans `.claude/results/backend-dev/{session}/Resume-YYYY-MM-DD.md`. Inclus uniquement tes resultats/conclusions (pas la reflexion intermediaire). Format : tache accomplie, fichiers modifies, decisions prises, points bloquants.

SESSION_ID: 2026-05-22-stun-acme-investigation
