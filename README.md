<div align="center">

# 🌙 Moonlight‑Web

**Streamez vos jeux PC depuis n'importe quel navigateur.**
Un client [Sunshine](https://github.com/LizardByte/Sunshine) / GameStream 100 % web — aucune installation côté client, juste une URL.

[![Licence: GPL v3](https://img.shields.io/badge/Licence-GPLv3-blue.svg)](LICENSE)
![Qt](https://img.shields.io/badge/Qt-6.11-41CD52?logo=qt&logoColor=white)
![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus&logoColor=white)
![WebRTC](https://img.shields.io/badge/Transport-WebRTC-333?logo=webrtc)
![Plateformes](https://img.shields.io/badge/Serveur-Windows%20%C2%B7%20Linux%20%C2%B7%20macOS-success)

</div>

---

## ❤️ Soutenir le projet

Moonlight‑Web est **gratuit et open‑source**. Mais l'accès Internet repose sur un
**serveur DNS dédié que je maintiens 24/7 à mes frais** (VM, IP statique, bande
passante) pour que vous puissiez obtenir un sous‑domaine et un certificat TLS
automatiquement. Si l'outil vous est utile, un café aide à garder ce serveur en
ligne 🙏

<div align="center">

<a href="https://buymeacoffee.com/brunoocto">
  <img src="https://img.buymeacoffee.com/button-api/?text=Offrez-moi un café&emoji=☕&slug=brunoocto&button_colour=FFDD00&font_colour=000000&font_family=Cookie&outline_colour=000000&coffee_colour=ffffff" alt="Buy Me A Coffee" height="48">
</a>

</div>

---

## ✨ Ce que fait l'application

Moonlight‑Web transforme **n'importe quel appareil avec un navigateur moderne**
(PC, Mac, tablette, téléphone, TV) en client de streaming pour votre PC de jeu
équipé de Sunshine — **sans rien installer**.

- 🎮 **Streaming basse latence** jusqu'en 4K HDR, 120+ FPS, codecs **H.264 / HEVC / AV1**.
- 🌐 **Transport WebRTC** (DataChannels + pistes média RTP) avec repli automatique WSS.
- 🔊 **Audio Opus** décodé dans le navigateur (jitter buffer adaptatif, surround).
- ⌨️🖱️🎮 **Entrées complètes** : clavier, souris (pointer‑lock), tactile en mode trackpad,
  et **manettes Xbox/PS** avec vibration.
- 🔎 **Découverte automatique** des hôtes Sunshine sur le réseau local (mDNS) + ajout manuel par IP.
- 🔐 **Pairing** sécurisé, multi‑hôtes, sessions persistantes.
- 🌍 **Accès depuis Internet** en un clic : sous‑domaine + certificat TLS obtenus automatiquement.
- 🪄 **Video Enhancement** (bonus) : upscaling & sharpening GPU dans le navigateur.

<div align="center">

![Accueil — liste des hôtes Sunshine](docs/screenshots/home.png)

*Découverte multi‑hôtes, thème Cyberpunk — une simple page web.*

| 🖥️ Sur ordinateur | 📱 Sur mobile |
|:---:|:---:|
| ![Streaming bureau dans le navigateur](docs/screenshots/desktop.png) | ![Streaming sur iPhone avec clavier virtuel](docs/screenshots/mobile.png) |
| *Le bureau distant streamé dans un onglet, overlay de stats.* | *Même expérience sur téléphone : trackpad tactile + clavier virtuel.* |

</div>

---

## 🚀 Comment ça marche (tour d'horizon)

1. **Lancez le serveur** Moonlight‑Web sur la machine où tourne Sunshine.
2. **Ouvrez le navigateur** sur `https://localhost` (ou l'IP locale du PC, ou votre
   domaine si l'accès Internet est activé).
3. Le serveur **découvre vos hôtes Sunshine** (mDNS) — sinon ajoutez l'IP à la main.
4. **Pairez** l'hôte (saisie d'un PIN), choisissez une application, **streamez**.

Le backend C++/Qt intègre directement `moonlight-common-c` : il parle le protocole
GameStream (RTSP/RTP/ENet) côté Sunshine, et relaie la vidéo/audio/entrées vers le
navigateur via WebRTC. Le décodage vidéo se fait en **WebCodecs + WebGPU/canvas**,
l'audio en **AudioWorklet**.

### ⚙️ La page Settings (réglages de stream)

Accessible depuis l'overlay de l'application, elle contrôle la **qualité du flux** :

| Réglage | Valeurs |
|---|---|
| **Bitrate** | 5 – 150 Mbps (ou auto) |
| **Résolution** | 720p / 1080p / 1440p / 2160p |
| **FPS** | 30 → 240 |
| **Codec** | Auto / H.264 / HEVC / AV1 *(les options non supportées par le navigateur sont grisées)* |
| **HDR** | activable si l'hôte et l'affichage le supportent |
| **Chroma 4:4:4** | texte plus net (opt‑in, selon codec) |
| **Mode Gaming** | capture souris en pointer‑lock, raccourcis clavier |
| **VSync**, **stats de perf**, **ratio d'image** (16:9 / 21:9 / 32:9) | — |

<div align="center">

| Réglages vidéo | Options avancées |
|:---:|:---:|
| ![Réglages de stream](docs/screenshots/settings.png) | ![Options avancées (mobile)](docs/screenshots/advanced.png) |
| *Résolution, FPS, HDR, VSync, bitrate.* | *Video Enhancement, codec, mute, 4:4:4, mode tactile.* |

</div>

### 🪄 Video Enhancement (bonus)

Une fonctionnalité **bonus** d'amélioration d'image **côté navigateur**, exécutée
sur le GPU via WebGPU. Elle combine **upscaling (SGSRv1 + FSR1)** et **sharpening**
pour gagner en netteté lorsque la résolution de stream est inférieure à celle de
l'écran. Activable dans les réglages (`auto`, ou forçage `sgsr` / `fsr1`).
*(Voir aussi mes contributions Video Super Resolution aux clients Moonlight natifs,
plus bas.)*

<div align="center">

![Video Enhancement — 720p upscalé en 1440p](docs/screenshots/video_enhancement.png)

*Streamez en 720p (peu de bande passante) et affichez en 1440p net, côté GPU navigateur.*

</div>

---

## 📦 Installation

> ✅ **Recommandé : installez Moonlight‑Web sur la même machine que Sunshine.**
> La latence est minimale (localhost), la découverte mDNS est immédiate, et la
> redirection de ports pour l'accès Internet est simplifiée.

1. **Pré‑requis** : un PC avec **Sunshine** installé et fonctionnel.
2. **Récupérez** la dernière release (binaire) **ou** compilez depuis les sources
   (voir [Forker & compiler](#-forker--compiler)).
3. **Lancez** `mw-server` (Windows : `mw-server.exe`). Une **icône apparaît dans la
   barre système**.
4. Ouvrez **`https://localhost`** dans Chrome / Edge / Safari récent.
   - Le serveur écoute par défaut sur **HTTP :80** (redirigé) et **HTTPS :443**.
   - Au premier lancement, le certificat est **auto‑signé** : votre navigateur
     affichera un avertissement à accepter (normal en LAN).
5. **Pairez** votre hôte, et c'est parti.

> 💡 Vous pouvez aussi y accéder depuis un autre appareil du réseau via
> `https://<IP-locale-du-PC>` (ex. `https://192.168.1.20`).

---

## 🛠️ Page Admin

La page **Admin** configure le serveur lui‑même (et non un stream). Pour des
raisons de sécurité, elle n'est accessible **que depuis la machine locale**.

### Comment y accéder

- **URL directe** : **`https://localhost/admin`**
- **Via l'icône de la barre système** : clic droit → **« Server Settings »**
  (l'entrée **« Open »** ouvre l'app normale, **« Restart »** redémarre le serveur,
  **« Quit »** l'arrête).

> 🔒 Toutes les routes `/api/admin/*` renvoient **403** si la requête ne vient pas
> de `localhost`. Impossible de modifier la config du serveur à distance.

### Ce qu'on y règle

- **Code PIN admin** : générer / régénérer / effacer le PIN protégeant l'accès distant.
- **Sessions** : lister et révoquer les sessions actives.
- **Ports** HTTP / HTTPS du serveur.
- **Transport** (WebRTC / WSS) et mode (auto / manuel).
- **Accès Internet** (voir ci‑dessous).
- **Jeton de certificat** (authentification par certificat).

### 🌍 Rendre l'application accessible depuis Internet

Activez **Internet Access** dans l'Admin. Le serveur va alors automatiquement :

1. **Détecter votre IP publique** (STUN, repli HTTPS).
2. **Créer un sous‑domaine** `「identifiant」.votredomaine` via l'API PowerDNS,
   pointant un enregistrement `A` vers votre IP publique (avec un jeton de
   propriété TXT pour éviter les collisions entre instances).
3. **Obtenir un certificat TLS** valide automatiquement (ACME DNS‑01).
4. **Ouvrir les ports** sur votre box via **UPnP** (TCP 80/443 + UDP 47999).

<div align="center">

![Page Admin — accès Internet & config serveur](docs/screenshots/admin.png)

*L'Admin (localhost) : accès Internet, URL publique générée, accès local, transport.*

</div>

#### ⚠️ Limitations possibles

- **UPnP indisponible / désactivé sur le routeur** → les ports ne sont pas ouverts
  automatiquement. Vous devrez **rediriger manuellement** dans votre box :
  **TCP 80, TCP 443, UDP 47999** vers l'IP locale du PC.
- **CGNAT / double‑NAT** (fréquent en 4G/5G/fibre opérateur) : votre « IP publique »
  est partagée et non routable. Le serveur le **détecte et vous prévient** — la
  redirection de ports ne fonctionnera pas, contactez votre FAI ou utilisez un relais.
- **Port déjà mappé** à un autre appareil sur le routeur → conflit signalé dans l'Admin.
- **Réseaux d'entreprise restrictifs** : certains filtrent l'autorité de
  certification par défaut. Voir la section [SSL](#-ssl--votre-domaine-et-votre-certificat).

---

## 🏗️ Architecture

```
   NAVIGATEUR (n'importe quel appareil)        SERVEUR Moonlight-Web (C++/Qt)              HÔTE Sunshine
 ┌───────────────────────────────────┐      ┌──────────────────────────────────┐      ┌──────────────────┐
 │  Web App (Vanilla JS, ES6)        │      │  HTTP :80 ─redirect→ HTTPS :443   │      │                  │
 │  • Liste hôtes / apps / pairing   │ REST │  • Fichiers statiques + API REST  │HTTPS │  API GameStream  │
 │  • UI / Settings / Admin          │◄────►│  • Proxy vers Sunshine           │◄────►│  /serverinfo     │
 │                                   │      │                                  │      │  /applist /launch│
 │  Vidéo : WebCodecs + WebGPU       │      │  ┌────────────────────────────┐  │      │  /pair           │
 │  Audio : Opus / AudioWorklet      │WebRTC│  │   moonlight-common-c       │  │ RTSP │                  │
 │  Entrées : clavier/souris/manette │◄════►│  │   (RTSP/RTP/ENet embarqué) │  │ RTP  │  Encodeur GPU    │
 │  Video Enhancement (WebGPU)       │ (WSS │  │   MoonlightShim → Relay    │  │◄════►│  (NVENC/AMF/QSV) │
 │                                   │ repli)│  └────────────────────────────┘  │ UDP  │                  │
 └───────────────────────────────────┘      └──────────────────────────────────┘      └──────────────────┘
        ▲                                                                                       
        │ DNS (sous-domaine) + TLS                                                              
 ┌──────┴───────────────────────────────────────────────────┐                                  
 │  Stack DNS auto-hébergée (Docker, machine séparée)        │   ← maintenue par l'auteur,      
 │  dnsdist :53  ·  PowerDNS (API)  ·  Caddy :80/:443        │     ou hébergez la vôtre         
 └──────────────────────────────────────────────────────────┘                                  
```

**Principe** : le serveur est à la fois un **serveur web** (sert le frontend +
API REST), un **proxy** vers l'API de Sunshine, et un **pont de streaming** qui
embarque `moonlight-common-c`. La vidéo H.264/HEVC/AV1 et l'audio Opus sont relayés
au navigateur via **WebRTC** (DataChannels + pistes RTP), avec repli **WSS** si
WebRTC échoue. Les entrées remontent du navigateur, sont chiffrées (AES‑128‑GCM)
et envoyées à Sunshine via le canal de contrôle **ENet**.

### 🌐 Le réseau quand l'accès Internet est ouvert

1. Le serveur publie un enregistrement **`A`** `「id」.votredomaine → IP publique`
   sur **votre serveur DNS** (PowerDNS), via son API REST sécurisée.
2. Il obtient un **certificat TLS** pour ce FQDN via **ACME (challenge DNS‑01)** —
   aucune ouverture de port n'est nécessaire pour la validation.
3. **UPnP** mappe les ports sur la box (ou redirection manuelle).
4. Le client distant ouvre `https://「id」.votredomaine` → arrive sur votre serveur,
   établit la session **WebRTC** (ICE host candidates réécrits + STUN pour le
   NAT‑traversal). Si la connexion directe échoue, repli **WSS** sur HTTPS :443.

Le **DNS et l'app sont découplés** : l'API PowerDNS peut vivre sur une machine
dédiée (le « DNS box »). C'est ce serveur que vos dons aident à maintenir — mais
vous pouvez **héberger le vôtre** (voir [Forker & compiler](#-forker--compiler)).

---

## 🔧 Configuration avancée — `settings.json`

La plupart des réglages se font via l'UI (Settings/Admin) et sont stockés
**côté serveur** dans `settings.json`, dans le dossier de données applicatives de
l'OS :

| OS | Chemin |
|---|---|
| **Windows** | `%APPDATA%\Moonlight-Web\Moonlight-Web\settings.json`<br>(`C:\Users\<vous>\AppData\Roaming\Moonlight-Web\Moonlight-Web\`) |
| **macOS** | `~/Library/Application Support/Moonlight-Web/Moonlight-Web/settings.json` |
| **Linux** | `~/.local/share/Moonlight-Web/Moonlight-Web/settings.json` |

> Redémarrez le serveur (ou relancez le stream) après une édition manuelle.

Vous pouvez y modifier des clés non exposées dans l'UI, notamment :

| Clé | Rôle |
|---|---|
| `domain` | **Votre propre FQDN** (domaine personnalisé) au lieu du sous‑domaine auto. |
| `cert_pem` / `cert_key` | Chemins (ou noms de variables d'env) vers **votre certificat** et sa clé. |
| `audio_time_stretch` | Lissage audio WSOLA (préserve la hauteur) sur réseau instable — `true` par défaut. |
| `http_port` / `https_port` | Ports du serveur. |
| `stun_server` | Serveur STUN personnalisé pour le NAT‑traversal. |

### 🔐 SSL — votre domaine et votre certificat

Par défaut, Moonlight‑Web obtient un certificat **automatiquement et gratuitement
via ZeroSSL** (ou Let's Encrypt), avec renouvellement automatique. Pratique, mais
**certains réseaux d'entreprise très restrictifs** bloquent ou n'ont pas confiance
dans certaines autorités de certification, ce qui peut empêcher la connexion.

**Solution : utilisez votre propre domaine et votre propre certificat.** Dans
`settings.json`, ne touchez à rien dans `.env` et renseignez :

```json
{
  "domain":   "stream.mondomaine.com",
  "cert_pem": "C:/chemin/vers/fullchain.pem",
  "cert_key": "C:/chemin/vers/privkey.pem"
}
```

- `cert_pem` / `cert_key` acceptent un **chemin de fichier** ou un **nom de
  variable d'environnement**.
- Le **CN du certificat doit correspondre** à `domain`.
- Un cert géré ainsi n'est **pas** touché par le renouvellement automatique : son
  cycle de vie est à vous. Faites pointer votre DNS (`A`/`CNAME`) vers votre IP.

Cela vous permet d'utiliser un certificat d'une AC reconnue partout, ou un
certificat interne d'entreprise déjà approuvé sur le réseau cible.

---

## 🍴 Forker & compiler

### 1. Installer Qt Creator

Téléchargez le **Qt Online Installer** (compte Qt gratuit) et installez :

- **Qt 6.11** (ou 6.x récent) avec :
  - le module **Qt Core**, **Qt Network**, **Qt WebSockets** ;
  - sur Windows, le **kit MSVC 2022 64‑bit** (pas MinGW) ;
  - **Qt Creator** (IDE) et les **debugging tools**.
- **OpenSSL 3.x** (headers + libs ; sous Windows ils sont fournis dans
  `backend/libs/windows/`).

Puis :

```bash
git clone <ce-repo>
cd moonlight-web-deepseek
git submodule update --init --recursive   # moonlight-common-c, qmdnsengine, libdatachannel...
```

**Compilation**

```bash
# Windows (MSVC) — via le script :
cmd //c backend/build_msvc.bat
#   ou ouvrez backend/backend.pro dans Qt Creator (kit MSVC2022 64-bit) → Ctrl+B

# Linux / macOS — via CMake :
cmake -S backend -B backend/build -DCMAKE_BUILD_TYPE=Release
cmake --build backend/build -j
```

**Lancement**

```bash
cd backend/build/release && ./mw-server     # Windows : mw-server.exe
# Ouvrez https://localhost
```

> Le projet est **multi‑plateforme** (Windows x64/ARM64, Linux, macOS) via CMake ;
> le `.pro` qmake reste valide pour le flux Qt Creator/MSVC.

### 2. L'image Docker (stack DNS) — à quoi elle sert

Pour offrir l'**accès Internet** (sous‑domaine + TLS auto), il faut un **serveur
DNS faisant autorité** sur un domaine que vous possédez. Le dossier
[`deploy/powerdns/`](deploy/powerdns/) fournit une stack Docker clé en main
(3 conteneurs) :

| Conteneur | Rôle |
|---|---|
| **dnsdist** | Frontal DNS public `:53` (anti‑amplification, rate‑limit) → PowerDNS |
| **pdns** (PowerDNS) | Serveur DNS faisant autorité + API REST (interne uniquement) |
| **caddy** | Reverse‑proxy HTTPS exposant l'API DNS en `api.votredomaine` (`:80`/`:443`, TLS auto) |

**Installation** (sur une petite VM Linux dédiée, ex. Azure B2ats v2) :

```bash
cd deploy/powerdns
sudo ./install.sh     # installe Docker, sécurise, ouvre le firewall, démarre la stack
```

**Ports / protocoles à ouvrir publiquement** sur la VM (firewall + groupe de
sécurité cloud) :

| Port | Protocole | Pourquoi |
|---|---|---|
| **53** | **UDP** | Requêtes DNS (chemin principal) |
| **53** | **TCP** | Réponses tronquées, DNSSEC |
| **80** | **TCP** | Challenge HTTP‑01 Let's Encrypt (API) |
| **443** | **TCP** | API PowerDNS exposée en `api.votredomaine` |

> Les ports internes de PowerDNS (`5300` DNS, `8081` API) **ne sont jamais publiés**.

**Enregistrer le serveur de noms (ns1) chez votre registrar** — étape manuelle
indispensable pour que le domaine soit autoritatif sur Internet :

1. **Glue records** : `ns1.votredomaine` → **IP publique de la VM**
   (et `ns2.votredomaine` → même IP, ou une 2ᵉ VM pour la redondance).
2. **Délégation (NS)** : les serveurs de noms du domaine pointent vers
   `ns1.votredomaine` / `ns2.votredomaine`.
3. **DNSSEC** (recommandé) : soumettez le **DS record** affiché au premier
   démarrage (`docker compose logs pdns`) à la section DNSSEC de votre registrar.

Côté serveur Moonlight‑Web, renseignez dans son `.env` :

```bash
MW_DOMAIN=votredomaine.top
MW_PDNS_URL=https://api.votredomaine.top/api/v1/servers/localhost
MW_PDNS_TOKEN=<même valeur que MW_PDNS_API_KEY de la stack DNS>
# Optionnel — pour émettre via ZeroSSL (réseaux restrictifs) :
# MW_ZEROSSL_EAB_KID=...
# MW_ZEROSSL_EAB_HMAC=...
```

Voir le [README détaillé de la stack DNS](deploy/powerdns/README.md) pour Azure,
le durcissement sécurité et la vérification.

---

## 👤 About the author

I'm an experienced web developer with **15+ years** in the industry. I'm also a
long‑time contributor to the **Moonlight** game‑streaming ecosystem: I built and
upstreamed **Video Super Resolution** (real‑time GPU upscaling) across *every*
major Moonlight client — Windows, Linux, macOS, Windows ARM, Android, iOS, tvOS
and Xbox (see the pull requests below).

**Moonlight‑Web is the natural next step**: bringing that same low‑latency,
high‑quality streaming experience to *any* device with a browser — no native app,
no install, just a URL — with WebRTC transport, WebCodecs/WebGPU video and a
browser‑side Video Enhancement pipeline. It's where my web background and my
Moonlight work meet.

## 🔭 Mes autres projets Moonlight (Video Super Resolution)

J'ai contribué la **Video Super Resolution** (upscaling GPU temps réel) aux clients
Moonlight officiels — la version native de la fonctionnalité « Video Enhancement »
bonus de ce projet :

| Plateforme | Contribution |
|---|---|
| 🪟🐧🍎 **Windows (x64 AMD/Intel/NVIDIA), Windows ARM (Snapdragon), Linux, macOS** | [moonlight‑qt #1557](https://github.com/moonlight-stream/moonlight-qt/pull/1557) |
| 🤖 **Android** | [moonlight‑android #1567](https://github.com/moonlight-stream/moonlight-android/pull/1567) |
| 🍏 **iOS & tvOS** | [moonlight‑ios #704](https://github.com/moonlight-stream/moonlight-ios/pull/704) |
| 🎮 **Xbox** | [moonlight‑xbox #267](https://github.com/TheElixZammuto/moonlight-xbox/pull/267) |

---

## 📜 Licence

Moonlight‑Web est un logiciel libre sous **GNU General Public License v3.0**
(GPL‑3.0). Vous êtes libre de l'utiliser, l'étudier, le modifier, le forker et le
redistribuer, à condition de le garder open‑source sous la même licence et de
**conserver la notice de copyright et créditer l'auteur original**.

> Copyright © 2026 Bruno Martin &lt;brunoocto@gmail.com&gt;

Voir [LICENSE](LICENSE) pour le texte complet et [COPYRIGHT](COPYRIGHT) pour les
licences des composants tiers.

---

<div align="center">

**Ce projet vous plaît ?** Mettez une ⭐ et [offrez un café au serveur DNS](#-soutenir-le-projet) ☕

</div>
