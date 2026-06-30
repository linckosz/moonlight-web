Voici la documentation complète en Markdown sur l'interaction entre un client Moonlight et l'API de Sunshine.

# API GameStream

## HTTP (port 47989, non authentifié)

| Endpoint            | Description                                                                 |
|---------------------|-----------------------------------------------------------------------------|
| GET /serverinfo     | Informations du serveur (nom, version, codecs, état) sans adresse MAC réelle |
| GET /pair           | Initier ou continuer le processus d'appairage                               |

---

## HTTPS (port 47990, TLS mutuel)

| Endpoint                     | Description                                                                 |
|-----------------------------|-----------------------------------------------------------------------------|
| GET /serverinfo             | Informations du serveur avec adresse MAC réelle et `PairStatus`            |
| GET /pair                   | Continuer l'appairage ou vérifier le statut (`pairchallenge`)              |
| GET /applist                | Liste des applications disponibles sur le serveur                          |
| GET /appasset?appid=<id>&AssetType=2&AssetIdx=0 | Image de couverture (PNG) d'une application                                |
| GET /launch                 | Lancer une application et obtenir l'URL RTSP de session                    |
| GET /resume                 | Reprendre une session existante et obtenir l'URL RTSP                      |
| GET /cancel                 | Arrêter l'application et la session en cours                               |

---

```markdown
# Documentation : Interaction Client Moonlight ↔ API Sunshine

## Vue d'ensemble

Sunshine implémente le protocole **NVIDIA GameStream** (compatible GFE). La communication
se déroule en trois couches :

1. **HTTP/HTTPS (GameStream)** — découverte, appairage, lancement d'applications
2. **RTSP** — négociation de session de streaming
3. **UDP** — flux vidéo, audio et contrôle

### Ports par défaut

| Port  | Protocole | Usage                                      |
|-------|-----------|--------------------------------------------|
| 47989 | HTTP      | Découverte, appairage (4 phases)           |
| 47984 | HTTPS     | GameStream sécurisé (port GFE par défaut)  |
| 47990 | HTTPS     | GameStream sécurisé (port Sunshine par défaut) |
| 48010 | TCP       | Négociation RTSP                           |
| 47998 | UDP       | Flux vidéo                                 |
| 47999 | UDP       | Flux audio                                 |
| 48000 | UDP       | Canal de contrôle (input)                  |

> **Important : le port HTTPS est dynamique.** Le client lit le champ `<HttpsPort>`
> dans la réponse `/serverinfo` et l'utilise pour toutes les requêtes HTTPS suivantes.
> GFE utilise 47984 par défaut ; Sunshine utilise 47990. moonlight-qt utilise
> `DEFAULT_HTTPS_PORT = 47984` comme fallback si `HttpsPort` est vide ou nul.
> Notre backend doit implémenter cette résolution dynamique.
>
> Les ports RTSP et UDP sont calculés par rapport au port de base via `net::map_port()`.
> Le port RTSP est `base_port + 21` (`RTSP_SETUP_PORT = 21`).

---

## Format des réponses

Toutes les réponses des endpoints GameStream sont en **XML** avec la structure :

```xml
<root status_code="200">
  <champ>valeur</champ>
</root>
```

Un `status_code` différent de `200` indique une erreur. Le champ `status_message`
contient le message d'erreur le cas échéant.

### Paramètres communs à toutes les requêtes

**Toutes** les requêtes HTTP/HTTPS vers Sunshine (découverte, pairing, applist,
launch, cancel) doivent inclure ces deux paramètres :

| Paramètre | Valeur | Description |
|-----------|--------|-------------|
| `uniqueid` | `0123456789ABCDEF` | UUID fixe commun à tous les clients Moonlight (permet de fermer les sessions des autres clients) |
| `uuid` | UUID v4 aléatoire hex | Généré aléatoirement à chaque requête |

Ils sont ajoutés automatiquement par `NvHTTP::openConnection()` à chaque appel.

> **Note** : le `uniqueid=0123456789ABCDEF` est une valeur fixe **partagée par tous
> les clients Moonlight** (littéralement "Common UID for Moonlight clients to allow
> them to quit games for each other"). Ne pas utiliser un UUID différent par client,
> sinon GFE/Sunshine ne permettra pas de quit/overwrite la session d'un autre client.
>
> **L'identité réelle du client** est le certificat X.509 (RSA 2048 bits, CN="NVIDIA
> GameStream Client"). Ce certificat est : (1) envoyé en hex dans la phase 1 du pairing,
> (2) présenté lors du handshake TLS pour toutes les requêtes HTTPS. Le `uniqueid`
> n'est qu'un paramètre HTTP partagé, pas une clé d'identité.

---

## Phase 1 : Découverte du serveur

### `GET /serverinfo`

Disponible en HTTP (non authentifié) et HTTPS (client appairé).

**Requête HTTP (découverte initiale) :**
```
GET http://<host>:47989/serverinfo
```

**Requête HTTPS (client déjà appairé) :**
```
GET https://<host>:47990/serverinfo?uniqueid=<client_unique_id>
```

**Réponse XML :**
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
</root>
```

**Champs importants :**

| Champ                  | Description                                                                 |
|------------------------|-----------------------------------------------------------------------------|
| `appversion`           | Version du protocole. Le `-1` en 4e position indique Sunshine (pas GFE)    |
| `uniqueid`             | UUID stable du serveur, utilisé pour l'identifier                           |
| `ServerCodecModeSupport` | Bitmask des codecs supportés (voir tableau ci-dessous)                   |
| `PairStatus`           | `1` si le client est appairé (HTTPS uniquement), `0` sinon                 |
| `currentgame`          | ID de l'app en cours. **Workaround GFE 2.8+** : forcé à `0` si `state` ne se termine pas par `_SERVER_BUSY` |
| `state`                | `SUNSHINE_SERVER_FREE` ou `SUNSHINE_SERVER_BUSY`. Pour GFE/RTX Experience, contient `MJOLNIR_*` |
| `mac`                  | Adresse MAC réelle (HTTPS) ou `00:00:00:00:00:00` (HTTP)                   |
| `gputype`              | Modèle du GPU serveur (ex: `NVIDIA GeForce RTX 3080`)                      |
| `ExternalPort`         | Port HTTP WAN (extension Sunshine, pas dans GFE)                           |
| `ExternalIP`           | Adresse IP WAN (extension Sunshine, pas dans GFE)                          |
| `GsVersion`            | Version du GameStream (GFE uniquement, pas toujours présent)                |

La réponse contient aussi une liste d'éléments `<DisplayMode>` avec les champs
`Width`, `Height`, `RefreshRate` pour chaque mode d'affichage supporté.

**Bitmask `ServerCodecModeSupport` :**

| Bit (valeur) | Codec                    |
|--------------|--------------------------|
| `0x0001`     | H.264                    |
| `0x0002`     | H.264 High 4:4:4         |
| `0x0100`     | HEVC Main 8-bit          |
| `0x0200`     | HEVC Main10 (HDR)        |
| `0x0400`     | HEVC RExt 8-bit 4:4:4    |
| `0x0800`     | HEVC RExt 10-bit 4:4:4   |
| `0x10000`    | AV1 Main 8-bit           |
| `0x20000`    | AV1 Main10 (HDR)         |
| `0x40000`    | AV1 High 8-bit 4:4:4     |
| `0x80000`    | AV1 High 10-bit 4:4:4    |

