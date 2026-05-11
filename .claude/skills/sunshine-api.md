---
name: sunshine-api
description: Reference complete de l'API Sunshine — protocole GameStream (NvHTTP, XML, pairing, launch, RTSP) et API REST moderne. Distingue les deux APIs, leurs ports, leur authentification.
---

# Skill — Sunshine API

Reference complete de l'API Sunshine pour le projet Moonlight-Web.

Sunshine expose deux APIs distinctes :

1. **API GameStream (NvHTTP)** — protocole historique NVIDIA (XML, ports variables, TLS mutuel). Utilise par Moonlight pour le discovery, pairing, launch, et streaming.
2. **API REST** — API moderne (JSON, Basic Auth, port principal). Utilisee pour la gestion et la configuration de Sunshine.

---

## 1. API GameStream (NvHTTP)

Protocole herite de NVIDIA GameStream, compatible GFE. Utilise par moonlight-qt, moonlight-xbox, et moonlight-web.

### Ports

Les ports sont **dynamiques** et decouverts via le champ `HttpsPort` de `/serverinfo`.

| Protocole | Port GFE par defaut | Port Sunshine par defaut | Usage |
|-----------|-------------------|------------------------|-------|
| HTTP | 47989 | 47989 | Discovery, pairing (non authentifie) |
| HTTPS | 47984 | **47990** | Apps, launch, cancel, TLS mutuel |
| RTSP | 48010 | 48010 | Negociation session (TCP) |
| UDP Video | 47998 | 47998 | Flux video |
| UDP Audio | 47999 | 47999 | Flux audio |
| UDP Control | 48000 | 48000 | Canal de controle (input) |

> **Important** : moonlight-qt utilise `DEFAULT_HTTPS_PORT = 47984` (port GFE) comme fallback.
> Sunshine utilise 47990 par defaut. Le port HTTPS est **resolu dynamiquement** : le client lit
> le champ `<HttpsPort>` dans la reponse `/serverinfo` et l'utilise pour toutes les requetes
> HTTPS suivantes. Notre backend doit faire de meme.

### Authentification

- **HTTP (port 47989)** : non authentifie, pas de TLS. Utilise pour la decouverte initiale
  et pour les 4 phases du pairing. Le certificat serveur et la cle AES derivee du PIN
  assurent la securite au niveau applicatif.
- **HTTPS (port 47990 ou dynamique)** : TLS mutuel. Le client presente son certificat X.509
  apres appairage. Si la verification echoue, le serveur repond `status_code=401`.
- **Toutes** les requetes HTTPS contiennent le certificat client via `SslConfiguration`.
  En cas d'erreur 401 sur `/serverinfo`, le client retombe en HTTP et peut re-pairer
  si le certificat serveur a change.

### Parametre `uniqueid` et `uuid`

Contrairement a ce que suggerent certaines documentations, **TOUTES** les requetes
(HTTP et HTTPS) incluent systematiquement deux parametres :

```
uniqueid=0123456789ABCDEF&uuid=<UUID_random_hex>
```

- `uniqueid` : valeur fixe `0123456789ABCDEF` utilisee par tous les clients Moonlight
  pour permettre de fermer les sessions des autres clients.
- `uuid` : UUID v4 aleatoire genere a chaque requete (`QUuid::createUuid()`).

Notre backend doit reproduire ce comportement : ajouter ces deux parametres a chaque
appel HTTP/HTTPS vers Sunshine.

### Endpoints

Toutes les reponses sont en XML avec la structure :

```xml
<root status_code="200">
  <champ>valeur</champ>
</root>
```

`status_code != 200` indique une erreur. `status_message` contient le detail.

#### GET /serverinfo

Disponible en HTTP (decouverte) et HTTPS (client appaire).

**HTTP (decouverte initiale, cert inconnu)** :
```
GET http://<host>:47989/serverinfo?uniqueid=0123456789ABCDEF&uuid=...
```

**HTTPS (client deja appaire)** :
```
GET https://<host>:<HttpsPort>/serverinfo?uniqueid=0123456789ABCDEF&uuid=...
```

**Reponse XML** :
```xml
<root status_code="200">
  <hostname>NomDuServeur</hostname>
  <appversion>7.1.431.-1</appversion>
  <GfeVersion>3.23.0.74</GfeVersion>
  <uniqueid>UUID-DU-SERVEUR</uniqueid>
  <HttpsPort>47990</HttpsPort>
  <ExternalPort>47989</ExternalPort>
  <MaxLumaPixelsHEVC>1869449984</MaxLumaPixelsHEVC>
  <mac>AA:BB:CC:DD:EE:FF</mac>
  <LocalIP>192.168.1.x</LocalIP>
  <ServerCodecModeSupport>3</ServerCodecModeSupport>
  <PairStatus>1</PairStatus>
  <currentgame>0</currentgame>
  <state>SUNSHINE_SERVER_FREE</state>
  <gputype>NVIDIA GeForce RTX 3080</gputype>
</root>
```

