# Moonlight Web Booster — étude de faisabilité et plan

> Un petit composant natif, optionnel, installé sur la machine **cliente**, à qui
> la page MoonlightWeb pourrait confier le décodage, le traitement ou l'affichage
> de la vidéo. L'interface reste la page ; sans Booster, rien ne change.
> Étude demandée par Bruno le 09/10/2026. **Prévue après la v0.4.0** : ce
> document ne lance aucun développement, aucun POC, aucun banc.
>
> **Statut au 09/10/2026** : étude seule. Aucun code, aucun composant, aucun
> installateur. Chaque phase du §10 attend le « go » de Bruno.
> **21/10/2026** : Bruno place B0 après la v0.4.1.
> **23/10/2026** : « go » de B0 (Bruno), en attendant que l'UM790Pro sous
> Windows soit libre (invitations spectateur jusqu'au 24/10 au soir, puis le
> banc N95). Protocole au §10.3 ; rien n'est lancé.
> **04/11/2026** : **produit clos par Bruno** (§0, §13). Ce document reste comme
> trace. Une mesure sans Booster en sort pour la v0.4.2 (§16).
>
> Légende : **[D]** fait documenté (source officielle, §14) ; **[C]** constat du
> code ou des bancs de ce dépôt (chemin ou section cités) ; **[H]** hypothèse à
> mesurer.

## 0. Verdict

**Clos le 04/11/2026, décision de Bruno : le Booster ne se fera pas.** Au clic →
photon, B0 n'a rien trouvé à reprendre en SDR (§10.4). Les intérêts hors latence,
rejugés le 04/11 (§9.3), ne paient pas son coût. B0 en HDR n'a pas été mesuré et
ne le sera pas. La suite de ce verdict est celle de l'étude, gardée telle quelle.

**Faisable sous une seule forme, et pas celle qu'on imagine d'abord.**

- **B0 mesurée le 24/10/2026 : porte non franchie** (§10.4). Au clic → photon,
  la page devant Sunshine vaut moonlight-qt à 1 ms près. L'image perdue par
  MoonlightWeb (≈ 8 ms, une image à 120 Hz) **n'a pas été reproduite** le soir
  même : elle suivait la phase entre l'écran de l'hôte et celui du client, et
  l'hôte natif vaut Sunshine sur des passes d'un tour de phase entier. Le
  Booster n'a rien à reprendre sur ce chemin ; B1 ne s'ouvre pas.
- **Le Booster ne peut pas rendre ses images à la page sans les copier par la
  mémoire du CPU.** Aucune API web n'importe une surface GPU venue d'un autre
  processus : un `VideoFrame` se construit depuis une image du DOM, un autre
  `VideoFrame` ou un tampon CPU ; WebGPU n'importe que `HTMLVideoElement` et
  `VideoFrame` [D]. Le chemin « décodé par le Booster, affiché par Chrome »
  coûte donc relecture GPU → CPU, transfert local, copie dans la page, envoi
  au GPU de Chrome. Soit 4 à 10 ms et 0,7 à 1,5 Go/s de copies en 1440p120
  (§4), là où le décodeur matériel du navigateur coûte aujourd'hui 0,4 à 5 ms
  sans copie [C]. **Ce chemin est écarté d'avance.**
- **La seule forme qui peut gagner : le Booster décode *et affiche lui-même*,
  dans une fenêtre native posée sur la zone vidéo de la page** (le
  « présentateur superposé », §6). La page garde l'interface, les entrées,
  l'audio, le transport et le repli. Ce que cette forme peut reprendre, c'est le
  poste que le navigateur ajoute après le dessin : **15 à 25 ms entre le dessin
  dans le canevas et le bureau composé**, mesurés au POC Ultra [C,
  `ultra-lan-poc.md` §6.5, §6.9]. Aucun présentateur dans Chrome, ni le plein
  écran, ne les a rendus [C, §6.9].
- **Premier obstacle technique : la superposition elle-même.** Poser une
  fenêtre native, transparente aux clics et jamais active, exactement sur la
  vidéo d'une fenêtre de navigateur, sans que Chrome se croie masqué et
  ralentisse la page (§3.5), en suivant plein écran, multi-écran, DPI et
  changements de bureau. C'est faisable sous Windows, faisable avec plus de
  pièges sous macOS, faisable sous X11 et KDE Wayland. **C'est impossible sous
  GNOME Wayland** : le protocole ne laisse aucun client se placer ni se poser
  au-dessus d'un autre [D].
- **Second obstacle : la porte d'entrée.** Depuis 2026, une page publique qui
  joint `127.0.0.1` déclenche une demande d'autorisation, dans Chrome pour
  `fetch` (142) puis WebSocket et WebTransport (147), et dans Firefox (149 en
  strict, déploiement général à partir de 151) [D]. Safari bloque encore
  `http://127.0.0.1` depuis une page HTTPS selon les versions [D]. Le Booster
  coûtera donc au moins **une autorisation par origine**, ou une extension.
- **Le gain n'est pas démontré.** Le seul repère natif au même outil, Steam en
  HEVC, affiche le clic **au même moment que MoonlightWeb** (57,9-58,5 ms contre
  57,3-58,2 ms) [C, §6.5]. Seul le PyroWave de Steam fait mieux (42,6-49,2 ms),
  sans qu'on sache quelle part revient au codec et quelle part à sa
  présentation. **Le plan commence donc par une borne sans Booster (B0, §10) :
  si un client natif sobre ne bat pas MoonlightWeb d'au moins une image
  d'affichage au même outil, on arrête là.**

| Plateforme | Verdict | Pourquoi |
|---|---|---|
| Windows 10/11 | **Oui, en premier** | D3D11VA/D3D12 Video, DXGI flip + tearing, fenêtre superposée maîtrisée (§7.1) |
| macOS | **Oui, ensuite, sous réserve** | VideoToolbox + `CAMetalLayer` sans copie ; superposition fragile en plein écran natif (Spaces) ; distribution non notariée aujourd'hui (§7.2) |
| Linux X11, KDE Wayland | **Oui, en dernier** | VA-API / Vulkan Video + Vulkan ; superposition possible (X11, `wlr-layer-shell` sur KWin) (§7.3) |
| Linux GNOME Wayland | **Non** pour la superposition | xdg-shell interdit placement et empilement ; seul recours : une fenêtre Booster à soi, donc un second client (§5, D) |
| Android | **Non** depuis une page | pas de fenêtre au-dessus du navigateur sans permission d'overlay système ; la voie réaliste est l'appli native (`moonlight-web-android-tv`, en pause) (§7.4) |
| iOS / iPadOS | **Non** | aucun processus d'arrière-plan joignable, aucune superposition ; seule voie : une app native (§7.4) |

**Gain estimé** (§9) : sur HEVC/H.264, entre **0 et 2 images d'affichage**
(0 à 16 ms à 120 Hz) en médiane, à prouver par B0 et B1 ; avec PyroWave, en plus,
**~3 ms de décodage** (la file WebGPU disparaît). Hors latence, deux gains
réels mais secondaires : des codecs que le navigateur refuse (HEVC sous
Firefox, décodage matériel sous Chrome Linux, HEVC 4:4:4 10 bits) et une
présentation HDR native.

**Coût de maintenance** : élevé pour ce que ça couvre. Un binaire natif par
OS et par architecture, trois piles de décodage et trois piles de
présentation, une fenêtre superposée qui dépend du comportement de chaque
navigateur, une signature et une mise à jour par OS, et un protocole
page ↔ Booster versionné (§8, §11). C'est l'équivalent d'un petit client
natif sans interface.

**Recommandation** : ne rien engager avant B0. Si B0 passe, faire B1 (labo
superposé, Windows seul, hors produit) avant toute ligne dans le produit.

## 1. La question et le périmètre

- **Garde-fous de Bruno** : la page reste le seul client ; pas d'installation
  obligatoire ; pas d'Electron ni de runtime desktop complet sans raison
  technique solide ; le Booster est optionnel et sa panne rend la main au
  pipeline web sans rupture.
- **Hors périmètre** : l'hôte (capture, encodage, transport côté hôte),
  la v0.4.0, l'appli Android TV (en pause), l'audio. L'audio reste au
  mécanisme natif du navigateur (règle du projet).
- **Objectif mesurable** : moins de latence au photon (clic → pixel sur le
  bureau composé du client) et moins d'à-coups, pas un décodage plus court
  pris isolément.

## 2. Le pipeline client aujourd'hui (constats du code)

### 2.1 Le chemin d'une image

| Étape | Où | Fil | Constat |
|---|---|---|---|
| Transport | `webrtc-dc-udp` par défaut, puis `dc-tcp`, `media-udp/tcp`, `wss` | — | [C] `backend/src/streaming/TransportPriorities.h:15-52`, `frontend/js/ui/StreamView.js:903-916` |
| Remontage de l'image | `WebRtcDataChannel._onVideoChunk`, en-tête de 17 octets | **principal** | [C] `frontend/js/api/WebRtcDataChannel.js:1092-1553` |
| Piste RTP (POC U1.4, coupée) | `RTCRtpScriptTransform` dans un worker, « route audio » | worker | [C] `frontend/js/api/RtpVideo.js`, `rtpVideoTransformWorker.js` |
| Tri, pertes, péremption | `handleVideoFrame` | principal | [C] `StreamView.js:7044-7101` |
| Décodage | WebCodecs, `prefer-hardware` si `isConfigSupported`, `optimizeForLatency:true`, file max 8 | principal (worker optionnel, coupé par défaut) | [C] `StreamView.js:2608-3063`, `:1674`, `:600-633` |
| Rendu | Canvas2D `desynchronized` par défaut ; WebGL2 FSR1/SGSR ; WebGPU (HDR, upscalers) ; `<video>` pour les TV | principal | [C] `frontend/js/stream/renderers/createRenderer.js:43-90`, `StreamView.js:635-727` |
| Cadence | dessin au décodage (« tearing », défaut sur Chromium bureau), sinon rAF ; la plus fraîche gagne | principal | [C] `StreamView.js:4360-4580`, `BrowserDetect.js:409-432` |
| Composition | Chrome (viz) puis DWM / WindowServer / compositeur Wayland | hors page | aucune API ; **15-25 ms** mesurés [C, `ultra-lan-poc.md` §6.5] |