---

## Phase 2 : Appairage (Pairing)

L'appairage est un protocole en **4 phases** basé sur un PIN à 4 chiffres.
Il utilise AES-ECB pour le chiffrement symétrique et RSA/SHA-256 pour les signatures.

> **Important :** La requête de la Phase 1 est **bloquante** côté serveur.
> Sunshine attend que l'utilisateur entre le PIN dans son interface web avant de répondre.

```
GET /pair?uniqueid=<id>&phrase=getservercert&...   ← bloque jusqu'au PIN
```

> **Important : les 4 phases de pairing s'effectuent en HTTP (port 47989), pas en HTTPS.**
> Seule la vérification finale `pairchallenge` utilise HTTPS pour confirmer que le TLS
> mutuel fonctionne avec le certificat serveur récupéré.
>
> Toutes les requêtes de pairing incluent `devicename=roth&updateState=1` (constante moonlight-qt).
>
> En cas d'échec à n'importe quelle phase, appeler `/unpair` pour annuler la session
> de pairing côté serveur avant de recommencer.
>
> **Dérivation de la clé AES :**
> ```
> AES_KEY = SHA256(salt_bytes + PIN_bytes)[:16]
> ```
> Pour les serveurs Gen 7+ (Sunshine, GFE récent) : SHA-256. Pour Gen < 7 : SHA-1.

### Diagramme de séquence

```
Client Moonlight                    Sunshine (HTTP)
       |                                |
       |-- GET /pair?devicename=roth&updateState=1 -->|
       |       &phrase=getservercert&salt=...         |
       |       &clientcert=...                        |
       |                                |  (Sunshine attend le PIN de l'utilisateur)
       |                                |  [utilisateur entre le PIN dans l'UI web]
       |<-- XML: plaincert (cert serveur) --|
       |                                |
       |-- GET /pair?devicename=roth&updateState=1 -->|
       |       &clientchallenge=...                   |
       |<-- XML: challengeresponse ---------|
       |                                |
       |-- GET /pair?devicename=roth&updateState=1 -->|
       |       &serverchallengeresp=...                |
       |<-- XML: pairingsecret ----------|
       |                                |
       |-- GET /pair?devicename=roth&updateState=1 -->|
       |       &clientpairingsecret=...                |
       |<-- XML: paired=1 ou 0 ----------|
       |                                |
       |-- GET /pair?devicename=roth&updateState=1 -->|  (HTTPS !)
       |       &phrase=pairchallenge                   |  (vérification TLS mutuel)
       |<-- XML: paired=1 --------------|
       |                                |
       |  [Nouveau certificat serveur persistant]
```

---

### Phase 1 — `getservercert`

**Requête (HTTP) :**
```
GET /pair?devicename=roth&updateState=1&phrase=getservercert
         &clientcert=<cert_hex>&salt=<16_bytes_hex>
```

| Paramètre   | Description                                                    |
|-------------|----------------------------------------------------------------|
| `uniqueid`  | Identifiant unique du client (UUID généré par le client)       |
| `clientcert`| Certificat X.509 du client encodé en hexadécimal              |
| `salt`      | 16 octets aléatoires en hex, utilisés pour dériver la clé AES |

**Dérivation de la clé AES :**
```
AES_KEY = SHA256(salt_bytes + PIN_bytes)[:16]
```

**Réponse (après saisie du PIN) :**
```xml
<root status_code="200">
  <paired>1</paired>
  <plaincert>CERT_SERVEUR_EN_HEX</plaincert>
</root>
```

---

### Phase 2 — `clientchallenge`

Le client envoie un challenge AES-ECB chiffré avec la clé dérivée.

**Requête (HTTP) :**
```
GET /pair?devicename=roth&updateState=1&clientchallenge=<16_bytes_AES_chiffré_hex>
```

**Traitement côté serveur :**
1. Déchiffre le challenge avec la clé AES
2. Calcule : `SHA256(challenge_déchiffré + signature_cert_serveur + server_secret_aléatoire)`
3. Chiffre `(hash + server_challenge_aléatoire)` avec AES

**Réponse :**
```xml
<root status_code="200">
  <paired>1</paired>
  <challengeresponse>HASH_CHIFFRÉ_HEX</challengeresponse>
</root>
```

---

### Phase 3 — `serverchallengeresp`

Le client envoie sa réponse au challenge serveur.

**Requête (HTTP) :**
```
GET /pair?devicename=roth&updateState=1&serverchallengeresp=<AES_chiffré_hex>
```

**Contenu chiffré (client_hash) :**
```
AES_ECB_encrypt(
  SHA256(server_challenge + signature_cert_client + client_secret)
)
```

**Traitement côté serveur :**
- Déchiffre et stocke le `client_hash`
- Signe `server_secret` avec sa clé privée RSA

**Réponse :**
```xml
<root status_code="200">
  <paired>1</paired>
  <pairingsecret>SERVER_SECRET + RSA_SIGNATURE_HEX</pairingsecret>
</root>
```

---

### Phase 4 — `clientpairingsecret` (finale)

**Requête (HTTP) :**
```
GET /pair?devicename=roth&updateState=1&clientpairingsecret=<secret_16bytes + signature_RSA_hex>
```

**Vérification côté serveur :**
1. Extrait `client_secret` (16 premiers octets) et `client_signature` (reste)
2. Vérifie : `SHA256(server_challenge + sig_cert_client + client_secret) == client_hash`
3. Vérifie la signature RSA du `client_secret` avec la clé publique du certificat client

**Réponse :**
```xml
<root status_code="200">
  <paired>1</paired>   <!-- ou 0 si échec -->
</root>
```

Si `paired=1`, le certificat client est ajouté à la chaîne de confiance et persisté
dans `sunshine_state.json`.

---

### Vérification post-appairage — `pairchallenge`

Après un appairage réussi (ou lors de reconnexions), le client vérifie que le TLS
mutuel fonctionne. **Cette requête est en HTTPS** (contrairement aux 4 phases précédentes).

```
GET /pair?uniqueid=0123456789ABCDEF&uuid=...&devicename=roth&updateState=1&phrase=pairchallenge
```

**Réponse :**
```xml
<root status_code="200">
  <paired>1</paired>
</root>
```

---

### GET /unpair

Utilisé pour annuler une session de pairing en cours (en cas d'échec à n'importe
quelle phase du pairing). HTTP.

```
GET http://<host>:47989/unpair
```

---

### Authentification TLS mutuelle

Une fois appairé, toutes les requêtes HTTPS utilisent l'**authentification mutuelle TLS**.
Le client présente son certificat X.509 lors du handshake TLS. Si la vérification échoue :

```xml
<root status_code="401"
      query="/applist"
      status_message="The client is not authorized. Certificate verification failed." />
```

---

## Phase 3 : Liste des applications

### `GET /applist`

**Requête (HTTPS, client appairé) :**
```
GET https://<host>:47990/applist
```

