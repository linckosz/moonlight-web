# Investigation SSL Certificate Mismatch

## Contexte
L'utilisateur a une erreur SSL : `ERR_CERT_COMMON_NAME_INVALID` quand il se connecte à `https://brunoocto.moonlightweb.top`. Le certificat servi par le serveur a pour CN `e6d31057.moonlightweb.top` au lieu de `brunoocto.moonlightweb.top`.

## Taches a effectuer

### 1. Lire le fichier certificat
Lis le fichier :
```
c:\Users\Minis\AppData\Roaming\Moonlight-Web\Moonlight-Web\cert\brunoocto_moonlightweb_top\fullchain.pem
```
Verifie le CN (Common Name) et les Subject Alternative Names (SANs) de ce certificat. Decris ce que tu trouves.

### 2. Chercher comment le certificat est charge dans le backend
Cherche dans les fichiers suivants du backend :
- `backend/src/HttpServer.*` ou `backend/src/HttpServer.h`
- `backend/src/streaming/SslServer.*` ou `backend/src/streaming/SslServer.h`
- `backend/src/main.cpp`
- Tout fichier qui contient "cert" ou "ssl" ou "loadCert" ou "fullchain" ou "privkey"

Trouve :
- Comment le chemin du certificat est determine (cert_path, settings, etc.)
- Quand le certificat est charge (au demarrage ? a chaque connexion ?)
- Si le serveur supporte le rechargement du certificat quand le sous-domaine change

### 3. Chercher la logique de rechargement de certificat
Cherche specifiquement :
- Y a-t-il un mecanisme pour detecter que le sous-domaine a change ?
- Le certificat est-il recharge automatiquement ?
- Y a-t-il un endpoint API pour forcer le rechargement ?
- Regarde les fichiers : `DdnsClient.*`, `HttpServer.*`, `Settings.*`, `main.cpp`

### 4. Identifier la cause racine
- Pourquoi le serveur sert-il le certificat de `e6d31057.moonlightweb.top` ?
- Est-ce que le serveur cache le certificat en memoire et ne le recharge pas ?
- Est-ce que le certificat pour `brunoocto.moonlightweb.top` existe bien ?
- Quel est le flux de demarrage du serveur vis-a-vis des certificats ?

## Fichiers a lire absolument
- `backend/src/main.cpp`
- `backend/src/HttpServer.h` (s'il existe, sinon chercher le fichier qui definit HttpServer)
- `backend/src/HttpServer.cpp` (idem)
- `backend/src/streaming/SslServer.cpp` (ou fichier equivalent)
- `backend/src/streaming/SslServer.h`
- Regarde aussi si un fichier comme `CertificateManager.*` ou `CertManager.*` existe
- Regarde `backend/src/settings/` pour `AppSettings` ou equivalent
- Regarde `backend/src/network/` pour `DdnsClient.*` ou gestion DNS

## Format de la reponse
Tu dois produire un diagnostic complet en francais avec :
1. Resultat de la lecture du fichier fullchain.pem (CN, SANs, validite)
2. Explication du flux de chargement du certificat
3. Identification du bug (pourquoi le mauvais cert est servi)
4. Recommandation de correction

## Fichier de resultat
En fin de travail, ecris ton resume dans :
`.claude/results/backend-dev/2026-05-27-ssl-cert-investigation/Resume-2026-05-27.md`
