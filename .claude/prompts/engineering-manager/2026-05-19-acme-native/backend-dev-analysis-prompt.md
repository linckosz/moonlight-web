# Prompt pour backend-dev — Analyse du code existant

## Contexte

Nous devons remplacer la dépendance à acme.sh par un client ACMEv2 natif en C++. Avant de concevoir la solution, nous devons comprendre le code existant.

## Tâches à réaliser

### 1. Lire et analyser InternetAccessManager.cpp/.h

Fichiers :
- `d:\Code\moonlight-web-deepseek\backend\src\network\InternetAccessManager.cpp`
- `d:\Code\moonlight-web-deepseek\backend\src\network\InternetAccessManager.h`

Lis ces fichiers en entier. Tu dois identifier :

1. **Toutes les méthodes liées à acme.sh** : 
   - `findAcmeBinary()` — où elle cherche, quel message exact elle produit
   - `issueCertificate()` — comment elle appelle acme.sh, quels arguments
   - `checkCertificate()` — comment elle vérifie l'existence
   - `renewCertificate()` — comment elle renouvelle
   - Tout autre code qui référence acme.sh ou le chemin de certificat

2. **Les chemins de certificats** : où sont stockés les certificats (fullchain.pem, key.pem), comment ils sont chargés par HttpServer

3. **Les dépendances** : tout le code qui dépend de la présence d'un certificat signé par CA (vs auto-signé)

4. **Le flux asynchrone** : comment les callbacks sont chaînés (issue → check → loadCert)

### 2. Lire et analyser le client deSEC

Fichiers :
- `d:\Code\moonlight-web-deepseek\backend\src\network\DeSecClient.cpp`
- `d:\Code\moonlight-web-deepseek\backend\src\network\DeSecClient.h`

Tu dois comprendre :

1. Comment il communique avec l'API deSEC (endpoints, auth)
2. Comment il crée/modifie/supprime les enregistrements TXT DNS (pour DNS-01 challenge)
3. Le format des appels API (POST/PUT/DELETE sur quel endpoint)
4. Les mécanismes de polling (temps d'attente pour propagation DNS)
5. Comment il gère les erreurs et les timeouts

### 3. Documentation du flux TLS

Fichier :
- `d:\Code\moonlight-web-deepseek\backend\src\server\HttpServer.cpp` (lis au moins la section loadCert() et les parties TLS)

Comprends :
1. Comment HttpServer charge les certificats
2. Le fallback auto-signé
3. Où les certificats ACME sont censés être stockés

## Résultat attendu

Écris ton résumé dans :
`.claude/results/backend-dev/2026-05-19-acme-native-analysis/Resume-2026-05-19.md`

Inclus :
- Liste complète des méthodes et variables liées à acme.sh
- Structure des appels (ordre, callbacks)
- Chemins de certificats utilisés
- Analyse du client deSEC (endpoints, auth, flux DNS-01)
- Points d'intégration pour le futur client ACME
- Toute complexité ou piège à éviter

N'écris PAS de code, fais uniquement de l'analyse.