**Réponse :**
```xml
<root status_code="200">
  <App>
    <IsHdrSupported>1</IsHdrSupported>
    <AppTitle>Desktop</AppTitle>
    <ID>1</ID>
    <IsAppCollectorGame>0</IsAppCollectorGame>
  </App>
  <App>
    <IsHdrSupported>0</IsHdrSupported>
    <AppTitle>Mon Jeu</AppTitle>
    <ID>2</ID>
    <IsAppCollectorGame>0</IsAppCollectorGame>
  </App>
</root>
```

> La doc officielle GFE mentionne aussi `IsAppCollectorGame` (booléen, GFE uniquement).
> Sunshine peut ne pas inclure ce champ.

### `GET /appasset`

Récupère l'image de couverture d'une application (PNG).

```
GET https://<host>:47990/appasset?appid=<id>
```

Réponse : image PNG binaire avec `Content-Type: image/png`.

---

## Phase 4 : Lancement d'une session

### `GET /launch`

Lance une application et initie une session de streaming.

**Requête :**
```
GET https://<host>:47990/launch
    ?rikey=<AES_GCM_key_hex>
    &rikeyid=<key_id_int>
    &appid=<app_id>
    &mode=<width>x<height>x<fps>
    &additionalStates=1
    &localAudioPlayMode=<0|1>
    &sops=<0|1>
    &surroundAudioInfo=<int>
    &gcmap=<int>
    &remoteControllersBitmap=<int>
    &gcpersist=<0|1>
    &hdrMode=<0|1>
    &corever=<int>
```

**Paramètres importants :**

| Paramètre          | Description                                                          |
|--------------------|----------------------------------------------------------------------|
| `rikey`            | Clé AES-GCM 128-bit pour chiffrer les flux (hex)                    |
| `rikeyid`          | ID de la clé, utilisé comme IV initial (big-endian uint32)          |
| `appid`            | ID de l'application à lancer (`0` = bureau)                         |
| `mode`             | Résolution et FPS ex: `1920x1080x60`                                |
| `localAudioPlayMode` | `1` = audio joué sur le serveur, `0` = audio envoyé au client     |
| `sops`             | Server-side Optimal Playback Settings                               |
| `additionalStates` | Toujours `1`                                                       |
| `surroundAudioInfo` | Info surround (canaux en bits bas, flags en bits hauts)            |
| `remoteControllersBitmap` | Bitmap des manettes connectées (même valeur que `gcmap`)     |
| `gcmap`            | Gamepad capabilities map                                            |
| `gcpersist`        | Persist game controllers on disconnect (`0` ou `1`)                 |
| `hdrMode`          | `1` = demande HDR. Les 4 paramètres `clientHdrCap*` suivants ne sont envoyés que si `hdrMode=1` |
| `clientHdrCapVersion` | Version capacité HDR (conditionnel : hdrMode=1)                 |
| `clientHdrCapSupportedFlagsInUint32` | Flags HDR (conditionnel : hdrMode=1)             |
| `clientHdrCapMetaDataId` | Metadata ID HDR (conditionnel : hdrMode=1)                    |
| `clientHdrCapDisplayData` | Display data HDR (conditionnel : hdrMode=1)                   |
| `corever`          | Version du protocole core. `>= 1` active le RTSP chiffré. Ajouté par `LiGetLaunchUrlQueryParameters()` |

**Réponse succès :**
```xml
<root status_code="200">
  <sessionUrl0>rtsp://192.168.1.x:48010</sessionUrl0>
  <gamesession>1</gamesession>
</root>
```

> Si `corever >= 1`, l'URL sera `rtspenc://...` indiquant un RTSP chiffré avec AES-GCM.

**Réponses d'erreur possibles :**

| status_code | Cause                                              |
|-------------|----------------------------------------------------|
| 400         | Paramètres manquants ou app déjà en cours          |
| 403         | Chiffrement obligatoire non supporté par le client |
| 503         | Échec d'initialisation de l'encodeur vidéo         |

---

### `GET /resume`

Reprend une session existante (reconnexion).

```
GET https://<host>:47990/resume
    ?rikey=<key_hex>
    &rikeyid=<id>
    [&localAudioPlayMode=<0|1>]
```

**Réponse :**
```xml
<root status_code="200">
  <sessionUrl0>rtsp://192.168.1.x:48010</sessionUrl0>
  <resume>1</resume>
</root>
```

---

### `GET /cancel`

Arrête la session et l'application en cours.

```
GET https://<host>:47990/cancel
```

**Réponse :**
```xml
<root status_code="200">
  <cancel>1</cancel>
</root>
```

---

## Phase 5 : Négociation RTSP

Après `/launch` ou `/resume`, le client se connecte à l'URL `sessionUrl0` retournée.
La séquence RTSP standard est :

```
Client                          Sunshine (port 48010)
  |-- OPTIONS rtsp://... -->        |
  |<-- 200 OK ----------------      |
  |                                 |
  |-- DESCRIBE rtsp://... -->       |
  |<-- 200 OK + SDP payload --      |  (capacités: codecs, audio, chiffrement)
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
  |-- ANNOUNCE + SDP config -->     |  (config vidéo/audio détaillée)
  |<-- 200 OK ----------------      |
  |                                 |
  |-- PLAY -->                      |
  |<-- 200 OK ----------------      |
  |                                 |
  |  [streaming UDP démarre]        |
```

### Réponse DESCRIBE (SDP)

Le payload SDP contient les capacités du serveur :

```
a=x-ss-general.featureFlags:<uint32>
a=x-ss-general.encryptionSupported:<flags>
a=x-ss-general.encryptionRequested:<flags>
a=x-nv-video[0].refPicInvalidation:1
sprop-parameter-sets=AAAAAU          (si HEVC supporté)
a=rtpmap:98 AV1/90000                (si AV1 supporté)
a=fmtp:97 surround-params=...        (configs audio surround)
```

**Flags de chiffrement :**

| Flag              | Valeur | Description                    |
|-------------------|--------|--------------------------------|
| `SS_ENC_CONTROL_V2` | `0x1` | Chiffrement du canal de contrôle |
| `SS_ENC_VIDEO`    | `0x2`  | Chiffrement vidéo              |
| `SS_ENC_AUDIO`    | `0x4`  | Chiffrement audio              |

> Ces valeurs correspondent aux définitions de moonlight-common-c (`Limelight-internal.h`).
> Attention : l'ordre des bits diffère de celui des flags `ENCFLG_*` utilisés par `LiStartConnection()`.

### Requête ANNOUNCE (SDP du client)

Le client envoie sa configuration dans le payload SDP de l'ANNOUNCE :

```
s=<client_unique_id>
a=x-nv-video[0].clientViewportWd:<width>
a=x-nv-video[0].clientViewportHt:<height>
a=x-nv-video[0].maxFPS:<fps>
a=x-nv-video[0].packetSize:<bytes>
a=x-nv-video[0].maxNumReferenceFrames:<n>
a=x-nv-video[0].encoderCscMode:<mode>
a=x-nv-video[0].dynamicRangeMode:<0|1>
a=x-nv-video[0].clientRefreshRateX100:<rate>
a=x-nv-vqos[0].bw.maximumBitrateKbps:<kbps>
a=x-nv-vqos[0].bitStreamFormat:<0=H264|1=HEVC|2=AV1>
a=x-nv-vqos[0].fec.minRequiredFecPackets:<n>
a=x-nv-vqos[0].qosTrafficType:<type>
a=x-nv-audio.surround.numChannels:<2|6|8>
a=x-nv-audio.surround.channelMask:<mask>
a=x-nv-audio.surround.AudioQuality:<0|1>
a=x-nv-aqos.packetDuration:<ms>
a=x-nv-aqos.qosTrafficType:<type>
a=x-nv-general.useReliableUdp:<0|1>
a=x-nv-general.featureFlags:<flags>
a=x-ml-general.featureFlags:<flags>
a=x-ml-video.configuredBitrateKbps:<kbps>
a=x-ss-general.encryptionEnabled:<flags>
a=x-ss-video[0].chromaSamplingType:<0=4:2:0|1=4:4:4>
a=x-ss-video[0].intraRefresh:<0|1>
```