**Champs importants** :

| Champ | Description |
|-------|-------------|
| `appversion` | `7.1.431.-1`. `-1` en 4e position = Sunshine (pas GFE) |
| `HttpsPort` | Port HTTPS effectif du serveur. **Utiliser ce port pour toutes les requetes HTTPS** |
| `ExternalPort` | Port HTTP pour l'acces WAN |
| `ServerCodecModeSupport` | Bitmask des codecs supportes |
| `PairStatus` | `1` si le client est appaire (HTTPS uniquement) |
| `currentgame` | ID de l'app en cours (`0` = aucune) |
| `state` | `SUNSHINE_SERVER_FREE` ou `SUNSHINE_SERVER_BUSY` |
| `mac` | Adresse MAC reelle (HTTPS) ou `00:00:00:00:00:00` (HTTP) |
| `gputype` | Modele du GPU (extension Sunshine, absent de GFE) |
| `MaxLumaPixelsHEVC` | Pixels lumineux max pour HEVC |

**Bitmask `ServerCodecModeSupport`** :

| Bit (valeur) | Codec |
|-------------|-------|
| `0x0001` | H.264 |
| `0x0002` | H.264 High 4:4:4 |
| `0x0100` | HEVC Main 8-bit |
| `0x0200` | HEVC Main10 (HDR) |
| `0x0400` | HEVC RExt 8-bit 4:4:4 |
| `0x0800` | HEVC RExt 10-bit 4:4:4 |
| `0x10000` | AV1 Main 8-bit |
| `0x20000` | AV1 Main10 (HDR) |
| `0x40000` | AV1 High 8-bit 4:4:4 |
| `0x80000` | AV1 High 10-bit 4:4:4 |

#### GET /applist

HTTPS uniquement. Retourne la liste des applications.

```
GET https://<host>:<port>/applist?uniqueid=0123456789ABCDEF&uuid=...
```

**Reponse** :
```xml
<root status_code="200">
  <App>
    <IsHdrSupported>1</IsHdrSupported>
    <AppTitle>Desktop</AppTitle>
    <ID>1</ID>
    <IsAppCollectorGame>0</IsAppCollectorGame>
  </App>
</root>
```

#### GET /appasset

Recupere l'image de couverture (PNG) d'une application.

```
GET https://<host>:<port>/appasset?uniqueid=0123456789ABCDEF&uuid=...&appid=<id>&AssetType=2&AssetIdx=0
```

| Parametre | Valeur | Description |
|-----------|--------|-------------|
| `appid` | int | ID de l'application |
| `AssetType` | `2` | Type d'asset (toujours 2 pour cover art) |
| `AssetIdx` | `0` | Index (toujours 0) |

Reponse : image PNG binaire (`Content-Type: image/png`).

#### GET /launch

Lance une application. HTTPS uniquement.

```
GET https://<host>:<port>/launch?uniqueid=0123456789ABCDEF&uuid=...
    &appid=<id>&mode=<W>x<H>x<FPS>&rikey=<hex>&rikeyid=<int>
    &sops=<0|1>&additionalStates=1&localAudioPlayMode=<0|1>
    &surroundAudioInfo=<int>&remoteControllersBitmap=<int>&gcmap=<int>
    &gcpersist=<0|1>&hdrMode=<0|1>
    [&clientHdrCapVersion=0&clientHdrCapSupportedFlagsInUint32=0
     &clientHdrCapMetaDataId=NV_STATIC_METADATA_TYPE_1
     &clientHdrCapDisplayData=0x0x0x0x0x0x0x0x0x0x0]
    &corever=1
```

**Parametres complets** :