PyroWave (Ultra, banc seulement) : décodeur WGSL sur WebGPU vers un
`OffscreenCanvas`, puis `VideoFrame` vers le présentateur Canvas2D ; repli
WebGL2 [C, `frontend/js/stream/ultra/`].

### 2.2 Où le temps passe déjà (mesures du dépôt)

| Poste | Valeur | Source |
|---|---|---|
| Décodage HEVC matériel dans la page | 0,4-5 ms (780M : 0,7 ms médiane) | [C] `ultra-lan-poc.md` §6.6, §6.18 |
| File et dessin Canvas2D | 0,2-0,3 ms | [C] §6.18 |
| Dessin → bureau composé | **15-25 ms** (58 ms au photon contre 33-43 ms lus dans le canevas) | [C] §6.5, §6.9 |
| Présentateurs et plein écran dans Chrome | tous entre 58,8 et 65,7 ms : aucun ne rend ce poste | [C] §6.9 (présentateur réellement actif non vérifié) |
| Métronome de Chrome sur une piste vidéo RTP | 7,5-7,8 ms en moyenne | [C] §6.11 |
| WebGPU, `onSubmittedWorkDone` | 3,5-3,7 ms pour un envoi vide | [C] `click-waits.md` |
| PyroWave dans la page, 1440p, 780M | 6,1-6,4 ms envoi → fin, dont 3,15 ms de GPU | [C] §6.18 |
| PyroWave natif (référence), 1080p | RTX 0,17 ms, Arc 1,16 ms | [C] §6.13 |
| Repères natifs au même outil (RTX, 120 Hz) | Steam HEVC 57,9-58,5 ms ; Steam PyroWave 42,6-49,2 ms ; MoonlightWeb HEVC 57,3-58,2 ms | [C] §6.5 |

Lecture :
- Le décodage du navigateur n'est **pas** le goulot en HEVC/H.264 : moins de
  1 ms sur un iGPU récent. Un décodeur natif n'a presque rien à reprendre là.
- Le poste que le natif peut viser, c'est la composition (15-25 ms) et, pour
  PyroWave, la file WebGPU (~3 ms au-delà du GPU).
- Le repère Steam HEVC tempère tout : un client natif n'est pas plus rapide
  par nature. Steam a peut-être sa propre file. **Il faut une borne native
  sobre (B0).**
- Les médianes tombent sur des marches de 8,3 ms (l'écran à 120 Hz) [C, §6.5] :
  le gain se compte en images d'affichage, pas en millisecondes fines.

### 2.3 Ce que le Booster ne doit pas casser

- La chaîne de repli des transports et des codecs (`app.js:2639`,
  `_computeCodecFallbackTarget` `StreamView.js:3467`).
- Les réparations de pertes : liste des images perdues renvoyée à l'hôte
  natif, `decoderRidesOutGaps()` faux sur Apple [C, `BrowserDetect.js:220-262`].
- La remontée de profondeur de file à l'hôte (`DecodeQueueSignal`,
  `StreamView.js:3712`), le `VsyncGrid`, l'« Auto » (`CadenceStepper`) : ils
  lisent l'état du décodeur et du dessin dans la page. Avec le Booster, ces
  signaux doivent venir de lui.
- Les sondes (`mwLatency`, `FrameLog`, `ContentAgeProbe`) : elles lisent le
  canevas. Elles ne verront plus l'image affichée.
- Le flux commun des invités et les sessions partagées : rien ne change côté
  hôte tant que la page reste le seul pair (§6.4).

## 3. Ce qu'une page peut et ne peut pas faire avec un programme local

### 3.1 Joindre le Booster depuis la page

**D'où vient la page** [C] : en LAN direct, `https://<IP du LAN>[:port]` ou
`https://localhost` (certificat autosigné) ; par le rendez-vous (le défaut, LAN
compris), `https://stream.moonlightweb.top/<id>`, une origine publique servie
par un service worker [C, `bootstrap/sw.js`, `bootstrap/v1/boot.js:615`].

| Canal | Chrome / Edge | Firefox | Safari |
|---|---|---|---|
| `fetch` vers `http://127.0.0.1` depuis HTTPS | permis par le contenu mixte (le loopback est « potentiellement digne de confiance ») [D] ; **demande « réseau local » depuis Chrome 142** [D] | permis ; demande « services de l'appareil » (149 en strict, 151+ pour tous) [D] | **bloqué ou non selon la version** (régression iOS 18, bogue WebKit 279249) [D] |
| `ws://127.0.0.1` | idem ; **soumis à la demande depuis Chrome 147** [D] | idem [D] | idem fetch [D] |
| WebTransport + `serverCertificateHashes` | possible, certificat ECDSA P-256 de 14 jours au plus ; soumis à la demande (147) [D] | possible [D] | **[H]** support à vérifier |
| WebRTC vers le loopback | hors demande en 2026 [D] | **[H]** | **[H]** |
| Extension + Native Messaging | possible ; une extension par navigateur [D] | possible [D] | possible, mais par une extension d'app Xcode [D] |
| Schéma d'URL (`mwbooster://…`) | lance le programme après une confirmation du navigateur [D] | idem [D] | idem [D] |

Conséquences :
- **Une page publique ne joint plus le loopback sans autorisation.** La
  demande est par origine et mémorisée. Pour le rendez-vous, c'est une
  origine (`stream.moonlightweb.top`), donc une demande une fois. En LAN direct,
  une par IP d'hôte.
- Dans le rendez-vous, la page tourne sous un service worker. La demande
  doit partir **de la page** : une requête d'un worker n'obtient pas
  l'autorisation seule [D].
- WebRTC échappe à la demande aujourd'hui. **Ne pas bâtir dessus** : c'est
  annoncé comme provisoire et ce serait contourner l'intention de la règle.
- Les politiques d'entreprise (Chrome `LocalNetworkAccess…`, Firefox
  `LocalNetworkAccess`) peuvent pré-autoriser ou interdire [D].

### 3.2 Native Messaging : un canal de contrôle, pas de données

- JSON préfixé de sa longueur sur stdin/stdout ; **1 Mo au plus par message
  de l'hôte natif vers l'extension**, 64 Mio dans l'autre sens [D].
- La page ne parle pas au programme : page → script de contenu ou
  `externally_connectable` (Chrome) → service worker de l'extension →
  programme [D]. Trois sauts d'IPC et du JSON : trop lent pour des images
  vidéo, assez pour un jeton et un numéro de port.
- Coût : une extension publiée sur trois magasins (Chrome Web Store, AMO,
  App Store pour Safari), un manifeste d'hôte natif par navigateur et par OS
  (registre sous Windows, dossiers sous macOS/Linux) [D].
- Intérêt : pas de demande « réseau local », authentification forte (le
  programme reçoit l'origine de l'extension en argument [D]).

### 3.3 Démarrer le Booster sans service permanent

- Un schéma d'URL déclaré à l'installation (`HKCU\Software\Classes` sous
  Windows, `CFBundleURLTypes` sous macOS, `.desktop` + `x-scheme-handler` sous
  Linux) [D]. La page ouvre `mwbooster://start?…` sur un geste de l'utilisateur ;
  le navigateur demande confirmation (case « toujours autoriser » dans Chrome).
- Le Booster écoute alors sur le loopback, sert une session, et **quitte seul**
  à la fermeture du canal (délai court).
- Pas de service, pas de démarrage avec la session, pas de droits
  administrateur.

### 3.4 Ce qui n'existe pas

- **Importer une texture ou un handle GPU d'un autre processus** : aucune API
  (WebCodecs, WebGPU, WebGL) [D]. `SharedArrayBuffer` ne traverse pas les
  processus.
- **Placer une fenêtre native dans la page** : impossible. Seule une
  fenêtre *au-dessus* du navigateur est possible, gérée par le Booster.
- **Connaître la position de l'élément vidéo à l'écran** de façon exacte :
  `screenX` + rectangle de l'élément + `devicePixelRatio` ignorent la hauteur
  des barres du navigateur. Le Booster doit la trouver lui-même (§6.2).
- **Une caméra virtuelle** (Booster → `getUserMedia`) : possible, mais la
  chaîne de capture de Chrome ajoute ses copies, sa file et une demande
  « caméra ». Écartée.

### 3.5 La fenêtre superposée et l'« occlusion » de Chrome

- Sous Windows, Chrome calcule si sa fenêtre est **masquée** par d'autres ; un
  onglet masqué est traité comme en arrière-plan : **rendu arrêté, JavaScript
  ralenti** [D]. Les fenêtres transparentes ne comptent pas comme masquantes
  [D], mais le critère exact de « transparente » n'est pas documenté.
- Si la superposition couvre toute la fenêtre en plein écran et que Chrome la
  juge opaque, la page (transport, entrées, audio) serait ralentie. **Risque
  n°1 à lever en B1** [H].

## 4. Le chemin « le Booster décode, la page affiche », chiffré

Taille d'une image décodée en NV12 (1,5 octet par pixel) :

| Format | Image | 60 i/s | 120 i/s | 240 i/s |
|---|---|---|---|---|
| 1080p | 3,1 Mo | 187 Mo/s | 373 Mo/s | 746 Mo/s |
| 1440p | 5,5 Mo | 332 Mo/s | 664 Mo/s | 1,33 Go/s |
| 2160p | 12,4 Mo | 746 Mo/s | 1,49 Go/s | — |