**Format vidéo (`bitStreamFormat`) :**

| Valeur | Codec |
|--------|-------|
| `0`    | H.264 |
| `1`    | HEVC  |
| `2`    | AV1   |

---

## Détail des réponses SETUP

Chaque réponse SETUP contient un header `Transport` au format :
```
Transport: unicast;server_port=<port>-<port+1>;source=<ip_serveur>
```

Le client parse le `server_port` pour déterminer le port UDP de chaque flux.
Les headers additionnels inclus :

| Type de flux | Header additionnel |
|---|---|
| Video | `X-SS-Ping-Payload` (8 bytes hex, généré aléatoirement par le serveur) |
| Audio | `X-SS-Ping-Payload` (identique au payload vidéo) |
| Control | `X-SS-Connect-Data` (uint32, identifiant de session de contrôle) |

---

## Flux de streaming UDP

Après le PLAY RTSP, trois flux UDP sont établis. **Les ports sont assignés dynamiquement
par le serveur via le header `Transport` de chaque réponse SETUP RTSP.** Les valeurs
ci-dessous sont les ports par défaut de Sunshine :

### Flux vidéo (port 47998)
- Paquets RTP avec FEC Reed-Solomon
- Chiffrement AES-GCM optionnel (si `SS_ENC_VIDEO` activé)
- Clé : `rikey` fourni dans `/launch`

### Flux audio (port 47999)
- Paquets RTP avec FEC
- Codec Opus
- Chiffrement AES-CBC avec la même `rikey`
- IV initial : `rikeyid` (big-endian)

### Canal de contrôle (port 48000)
- Entrées clavier, souris, gamepad
- Chiffrement AES-GCM (si `SS_ENC_CONTROL_V2` activé)
- Identifié par `X-SS-Connect-Data` reçu lors du SETUP RTSP

> **Note sur les fallbacks moonlight-common-c** : Si le parsing du header `Transport`
> échoue, le client applique ces defaults : vidéo=47998, audio=48000, contrôle=47999
> (les ports audio et contrôle sont inversés par rapport à la configuration Sunshine
> standard). En pratique, le serveur spécifie toujours les ports corrects.

---

## Flux complet d'une session

```
1. Découverte    GET /serverinfo (HTTP:47989)
2. Appairage     GET /pair × 4 phases (HTTP ou HTTPS)
                 [PIN entré dans l'UI Sunshine]
3. Liste apps    GET /applist (HTTPS:47990)
4. Lancement     GET /launch?appid=...&rikey=...&mode=... (HTTPS:47990)
5. RTSP          OPTIONS → DESCRIBE → SETUP×3 → ANNOUNCE → PLAY (TCP:48010)
6. Streaming     Flux UDP vidéo/audio/contrôle (UDP:47998-48000)
7. Arrêt         GET /cancel (HTTPS:47990)
```

---

## API REST Sunshine (gestion/configuration)

Sunshine expose aussi une API REST moderne sur le **même port HTTPS** (47990 par défaut).
Cette API n'est **pas utilisée par MoonlightWeb pour le streaming**, mais est documentée
ici pour référence (pourrait servir à l'avenir pour l'envoi automatique du PIN).

### Authentification

- Basic Auth (identifiants du compte administrateur Sunshine)
- TLS simple (pas de certificat client)

### Endpoints

| Méthode | Endpoint | Description |
|---------|----------|-------------|
| GET | `/api/apps` | Liste des applications (JSON) |
| POST | `/api/apps` | Ajouter/modifier une application |
| POST | `/api/apps/close` | Fermer l'application en cours |
| DELETE | `/api/apps/{index}` | Supprimer une application |
| GET | `/api/clients/list` | Liste des clients appairés |
| POST | `/api/clients/unpair` | Dépairer un client |
| POST | `/api/clients/unpair-all` | Dépairer tous les clients |
| GET | `/api/config` | Configuration complète |
| POST | `/api/config` | Mettre à jour la configuration |
| POST | `/api/pin` | Envoyer le PIN de pairing |
| POST | `/api/password` | Changer le mot de passe |
| GET | `/api/logs` | Logs du serveur |
| POST | `/api/restart` | Redémarrer Sunshine |

Documentation officielle : https://docs.lizardbyte.dev/projects/sunshine/latest/md_docs_2api.html

---

## Notes d'implémentation pour MoonlightWeb

- **`uniqueid` et `uuid`** : envoyés dans TOUTES les requêtes HTTP et HTTPS.
  `uniqueid` = `0123456789ABCDEF` (valeur fixe partagée), `uuid` = UUID v4 aléatoire
  généré à chaque requête.
- **Certificat client** : Généré une fois (RSA 2048 bits, X.509 auto-signé, CN="NVIDIA GameStream Client"),
  présenté lors de l'appairage (phase 1, encodé en hex) et dans chaque connexion TLS mutuelle (HTTPS).
  C'est la véritable identité du client — le `uniqueid` n'est qu'un paramètre HTTP partagé.
- **Pairing en HTTP** : Les 4 premières phases sont en HTTP (port 47989), pas en HTTPS.
  Seule la vérification `pairchallenge` finale est en HTTPS.
- **`devicename=roth&updateState=1`** : envoyé dans toutes les requêtes de pairing.
- **`/unpair`** : appeler en cas d'échec de pairing pour nettoyer la session.
- **`rikey` / `rikeyid`** : Générés aléatoirement à chaque session de streaming.
  Ne pas réutiliser entre sessions.
- **`corever >= 1`** : Active le RTSP chiffré (`rtspenc://`) + chiffrement vidéo
  et contrôle. Ajouté automatiquement par `LiGetLaunchUrlQueryParameters()`.
  Toujours `1` pour les clients modernes.
- **Port HTTPS dynamique** : Résolu via le champ `<HttpsPort>` de `/serverinfo`.
  Fallback : `DEFAULT_HTTPS_PORT = 47984` (comportement moonlight-qt).
- **Serialisation par host** : Une seule requête HTTPS sortante à la fois par host
  Sunshine (sinon "Operation canceled" / échec GFE).
- **Ordre des phases d'appairage** : Strictement imposé. Toute requête hors ordre
  retourne `paired=0` et invalide la session.
- **Version du protocole** : Le `appversion` `7.1.431.-1` est fixe. Le `-1` en
  4e position est le signal que le serveur est Sunshine et non GFE.

---

Voici les références dans le code source pour chaque partie :

