# Investigation : Chaîne de certificat SSL incomplète

## Contexte

Le serveur moonlightweb.top (IP 82.67.150.202) a un rapport SSL Labs indiquant :

> "This server's certificate chain is incomplete. Grade capped to B."

Seul le certificat leaf (1297 bytes, Let's Encrypt RSA 2048) est servi — l'intermédiaire Let's Encrypt R3/R13 manque.

## Tâche

Tu dois investiguer le code backend et me donner une analyse complète de :

1. **Comment le certificat SSL est chargé** — dans `HttpServer.cpp`, cherche `QSslCertificate`, `QSslKey`, `QSslConfiguration`, `setLocalCertificate` et `loadCert` ou `setupTls` ou similaire.

2. **Comment le certificat est stocké après renouvellement** — dans `AcmeClient.cpp`, regarde comment le cert et la key sont sauvés sur disque après le challenge ACME. Est-ce qu'on sauve le cert leaf seul, ou bien la chaîne complète ?

3. **Où les chemins des certificats sont configurés** — dans `AppSettings.cpp`/`AppSettings.h`, regarde les méthodes `sslCertPath()`, `sslKeyPath()`, ou tout paramètre pointant vers les fichiers de cert.

4. **Le format de sauvegarde** — est-ce que le fichier .pem contient le fullchain (leaf + intermediates) ou seulement le cert leaf ?

5. **Où la chaîne intermédiaire pourrait être perdue** — Y a-t-il un endroit où on écrit le cert via `toPem()` ou `toDer()` sans inclure les CA intermédiaires ?

## Fichiers à lire

- `d:\Code\moonlight-web-deepseek\backend\src\server\HttpServer.cpp`
- `d:\Code\moonlight-web-deepseek\backend\src\server\HttpServer.h`
- `d:\Code\moonlight-web-deepseek\backend\src\network\AcmeClient.cpp`
- `d:\Code\moonlight-web-deepseek\backend\src\network\AcmeClient.h`
- `d:\Code\moonlight-web-deepseek\backend\src\server\AppSettings.cpp`
- `d:\Code\moonlight-web-deepseek\backend\src\server\AppSettings.h`

S'il y a d'autres fichiers pertinents (par ex. `InternetAccessManager.cpp` ou `TlsManager` ou autre), lis-les aussi.

## Livrable

Écris ton analyse dans `.claude/results/backend-dev/2026-05-25-ssl-chain-investigation/Resume-2026-05-25.md` avec au moins :

- Les mécanismes exacts (nom de fonction, ligne) de chargement du cert SSL
- Où le cert est sauvegardé après renouvellement ACME
- Le maillon manquant : pourquoi la chaîne intermédiaire n'est pas servie
- Un diagnostic précis du bug (1-2 paragraphes)
- Une proposition de fix concrète (quels fichiers modifier et comment)