| Parametre | Description |
|-----------|-------------|
| `appid` | ID de l'application (`0` = bureau) |
| `mode` | Resolution + FPS : `<largueur>x<hauteur>x<fps>` |
| `rikey` | Cle AES-GCM 128-bit pour chiffrer les flux (hex) |
| `rikeyid` | ID de cle, utilise comme IV initial (big-endian uint32) |
| `sops` | Server-side Optimal Playback Settings (`0` ou `1`) |
| `additionalStates` | Toujours `1` (envoye par moonlight-qt) |
| `localAudioPlayMode` | `1` = audio sur le serveur, `0` = audio vers le client |
| `surroundAudioInfo` | Info surround : `(channelMask << 16) \| channelCount` |
| `remoteControllersBitmap` | Bitmap des manettes connectees (meme valeur que `gcmap`) |
| `gcmap` | Gamepad capabilities map |
| `gcpersist` | `1` = persist game controllers on disconnect |
| `hdrMode` | `0` ou `1` |
| `clientHdrCapVersion` | Versione capacite HDR (si hdrMode=1) |
| `clientHdrCapSupportedFlagsInUint32` | Flags HDR (si hdrMode=1) |
| `clientHdrCapMetaDataId` | Metadata ID HDR (si hdrMode=1) |
| `clientHdrCapDisplayData` | Display data HDR (si hdrMode=1) |
| `corever` | `>= 1` active RTSP chiffre AES-GCM + video/control encryption. Ajoute automatiquement par `LiGetLaunchUrlQueryParameters()`. Toujours `1` pour les clients modernes. |

**Reponse succes** :
```xml
<root status_code="200">
  <sessionUrl0>rtsp://192.168.1.x:48010</sessionUrl0>
  <gamesession>1</gamesession>
</root>
```

Si `corever >= 1`, l'URL est `rtspenc://` (RTSP chiffre AES-GCM).

**Erreurs** :

| status_code | Cause |
|-------------|-------|
| 400 | Parametres manquants ou app deja en cours |
| 403 | Chiffrement obligatoire non supporte par le client |
| 503 | Echec d'initialisation de l'encodeur video |

#### GET /resume

Reprend une session existante. Meme parametres que `/launch`.

```
GET https://<host>:<port>/resume?uniqueid=0123456789ABCDEF&uuid=...
    &appid=<id>&rikey=<hex>&rikeyid=<int>&mode=<W>x<H>x<FPS>
    &localAudioPlayMode=<0|1>&corever=1
```

**Reponse** :
```xml
<root status_code="200">
  <sessionUrl0>rtsp://192.168.1.x:48010</sessionUrl0>
  <resume>1</resume>
</root>
```

#### GET /cancel

Arrete la session et l'application en cours.

```
GET https://<host>:<port>/cancel?uniqueid=0123456789ABCDEF&uuid=...
```

**Reponse** :
```xml
<root status_code="200">
  <cancel>1</cancel>
</root>
```

---

### Pairing (4 phases)

Le pairing s'effectue exclusivement en **HTTP** (port 47989), pas en HTTPS.
Chaque requete inclut `devicename=roth&updateState=1`.

En cas d'echec a n'importe quelle phase, le client appelle `GET /unpair` pour
annuler la session de pairing en cours.

```
Phase 1: GET /pair?devicename=roth&updateState=1
               &phrase=getservercert&salt=<16bytes_hex>&clientcert=<cert_hex>
         → repond plaincert (certificat serveur) apres saisie du PIN
         [Sunshine BLOQUE jusqu'a ce que le PIN soit entre dans l'UI web]

Phase 2: GET /pair?devicename=roth&updateState=1
               &clientchallenge=<AES_ECB_chiffre_hex>
         → repond challengeresponse

Phase 3: GET /pair?devicename=roth&updateState=1
               &serverchallengeresp=<AES_chiffre_hex>
         → repond pairingsecret (server_secret + signature RSA)

Phase 4: GET /pair?devicename=roth&updateState=1
               &clientpairingsecret=<secret+signature_hex>
         → repond paired=1 ou 0

Verif:  GET /pair?devicename=roth&updateState=1&phrase=pairchallenge
        → HTTPS cette fois ! Verifie que le TLS mutuel fonctionne.
        → repond paired=1
```

**Derivation de la cle AES** :
```
AES_KEY = SHA256(salt_bytes + PIN_bytes)[:16]
```

Pour les serveurs Gen 7+ (Sunshine, GFE recent) : SHA-256.
Pour les serveurs plus anciens (Gen < 7) : SHA-1.

**Chiffrement** : AES-128-ECB, padding desactive.
**Signature** : RSA-2048 + SHA-256 (`EVP_DigestSign`).

#### Phase 1 — `getservercert`

```
GET /pair?devicename=roth&updateState=1&phrase=getservercert
         &clientcert=<cert_hex>&salt=<16bytes_hex>
```

**Reponse** (apres saisie du PIN) :
```xml
<root status_code="200">
  <paired>1</paired>
  <plaincert>CERT_SERVEUR_EN_HEX</plaincert>
</root>
```

Si `plaincert` est absent, un autre client est deja en train de pairer.
Il faut appeler `/unpair` et recommencer (ou attendre).

#### Phase 2 — `clientchallenge`