**Endpoints GameStream** : [1](#0-0) 

**Réponse `/serverinfo`** : [2](#0-1) 

**Version du protocole** : [3](#0-2) 

**Phases d'appairage (enum)** : [4](#0-3) 

**Structure `pair_session_t`** : [5](#0-4) 

**Phase 1 — `getservercert`** : [6](#0-5) 

**Phase 2 — `clientchallenge`** : [7](#0-6) 

**Phase 3 — `serverchallengeresp`** : [8](#0-7) 

**Phase 4 — `clientpairingsecret`** : [9](#0-8) 

**Routage des phases dans `/pair`** : [10](#0-9) 

**Paramètres de `/launch`** : [11](#0-10) 

**Réponse `/launch`** : [12](#0-11) 

**RTSP DESCRIBE (SDP serveur)** : [13](#0-12) 

**RTSP SETUP (ports UDP)** : [14](#0-13) 

**RTSP ANNOUNCE (config client)** : [15](#0-14) 

**Port RTSP** : [16](#0-15) 

**Structure `launch_session_t`** : [17](#0-16)

### Citations

**File:** src/nvhttp.cpp (L282-338)
```cpp
  std::shared_ptr<rtsp_stream::launch_session_t> make_launch_session(bool host_audio, const args_t &args) {
    auto launch_session = std::make_shared<rtsp_stream::launch_session_t>();

    launch_session->id = ++session_id_counter;

    auto rikey = util::from_hex_vec(get_arg(args, "rikey"), true);
    std::copy(rikey.cbegin(), rikey.cend(), std::back_inserter(launch_session->gcm_key));

    launch_session->host_audio = host_audio;
    std::stringstream mode = std::stringstream(get_arg(args, "mode", "0x0x0"));
    // Split mode by the char "x", to populate width/height/fps
    int x = 0;
    std::string segment;
    while (std::getline(mode, segment, 'x')) {
      if (x == 0) {
        launch_session->width = atoi(segment.c_str());
      }
      if (x == 1) {
        launch_session->height = atoi(segment.c_str());
      }
      if (x == 2) {
        launch_session->fps = atoi(segment.c_str());
      }
      x++;
    }
    launch_session->unique_id = (get_arg(args, "uniqueid", "unknown"));
    launch_session->appid = (int) util::from_view(get_arg(args, "appid", "unknown"));
    launch_session->enable_sops = util::from_view(get_arg(args, "sops", "0"));
    launch_session->surround_info = (int) util::from_view(get_arg(args, "surroundAudioInfo", "196610"));
    launch_session->surround_params = (get_arg(args, "surroundParams", ""));
    launch_session->continuous_audio = util::from_view(get_arg(args, "continuousAudio", "0"));
    launch_session->gcmap = (int) util::from_view(get_arg(args, "gcmap", "0"));
    launch_session->enable_hdr = util::from_view(get_arg(args, "hdrMode", "0"));

    // Encrypted RTSP is enabled with client reported corever >= 1
    auto corever = util::from_view(get_arg(args, "corever", "0"));
    if (corever >= 1) {
      launch_session->rtsp_cipher = crypto::cipher::gcm_t {
        launch_session->gcm_key,
        false
      };
      launch_session->rtsp_iv_counter = 0;
    }
    launch_session->rtsp_url_scheme = launch_session->rtsp_cipher ? "rtspenc://"s : "rtsp://"s;

    // Generate the unique identifiers for this connection that we will send later during RTSP handshake
    unsigned char raw_payload[8];
    RAND_bytes(raw_payload, sizeof(raw_payload));
    launch_session->av_ping_payload = util::hex_vec(raw_payload);
    RAND_bytes((unsigned char *) &launch_session->control_connect_data, sizeof(launch_session->control_connect_data));

    launch_session->iv.resize(16);
    uint32_t prepend_iv = util::endian::big<uint32_t>((int) util::from_view(get_arg(args, "rikeyid")));
    auto prepend_iv_p = (uint8_t *) &prepend_iv;
    std::copy(prepend_iv_p, prepend_iv_p + sizeof(prepend_iv), std::begin(launch_session->iv));
    return launch_session;
  }
```

**File:** src/nvhttp.cpp (L351-373)
```cpp
  void getservercert(pair_session_t &sess, pt::ptree &tree, const std::string &pin) {
    if (sess.last_phase != PAIR_PHASE::NONE) {
      fail_pair(sess, tree, "Out of order call to getservercert");
      return;
    }
    sess.last_phase = PAIR_PHASE::GETSERVERCERT;

    if (sess.async_insert_pin.salt.size() < 32) {
      fail_pair(sess, tree, "Salt too short");
      return;
    }

    std::string_view salt_view {sess.async_insert_pin.salt.data(), 32};

    auto salt = util::from_hex<std::array<uint8_t, 16>>(salt_view, true);

    auto key = crypto::gen_aes_key(salt, pin);
    sess.cipher_key = std::make_unique<crypto::aes_t>(key);

    tree.put("root.paired", 1);
    tree.put("root.plaincert", util::hex_vec(conf_intern.servercert, true));
    tree.put("root.<xmlattr>.status_code", 200);
  }
```

**File:** src/nvhttp.cpp (L375-416)
```cpp
  void clientchallenge(pair_session_t &sess, pt::ptree &tree, const std::string &challenge) {
    if (sess.last_phase != PAIR_PHASE::GETSERVERCERT) {
      fail_pair(sess, tree, "Out of order call to clientchallenge");
      return;
    }
    sess.last_phase = PAIR_PHASE::CLIENTCHALLENGE;

    if (!sess.cipher_key) {
      fail_pair(sess, tree, "Cipher key not set");
      return;
    }
    crypto::cipher::ecb_t cipher(*sess.cipher_key, false);

    std::vector<uint8_t> decrypted;
    cipher.decrypt(challenge, decrypted);

    auto x509 = crypto::x509(conf_intern.servercert);
    auto sign = crypto::signature(x509);
    auto serversecret = crypto::rand(16);

    decrypted.insert(std::end(decrypted), std::begin(sign), std::end(sign));
    decrypted.insert(std::end(decrypted), std::begin(serversecret), std::end(serversecret));

    auto hash = crypto::hash({(char *) decrypted.data(), decrypted.size()});
    auto serverchallenge = crypto::rand(16);

    std::string plaintext;
    plaintext.reserve(hash.size() + serverchallenge.size());

    plaintext.insert(std::end(plaintext), std::begin(hash), std::end(hash));
    plaintext.insert(std::end(plaintext), std::begin(serverchallenge), std::end(serverchallenge));

    std::vector<uint8_t> encrypted;
    cipher.encrypt(plaintext, encrypted);

    sess.serversecret = std::move(serversecret);
    sess.serverchallenge = std::move(serverchallenge);

    tree.put("root.paired", 1);
    tree.put("root.challengeresponse", util::hex_vec(encrypted, true));
    tree.put("root.<xmlattr>.status_code", 200);
  }
```

**File:** src/nvhttp.cpp (L418-445)
```cpp
  void serverchallengeresp(pair_session_t &sess, pt::ptree &tree, const std::string &encrypted_response) {
    if (sess.last_phase != PAIR_PHASE::CLIENTCHALLENGE) {
      fail_pair(sess, tree, "Out of order call to serverchallengeresp");
      return;
    }
    sess.last_phase = PAIR_PHASE::SERVERCHALLENGERESP;

    if (!sess.cipher_key || sess.serversecret.empty()) {
      fail_pair(sess, tree, "Cipher key or serversecret not set");
      return;
    }

    std::vector<uint8_t> decrypted;
    crypto::cipher::ecb_t cipher(*sess.cipher_key, false);

    cipher.decrypt(encrypted_response, decrypted);

    sess.clienthash = std::move(decrypted);

    auto serversecret = sess.serversecret;
    auto sign = crypto::sign256(crypto::pkey(conf_intern.pkey), serversecret);

    serversecret.insert(std::end(serversecret), std::begin(sign), std::end(sign));

    tree.put("root.pairingsecret", util::hex_vec(serversecret, true));
    tree.put("root.paired", 1);
    tree.put("root.<xmlattr>.status_code", 200);
  }
```

**File:** src/nvhttp.cpp (L447-495)
```cpp
  void clientpairingsecret(pair_session_t &sess, std::shared_ptr<safe::queue_t<crypto::x509_t>> &add_cert, pt::ptree &tree, const std::string &client_pairing_secret) {
    if (sess.last_phase != PAIR_PHASE::SERVERCHALLENGERESP) {
      fail_pair(sess, tree, "Out of order call to clientpairingsecret");
      return;
    }
    sess.last_phase = PAIR_PHASE::CLIENTPAIRINGSECRET;

    auto &client = sess.client;

    if (client_pairing_secret.size() <= 16) {
      fail_pair(sess, tree, "Client pairing secret too short");
      return;
    }

    std::string_view secret {client_pairing_secret.data(), 16};
    std::string_view sign {client_pairing_secret.data() + secret.size(), client_pairing_secret.size() - secret.size()};

    auto x509 = crypto::x509(client.cert);
    if (!x509) {
      fail_pair(sess, tree, "Invalid client certificate");
      return;
    }
    auto x509_sign = crypto::signature(x509);

    std::string data;
    data.reserve(sess.serverchallenge.size() + x509_sign.size() + secret.size());

    data.insert(std::end(data), std::begin(sess.serverchallenge), std::end(sess.serverchallenge));
    data.insert(std::end(data), std::begin(x509_sign), std::end(x509_sign));
    data.insert(std::end(data), std::begin(secret), std::end(secret));

    auto hash = crypto::hash(data);

    // if hash not correct, probably MITM
    bool same_hash = hash.size() == sess.clienthash.size() && std::equal(hash.begin(), hash.end(), sess.clienthash.begin());
    auto verify = crypto::verify256(crypto::x509(client.cert), secret, sign);
    if (same_hash && verify) {
      tree.put("root.paired", 1);
      add_cert->raise(crypto::x509(client.cert));

      // The client is now successfully paired and will be authorized to connect
      add_authorized_client(client.name, std::move(client.cert));
    } else {
      tree.put("root.paired", 0);
    }

    remove_session(sess);
    tree.put("root.<xmlattr>.status_code", 200);
  }
```

**File:** src/nvhttp.cpp (L563-629)
```cpp
    auto args = request->parse_query_string();
    if (args.find("uniqueid"s) == std::end(args)) {
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Missing uniqueid parameter");

      return;
    }

    auto uniqID {get_arg(args, "uniqueid")};

    args_t::const_iterator it;
    if (it = args.find("phrase"); it != std::end(args)) {
      if (it->second == "getservercert"sv) {
        pair_session_t sess;

        sess.client.uniqueID = std::move(uniqID);
        sess.client.cert = util::from_hex_vec(get_arg(args, "clientcert"), true);

        BOOST_LOG(debug) << sess.client.cert;
        auto ptr = map_id_sess.emplace(sess.client.uniqueID, std::move(sess)).first;

        ptr->second.async_insert_pin.salt = std::move(get_arg(args, "salt"));
        if (config::sunshine.flags[config::flag::PIN_STDIN]) {
          std::string pin;

          std::cout << "Please insert pin: "sv;
          std::getline(std::cin, pin);

          getservercert(ptr->second, tree, pin);
        } else {
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
          system_tray::update_tray_require_pin();
#endif
          ptr->second.async_insert_pin.response = std::move(response);

          fg.disable();
          return;
        }
      } else if (it->second == "pairchallenge"sv) {
        tree.put("root.paired", 1);
        tree.put("root.<xmlattr>.status_code", 200);
        return;
      }
    }

    auto sess_it = map_id_sess.find(uniqID);
    if (sess_it == std::end(map_id_sess)) {
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Invalid uniqueid");

      return;
    }

    if (it = args.find("clientchallenge"); it != std::end(args)) {
      auto challenge = util::from_hex_vec(it->second, true);
      clientchallenge(sess_it->second, tree, challenge);
    } else if (it = args.find("serverchallengeresp"); it != std::end(args)) {
      auto encrypted_response = util::from_hex_vec(it->second, true);
      serverchallengeresp(sess_it->second, tree, encrypted_response);
    } else if (it = args.find("clientpairingsecret"); it != std::end(args)) {
      auto pairingsecret = util::from_hex_vec(it->second, true);
      clientpairingsecret(sess_it->second, add_cert, tree, pairingsecret);
    } else {
      tree.put("root.<xmlattr>.status_code", 404);
      tree.put("root.<xmlattr>.status_message", "Invalid pairing request");
    }
  }
```

**File:** src/nvhttp.cpp (L697-763)
```cpp
    tree.put("root.<xmlattr>.status_code", 200);
    tree.put("root.hostname", config::nvhttp.sunshine_name);

    tree.put("root.appversion", VERSION);
    tree.put("root.GfeVersion", GFE_VERSION);
    tree.put("root.uniqueid", http::unique_id);
    tree.put("root.HttpsPort", net::map_port(PORT_HTTPS));
    tree.put("root.ExternalPort", net::map_port(PORT_HTTP));
    tree.put("root.MaxLumaPixelsHEVC", video::active_hevc_mode > 1 ? "1869449984" : "0");

    // Only include the MAC address for requests sent from paired clients over HTTPS.
    // For HTTP requests, use a placeholder MAC address that Moonlight knows to ignore.
    if constexpr (std::is_same_v<SunshineHTTPS, T>) {
      tree.put("root.mac", platf::get_mac_address(net::addr_to_normalized_string(local_endpoint.address())));
    } else {
      tree.put("root.mac", "00:00:00:00:00:00");
    }

    // Moonlight clients track LAN IPv6 addresses separately from LocalIP which is expected to
    // always be an IPv4 address. If we return that same IPv6 address here, it will clobber the
    // stored LAN IPv4 address. To avoid this, we need to return an IPv4 address in this field
    // when we get a request over IPv6.
    //
    // HACK: We should return the IPv4 address of local interface here, but we don't currently
    // have that implemented. For now, we will emulate the behavior of GFE+GS-IPv6-Forwarder,
    // which returns 127.0.0.1 as LocalIP for IPv6 connections. Moonlight clients with IPv6
    // support know to ignore this bogus address.
    if (local_endpoint.address().is_v6() && !local_endpoint.address().to_v6().is_v4_mapped()) {
      tree.put("root.LocalIP", "127.0.0.1");
    } else {
      tree.put("root.LocalIP", net::addr_to_normalized_string(local_endpoint.address()));
    }

    uint32_t codec_mode_flags = SCM_H264;
    if (video::last_encoder_probe_supported_yuv444_for_codec[0]) {
      codec_mode_flags |= SCM_H264_HIGH8_444;
    }
    if (video::active_hevc_mode >= 2) {
      codec_mode_flags |= SCM_HEVC;
      if (video::last_encoder_probe_supported_yuv444_for_codec[1]) {
        codec_mode_flags |= SCM_HEVC_REXT8_444;
      }
    }
    if (video::active_hevc_mode >= 3) {
      codec_mode_flags |= SCM_HEVC_MAIN10;
      if (video::last_encoder_probe_supported_yuv444_for_codec[1]) {
        codec_mode_flags |= SCM_HEVC_REXT10_444;
      }
    }
    if (video::active_av1_mode >= 2) {
      codec_mode_flags |= SCM_AV1_MAIN8;
      if (video::last_encoder_probe_supported_yuv444_for_codec[2]) {
        codec_mode_flags |= SCM_AV1_HIGH8_444;
      }
    }
    if (video::active_av1_mode >= 3) {
      codec_mode_flags |= SCM_AV1_MAIN10;
      if (video::last_encoder_probe_supported_yuv444_for_codec[2]) {
        codec_mode_flags |= SCM_AV1_HIGH10_444;
      }
    }
    tree.put("root.ServerCodecModeSupport", codec_mode_flags);

    auto current_appid = proc::proc.running();
    tree.put("root.PairStatus", pair_status);
    tree.put("root.currentgame", current_appid);
    tree.put("root.state", current_appid > 0 ? "SUNSHINE_SERVER_BUSY" : "SUNSHINE_SERVER_FREE");
```

**File:** src/nvhttp.cpp (L906-918)
```cpp
    tree.put("root.<xmlattr>.status_code", 200);
    tree.put(
      "root.sessionUrl0",
      std::format(
        "{}{}:{}",
        launch_session->rtsp_url_scheme,
        net::addr_to_url_escaped_string(request->local_endpoint().address()),
        static_cast<int>(net::map_port(rtsp_stream::RTSP_SETUP_PORT))
      )
    );
    tree.put("root.gamesession", 1);

    rtsp_stream::launch_session_raise(launch_session);
```

**File:** src/nvhttp.cpp (L1146-1159)
```cpp
    https_server.default_resource["GET"] = not_found<SunshineHTTPS>;
    https_server.resource["^/serverinfo$"]["GET"] = serverinfo<SunshineHTTPS>;
    https_server.resource["^/pair$"]["GET"] = [&add_cert](auto resp, auto req) {
      pair<SunshineHTTPS>(add_cert, resp, req);
    };
    https_server.resource["^/applist$"]["GET"] = applist;
    https_server.resource["^/appasset$"]["GET"] = appasset;
    https_server.resource["^/launch$"]["GET"] = [&host_audio](auto resp, auto req) {
      launch(host_audio, resp, req);
    };
    https_server.resource["^/resume$"]["GET"] = [&host_audio](auto resp, auto req) {
      resume(host_audio, resp, req);
    };
    https_server.resource["^/cancel$"]["GET"] = cancel;
```

**File:** src/nvhttp.h (L29-35)
```text
   */
  constexpr auto VERSION = "7.1.431.-1";

  /**
   * @brief The GFE version we are replicating.
   */
  constexpr auto GFE_VERSION = "3.23.0.74";
```

**File:** src/nvhttp.h (L75-81)
```text
  enum class PAIR_PHASE {
    NONE,  ///< Sunshine is not in a pairing phase
    GETSERVERCERT,  ///< Sunshine is in the get server certificate phase
    CLIENTCHALLENGE,  ///< Sunshine is in the client challenge phase
    SERVERCHALLENGERESP,  ///< Sunshine is in the server challenge response phase
    CLIENTPAIRINGSECRET  ///< Sunshine is in the client pairing secret phase
  };
```

**File:** src/nvhttp.h (L83-108)
```text
  struct pair_session_t {
    struct {
      std::string uniqueID = {};
      std::string cert = {};
      std::string name = {};
    } client;

    std::unique_ptr<crypto::aes_t> cipher_key = {};
    std::vector<uint8_t> clienthash = {};

    std::string serversecret = {};
    std::string serverchallenge = {};

    struct {
      util::Either<
        std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTP>::Response>,
        std::shared_ptr<typename SimpleWeb::ServerBase<SunshineHTTPS>::Response>>
        response;
      std::string salt = {};
    } async_insert_pin;

    /**
     * @brief used as a security measure to prevent out of order calls
     */
    PAIR_PHASE last_phase = PAIR_PHASE::NONE;
  };
```

**File:** src/rtsp.cpp (L753-834)
```cpp
  void cmd_describe(rtsp_server_t *server, tcp::socket &sock, launch_session_t &session, msg_t &&req) {
    OPTION_ITEM option {};

    // I know these string literals will not be modified
    option.option = const_cast<char *>("CSeq");

    auto seqn_str = std::to_string(req->sequenceNumber);
    option.content = const_cast<char *>(seqn_str.c_str());

    std::stringstream ss;

    // Tell the client about our supported features
    ss << "a=x-ss-general.featureFlags:" << (uint32_t) platf::get_capabilities() << std::endl;

    // Always request new control stream encryption if the client supports it
    uint32_t encryption_flags_supported = SS_ENC_CONTROL_V2 | SS_ENC_AUDIO;
    uint32_t encryption_flags_requested = SS_ENC_CONTROL_V2;

    // Determine the encryption desired for this remote endpoint
    auto encryption_mode = net::encryption_mode_for_address(sock.remote_endpoint().address());
    if (encryption_mode != config::ENCRYPTION_MODE_NEVER) {
      // Advertise support for video encryption if it's not disabled
      encryption_flags_supported |= SS_ENC_VIDEO;

      // If it's mandatory, also request it to enable use if the client
      // didn't explicitly opt in, but it otherwise has support.
      if (encryption_mode == config::ENCRYPTION_MODE_MANDATORY) {
        encryption_flags_requested |= SS_ENC_VIDEO | SS_ENC_AUDIO;
      }
    }

    // Report supported and required encryption flags
    ss << "a=x-ss-general.encryptionSupported:" << encryption_flags_supported << std::endl;
    ss << "a=x-ss-general.encryptionRequested:" << encryption_flags_requested << std::endl;

    if (video::last_encoder_probe_supported_ref_frames_invalidation) {
      ss << "a=x-nv-video[0].refPicInvalidation:1"sv << std::endl;
    }

    if (video::active_hevc_mode != 1) {
      ss << "sprop-parameter-sets=AAAAAU"sv << std::endl;
    }

    if (video::active_av1_mode != 1) {
      ss << "a=rtpmap:98 AV1/90000"sv << std::endl;
    }

    if (!session.surround_params.empty()) {
      // If we have our own surround parameters, advertise them twice first
      ss << "a=fmtp:97 surround-params="sv << session.surround_params << std::endl;
      ss << "a=fmtp:97 surround-params="sv << session.surround_params << std::endl;
    }

    for (int x = 0; x < audio::MAX_STREAM_CONFIG; ++x) {
      auto &stream_config = audio::stream_configs[x];
      std::uint8_t mapping[platf::speaker::MAX_SPEAKERS];

      auto mapping_p = stream_config.mapping;

      /**
       * GFE advertises incorrect mapping for normal quality configurations,
       * as a result, Moonlight rotates all channels from index '3' to the right
       * To work around this, rotate channels to the left from index '3'
       */
      if (x == audio::SURROUND51 || x == audio::SURROUND71) {
        std::copy_n(mapping_p, stream_config.channelCount, mapping);
        std::rotate(mapping + 3, mapping + 4, mapping + audio::MAX_STREAM_CONFIG);

        mapping_p = mapping;
      }

      ss << "a=fmtp:97 surround-params="sv << stream_config.channelCount << stream_config.streams << stream_config.coupledStreams;

      std::for_each_n(mapping_p, stream_config.channelCount, [&ss](std::uint8_t digit) {
        ss << (char) (digit + '0');
      });

      ss << std::endl;
    }

    respond(sock, session, &option, 200, "OK", req->sequenceNumber, ss.str());
  }
```

**File:** src/rtsp.cpp (L836-893)
```cpp
  void cmd_setup(rtsp_server_t *server, tcp::socket &sock, launch_session_t &session, msg_t &&req) {
    OPTION_ITEM options[4] {};

    auto &seqn = options[0];
    auto &session_option = options[1];
    auto &port_option = options[2];
    auto &payload_option = options[3];

    seqn.option = const_cast<char *>("CSeq");

    auto seqn_str = std::to_string(req->sequenceNumber);
    seqn.content = const_cast<char *>(seqn_str.c_str());

    std::string_view target {req->message.request.target};
    auto begin = std::find(std::begin(target), std::end(target), '=') + 1;
    auto end = std::find(begin, std::end(target), '/');
    std::string_view type {begin, (size_t) std::distance(begin, end)};

    std::uint16_t port;
    if (type == "audio"sv) {
      port = net::map_port(stream::AUDIO_STREAM_PORT);
    } else if (type == "video"sv) {
      port = net::map_port(stream::VIDEO_STREAM_PORT);
    } else if (type == "control"sv) {
      port = net::map_port(stream::CONTROL_PORT);
    } else {
      cmd_not_found(sock, session, std::move(req));

      return;
    }

    seqn.next = &session_option;

    session_option.option = const_cast<char *>("Session");
    session_option.content = const_cast<char *>("DEADBEEFCAFE;timeout = 90");

    session_option.next = &port_option;

    // Moonlight merely requires 'server_port=<port>'
    auto port_value = std::format("server_port={}", static_cast<int>(port));

    port_option.option = const_cast<char *>("Transport");
    port_option.content = port_value.data();

    // Send identifiers that will be echoed in the other connections
    auto connect_data = std::to_string(session.control_connect_data);
    if (type == "control"sv) {
      payload_option.option = const_cast<char *>("X-SS-Connect-Data");
      payload_option.content = connect_data.data();
    } else {
      payload_option.option = const_cast<char *>("X-SS-Ping-Payload");
      payload_option.content = session.av_ping_payload.data();
    }

    port_option.next = &payload_option;

    respond(sock, session, &seqn, 200, "OK", req->sequenceNumber, {});
  }
```

**File:** src/rtsp.cpp (L964-1006)
```cpp
    stream::config_t config;

    std::int64_t configuredBitrateKbps;
    config.audio.flags[audio::config_t::HOST_AUDIO] = session.host_audio;
    try {
      config.audio.channels = (int) util::from_view(args.at("x-nv-audio.surround.numChannels"sv));
      config.audio.mask = (int) util::from_view(args.at("x-nv-audio.surround.channelMask"sv));
      config.audio.packetDuration = (int) util::from_view(args.at("x-nv-aqos.packetDuration"sv));

      config.audio.flags[audio::config_t::HIGH_QUALITY] =
        util::from_view(args.at("x-nv-audio.surround.AudioQuality"sv));

      config.controlProtocolType = (int) util::from_view(args.at("x-nv-general.useReliableUdp"sv));
      config.packetsize = (int) util::from_view(args.at("x-nv-video[0].packetSize"sv));
      config.minRequiredFecPackets = (int) util::from_view(args.at("x-nv-vqos[0].fec.minRequiredFecPackets"sv));
      config.mlFeatureFlags = (int) util::from_view(args.at("x-ml-general.featureFlags"sv));
      config.audioQosType = (int) util::from_view(args.at("x-nv-aqos.qosTrafficType"sv));
      config.videoQosType = (int) util::from_view(args.at("x-nv-vqos[0].qosTrafficType"sv));
      config.encryptionFlagsEnabled = (uint32_t) util::from_view(args.at("x-ss-general.encryptionEnabled"sv));

      // Legacy clients use nvFeatureFlags to indicate support for audio encryption
      if (util::from_view(args.at("x-nv-general.featureFlags"sv)) & 0x20) {
        config.encryptionFlagsEnabled |= SS_ENC_AUDIO;
      }

      config.monitor.height = (int) util::from_view(args.at("x-nv-video[0].clientViewportHt"sv));
      config.monitor.width = (int) util::from_view(args.at("x-nv-video[0].clientViewportWd"sv));
      config.monitor.framerate = (int) util::from_view(args.at("x-nv-video[0].maxFPS"sv));
      config.monitor.framerateX100 = (int) util::from_view(args.at("x-nv-video[0].clientRefreshRateX100"sv));
      config.monitor.bitrate = (int) util::from_view(args.at("x-nv-vqos[0].bw.maximumBitrateKbps"sv));
      config.monitor.slicesPerFrame = (int) util::from_view(args.at("x-nv-video[0].videoEncoderSlicesPerFrame"sv));
      config.monitor.numRefFrames = (int) util::from_view(args.at("x-nv-video[0].maxNumReferenceFrames"sv));
      config.monitor.encoderCscMode = (int) util::from_view(args.at("x-nv-video[0].encoderCscMode"sv));
      config.monitor.videoFormat = (int) util::from_view(args.at("x-nv-vqos[0].bitStreamFormat"sv));
      config.monitor.dynamicRange = (int) util::from_view(args.at("x-nv-video[0].dynamicRangeMode"sv));
      config.monitor.chromaSamplingType = (int) util::from_view(args.at("x-ss-video[0].chromaSamplingType"sv));
      config.monitor.enableIntraRefresh = (int) util::from_view(args.at("x-ss-video[0].intraRefresh"sv));

      configuredBitrateKbps = util::from_view(args.at("x-ml-video.configuredBitrateKbps"sv));
    } catch (std::out_of_range &) {
      respond(sock, session, &option, 400, "BAD REQUEST", req->sequenceNumber, {});
      return;
    }
```

**File:** src/rtsp.h (L15-15)
```text
  constexpr auto RTSP_SETUP_PORT = 21;
```

**File:** src/rtsp.h (L17-42)
```text
  struct launch_session_t {
    uint32_t id;

    crypto::aes_t gcm_key;
    crypto::aes_t iv;

    std::string av_ping_payload;
    uint32_t control_connect_data;

    bool host_audio;
    std::string unique_id;
    int width;
    int height;
    int fps;
    int gcmap;
    int appid;
    int surround_info;
    std::string surround_params;
    bool continuous_audio;
    bool enable_hdr;
    bool enable_sops;

    std::optional<crypto::cipher::gcm_t> rtsp_cipher;
    std::string rtsp_url_scheme;
    uint32_t rtsp_iv_counter;
  };
```