Étapes par image, toutes **[H]** (ordres de grandeur, à ne mesurer que si
quelqu'un rouvre ce chemin) :

| Étape | Coût par image (1440p) |
|---|---|
| Copie GPU → mémoire lisible, attente de la fin du décodage | 0,5-2 ms, plus un point de synchronisation |
| Envoi local (WebSocket ou WebTransport) : noyau, processus réseau de Chrome, processus de rendu | 1-3 ms, deux à trois copies |
| `new VideoFrame(buffer)` ou `writeTexture` | 0,5-1,5 ms |
| Envoi vers le GPU de Chrome | 1-2 ms (iGPU : partage de bande mémoire avec le jeu de l'écran) |
| **Total** | **~3-8 ms**, plus 0,7-1,5 Go/s de bande mémoire |

Contre 0,4-5 ms aujourd'hui sans copie [C]. Le résultat passe ensuite par
la même composition de Chrome (15-25 ms) : **ce chemin ne peut que perdre.**
Seule exception concevable : un codec que le navigateur ne décode pas du tout
(§9.3), en basse résolution. Ce n'est pas un objectif.

## 5. Les architectures comparées

| | Ce que c'est | Gain possible | Coût | Verdict |
|---|---|---|---|---|
| **A. Web seul (aujourd'hui) / PWA** | la page, installable en PWA | — (la PWA a le même compositeur) | nul | référence ; reste le défaut |
| **B. Booster décodeur → page** | le Booster décode, renvoie des images CPU | négatif (§4) | moyen | **écarté** |
| **C. Booster présentateur superposé** | le Booster décode et affiche au-dessus de la page ; la page garde UI, entrées, audio, transport | la composition (0-2 images), PyroWave natif | élevé | **recommandé, sous les portes B0/B1** |
| C1 | … flux codé relayé par la page (loopback) | idem | le plus simple | première forme |
| C2 | … le Booster reçoit lui-même le flux de l'hôte (pair WebRTC) | + supprime la pile réseau de Chrome (métronome RTP, SCTP de Chrome) | très élevé : appairage, ICE, rendez-vous, slots hôte | plus tard, seulement si C1 gagne et que le transport domine |
| **D. Booster client natif lancé par la page** | sa propre fenêtre, ses entrées, son audio | le maximum (≈ moonlight-qt) | très élevé ; **deuxième client** | contraire aux garde-fous ; seul recours sous GNOME Wayland |
| **E. Coquille native + WebView système** | une fenêtre native qui héberge la même page dans WebView2 / WKWebView / WebKitGTK, la vidéo native dessous | comme C, sans les pièges de superposition (même fenêtre) | élevé ; WebKitGTK et WKWebView ont d'autres limites (WebCodecs, WebGPU, WebHID) | **alternative n°1** si la superposition échoue |
| **F. Electron** | Chromium + Node embarqués | aucun sur la composition (même Chromium) ; un module natif peut dessiner une surface à côté, comme E | 100+ Mo par OS, Chromium à suivre | écarté sans raison nouvelle |
| **G. Tauri** | forme outillée de E (WebView2, WKWebView, WebKitGTK) | comme E | comme E, plus Rust | variante de E, à trancher si E s'ouvre |
| **H. Service local permanent** | un démon qui écoute en continu | aucun en plus | surface d'attaque constante | écarté ; lancement à la demande (§3.3) |
| **Extension + Native Messaging** | un canal, pas une architecture | évite la demande « réseau local » | trois magasins | option pour C (§8.2) |

Pourquoi C plutôt que E : C ne change rien pour qui n'installe rien, et la
page reste servie par l'hôte ou le rendez-vous, dans le vrai Chrome (WebHID,
WebCodecs, WebGPU tels qu'on les connaît). E réintroduit une application à
distribuer, avec un moteur web qui change d'un OS à l'autre. Mais E résout
tout ce qui rend C fragile (placement, occlusion, Wayland). **Si B1 montre que
la superposition est trop fragile, E est l'alternative à évaluer, pas D.**

## 6. L'architecture recommandée (C1)

### 6.1 Schéma

```
 Hôte ──(WebRTC, inchangé)──► Page MoonlightWeb (Chrome)
                               │  UI, entrées, audio, transport, repli
                               │  ① détecte le Booster (§8)
                               │  ② relaie les images codées (unités d'accès)
                               ▼  ws://127.0.0.1:<port>  (binaire, 1 message = 1 image)
                         Booster (processus utilisateur, lancé à la demande)
                               │  ③ décode (D3D11VA / VideoToolbox / VA-API·Vulkan)
                               │  ④ présente : flip model, tearing, dernière image gagne
                               ▼
                         Fenêtre superposée, sans focus, transparente aux clics,
                         calée sur la zone vidéo de la page
                               │
                               └─► retours vers la page : image affichée (id, horodatage),
                                   profondeur de file, erreurs → signaux vers l'hôte
```

### 6.2 Ce que fait chaque côté

**La page** (changements futurs, derrière une clé cachée) :
- Garde tout le pipeline actuel, prêt. Avec le Booster, elle **cesse de
  décoder** et relaie chaque image codée remontée (`onVideo`, déjà en JS
  pour le DataChannel et la route audio [C]).
- La piste `webrtc-media` (lecture par `<video>`) n'expose pas les images
  codées : avec le Booster, elle passe par l'Encoded Transform comme U1.4 [C],
  ou le Booster reste inactif sur ce transport.
- Envoie au Booster : rectangle de la vidéo, `devicePixelRatio`, plein écran
  ou non, visibilité de l'onglet, codec et paramètres de décodage.
- Reçoit : « image N affichée à T », profondeur de file, erreur de décodage.
  Elle en tire les signaux actuels vers l'hôte (pertes, `DecodeQueueSignal`,
  demande d'IDR).
- Repli : canal fermé, erreur ou silence de 250 ms → la page reconfigure son
  décodeur, demande une IDR (déjà prévu à la reconfiguration [C,
  `StreamView.js:2938-2948`]) et reprend le dessin. Coût : une IDR et
  ~1 aller-retour [H].

**Le Booster** :
- Un exécutable, sans interface hors d'une icône facultative. C++ comme le
  backend, pour reprendre ce qui existe : parsing SPS/PPS, PyroWave de
  référence (`third_party/pyrowave`), libdatachannel pour C2 [C].
- Décode dans un processus à droits réduits (le flux vient du réseau ; le
  navigateur, lui, décode dans un processus GPU isolé).
- Trouve la fenêtre du navigateur et sa zone de contenu :
  Windows par le processus et la classe de la fenêtre de rendu ; macOS par
  `CGWindowList` (limites de fenêtre, sans le titre) ; X11 par `_NET_*`.
  **V1 : plein écran seulement**. L'écran entier est alors la zone, et le
  placement devient trivial. La fenêtre agrandie vient après, si la V1 tient.
- Présente « la dernière image gagne », au plus une image en file, et respecte
  la règle du projet : latence d'abord, cadencement en option.
- Masque sa fenêtre dès que la page le demande (menu, onglet caché,
  sortie du plein écran), et la page reprend la main.

**Entrées** : elles restent au navigateur. La superposition ne prend jamais le
focus (`WS_EX_NOACTIVATE`) et laisse passer les clics (`WS_EX_TRANSPARENT`,
`ignoresMouseEvents`, forme d'entrée vide sous X11) [D]. Le verrouillage du
pointeur et le clavier restent ceux de Chrome. Le curseur système passe
au-dessus de toute fenêtre.

**Audio** : reste dans la page. La vidéo arrivant plus tôt, la
synchronisation labiale se décale de ce gain (quelques ms) [H] : sans effet en
jeu, à surveiller en vidéo.

### 6.3 Ce que le Booster peut prendre en plus, plus tard

- **PyroWave natif** : décodeur Vulkan de référence, ou portage D3D12 du HLSL
  de l'hôte [C, `ultra-lan-poc.md` §6.14]. La file WebGPU disparaît, et la
  limite des 4:4:4/HDR du navigateur aussi. *Relu le 23/10* : le POC Ultra est
  clos depuis le 10/10 (§6.43) ; sur un PC NVIDIA en Ethernet, PyroWave arrive
  ~1 ms après le HEVC. Depuis la v0.4.0 il est un codec à choisir soi-même,
  « PyroWave (LAN filaire uniquement) », jamais pris seul (`bc9f64f2`). Un
  décodeur natif ne profiterait donc qu'à ce choix : option, pas objectif.
- **HDR** : swapchain HDR10/scRGB, `CAMetalLayer` EDR, sans le tone mapping
  par WebGPU [C, `_hdrMode`].
- **Upscalers** (FSR1, NIS) en natif, sans le coût WebGPU [C, wiki §15.2].

### 6.4 C2, le Booster pair de l'hôte (option)

Le Booster rejoint la session comme second pair de l'hôte, pour la vidéo
seule, comme un invité du flux commun. C'est le seul moyen d'échapper au
métronome RTP et à la pile SCTP de Chrome. Prix : authentification
(clés MW-BIND), ICE, rendez-vous, et les invariants des sessions partagées
côté hôte (`quit()` du jumeau, take-over) [C, notes de projet]. À n'ouvrir
que si B3 montre que le transport, et non la présentation, domine.

## 7. Choix par plateforme

### 7.1 Windows (cible de la V1)

- **Décodage** : D3D11 Video (D3D11VA) pour H.264, HEVC (dont 4:4:4 et 10 bits
  si le GPU le décode), AV1 [D]. D3D12 Video possible mais plus coûteux à
  écrire, sans gain attendu sur un décodage déjà < 1 ms [C]. Media Foundation :
  à éviter (sa MFT ajoute de la file) [H].
- **Présentation** : swapchain `FLIP_DISCARD` + `ALLOW_TEARING`, objet
  d'attente (`GetFrameLatencyWaitableObject`), latence max 1 [D]. Fenêtre
  `WS_EX_NOREDIRECTIONBITMAP | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT |
  WS_EX_TOPMOST`, composée par DirectComposition [D].
  À vérifier par PresentMon : passage en « independent flip » ou plan
  matériel (MPO) en plein écran [H]. Sinon, le gain se réduit à l'écart entre
  la composition de Chrome et celle du DWM seul.
- **Pièges** : occlusion de Chrome (§3.5) ; mise à l'échelle DPI par écran ;
  Alt+Tab et notifications par-dessus ; Edge et Chrome ont des fenêtres
  différentes ; GPU hybride (le Booster doit décoder et présenter sur le GPU
  de l'écran, jamais copier entre GPU).
- **Distribution** : installateur par utilisateur, sans droits administrateur
  (schéma d'URL en `HKCU`), signé Authenticode par la chaîne SignPath déjà en
  place pour l'hôte [C, `release.yml`]. Mise à jour : reprise de
  `SelfUpdater` [C].

### 7.2 macOS

- **Décodage** : `VTDecompressionSession` avec la propriété temps réel ;
  sorties `CVPixelBuffer` adossées à `IOSurface`, transformées en textures
  Metal sans copie par `CVMetalTextureCache` [D].
- **Présentation** : `CAMetalLayer`, `maximumDrawableCount` 2,
  `displaySyncEnabled` à faux si possible [D]. Pas de tearing en fenêtre sous
  macOS ; en plein écran, le passage direct à l'écran est décidé par le
  système [H].
- **Pièges** : le plein écran natif d'un navigateur ouvre un Space à part ; la
  superposition doit le rejoindre (`fullScreenAuxiliary`,
  `canJoinAllSpaces`) [D], et le comportement varie d'une version à l'autre
  [H]. VideoToolbox ne passe pas une perte de référence [C,
  `decoderRidesOutGaps`] : la logique de réparation de la page reste valable.
- **Distribution** : l'app de l'hôte est signée par un certificat autosigné,
  le `.pkg` n'est ni signé ni notarié [C]. Pour un composant que des joueurs
  téléchargent sur leur Mac, Gatekeeper bloquera sans notarisation :
  **compte Apple Developer (99 $/an) à décider** (§13).

### 7.3 Linux

- **Décodage** : VA-API (Intel, AMD), Vulkan Video (AMD, NVIDIA, Intel
  récents), NVDEC en option ; sortie DMA-BUF importée dans Vulkan sans
  copie [D]. Le dépôt a déjà l'expérience de Vulkan Video et de VA-API côté
  hôte [C, notes D3D12/Vulkan].
- **Présentation** : swapchain Vulkan `MAILBOX` ou `IMMEDIATE`.
- **Superposition** : X11 sûr (fenêtre `override-redirect`, forme d'entrée
  vide) ; KDE Wayland par `wlr-layer-shell` (KWin le prend en charge) [D] ;
  **GNOME Wayland impossible** (xdg-shell : ni position ni empilement) [D].
  Chrome sous Wayland natif ou XWayland change la donne [H].
- **Intérêt propre** : c'est sous Linux que Chrome décode le plus souvent en
  logiciel (VA-API inégal selon distributions et versions) [H]. Le Booster y
  gagnerait du décodage, mais c'est aussi là qu'il s'affiche le moins bien.
- **Distribution** : `.deb`/`.rpm`/AppImage et dépôts signés déjà en place [C,
  `make-packages.sh`, `make-repo.sh`]. Les navigateurs en Flatpak ou Snap ne
  lancent pas facilement un programme de l'hôte : schéma d'URL par le portail,
  Native Messaging bloqué [H].

### 7.4 Android et iOS

- **Android** : une page ne lance pas une app et ne la joint pas en local
  sans les mêmes demandes ; surtout, une app ne s'affiche au-dessus du
  navigateur qu'avec la permission d'overlay système, gérée à part, qui
  n'est pas faite pour une vidéo plein écran [D]. **Voie réaliste : l'app
  native** (Trusted Web Activity pour l'interface, ou l'appli TV existante).
  Pas de Booster.
- **iOS / iPadOS** : pas de processus en arrière-plan joignable par Safari,
  pas de superposition. **Pas de Booster.** Voie possible : une app avec
  WKWebView + couche Metal (forme E), soumise à la revue de l'App Store.
  À rouvrir seulement si E réussit sur bureau.

## 8. Sécurité et communication

### 8.1 Menaces

- **N'importe quel site** peut sonder le port du Booster ou lui parler
  (empreinte, CSRF, DNS rebinding vers `localhost`).
- **Flux hostile** : le Booster décode des octets venus du réseau, hors du bac
  à sable du navigateur ; les décodeurs matériels et leurs pilotes sont une
  surface d'attaque.
- **Détournement** : un Booster qui accepterait des commandes (lancer,
  ouvrir, écrire) serait un outil d'exécution à distance.
- **Mise à jour** : un canal de mise à jour falsifié installe du code.

### 8.2 Mécanismes

1. **Pas de service** : lancé par le schéma d'URL sur un geste de
   l'utilisateur, il quitte seul (§3.3).
2. **Loopback seulement**, `127.0.0.1` et `::1`, jamais `0.0.0.0`. Port fixe
   documenté (à choisir loin de 47984-48010 de Sunshine et des ports du
   produit), repli sur deux ports voisins.
3. **Jeton à usage unique** : la page tire 256 bits au hasard, les passe dans
   l'URL de lancement ; le premier message du canal doit les prouver
   (HMAC du défi du Booster). Un site tiers ne peut pas lancer le Booster
   sans geste ni sans confirmation du navigateur, et ne connaît pas le jeton.
4. **Contrôle d'origine** : en-tête `Origin` comparé à une liste
   (`https://stream.moonlightweb.top`, `stream.dev…`, les origines LAN que
   l'utilisateur a validées). Refus de `Host` autre que `127.0.0.1:<port>`
   (contre le DNS rebinding).
5. **Protocole minimal et fermé** : « configure », « image codée »,
   « rectangle », « montrer / cacher », « fin ». Aucune commande, aucun
   chemin de fichier, aucune URL. Taille maximale par message ; un message
   hors protocole ferme le canal.
6. **Décodage confiné** : processus de décodage à droits réduits
   (AppContainer / jeton restreint sous Windows, App Sandbox sous macOS,
   seccomp sous Linux) [H, à chiffrer en B3].
7. **Version** : poignée de main avec version du protocole ; la page refuse
   un Booster trop ancien ou trop récent et reste en web.
8. **Mise à jour** : paquet signé, vérifié avant installation, par la chaîne
   de l'hôte (`SelfUpdater`, signature déjà vérifiée en mode « bundle » sur
   macOS [C]).
9. **Option extension** (plutôt qu'une demande « réseau local ») : l'extension
   reçoit le port et le jeton par Native Messaging, la page les obtient par
   `externally_connectable` ; l'hôte natif vérifie l'identifiant de
   l'extension [D]. À choisir en B2 selon ce que les utilisateurs acceptent
   le mieux : une demande d'autorisation ou une extension.

### 8.3 Ce que l'utilisateur voit

Une première fois : « Installer le Booster » (lien depuis les réglages), puis
la confirmation du navigateur pour ouvrir le Booster, puis la demande
« réseau local ». Ensuite : rien, sauf un indicateur « Booster actif » dans
l'overlay de stats.

## 9. Gains attendus, incertitudes, régressions

### 9.1 Latence (HEVC / H.264 / AV1)

| Poste | Aujourd'hui | Avec C1 | Nature |
|---|---|---|---|
| Relais page → Booster | 0 | +0,1-0,5 ms | [H] |
| Décodage | 0,4-5 ms | ~idem | [C] puis [H] |
| Dessin → photon | 15-25 ms au-delà du canevas | 1 rafraîchissement (DWM) à ~0 (independent flip + tearing) | [H] |
| **Total, médiane** | — | **0 à −16 ms à 120 Hz** (0 à 2 images), plus à 60 Hz | **[H], à prouver par B0 puis B1** |

L'incertitude est entière : le repère Steam HEVC (≈ MoonlightWeb) dit que le
natif peut ne rien gagner. Le PyroWave de Steam dit l'inverse, sans séparer
codec et présentation. Le gain sera par marches d'une image d'affichage, et
une même configuration peut changer de marche d'une passe à l'autre [C,
§6.5] : il faut des séries longues et alternées.

### 9.2 PyroWave

- Décodage : de 6,1-6,4 ms (page, 780M, 1440p) vers ~3,2 ms (le temps GPU) ;
  sur une carte dédiée, sous la milliseconde [C → H].
- Plus le gain de présentation du §9.1.
- *Relu le 23/10* : ne compte que pour qui choisit ce codec (§6.3) ; le POC a
  fini ses propres gains de page (relance, tranches, iDWT) le 10/10, et les
  6,1-6,4 ms ci-dessus datent d'avant.

### 9.3 Hors latence

- Codecs refusés par le navigateur : HEVC sous Firefox, décodage matériel
  sous Chrome Linux, HEVC 4:4:4 10 bits (vert sous Chrome Windows [C]).
- HDR natif, sans tone mapping dans WebGPU.
- Moins de travail sur le fil principal de la page : le décodage et le dessin
  sortent du fil principal (aujourd'hui les deux y sont [C]).

**Rejugé le 04/11/2026, sans banc : aucun de ces intérêts ne justifie le
Booster.** Sonde locale de `VideoDecoder.isConfigSupported`, décodage matériel
coupé, sans rien décoder [C] :

| Accepté en logiciel | Chrome 156 | Firefox 158 |
|---|---|---|
| H.264 High | oui | oui |
| H.264 High 4:4:4 | oui | non |
| HEVC, Main à RExt 4:4:4 | non | non |
| AV1 Main, 8 et 10 bits | oui | oui |
| AV1 High 4:4:4, 8 et 10 bits | oui | non |

Firefox répond aussi oui à `prefer-hardware` avec le matériel coupé : sa réponse
ne prouve pas un décodage matériel. Son HEVC avec le matériel actif n'est pas
sondé ; au champ, il a échoué sur l'UM790Pro le 28/10 [C, `CodecAuto.js`].

- **HEVC sous Firefox** : l'AV1 existe déjà dans le navigateur, sans rien
  installer (§16). Sous Windows, qui accepte d'installer un programme peut aussi
  prendre Chrome.
- **Décodage matériel sous Chrome Linux** : la superposition est impossible sous
  GNOME Wayland (§7.3), et moonlight-qt couvre déjà Sunshine. L'état réel de
  VA-API en 2026 n'est ni tranché par les sources ni mesuré ici [H].
- **4:4:4** : le 8 bits passe déjà sur RTX (wiki, ch. 15 §15.6). L'AV1 4:4:4
  manque à l'encodeur (NVENC), et un Booster n'y change rien. Reste le 10 bits,
  vert sous Chrome Windows : une niche.
- **HDR natif** : seule inconnue mesurable, B0 étant en SDR. Aucun client natif
  n'a été mesuré en HDR. Même avec un gain, les coûts du §11 resteraient, pour les
  seuls joueurs HDR sous Windows : ce banc ne sera pas fait.

### 9.4 Risques de régression

- **Chrome se croit masqué** et ralentit la page : entrées et transport
  dégradés (§3.5).
- **Double chemin** : chaque correctif du décodage de la page (repli de codecs,
  réparations, Apple sans ride-out) doit avoir son pendant dans le Booster.
- **Sondes aveugles** : `mwLatency`, `FrameLog`, `ContentAgeProbe` lisent le
  canevas. Il faudra leur pendant natif, sinon les bancs perdent leurs
  repères.
- **Repli lent** : une panne du Booster coûte une IDR et un temps sans image.
- **Énergie** sur portable : un deuxième processus GPU actif.
- **Capture d'écran et partage** (OBS, Discord, Teams) : une fenêtre
  superposée se capture autrement que l'onglet.
- **Support** : une classe de bogues « avec Booster seulement ».

## 10. Le parcours, du premier banc à la distribution

Chaque phase s'ouvre sur le « go » de Bruno, avec l'effort conseillé (Opus 5.5).
Rien ne se fait avant la sortie de la v0.4.0 ; le 21/10, Bruno a placé B0
après la v0.4.1, et l'a ouverte le 23/10 (§10.3).

| Phase | Ce qu'elle fait | Code produit ? | Porte | Effort |
|---|---|---|---|---|
| **B0** | Borne native sans Booster : même hôte, même clic, `click-photon` sur le bureau composé du client. Comparer MoonlightWeb (Chrome, Canvas2D, tearing) à **moonlight-qt** (D3D11VA, flip, V-Sync coupé) et à `mw-click-target` comme référence d'affichage. HEVC à 120 et 60 Hz, Windows (UM790Pro), puis Mac. | **non** | **B0** | `high` |
| **B1** | Labo de présentation superposée, outil hors produit (`tools/booster-lab`, comme `mw-click-target`) : il décode un flux enregistré ou relayé, et s'affiche au-dessus d'un Chrome en plein écran. PresentMon (mode de présentation), occlusion de Chrome (rAF, minuteries de la page), clic au photon. Windows seul. | **non** (outil) | **B1** | `xhigh` |
| **B2** | Canal et lancement : schéma d'URL, WebSocket loopback, demande « réseau local » dans Chrome, Edge, Firefox ; coût par image ; option extension. Labo seulement. | **non** (labo) | **B2** | `high` |
| **B3** | Intégration Windows derrière `mw_booster=1` : relais des images, retours vers l'hôte, repli, sondes natives ; H.264, HEVC, AV1. Plein écran seulement. | oui, caché | **B3** | `xhigh` |
| **B4** | PyroWave natif dans le Booster (codec à choisir soi-même depuis la v0.4.0, §6.3) ; HDR natif. | oui, caché | — | `high` |
| **B5** | macOS (VideoToolbox, Metal, Spaces). | oui, caché | **B5** | `xhigh` |
| **B6** | Linux X11 et KDE Wayland ; GNOME Wayland déclaré non pris en charge. | oui, caché | — | `high` |
| **B7** | Distribution : installateurs par utilisateur, signature (Authenticode, notarisation si décidée), mise à jour, page « Installer le Booster », textes i18n. | oui | **B7** | `high` |
| **B8** | Fenêtre agrandie (hors plein écran), si la V1 tient. | oui | — | `high` |
| **B9** | (option) C2 : le Booster pair de l'hôte. | oui | **B9** | `max` |

### 10.1 Mesures

- Clic → photon au bureau composé du client (`click-photon.ps1`, déjà utilisé
  pour Steam [C]), 60 clics par case au moins, en alternance ABAB, médiane et
  p90, et l'écran du client nommé avec sa fréquence.
- Âge de l'hôte à l'affichage (le Booster horodate sa présentation sur
  l'horloge partagée, comme `FrameLog`).
- Images répétées et perdues ; à-coups au défilement.
- PresentMon : mode de présentation de la superposition et de Chrome.
- Page : cadence de rAF et des minuteries pendant la superposition
  (contrôle de l'occlusion).
- CPU, GPU, énergie (portable) avec et sans Booster.
- Repli : temps sans image après la mort forcée du Booster.

### 10.2 Portes et critères d'abandon

- **B0 — abandon** si moonlight-qt n'affiche pas le clic **au moins une image
  d'affichage plus tôt** que MoonlightWeb en médiane (≥ 4 ms à 120 Hz,
  ≥ 8 ms à 60 Hz) sans p90 plus mauvais, sur Windows. Le gain maximal du
  Booster serait alors nul, et seul l'intérêt « codecs » (§9.3) resterait :
  à rejuger à part.
- **B1 — abandon de C** si la superposition (a) ne gagne pas au moins 75 % de
  l'écart B0, ou (b) fait ralentir la page (rAF ou minuteries < 95 % de leur
  cadence), ou (c) ne tient pas en plein écran sur deux GPU (NVIDIA, AMD).
  Bascule vers l'étude de E, sur décision de Bruno.
- **B2 — abandon du loopback** si le canal coûte plus de 0,5 ms au p99 par
  image, ou si la demande « réseau local » ne peut pas être obtenue proprement
  depuis la page du rendez-vous. Alors extension, ou arrêt.
- **B3 — pas d'activation** si un seul repli (codec, transport, pertes) régresse
  dans la suite de bancs existante, ou si le repli prend plus de 500 ms.
- **B5 — macOS hors périmètre** si la superposition ne suit pas le Space du
  plein écran sur deux versions de macOS.
- **B7 — pas de distribution** sans signature reconnue par l'OS (sinon,
  l'installation demande de contourner un avertissement de sécurité).

### 10.3 Protocole de B0 (écrit le 23/10/2026, avant le banc)

**La question.** Un client natif sobre affiche-t-il le clic plus tôt que la
page, sur la même machine cliente, avec le même hôte ? B0 ne construit rien :
elle mesure moonlight-qt, le client natif de référence, contre MoonlightWeb.

**Le banc.**
- Hôte : DualRTX, encodage sur la RTX (NVENC), écran streamé = celui de la RTX
  (`\\.\DISPLAY5`, primaire). Le drapeau `scripts/bench/photon/flag-window.ps1`
  sur cet écran (`-Screen` par défaut = le primaire ; `-Log` pour dater chaque
  bascule). Luminosité de l'écran sans effet sur la mesure (DDA lit la mémoire
  d'image) ; les règles de nuit de DISPLAY5 restent telles quelles.
- Client : UM790Pro sous Windows (780M, décodage matériel), en Ethernet,
  session console de `minis`. Rien ne change aux écrans ni à leur mode ; la
  fréquence de l'écran du client est relevée au début (`monitors.ps1`, et
  `Get-CimInstance WmiMonitorListedSupportedSourceModes` ou le panneau
  d'affichage) et écrite dans le relevé : la marche d'une image en dépend.
- Mesure : `click-photon.ps1` par `run-in-console.ps1` (tâche interactive,
  session console), 60 clics par passe, 700 ms d'intervalle, lecture au point
  du clic (`-ReadDx 0 -ReadDy 0`, le drapeau de latence de la page éteint,
  comme au produit). Souris intouchée, fenêtre jamais couverte, **clients en
  fenêtre** (la lecture GDI du bureau composé n'est pas prouvée en plein écran
  exclusif ; même règle qu'au banc Steam du 04/10).
- Son : rien d'audible. Chrome en `--mute-audio` ; moonlight-qt n'a pas de
  réglage de sourdine, donc la sortie du client est coupée avant
  (`C:\Users\minis\mw-photon\audio-mute.ps1 -Action Mute`, `-Action Restore`
  à la fin).
- Préalables tenus par la session d'orchestration : la vidéo en kiosque
  arrêtée (`start-cod.ps1 -Stop`), les invitations spectateur finies, aucune
  `--dev` sur UDP 3478-3481, Chrome de banc avec son propre `--user-data-dir`.

**Les cases.** Trois clients pour le même hôte physique :

| Case | Client | Hôte | Ce qu'elle isole |
|---|---|---|---|
| **A** | moonlight-qt 6.2.0, décodage D3D11VA, swapchain flip + tearing (`d3d11va.cpp`), V-Sync coupé, cadencement coupé, fenêtre | **Sunshine** de DualRTX (installé, pairing web `admin`), app « Desktop » | le client natif |
| **B** | MoonlightWeb dans Chrome : Canvas2D, tearing, « Auto » (défauts du produit) | **hôte natif** MoonlightWeb de DualRTX (DDA + NVENC), `--dev` de `build\` prêtée par 59 ou démarrée pour le banc, « Display 1 » = DISPLAY5 | le produit tel quel |
| **C** | la même page, mêmes défauts | le **même Sunshine** que A, enregistré comme hôte dans MoonlightWeb (le backend relaie) | l'hôte : si C ≈ A, l'écart A/B vient de l'hôte, pas de la page |

Deux cadences, l'écran du client inchangé : **120 i/s** (moonlight-qt
`--fps 120` ; la page en « Auto », puis une passe à 120 fixes si l'« Auto »
s'est posé ailleurs) et **60 i/s**. HEVC partout, 1920×1080, même débit (celui
que la page règle pour 1080p ; moonlight-qt `--bitrate` sur la même valeur).

moonlight-qt, sans rien installer : `MoonlightPortable-x64-6.2.0.zip` de la
release du 04/10/2026 (ou `winget install MoonlightGameStreamingProject.Moonlight`
si Bruno préfère l'installer), décompressée dans le profil de `minis`, effacée
à la fin. Appairage : `Moonlight.exe pair <IP de DualRTX> --pin 1234`, PIN
confirmé à Sunshine (`https://<DualRTX>:47990`, `/api/pin` avec son
`pairing_id`, voir la note de parc). Passe :

```
Moonlight.exe stream <IP de DualRTX> Desktop --1080 --fps 120 --bitrate <kbps>
  --video-codec HEVC --video-decoder hardware --display-mode windowed
  --no-vsync --no-frame-pacing --no-audio-on-host --no-performance-overlay
```

puis, sur le client, `run-in-console.ps1 -Tag a-120-1 -Process Moonlight`.
Pour la page : `run-in-console.ps1 -Tag b-120-1 -Process chrome -Title MoonlightWeb`.
Les JSON vont dans `bench-out/photon/b0-*.json`, à côté de ceux du 04/10.

**L'ordre.** À 120 i/s : A1 B1 A2 B2, puis C1 C2 ; à 60 i/s : la même suite.
Douze passes, ~1 h 30 avec les bascules. Chaque passe note : codec, cadence
réglée, cadence vue (page : `Images dessinées par seconde` de l'overlay lu
après coup, pas pendant ; moonlight-qt : rien, overlay coupé), fréquence de
l'écran du client, hôte, heure.

**La lecture.** Médiane et p90 par case, et l'écart A − B en **images
d'affichage du client** (période = 1000 / fréquence de l'écran du client).
- A plus tôt que B d'au moins une image, et C ≈ B : **la page coûte au moins
  une image** ; la porte B0 est franchie, B1 peut s'ouvrir.
- A plus tôt que B, mais C ≈ A : l'écart vient de l'hôte (Sunshine contre hôte
  natif), pas du client ; **B0 n'est pas franchie** pour le Booster, et le
  constat part dans les plans de l'hôte.
- A ≈ B (moins d'une image) : **abandon**, comme au §10.2 ; reste l'intérêt
  « codecs » du §9.3, à rejuger à part.
- Les médianes tombent sur des marches (une image d'écran, §6.5 du POC) : un
  écart qui change de signe entre les deux manches n'en est pas un.

**Les pièges déjà connus.** Le pairage périmé de l'origine de banc
(`identity_refused` : effacer l'origine par CDP) ; un PIN de banc à la fois,
révoqué à la fin ; l'heure de l'UM790Pro souvent en retard de 2 h au retour
d'Ubuntu (sans effet sur `click-photon`, qui n'use que de l'horloge
monotone) ; `query user` qui sort en code 1 ; une fenêtre Chrome de Bruno
par-dessus l'écran streamé de l'hôte.

**Ce qui reste à Bruno avant le banc** (demandé par la session
d'orchestration) : accord pour moonlight-qt portable sur l'UM790Pro ; le
créneau lui-même, après le retour de Qwergin sur son test, la libération de
l'UM790Pro et le banc N95 (~5 min), avant le retour de la machine sous
Ubuntu ; et un moment où il n'est pas devant DualRTX, puisque le drapeau
couvre DISPLAY5, son écran principal, pendant chaque passe. Le PIN de la
`--dev` prêtée reste le sien : aucun PIN généré ni effacé sans le dire.

### 10.4 Résultats de B0 (banc du 24/10/2026) — porte non franchie, écart de l'hôte non reproduit

> **Non reproduit (24/10/2026 au soir).** L'image perdue par l'hôte natif, lue
> plus bas, ne tient pas. Deux bancs le soir même, même client, même hôte, la
> page en 1920×1080 à 120 i/s, HEVC, 40 Mbit/s [C] :
> - huit passes de 60 clics, en deux manches : hôte natif sans plancher de
>   cadence, avec un plancher à 30 puis à 120 i/s, et Sunshine. Les marches de
>   8,3 ms changent d'une manche à l'autre sans suivre l'hôte ; en moyenne,
>   38,9 ms pour l'hôte natif sans plancher, 38,4 ms pour Sunshine. La marche
>   suit la phase entre l'écran de DualRTX et celui de l'UM790Pro, qui glisse
>   d'une image toutes les quatre minutes environ. Le plancher ne déplace rien
>   de façon stable : l'hypothèse du GPU endormi est réfutée ;
> - quatre passes de 270 clics, chacune un tour de phase entier, hôte natif et
>   Sunshine alternés : médianes 33,3 · 33,9 · 33,5 · 34,4 ms ; réunies, sur
>   540 clics de chaque côté, **33,4 ms pour l'hôte natif, 34,1 ms pour
>   Sunshine**.
>
> L'écart de B0 s'explique ainsi [H] : une passe de 60 clics dure 42 s et tombe
> sur une seule marche, et les passes B sont tombées sur la marche haute. La
> porte reste non franchie, pour une raison plus simple : sur ce banc, ni la
> page ni l'hôte natif ne perdent d'image. Fichiers :
> `bench-out/photon/lost-*.json` et `phase-*.json` [C].

**Ce qui a tourné.** Client : UM790Pro sous Windows 11 26200, Radeon 780M,
écran M27Q 2560×1440 à 120 Hz, Ethernet 1 Gbit/s, session de `minis`, son
coupé. Hôte : DualRTX, écran mesuré DISPLAY5 (RTX 5060 Ti du bus 3, principal,
2560×1440 à 120 Hz), drapeau noir/blanc `flag-window.ps1` par-dessus ; hôte
natif = la `--dev` de `build\` au commit `18bfea44` (exe et frontend du même
commit, redémarrée à 15:12), tuile « Display 2 » ; Sunshine 2026.921.13701 en
configuration par défaut, app « Desktop ». Partout : HEVC, 60 clics par
passe espacés de 700 ms, lecture `GetPixel` sur le bureau composé du client
(`click-photon.ps1` via `run-in-console.ps1`). Le même débit partout, voulu
par le §10.3, n'a pas été tenu ; les journaux du worker, relus après coup,
donnent : à 120 i/s, B et C en 1920×1080 à 40 Mbit/s (ce que la page règle
pour 1080p120), A en 1920×1080 à 20 Mbit/s (`--bitrate 20000`) ; à 60 i/s,
A et C en 1920×1080 à 20 Mbit/s, mais B en 2560×1440 à 36 Mbit/s (la page
repassée en « Auto » après le réglage de 60 i/s). **Seule la série à 120 i/s
compte pour la porte**, et B s'y compare à C, même page et mêmes réglages.
Case A : moonlight-qt 6.2.0 portable, D3D11VA, fenêtré, V-Sync et cadencement
coupés. Cases B et C : MoonlightWeb dans un profil Chrome 155 neuf, défauts du
produit (Canvas2D, 120 puis 60 i/s réglés dans la page), la page en premier
plan. La pré-passe à l'overlay (non mesurée, à 120 i/s) a confirmé ce que la
page négocie : 1920×1080, HEVC, transport `webrtc-dc-udp`, latence réseau
affichée 4 à 5 ms.
Vingt passes au lieu de douze : quatre de plus à 120 i/s pour asseoir la case B.
Fichiers : `bench-out/photon/b0-<a|b|c>-<120|60>-<n>.json` [C].

**Les médianes, en millisecondes clic → photon** (p90 entre parenthèses) [C] :

| Cadence | A — moonlight-qt ↔ Sunshine | B — page ↔ hôte natif | C — page ↔ Sunshine |
|---|---|---|---|
| 120 i/s | 32,5 (33,8) · 32,7 (34,3) · 24,9 (33,1) | 33,6 (49,2) · 41,1 (49,6) · 41,8 (50,0) · 41,3 (49,0) | 34,0 (41,6) · 33,4 (40,1) · 34,0 (42,5) |
| 60 i/s | 33,2 (41,3) · 33,0 (34,6) | 41,3 (48,8) · 41,9 (50,0) † | 33,9 (42,6) · 34,5 (42,6) |
| **Toutes les mesures réunies, 120 i/s** | **32,3** (33,6), n = 177 | **41,3** (49,6), n = 240 | **33,6** (42,0), n = 180 |
| **Toutes les mesures réunies, 60 i/s** | **33,0** (41,0), n = 118 | **41,6** (49,8), n = 120 † | **34,3** (42,6), n = 120 |

Les médianes tombent sur les marches attendues, espacées d'une image de
l'écran du client (8,33 ms à 120 Hz) : A et C sur la marche 32-34 ms (A une
fois sur la marche du dessous, 24,9 ; B une fois sur la marche 33,6 à sa
première passe), B sur la marche 41-42 ms. moonlight-qt a perdu un clic sur 60
à chaque passe ; la page aucun. † B à 60 i/s tournait en 2560×1440 à
36 Mbit/s : ces deux cases ne se comparent pas aux autres.

**La lecture, selon les règles du §10.3.**
- **A est plus tôt que B d'une image** : 9,0 ms à 120 i/s, soit 1,1 image
  de l'écran du client, avec un p90 meilleur de 16 ms. À réglages égaux, B
  est en retard de 7,7 ms sur C. L'écart de 8,6 ms à 60 i/s ne compte pas
  (B en 1440p).
- **Mais C ≈ A** : 1,3 ms d'écart à 120 i/s, alors que la page recevait le
  double du débit de moonlight-qt (40 contre 20 Mbit/s), et 1,2 ms à 60 i/s,
  cette fois à réglages égaux ; bien moins qu'une image. La même page, devant
  le même Sunshine, affiche le clic en même temps que le client natif.
- Donc **l'image perdue vient de l'hôte, pas de la page** (lecture de
  l'après-midi, non reproduite, voir l'encadré en tête du §10.4) : l'hôte natif
  MoonlightWeb (DDA + NVENC, `--dev` du 24/10) livre le clic une image après
  Sunshine sur le même GPU et le même écran. C'est le deuxième cas du §10.3 :
  **B0 n'est pas franchie pour le Booster**, et le constat part dans les plans
  de l'hôte natif.

**Ce que cela ferme et ce que cela ouvre.**
- Le poste « 15 à 25 ms entre le dessin dans le canevas et le bureau
  composé » du POC Ultra (§0, §2.2) n'apparaît pas ici en défaveur de la page :
  au clic → photon, en fenêtré, Canvas2D avec tearing, la page vaut
  moonlight-qt à 1 ms près. Un présentateur superposé n'aurait donc rien, ou
  presque, à reprendre sur ce chemin : **B1 ne s'ouvre pas**. Reste l'intérêt
  « codecs » du §9.3, à rejuger à part si Bruno le souhaite.
- ~~La seule marge mesurée, une image à 120 Hz, est côté hôte natif~~ :
  non reproduite le soir même (encadré en tête du §10.4). Le banc dédié de
  l'hôte, avec ses horodatages de capture, d'encodage et d'envoi, a eu lieu ;
  il trouve l'hôte natif à égalité avec Sunshine.
- Mesures limitées à un client (780M sous Windows, écran 120 Hz, Ethernet) et à
  un hôte ; aucun élément ne laisse attendre un autre ordre sur un client plus
  lent, où le décodage pèserait pareil dans les deux clients [H].

**Incident sans trace.** Au premier lancement de moonlight-qt, la boîte du
pare-feu Windows s'est ouverte au centre de l'écran du client et a absorbé la
première passe A (0 clic mesuré) ; passe refaite après fermeture de la boîte
sans « Autoriser », et les deux règles « Block » créées par Windows pour cet
exécutable retirées au rangement. Tout le reste a été défait : moonlight-qt et
son appairage Sunshine, le profil Chrome de banc, les tâches planifiées, le
son, la session de banc de la `--dev` ; le PIN laissé tel quel.

## 11. Coûts de développement et de maintenance

| Poste | Windows | macOS | Linux |
|---|---|---|---|
| Décodage natif | moyen (D3D11VA, connu par moonlight-qt) | moyen (VideoToolbox) | élevé (VA-API + Vulkan Video + pilotes variés) |
| Présentation et superposition | moyen, risque occlusion | élevé (Spaces, versions) | élevé (X11 / KDE / non GNOME) |
| Distribution | faible (Inno, SignPath en place) | moyen à élevé (notarisation à décider) | moyen (paquets en place ; Flatpak/Snap des navigateurs) |
| Maintenance continue | suivre Chrome, Edge, Firefox (occlusion, demande réseau local) et les pilotes | suivre macOS chaque année | suivre Mesa, compositeurs |

Au total, un second pipeline de décodage et de présentation à garder au niveau
du premier, par OS. Le coût récurrent pèse plus que l'écriture : chaque
version de navigateur peut casser la superposition ou le canal.

## 12. Ce qui est établi, ce qui est supposé

- **Établi** [D] : pas d'import de surface GPU inter-processus dans une page ;
  demande « réseau local » de Chrome (142, 147) et Firefox (149/151) ; limites
  de Native Messaging ; occlusion de Chrome sous Windows ; xdg-shell sans
  placement.
- **Établi** [C] : décodage du navigateur < 1 ms sur iGPU récent ; 15-25 ms
  entre canevas et bureau composé ; MoonlightWeb ≈ Steam HEVC au photon ;
  file WebGPU ~3 ms ; décodage et dessin sur le fil principal.
- **Supposé** [H] : que l'independent flip s'obtient sous une superposition ;
  que Chrome ne se juge pas masqué ; le coût du relais loopback ; le gain
  final ; le comportement des Spaces ; le support de WebTransport loopback sur
  Safari ; l'effet de Chrome Wayland.

## 13. Décisions pour Bruno

**Clos le 04/11/2026** : « Produit Web Booster => A » (Bruno). Les décisions 2 à
10 ci-dessous n'ont plus d'objet. Il reste une seule suite, hors Booster : « Auto
en AV1 quand le HEVC échoue => B », à mesurer pour la v0.4.2 (§16).

1. ~~**Ouvrir B0** après la v0.4.0~~ — faite le 24/10/2026, non franchie (§10.4) :
   l'écart vu côté hôte natif n'a pas été reproduit le soir même (§10.4) ;
   rien à loger dans les plans de l'hôte.
2. Si B0 passe : **B1 sur Windows seul**, outil hors produit.
3. **Canal** (B2) : demande « réseau local » de la page, ou extension par
   navigateur, ou les deux.
4. **Port fixe** du Booster.
5. **Langage** : C++ (reprise du backend) ; Rust seulement si E/Tauri s'ouvre.
6. **macOS** : compte Apple Developer et notarisation, ou macOS hors V1.
7. **Linux** : GNOME Wayland déclaré non pris en charge.
8. **Périmètre V1** : plein écran seulement ; PyroWave et HDR natifs dans la
   V1 ou après.
9. **Nom** définitif du composant.
10. Si C échoue en B1 : étudier **E** (coquille native + WebView), ou arrêter.

## 14. Sources

Légende : ✓ = page ouverte et lue le 09/10/2026 ; ◦ = lue par extraits de
recherche le 09/10/2026 ; sans marque = référence officielle à relire en
B0-B2 avant de s'appuyer dessus.

Navigateurs, réseau local, contenu mixte
- ✓ Chrome, Local Network Access : https://developer.chrome.com/blog/local-network-access
- Spécification LNA (WICG) : https://wicg.github.io/local-network-access/
- ◦ Chrome, extension aux WebSockets (intention de livrer) : https://groups.google.com/a/chromium.org/g/blink-dev/c/O6GMKt44Ups
- Fiche Chrome Platform Status : https://chromestatus.com/feature/5152728072060928
- ◦ Firefox, autorisations réseau local : https://support.mozilla.org/en-US/kb/control-personal-device-local-network-permissions-firefox
- ◦ Firefox, politique `LocalNetworkAccess` : https://mozilla.github.io/enterprise-admin-reference/reference/policies/localnetworkaccess/
- ◦ WebKit, loopback et contenu mixte : https://bugs.webkit.org/show_bug.cgi?id=171934
- ◦ WebKit, régression iOS 18 : https://bugs.webkit.org/show_bug.cgi?id=279249
- Contenu mixte (W3C) : https://w3c.github.io/webappsec-mixed-content/
- Origines dignes de confiance : https://w3c.github.io/webappsec-secure-contexts/#is-origin-trustworthy
- WebTransport, `serverCertificateHashes` : https://www.w3.org/TR/webtransport/#dom-webtransportoptions-servercertificatehashes
- `registerProtocolHandler` : https://developer.mozilla.org/en-US/docs/Web/API/Navigator/registerProtocolHandler
- Schémas d'URL sous Windows : https://learn.microsoft.com/en-us/previous-versions/windows/internet-explorer/ie-developer/platform-apis/aa767914(v=vs.85)
- ◦ Chromium, occlusion des fenêtres sous Windows : https://chromium.googlesource.com/chromium/src/+/master/docs/windows_native_window_occlusion_tracking.md

Extensions et Native Messaging
- ✓ Chrome, Native Messaging : https://developer.chrome.com/docs/extensions/develop/concepts/native-messaging
- ◦ Chrome, `externally_connectable` : https://developer.chrome.com/docs/extensions/reference/manifest/externally-connectable
- Firefox, Native Messaging : https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/Native_messaging
- Safari, messages vers l'app native : https://developer.apple.com/documentation/safariservices/messaging-a-web-extension-s-native-app

API web média
- WebCodecs (constructeurs de `VideoFrame`) : https://www.w3.org/TR/webcodecs/#videoframe-constructors
- WebGPU, `importExternalTexture` : https://www.w3.org/TR/webgpu/#dom-gpudevice-importexternaltexture
- WebRTC Encoded Transform : https://w3c.github.io/webrtc-encoded-transform/

Windows
- Modèle flip DXGI : https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-flip-model
- Tearing et écrans à fréquence variable : https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/variable-refresh-rate-displays
- Objet d'attente de la swapchain : https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/nf-dxgi1_3-idxgiswapchain2-getframelatencywaitableobject
- API vidéo Direct3D 11 : https://learn.microsoft.com/en-us/windows/win32/medfound/direct3d-11-video-apis
- Direct3D 12 Video : https://learn.microsoft.com/en-us/windows/win32/medfound/direct3d-12-video-overview
- DirectComposition : https://learn.microsoft.com/en-us/windows/win32/directcomp/directcomposition-portal
- Styles étendus de fenêtre : https://learn.microsoft.com/en-us/windows/win32/winmsg/extended-window-styles

macOS
- `VTDecompressionSession` : https://developer.apple.com/documentation/videotoolbox/vtdecompressionsession
- `CVMetalTextureCache` : https://developer.apple.com/documentation/corevideo/cvmetaltexturecache
- `IOSurface` : https://developer.apple.com/documentation/iosurface
- `CAMetalLayer` : https://developer.apple.com/documentation/quartzcore/cametallayer
- `NSWindow.CollectionBehavior` : https://developer.apple.com/documentation/appkit/nswindow/collectionbehavior-swift.struct
- Notarisation : https://developer.apple.com/documentation/security/notarizing-macos-software-before-distribution

Linux
- VA-API : https://intel.github.io/libva/
- Vulkan Video, décodage : https://registry.khronos.org/vulkan/specs/latest/man/html/VK_KHR_video_decode_queue.html
- DMA-BUF : https://docs.kernel.org/driver-api/dma-buf.html
- xdg-shell : https://wayland.app/protocols/xdg-shell
- wlr-layer-shell : https://wayland.app/protocols/wlr-layer-shell-unstable-v1

Mobile et coquilles
- Trusted Web Activity : https://developer.android.com/develop/ui/views/layout/webapps/trusted-web-activities
- Android, permission d'overlay : https://developer.android.com/reference/android/Manifest.permission#SYSTEM_ALERT_WINDOW
- Electron : https://www.electronjs.org/docs/latest/
- Tauri, WebView par OS : https://v2.tauri.app/reference/webview-versions/
- moonlight-qt (borne B0) : https://github.com/moonlight-stream/moonlight-qt

Dans le dépôt : `docs/design/ultra-lan-poc.md` (§6.5, §6.9, §6.11, §6.13,
§6.17-6.18), `docs/design/click-waits.md`, `docs/design/glass-to-glass.md`,
`docs/wiki/05-Streaming-and-Transports.md`, `docs/wiki/09-Installers-and-Packaging.md`,
`frontend/js/ui/StreamView.js`, `frontend/js/api/WebRtcDataChannel.js`,
`frontend/js/stream/renderers/`, `frontend/js/stream/ultra/`.

## 15. Concrètement, pour l'utilisateur

Rien ne change aujourd'hui : c'est une étude, et MoonlightWeb reste une page
qu'on ouvre sans rien installer.

Si le projet aboutit, un joueur sur PC (puis sur Mac, et sous Linux hors
GNOME) pourra installer un petit programme facultatif. La première fois, le
navigateur lui demandera d'ouvrir ce programme et de laisser la page joindre
« les appareils du réseau local ». Ensuite, en plein écran, l'image ne passera
plus par l'affichage du navigateur : c'est le programme qui la montrera,
pendant que la page garde les menus, le son, la souris, le clavier et la
manette. Le joueur pourrait voir son geste à l'écran une ou deux images plus
tôt : un peu plus de réactivité en jeu rapide, rien de visible sur un bureau
ou une vidéo. Si le programme plante ou manque, l'image revient dans la page
après un court noir.

Ce que le projet ne règle pas : le Wi-Fi, l'encodage sur l'hôte, les
téléphones, les tablettes et les TV, et les bureaux Linux GNOME. Et il peut
très bien s'arrêter à sa première mesure, si un client natif ne fait pas
mieux que la page.

**C'est ce qui s'est passé (04/11/2026).** Le projet est clos : il n'y aura rien
à installer. MoonlightWeb reste une page, et elle réagit aussi vite qu'un client
natif sur le banc mesuré, en SDR.

## 16. Hors Booster : Auto en AV1 quand le HEVC échoue (protocole, v0.4.2)

**Rien n'est lancé ni codé** : le créneau vient de 59.

**La question.** Quand le HEVC échoue dans un navigateur, Auto demande le H.264
dès le lancement suivant (`autoLaunchCodec`). L'AV1 serait-il meilleur à sa place,
sans coûter en réactivité ? Les deux codecs se choisissent déjà dans le menu : le
banc ne demande aucun code.

**Le montage.** Hôte natif sur DualRTX, NVENC, instance `--dev` avec
`MW_LAN_ONLY=1`. Un seul facteur bouge : le codec. Le débit est fixé à la main,
le même pour les deux bras, à la valeur d'Auto relue à la passe préalable. Son
coupé sur le client, aucun mode d'écran touché, aucun drapeau de débogage laissé.

| Cas | Client | Ce qu'il représente | Réglages | Passes |
|---|---|---|---|---|
| C1 | Firefox sur l'UM790Pro sous Windows | le cas vu le 28/10 | 1080p120 et 1440p60 | 8 |
| C2 | Firefox sur le N95 | le processeur faible, le cas limite | 1080p60 | 4 |
| C3 | Chrome sur l'UM790Pro, `--disable-accelerated-video-decode` | Chrome Linux sans VA-API, **par approximation** | 1080p120 | 4 |

C3 tourne sous Windows, car `click-photon.ps1` n'existe que là : il dit le coût du
décodage logiciel, pas le comportement réel de Chrome sous Linux. Le vrai Linux
reste à lire au moment d'activer, sur l'UM790Pro sous Ubuntu.

**Les passes.** 270 clics chacune, soit un tour de phase entre DualRTX et
l'UM790Pro (≈ 3 min 10). Le tour change avec la paire d'écrans : il se relit à la
première passe du N95, et la passe s'allonge s'il est plus long. Bras alternés,
ABAB, sur deux manches. Lecture par la moyenne, avec médiane et p95.

**Ce qui est relevé à chaque passe.**
- clic → photon (`click-photon.ps1`) ;
- le décodeur vraiment utilisé (overlay, `frame.format`), car la réponse de
  Firefox à `prefer-hardware` n'en est pas une preuve (§9.3) ;
- processeur du client, en cœurs ;
- images perdues ou répétées, retard qui s'accumule (`docs/bench-campaign.md`
  a déjà vu l'AV1 logiciel accumuler des secondes).

**La qualité, à part.** Un clip de jeu déjà au banc, cadence réelle relue, encodé
hors stream en `h264_nvenc` et `av1_nvenc` (réglages à faible latence, proches de
ceux de l'hôte sans être les siens), au débit d'Auto et à sa moitié, puis comparé
en VMAF NEG (ffmpeg déjà installé sur DualRTX). Sur la RTX du bus 01, créneau de
59. Environ 15 min.

**Les portes, confirmées par Bruno le 04/11/2026** (« Portes du banc Auto en AV1
=> A »). L'AV1 remplace le H.264 dans Auto seulement si, pour C1, C2 et C3 :
1. le surcoût reste dans le seuil que Bruno a fixé le 26/10 pour l'AV1 en HDR :
   médiane +5 ms, p95 +10 ms, 2 cœurs au plus, à chaque réglage ;
2. pas plus d'images perdues qu'en H.264, et aucun retard qui s'accumule ;
3. VMAF NEG gagne au moins 3 points à l'un des deux débits : sans gain visible,
   changer de codec n'apporte rien.

Si C2 échoue seul, l'AV1 ne peut pas être réservé aux machines à décodeur
matériel sur la seule foi de `isConfigSupported` : ce serait une autre décision,
à prendre avec ses mesures.

**Durée.** Environ 2 h : 20 min de mise en place, 16 passes (≈ 1 h 10 avec les
lancements), 15 min de qualité, le reste en marge et en rangement.

**Machines et présence.** DualRTX (hôte, RTX du bus 01 pour la qualité), UM790Pro
sous Windows, N95. Bruno n'a pas à être là, et mieux vaut qu'il ne se serve pas de
DISPLAY5, où tombent les clics. Il intervient seulement pour redémarrer l'UM790Pro
sous Windows s'il est sous Ubuntu, taper la règle de pare-feu du N95 si elle
manque, et lire le verdict. Règles du N95 : 8 connexions ssh par série, présence
lue avant chaque passe.

**Résultats du 07/11/2026** (créneau de 59, 16:18-18:00, Bruno absent). Hôte natif
sur DualRTX, une seule `--dev` en `MW_LAN_ONLY=1`, aucune ligne `[UPNP]`. Écart au
protocole : le drapeau était sur l'**écran virtuel** de la carte Virtual Display, et
non sur un écran physique (règle de 59 : aucun écran physique filmé). Débit fixé à la
valeur d'Auto, relue au journal de l'hôte : 40 Mbit/s en 1080p120, 36 en 1440p60, 20
en 1080p60. Passes de 270 clics, ABAB, aucune perte de clic hors 2 sur 540 (C3,
H.264). Moyennes sur 540 clics par bras :

| Cas | H.264 moy. / méd. / p95 | AV1 moy. / méd. / p95 | AV1 − H.264 (moy.) |
|---|---|---|---|
| C1, Firefox UM790Pro, 1080p120 | 55,6 / 57,3 / 67,1 | 49,3 / 49,4 / 58,6 | −6,3 ms |
| C1, Firefox UM790Pro, 1440p60 | 65,5 / 66,0 / 75,8 | 55,4 / 57,5 / 61,0 | −10,1 ms |
| C2, Firefox N95, 1080p60 | 83,8 / 84,2 / 99,5 | 80,8 / 82,5 / 93,2 | −3,0 ms |
| C3, Chrome UM790Pro sans décodage matériel, 1080p120 | 41,0 / 41,4 / 49,7 | 32,6 / 32,6 / 40,6 | −8,4 ms |

- **Porte 1 tenue partout, et dans l'autre sens :** l'AV1 est plus rapide que le
  H.264 dans les quatre cas, d'environ une image à 120 Hz sur l'UM790Pro. Les
  moyennes par passe d'un même bras restent à 4 ms près. La cause n'est pas
  mesurée [H] : un décodeur H.264 logiciel qui garde une image de plus.
- **Processeur du client** : 0,2 à 0,4 cœur pendant les clics, l'image n'étant que
  le drapeau. Avec le clip de jeu joué sur l'écran virtuel (45 s, sans clics) :
  N95 1,20 cœur en H.264 et 1,33 en AV1 ; Chrome UM790Pro logiciel 1,13 et 1,39.
  Sous 2 cœurs. Pas de passe de charge pour C1 : son Firefox était déjà rangé.
- **Porte 2** : aucun clic perdu en AV1, aucune dérive qui s'accumule (fin de passe
  moins début, entre −2,3 et +2,2 ms en AV1 ; jusqu'à ±7 ms en H.264 à 60 Hz, la
  phase des écrans). Les images perdues ou répétées de la page ne sont pas relevées.
- **Décodeur réel sous Firefox : non prouvé.** La page demande `prefer-hardware`.
  Le compteur « Video Decode » du GPU de l'UM790Pro n'a rien vu pendant 20 s d'une
  passe sur le drapeau, qui ne donne que 2 images/s : ce n'est pas une preuve.
- **Porte 3, qualité** (cod.webm, 20 s en 1080p60, NVENC de la RTX du bus 01, CBR,
  P1, ULL, sans image B) : VMAF NEG H.264 98,6 et AV1 99,1 à 20 Mbit/s (+0,5) ;
  94,8 et 97,8 à 10 Mbit/s (**+2,97**). AV1 a aussi dépensé 11 % de bits en moins
  à 10 Mbit/s (8,9 contre 10,0). **La porte, à 3 points, n'est pas franchie de
  0,03 point.** Le clip est facile : les deux codecs sont hauts.

**Lecture.** Les portes 1 et 2 sont tenues. La porte 3 manque de 0,03 point à
10 Mbit/s, avec 11 % de bits en moins pour l'AV1. **À trancher par Bruno** :
(A) l'AV1 remplace le H.264 dans Auto quand le HEVC échoue,
puisqu'il est aussi plus rapide au clic ; (B) on s'en tient à la porte : H.264 ;
(C) une passe de qualité de plus, sur un clip plus dur, avant de décider.
Limites : un seul hôte (NVENC), un clip, écran virtuel côté hôte, frontend de
l'arbre partagé à `fcf0af2b` avec des changements d'autres sessions non commités.