```
GET /pair?devicename=roth&updateState=1&clientchallenge=<16_bytes_AES_ECB_hex>
```

Le client genere un challenge aleatoire de 16 octets, le chiffre avec AES-128-ECB
(cle derivee du PIN), et l'envoie.

Le serveur dechiffre, calcule `SHA256(challenge + signature_cert_serveur + server_secret_aleatoire)`,
concatene avec un `server_challenge` aleatoire, rechiffre le tout, et repond.

**Reponse** :
```xml
<root status_code="200">
  <paired>1</paired>
  <challengeresponse>HASH_CHIFFRE_EN_HEX</challengeresponse>
</root>
```

Le client dechiffre la reponse. Les 32 premiers octets (serveurs Gen 7+) sont le hash
de verification, les 16 octets suivants sont le `server_challenge`.

#### Phase 3 — `serverchallengeresp`

```
GET /pair?devicename=roth&updateState=1&serverchallengeresp=<AES_ECB_hex>
```

Le client calcule :
```
client_hash = SHA256(server_challenge + signature_cert_client + client_secret)
```
Puis chiffre `client_hash` (padded a 32 octets) avec AES-ECB.

Le serveur dechiffre et stocke le hash.

**Reponse** :
```xml
<root status_code="200">
  <paired>1</paired>
  <pairingsecret>SERVER_SECRET_16bytes + RSA_SIGNATURE_256bytes_HEX</pairingsecret>
</root>
```

Le client verifie la signature RSA du `server_secret` avec le certificat serveur
(recu en phase 1). Il verifie aussi que le hash correspond au challenge qu'il a envoye
en phase 2. Si les deux echouent, c'est un MITM ou un PIN incorrect.

#### Phase 4 — `clientpairingsecret`

```
GET /pair?devicename=roth&updateState=1&clientpairingsecret=<secret_16bytes + RSA_signature_256bytes_hex>
```

Le serveur verifie :
1. `SHA256(server_challenge + signature_cert_client + client_secret) == client_hash` stocke en phase 3
2. Signature RSA du `client_secret` avec la cle publique du certificat client

**Reponse** :
```xml
<root status_code="200">
  <paired>1</paired>   <!-- ou paired=0 si echec -->
</root>
```

Si `paired=1`, le certificat client est ajoute a la chaine de confiance et persiste.

#### Verification — `pairchallenge`

```
GET https://<host>:<port>/pair?uniqueid=0123456789ABCDEF&uuid=...
         &devicename=roth&updateState=1&phrase=pairchallenge
```

**Important** : cette requete est envoyee en **HTTPS** (TLS mutuel) pour verifier que
le handshake TLS fonctionne avec le nouveau certificat. Les 4 phases de pairing sont en HTTP.

**Reponse** :
```xml
<root status_code="200">
  <paired>1</paired>
</root>
```

#### GET /unpair

Utilise pour annuler une session de pairing en cours (en cas d'erreur a n'importe
quelle phase). HTTP.

```
GET http://<host>:47989/unpair
```

---

### Interaction HTTPS : gestion des erreurs TLS

- **Erreur 401** sur HTTPS : certificat serveur invalide ou client non appaire.
  Sur `/serverinfo` uniquement, le client retombe en HTTP pour continuer.
- **`SslHandshakeFailedError`** : le certificat serveur a change (reinstallation).
  Le client doit re-pairer.
- **`OperationCanceledError`** / timeout : la requete a depasse le delai.
- **Timeout** : 5s pour les requetes normales, 2s pour le fast-fail, 120s pour `/launch`.

---

## 2. API REST Sunshine

Sunshine expose une API REST moderne sur le **meme port HTTPS** que l'API GameStream
(47990 par defaut). Elle permet la gestion et la configuration de Sunshine.

### Authentification

- **Basic Auth** avec le compte utilisateur configure dans Sunshine.
- Pas de certificat client necessaire (TLS simple, pas mutuel).
- Les credentials sont stockes dans un fichier ou variables d'environnement.

### Endpoints REST

| Methode | Endpoint | Description |
|---------|----------|-------------|
| GET | `/api/apps` | Liste des applications (JSON) |
| POST | `/api/apps` | Ajouter/modifier une application |
| POST | `/api/apps/close` | Fermer l'application en cours |
| DELETE | `/api/apps/{index}` | Supprimer une application |
| GET | `/api/clients/list` | Liste des clients appaires |
| POST | `/api/clients/unpair` | Depairer un client |
| POST | `/api/clients/unpair-all` | Depairer tous les clients |
| GET | `/api/config` | Configuration complete (JSON) |
| POST | `/api/config` | Mettre a jour la configuration |
| POST | `/api/pin` | Envoyer le PIN de pairing |
| POST | `/api/password` | Changer le mot de passe |
| GET | `/api/logs` | Logs du serveur |
| POST | `/api/restart` | Redemarrer Sunshine |

### Utilisation dans Moonlight-Web

Actuellement, moonlight-web **n'utilise pas** l'API REST. Le PIN est entre
manuellement dans l'UI web de Sunshine par l'utilisateur. Les endpoints REST
pourraient etre utilises a l'avenir pour :
- Envoyer le PIN automatiquement (`POST /api/pin`)
- Decouvrir les applications au format JSON (`GET /api/apps`)
- Gerer la configuration dynamiquement

---

## 3. RTSP Handshake

Apres `/launch` ou `/resume`, le client se connecte a l'URL `sessionUrl0` retournee.

```
Client                          Sunshine (port 48010)
  |-- OPTIONS rtsp://... -->        |
  |<-- 200 OK ----------------      |
  |                                 |
  |-- DESCRIBE rtsp://... -->       |
  |<-- 200 OK + SDP payload --      |  (capacites: codecs, audio, chiffrement)
  |                                 |
  |-- SETUP streamid=video/... -->  |
  |<-- 200 OK + server_port=47998 - |  (+ X-SS-Ping-Payload)
  |                                 |
  |-- SETUP streamid=audio/... -->  |
  |<-- 200 OK + server_port=47999 - |  (+ X-SS-Ping-Payload)
  |                                 |
  |-- SETUP streamid=control/... -> |
  |<-- 200 OK + server_port=48000 - |  (+ X-SS-Connect-Data)
  |                                 |
  |-- ANNOUNCE + SDP config -->     |  (config video/audio detaillee)
  |<-- 200 OK ----------------      |
  |                                 |
  |-- PLAY -->                      |
  |<-- 200 OK ----------------      |
  |                                 |
  |  [streaming UDP demarre]        |
```

Voir `docs/moonlight-web-plan.md` pour les details du handshake RTSP et
les parametres SDP. Le plan contient la configuration complete de l'ANNOUNCE.

---

## 4. Flux de streaming UDP

Apres le PLAY RTSP, trois flux UDP sont etablis :

### Flux video (port 47998)
- Paquets RTP avec FEC Reed-Solomon
- Chiffrement AES-GCM optionnel (si `SS_ENC_VIDEO` active)
- Cle : `rikey` fourni dans `/launch`

### Flux audio (port 47999)
- Paquets RTP avec FEC
- Codec Opus (encapsule dans RTP)
- Chiffrement AES-CBC avec la meme `rikey`
- IV initial : `rikeyid` (big-endian uint32)

### Canal de controle (port 48000)
- Entrees clavier, souris, gamepad
- Protocole base sur des paquets batis avec `LiSend*Event()` (moonlight-common-c)
- Chiffrement AES-GCM (si `SS_ENC_CONTROL_V2` active)
- Identifie par `X-SS-Connect-Data` recu lors du SETUP RTSP

---

## 5. Contraintes d'implementation pour le backend

- **Serialisation par host** : une seule requete HTTPS sortante a la fois vers le
  meme host Sunshine. Necessaire car moonlight-qt utilise un seul QNetworkAccessManager
  et GFE/Sunshine ne supporte pas les requetes concurrentes.
- **`uniqueid` et `uuid`** : ajouter a TOUTES les requetes HTTP et HTTPS.
- **uniqueid fixe** : utiliser `0123456789ABCDEF` (valeur compatible moonlight-qt).
- **Pairing en HTTP** : les 4 phases sont en HTTP. Seule la verification `pairchallenge`
  est en HTTPS.
- **Certificat client** : genere localement, necessaire pour le TLS mutuel.
- **`rikey`/`rikeyid`** : generes aleatoirement a chaque session. Ne pas reutiliser.
- **`corever >= 1`** : active le RTSP chiffre et le chiffrement video/control.
  Toujours envoye par les clients modernes (via `LiGetLaunchUrlQueryParameters()`).
- **Timeout `/launch`** : 120 secondes. Sunshine peut prendre du temps a lancer
  l'application (notamment Desktop qui demarre un processus de capture).
- **Timeout `/cancel`** : 30 secondes.
- **Timeout normal** : 5 secondes. Fast-fail : 2 secondes.
- **H.264 only pour le MVP** : le flux video n'est pas decoupe par le backend.
  Il est forwarde tel quel au navigateur via WebSocket.
- **`/unpair`** : utiliser en cas d'echec de pairing pour nettoyer la session cote serveur.
