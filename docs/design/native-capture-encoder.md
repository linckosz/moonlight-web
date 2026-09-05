# Moteur natif de capture & encodage — MoonlightWeb Native Host

> Chantier demandé dans `moonlightweb-native-capture-encoder-plan.md`.
> Branche : `feature/native-capture-encoder`.
>
> Ce document est le livrable d'architecture de la mission (§34) **tenu à jour
> par ce qui a été mesuré**, pas par ce qui était prévu. Chaque chiffre ici a
> été relevé sur du matériel réel ; les sections marquées ⚠️ consignent une
> hypothèse que le banc a **réfutée**, et sont les plus utiles à lire.
>
> Le plan de session d'origine vit dans
> `~/.claude/plans/splendid-enchanting-rossum.md` ; en cas de divergence,
> **c'est ce fichier-ci qui fait foi** — il est le seul des deux à être
> versionné avec le code qu'il décrit.

---

## 1. Résumé exécutif

MoonlightWeb est un **client** GameStream : pour streamer la machine sur
laquelle il tourne, il fallait installer Sunshine. Ce chantier lui donne son
propre moteur de capture et d'encodage, dans le processus qui tient déjà la
PeerConnection.

Le gain de latence ne vient pas du transport (inchangé) mais de ce qui
disparaît en amont :

```
AVANT (host local via Sunshine)
  capture → encode → RTP+FEC+AES-GCM → UDP loopback → moonlight-common-c
    (réassemblage, déchiffrement, FEC) → QByteArray → signal Qt en file
    → relais → fragmentation → SCTP/DTLS → navigateur

APRÈS (moteur natif)
  capture (surface GPU) → encode (zéro-copie) → fragmentation → SCTP/DTLS → navigateur
```

Supprimés : un aller-retour réseau, RTP, FEC, un chiffrement AES-GCM redondant
(DTLS chiffre déjà), le réassemblage, **et un saut de signal Qt en file**.

### Mesuré (RTX 5060 Ti, 2560×1440, H.264)

| Étape | Mesure |
|---|---|
| Capture DXGI (présent → acquis) | **0,06 ms** moyenne, 0,11 ms au pire |
| Encodage NVENC (contenu statique) | **3,46 ms** moyenne, 3,70 ms au pire |
| Copies mémoire par frame | **1** (lecture du bitstream GPU→CPU) |

Capacités confirmées en ouvrant une vraie session : AV1, HEVC, H.264, 10-bit,
4:4:4.

---

## 2. Où le module se greffe

Deux points d'extension **existaient déjà** et étaient prévus pour ça :

| Point | Fichier |
|---|---|
| `IStreamBackend` | `backend/src/backend/streambackend/IStreamBackend.h` |
| `MediaDescriptor` (union taguée) | `.../MediaDescriptor.h` |

Le seul refactor du code existant est l'extraction d'**`IMediaEngine`**
(`backend/src/streaming/IMediaEngine.h`) hors de `MoonlightShim`, pour que les
relais parlent à un moteur abstrait plutôt qu'à moonlight-common-c.
`MoonlightShim` en dérive sans qu'une ligne de son corps change.

**Inchangé, et devant le rester** : tout le chemin `gamestream` / `wolf` /
`multiseat`, le format de trame sur le DataChannel (en-tête 17 o), le décodeur
WebCodecs du navigateur. L'encodeur natif produit de l'Annex-B/OBU exactement
comme Sunshine, donc le frontend n'a rien à changer pour la vidéo.

---

## 3. Structure

```
backend/native-host/              # cible CMake mw-native-host (STATIC)
  LICENSE.md                      # la frontière juridique, expliquée
  cmake/boundary_check.cmake      # …et rendue mécanique
  include/mw/native/              # API publique : C++17 pur, zéro Qt, zéro GPL
  src/core/                       # Probe, Selector, Log, façade
  src/capture/windows/            # DxgiDuplication (+ WGC en repli, à venir)
  src/convert/windows/            # ColorConvert : NV12 (4:2:0) et AYUV (4:4:4)
  src/encode/windows/             # NvencApi, NvencCapabilities, NvencEncoder
  src/platform/windows/           # sonde + boucle de session
  third_party/nvenc-headers/      # nv-codec-headers (MIT), SDK 12.0
  tests/                          # Qt-free, exécutables sur CI sans GPU
```

### La frontière de licence est vérifiée, pas déclarée

`mw-native-host` ne lie **ni Qt, ni moonlight-common-c, ni aucune dépendance
GPL**. C'est ce qui la garde relicenciable seule (§26 de la mission).

`cmake/boundary_check.cmake` tourne **à chaque build** et casse la compilation
en nommant fichier et ligne si un `#include` interdit apparaît (Qt,
`Limelight.h`, FFmpeg, x264/x265, `backend/src/`). Vérifié en le faisant
échouer volontairement.

---

## 4. Capture

### Windows — DXGI Desktop Duplication (retenu)

`AcquireNextFrame` **débloque sur le présent réel** au lieu de scruter, et
`LastPresentTime` date ce présent en QPC. Donc t₀ est une **mesure**, pas une
estimation, et tous les chiffres de latence en aval en héritent.

Repli prévu : Windows.Graphics.Capture, pour les cas où DDA répond
`DXGI_ERROR_UNSUPPORTED` (sorties hybrides). Les deux rendent un
`ID3D11Texture2D`, donc l'étage encodeur est identique.

Trois points de justesse invisibles hors exécution :

1. **Les horloges.** DXGI date en QPC, le reste du moteur en `steady_clock`.
   Même cadence, origines différentes : sans le couple de calibration pris au
   démarrage, la latence de capture serait un écart entre deux époques sans
   rapport — grand, stable, et vide de sens.
2. **Un présent à zéro n'est pas une frame.** DXGI réveille aussi sur un simple
   mouvement de pointeur ; le compter comme une frame enverrait un doublon
   horodaté n'importe comment.
4. **Six étapes, deux threads, un mutex.** Chaque frame porte t₀ présent, t₁
   acquis, t₂ converti, t₃ encodé ; le thread émetteur ajoute t₄ premier octet
   et t₅ dernier octet (`FrameSentSink`). `NativeMediaEngine` verse les six
   différences dans des histogrammes à pas logarithmique (`StageStats`, huit
   cases par octave, percentile rendu par le bord haut de sa case : il surestime,
   jamais l'inverse). Le message `stats` porte la fenêtre (`stages`), le log
   porte la session entière en une ligne à l'arrêt. Depuis le 02/09/2026 ; rien
   sur ce chemin ne s'optimise sur une estimation.
3. **L'époque du relais.** Le moteur date en absolu ; le relais, lui, attend le
   contrat du shim GameStream : un temps de présentation **relatif à la première
   frame**, qu'il rajoute à l'époque pour retrouver l'horloge. `NativeMediaEngine`
   fait la soustraction (époque = présent de la première frame). Livrer l'absolu
   comptait l'horloge deux fois : le `backendTs` du client avançait à 2×, et
   toute mesure de gigue en aval était fausse (bug B2, corrigé le 02/09/2026).

### Linux / macOS

Linux : KMS/DRM d'abord (§19.3–19.5), le portail PipeWire en repli reste à écrire ;
le son par PipeWire (§19.7).
macOS : ScreenCaptureKit, qui écrit directement le NV12 de l'encodeur (§20).

---

## 5. Association display → GPU

C'est ce qui décide de tout le zéro-copie : capturer sur le GPU A pour encoder
sur le GPU B impose une copie VRAM→RAM→VRAM qui écrase tout le reste.

Sur Windows, **DXGI répond exactement** : l'adaptateur qui énumère une sortie
est celui qui la scanne. Aucune heuristique. Le LUID est conservé et sert à
rouvrir le même adaptateur pour la duplication *et* pour l'encodeur.

> `D3D_DRIVER_TYPE_UNKNOWN` est obligatoire quand on fournit un adaptateur.
> Demander `HARDWARE` l'ignore silencieusement et prend le défaut — c'est ainsi
> qu'un pipeline « zéro-copie » se met à copier entre GPU sans rien dire.

**La copie inter-GPU existe depuis le 04/09/2026** (`CrossGpuBridge`). Quand le
Selector choisit un encodeur sur un autre adaptateur que celui qui scanne
l'écran — un écran piloté par un GPU sans encodeur, ou le banc qui force
`gpu=<id>` pour mesurer un iGPU sans écran — la trame capturée passe par une
texture de lecture (staging) sur le GPU source, un `Map`, une copie ligne à
ligne dans une texture dynamique du GPU d'encodage, puis la conversion et
l'encodeur y sont construits. D3D11 n'offre aucune autre route entre deux
adaptateurs. Coût mesuré : **14 Mo par trame 1440p, 2 à 5 ms**, compté dans
l'étape `convert` et résumé en fin de session (« cross-GPU copy: N frames of
14 MB, x ms mean, y ms max »). `SessionInfo::copiesPerFrame` passe à 3. La
session le dit en warning au démarrage : sur ce chemin, toutes les promesses
zéro-copie de ce document sont hors jeu. Avant cette date la session refusait
de démarrer.

---

## 6. ⚠️ Ce que le banc a corrigé

### 6.1 La taille du display était fausse (mise à l'échelle DPI)

La sonde annonçait l'écran en **2048×1152** alors qu'il fait **2560×1440** —
rapport exactement 1,25, la mise à l'échelle Windows à 125 %.

`DXGI_OUTPUT_DESC::DesktopCoordinates` est exprimé en coordonnées de bureau
virtuel, que Windows **met à l'échelle** pour un processus non
per-monitor-DPI-aware. Et déclarer cette awareness depuis une bibliothèque est
exclu : c'est un réglage de **processus**, qui appartient à l'UI Qt de
l'application hôte.

→ La taille vient du **mode SOURCE** de `QueryDisplayConfig`, avec
`EnumDisplaySettings` en repli. Un test scelle l'invariant : sonde et
duplication doivent donner la même taille.

Sans ça, l'utilisateur se serait vu proposer — et aurait streamé — une
résolution amputée d'un quart de ses pixels.

`QueryDisplayConfig` sert aussi au rafraîchissement, qui porte le rationnel
exact : `EnumDisplaySettings` arrondit, et un panneau 143,98 Hz rapporté 143
fait battre la capture visiblement.

### 6.2 Le plancher de pilote NVENC, pas « la version la plus récente »

Premier essai : SDK 13.1 vendoré. Le banc — une RTX 5060 Ti **neuve** — expose
l'API 208 (SDK 13.0) et refusait toute session.

NVENC n'est rétro-compatible que dans un sens : un pilote accepte les versions
de structure de sa génération **ou plus anciennes**, jamais plus récentes. La
version d'en-tête n'est donc pas « jusqu'où peut-on monter » mais **un plancher
imposé à tous les utilisateurs**.

→ SDK **12.0** (`n12.0.16.2`, pilotes 520+/oct. 2022). Vérifié comme contenant
tout ce que le moteur utilise : AV1, `ULTRA_LOW_LATENCY`, intra-refresh,
invalidation de référence, 10-bit, 4:4:4, surfaces D3D11.

Absents de 12.0 et vérifiés comme tels : `splitEncodeMode`, filtre temporel,
`lookaheadLevel`. Seul le premier pourrait compter un jour — il répartit une
frame sur plusieurs moteurs NVENC, ce qui ne concerne que les 5080/5090 en 4K
haute fréquence. Monter à 12.2 le rendrait accessible au prix d'un plancher
pilote 2024.

### 6.3 Les adaptateurs d'écran virtuel ne se disqualifient pas

Le banc rapporte **5 adaptateurs DXGI pour 3 GPU physiques** : les deux en trop
sont Parsec Virtual Display et Virtual Display Driver, qui présentent le nom et
le device-id NVIDIA sous un LUID propre.

L'hypothèse était qu'ils refuseraient une session NVENC et tomberaient d'eux-
mêmes. **Faux** : adossés à une vraie carte NVIDIA, ils en ouvrent une et
rapportent les mêmes codecs.

C'est en réalité la bonne réponse — un display accroché à l'un d'eux doit être
capturé **et** encodé là, DXGI routant les deux vers le même silicium. Il n'y a
donc rien à filtrer : l'identité d'adaptateur vient de DXGI, et la requête de
capacités sert seulement à connaître les codecs.

### 6.5 Un host sans adresse a fait déborder la pile

Le premier `/apps` sur le host natif tuait le processus — `0xc0000005` dans
`ntdll.dll`, sans une ligne de log (une pile épuisée ne peut pas se dérouler
pour écrire).

Le préchargement des jaquettes marque une app en attente, appelle
`startBoxArtFetch` qui ne trouve **aucune adresse** — le host natif EST ce
processus — et conclut l'échec *immédiatement* ; le gestionnaire de complétion
retire le marqueur puis rappelle le préchargement, qui rechoisit la même app.

Ce qui espace normalement les tentatives, c'est l'attente d'une réponse réseau.
Sans réseau, rien ne casse la boucle. Le code existant supposait, sans le dire,
que tout host de la liste a une adresse — invariant que le host natif a brisé.

Corrigé aux deux niveaux : un host sans adresse ne précharge rien, et une app
dont la jaquette a échoué n'est plus rechoisie dans la même passe (sans quoi un
échec réseau sur un host réel bouclait aussi — en requêtes plutôt qu'en pile,
donc invisible mais bien présent).

### 6.4 « Un encodeur » ne veut pas dire « peut encoder »

L'iGPU AMD du banc annonce le runtime AMF avec une **liste de codecs vide** (la
requête AMF n'est pas écrite). Le sélecteur s'y serait replié — payant une
copie inter-GPU — pour ensuite échouer à la négociation de codec, en
abandonnant la RTX qui pouvait le faire.

→ « Pouvoir encoder » exige un encodeur **et** au moins un codec. Même règle
dans `Probe.cpp` pour la disponibilité globale : un host qui échoue au clic est
pire que pas de host.

---

## 7. Encodage

Chaque réglage est une décision de latence, prise **contre** les défauts de
NVENC qui visent l'encodage de fichiers.

| Réglage | Pourquoi |
|---|---|
| `ULTRA_LOW_LATENCY`, **preset P1** (P4 jusqu'au 04/09/2026) | La croyance « P1 plus mou pour moins d'une milliseconde » était fausse (`docs/bench-native-host.md`) : sur une RTX 5060 Ti à 1440p, P1 encode un FPS en **3,4 ms contre 7,7** pour P4 (p99 5 contre 11), au **même QP** (25) et sans rien de visible sur l'image décodée ; +1 de QP sur un jeu de plateforme, +5 seulement sur du texte qui défile. Appliqué le 04/09/2026 sur confirmation de Bruno. Le preset ULL active de lui-même le multipass quart de résolution, **conservé** : l'éteindre gagne 0,6 ms mais casse la rafale de raffinement de l'écran fixe (§9.1, QP bloqué à 29 au lieu de 8) |
| **VUI `bitstream_restriction` en H.264** | sans `max_num_reorder_frames` le décodeur D3D11 de Chrome retient un DPB entier avant d'afficher : **200 ms** de décodage mesurés sur un flux sans B-frame, 1 ms après. Le HEVC le porte par défaut (04/09/2026) |
| **Aucune B-frame** (`frameIntervalP = 1`) | elle référencerait une image pas encore envoyée → une trame entière retenue |
| **GOP infini + intra-refresh** | une keyframe est un pic de débit ; `MediaTrackRelay` documente ce que ces pics font à un lien congestionné (perte → tempête de PLI → effondrement) |
| **CBR, VBV = une frame** | c'est le VBV qui impose réellement la faible latence : aucune frame ne peut être si grosse qu'elle mette plusieurs temps de trame à passer |
| **SPS/PPS à chaque keyframe** | le décodeur du navigateur s'y configure ; un client qui arrive en retard doit pouvoir démarrer sur la suivante |

Le zéro-copie tient : NVENC enregistre directement la texture D3D11 écrite par
la passe de conversion, sur le même adaptateur.

`setBitrate()` reconfigure sans redémarrer la session — la base du rate-control
piloté par le retour réel du client.

**Le VBV de `setBitrate()` passe par le même plancher que celui de l'init**
(`vbvBitsPerFrame`, `RateControl.h`), sur les trois encodeurs. Jusqu'au
02/09/2026 AMF divisait simplement par le fps : la rafale de raffinement (§9.1)
appelle `setBitrate()` deux fois par transition figé/mouvement, et chaque retour
au budget ordinaire laissait l'encodeur AMD avec un VBV 2,4× plus serré à 144 Hz
que celui choisi à l'init — l'image molle que le plancher existe pour éviter,
réintroduite par le chemin censé la rendre nette. Corrigé (audit B4) ; le test
`test_rate_control.cpp` fixe l'égalité init/ré-application à tout fps.

### L'intra-refresh ne sert à rien sans un récepteur qui l'accompagne

L'intra-refresh est implémenté sur les **trois** encodeurs (NVENC, AMF, oneVPL).
Mais il ne gagne rien tant que le navigateur continue de réclamer une keyframe
au premier trou : on paierait le coût de la vague de rafraîchissement **et** le
pic de la keyframe. Le gain n'est pas en régime établi, il est dans la
**récupération de congestion** — aujourd'hui un trou fait jeter les deltas des
deux côtés et exige la plus grosse frame possible sur un lien qui vient de
prouver qu'il saturait.

D'où un contrat en trois temps, chacun capable de dire non :

| Étape | Qui décide | Ce qui circule |
|---|---|---|
| Demande | le navigateur | `ride_out_loss` dans `/start` (constante `RIDE_OUT_LOSS` dans `BackendClient.js`, un booléen prévu pour les A/B) |
| Octroi | l'encodeur | `SessionInfo::intraRefresh` — ce qui a été **accordé**, pas ce qui a été demandé |
| Application | les deux extrémités | `intra_refresh` dans la réponse `/start` |

La direction compte : le serveur renvoie ce que le flux **fait**. Un récepteur
qui suppresserait ses demandes de keyframe face à un flux sans vague de
rafraîchissement resterait indéfiniment sur une image corrompue. Côté backend,
`DataChannelRelay::ridingOutLoss()` exige donc **et** l'opt-in du client **et**
`IMediaEngine::intraRefreshActive()`, et ne débraye que les deux portes de perte
— jamais celles du démarrage de session, où il n'y a aucune référence à
rattraper. Côté navigateur, la suppression est bornée par un chien de garde de
2,5 s : passé ce délai sans trame contiguë, on redemande une keyframe.

Le flag traverse le processus worker (`cfg["rideOutLoss"]`) : le moteur média
vit dans l'enfant, le poser sur la session du parent ne l'atteindrait jamais.

**La période de la vague est une durée, pas un nombre de frames** (audit B6,
corrigé le 03/09). Les trois encodeurs figeaient 120 frames, qui ne font
2 s qu'à 60 fps : un flux à 30 fps mettait 4 s à se réparer — plus que le
chien de garde du client, qui redemandait donc la keyframe et payait les deux —
et un flux à 144 fps balayait en 0,8 s, soit 2,5 fois les bits intra par
seconde nécessaires. `intraRefreshPeriodFrames(fps)` dans `RateControl.h`
donne 2 s **dans la cadence effective** (le réglage, ou le Hz de l'écran à
réglage 0 — jamais la cadence de l'écran quand le flux est bridé sous elle),
bornée à [30, 600] contre l'absurde ; NVENC prend la moitié comme longueur de
vague, AMF en dérive son compte de blocs par frame (arrondi vers le bas, donc
le balayage ne peut que dépasser légèrement 2 s, jamais y rester en dessous :
1080p HEVC à 240 fps = 1 CTB par frame, 510 frames), oneVPL le pose dans
`IntRefCycleSize`. La ligne « ready » de chaque encodeur dit la période
retenue (`intra-refresh over N frames`). Vérifié en flux réel sur la RX 7600
(AMF HEVC) : 120 frames à 60 fps, 480 à 240 fps, image présente dans les deux
cas.

---

## 8. Conversion couleur — et le 4:4:4

BT.709 plage limitée, l'espace que le pipeline négocie déjà pour le SDR : un
stream natif rend comme un stream Sunshine sur le même écran.

**Un rendu, pas un compute shader** : D3D11 ne sait pas lier un UAV sur un plan
de NV12 (les tranches de plan n'existent qu'en D3D12). Ce qu'il sait faire,
c'est une RTV typée — une vue `R8_UNORM` d'une texture NV12 adresse son luma,
une vue `R8G8_UNORM` son chroma.

| Chroma | Sortie | Passes |
|---|---|---|
| 4:2:0 (défaut) | NV12, 2 plans | 2 draws |
| 4:4:4 (option On) | AYUV empaqueté | **1 draw** |

Le 4:4:4 est donc plus *simple* que le 4:2:0. Mesuré : keyframe de **53 627
octets contre 39 764** en 4:2:0 — les ~35 % attendus.

Deux pièges silencieux :

- **l'ordre des octets d'AYUV** (V, U, Y, A dans un mot 32 bits) — se tromper
  échange les couleurs au lieu d'échouer ;
- **le profil ET `chromaFormatIDC`** doivent suivre le format d'entrée, sinon
  NVENC accepte du 4:4:4 et encode du 4:2:0 en jetant la chroma : indiscernable
  d'une option sans effet.

Le 4:4:4 n'est activé que si le client le demande **et** que l'encodeur sait le
faire, avec une trace explicite sinon. Dégrader en silence serait pire que
refuser : toute la raison de demander du 4:4:4 est la lisibilité du texte.

Le HDR est **refusé explicitement** plutôt que converti comme du SDR : le FP16
scRGB demande une transfert PQ et une cible P010. Le traiter avec la matrice SDR
donnerait une image délavée — faux d'une façon qui ne se voit pas.

**Corrigé le 02/09/2026 (B1).** Ce refus était atteint sur toute machine avec le
HDR Windows actif : la duplication demandait FP16 *en premier* quel que soit le
stream, DXGI livrait donc le bureau en FP16, et le convertisseur refusait → le
host natif échouait au clic. Désormais le format demandé suit **ce que la session
consomme**, pas ce que l'écran fait : `DxgiDuplication` reçoit `hdr` et ne nomme
`R16G16B16A16_FLOAT` que si la session rend du HDR ; sinon `B8G8R8A8_UNORM` seul,
et DXGI livre un bureau HDR **déjà ramené en SDR** (tone-mapping système), qui est
exactement l'image qu'un stream SDR doit porter. La session demande d'abord au
convertisseur (`ColorConvert::supportsSource`) s'il sait traiter FP16 ; tant que le
chemin P010 n'existe pas, un HDR négocié par le Selector est **rabattu en SDR avec
une trace** (« HDR negotiated but this build converts SDR only ») plutôt que
d'échouer. Vérifié sur écran SDR (comportement identique, `(SDR, BGRA8)` dans le
log) ; le cas HDR actif reste à confirmer à la main sur un écran en HDR.

---

## 9. Boucle de session

Un seul thread, aucune file d'attente. Ni l'un ni l'autre ne se justifierait :

- une file n'aide que si le producteur va plus vite que le consommateur, or le
  consommateur **est le réseau** — prendre du retard signifie que le lien est
  plein, et tamponner dans un lien plein ajoute du délai sans livrer plus ;
- un second thread coûterait un réveil par frame pour recouvrir un travail
  d'une milliseconde.

La boucle bloque dans `AcquireNextFrame` : elle est cadencée par l'écran, pas
par un minuteur choisi, et un bureau immobile ne coûte rien.

### 9.1 Le plancher sur écran immobile

La capture livre **sur dommage** : un écran où rien ne bouge ne produit rien, ce
qui est indiscernable d'un stream mort. D'où un plancher — une frame toutes les
500 ms, la dernière image ré-encodée, quelques centaines d'octets.

**Ce nombre appartient à la capture, pas au client.** Combien de temps un écran
figé peut se taire dépend de l'exactitude avec laquelle la plateforme signale un
dommage. Sur DDA le signal est exact : `AcquireNextFrame` rend la main sur le
présent réel, 0,06 ms après. Tout changement est donc une frame *immédiatement*,
puis la **rafale de raffinement** ré-encode cette image pendant 1 s à ×3 de
budget jusqu'à convergence. Un plancher plus haut ne ferait que continuer après
ça, au budget ordinaire, sur une image que l'encodeur a déclarée finie — donc en
mode bureau il n'apporte rien, quel que soit l'appareil. Une plateforme au
signal plus flou remonte ce nombre ici, et aucun client ne l'apprend.

**La rafale est cadencée sur le lien (02/09/2026, audit B5).** Jusque-là elle
partait à ×6 aussi vite que l'encodeur produisait : au banc, la première passe
d'un stream à 20 Mbps sortait à 248 Ko, exactement le plafond ×6, soit 100 ms
d'un lien à 20 Mbps — et la rafale entière (plusieurs centaines de Ko) se
retrouvait devant la première frame du mouvement suivant. Deux corrections :

- **×3 au lieu de ×6.** Le multiplicateur n'est pas une netteté : une image
  fixe converge vers le même total de bits quel que soit le plafond par frame
  (chaque passe code le résidu de la précédente), un plafond plus petit étale
  seulement les mêmes bits sur plus de passes. Ce qu'il est, c'est **une
  latence** : le temps maximal qu'une passe occupe le lien, donc l'attente
  maximale d'une frame de mouvement arrivée juste derrière — `boost / 60` s,
  soit 50 ms au lieu de 100.
- **Une passe n'est envoyée que lorsque le lien a fini la précédente**,
  estimé par le débit réglé du stream (`LinkOccupancy`, `RateControl.h`) : la
  rafale n'a jamais plus d'une passe d'avance sur le lien, quel que soit le
  débit. Le plancher de vivacité n'est pas cadencé (quelques centaines
  d'octets). Le log de fin de rafale dit combien de réveils ont été retenus
  (`N passes, M held for the link`).
- **La sortie de rafale ne se fie plus à la taille seule.** Le premier flux
  réel rejoué après les deux points ci-dessus a montré `58 KB + 3097 KB over
  26 passes, 8 held for the link (window closed)` sur un écran immobile à
  55 Mbps / 165 fps : en CBR l'encodeur remplit chaque passe jusqu'à sa cible
  (~120 Ko) quoi qu'il ait à dire, donc le critère « passe ≤ 2 Ko » n'était
  atteint qu'à 1 Mbps, et partout ailleurs la rafale durait toute sa seconde —
  3 Mo par arrêt de souris, sans une ligne de log (seule la convergence était
  journalisée). `RefineConvergence` (`RateControl.h`) tranche désormais sur
  **une passe minuscule ou un QP qui ne baisse plus** (deux passes de suite
  après quatre), avec un **plafond de 8 passes** pour l'encodeur qui ne
  rapporte pas de QP — soit 400 ms de lien au plus pour une image fixe. Toute
  sortie est journalisée (`converged`, `pass cap`, `window closed`, `screen
  moved`, `display lost`), avec le QP première → dernière passe quand il est
  connu. Rejoué sur le même écran : `58 KB + 1081 KB over 8 passes, 2 held for
  the link, QP 32 -> 11 (pass cap)` — le tiers des octets, et le QP dit ce que
  la rafale a acheté.

Pourquoi une estimation et pas `bufferedAmount` : ce compteur ne mesure que ce
que libdatachannel garde **après** refus d'usrsctp, dont le tampon d'émission
fait 1 Mio (`sctptransport.cpp`, `sctp_sendspace`). Sur un lien à 20 Mbps ce
sont 420 ms de retard qui se lisent zéro ; le compteur ne bouge pas pour une
rafale de quelques centaines de Ko. Le débit réglé est la parole de
l'utilisateur sur son lien, celle que le rate control croit déjà pour chaque
frame ; le retour mesuré du client (§9.3) le remplacera comme débit du modèle
quand il existera, le modèle ne change pas.

Un client peut demander **plus que la vivacité**, par un message `framefloor`
que les trois relais transmettent :

| Situation du client | Plancher demandé |
|---|---|
| Bureau, pointeur libre (tout appareil) | rien — celui de l'hôte |
| Mode jeu, pointeur capturé | **30 fps** — le pointeur est *dans* l'image, et écran figé ≠ session inactive (pause, menu, chargement) |

Deux bornes, côté hôte : jamais plus vite que le fps du stream (le réglage de
l'utilisateur passe avant une demande venue d'une page), jamais plus lentement
que les 500 ms. Le timeout d'`AcquireNextFrame` suit le plancher, sinon une
frame due à 33 ms serait livrée à 100.

### 9.6 Cadence : le fps réglé est tenu (02/09/2026)

Jusqu'au 02/09, la borne « jamais plus vite que le fps du stream » ne valait
que pour le plancher : la boucle, elle, encodait **chaque présent DXGI**. Sur
l'écran 165 Hz du banc avec 60 réglés, 164 images/s traversaient un CBR
dimensionné pour 60 — le budget est *par image*, donc le fil portait 2,75 × le
débit réglé sur un écran en mouvement (mesuré : 21,7 Ko de moyenne × 164 =
28 Mbit/s pour 20 réglés, 63 Mbit/s quand les images plafonnaient au VBV en
déplaçant une fenêtre). Invisible en LAN ; sur Internet l'excédent attend dans
le SCTP et se sent comme un pointeur qui traîne.

Le mécanisme (`FrameCadence`, pur, testé sans écran) : l'intervalle du stream
est une grille. Chaque présent est **converti** (la texture de sortie du
convertisseur est ce que les chemins « écran fixe » réémettent, elle doit
rester la plus fraîche) mais seul le **premier présent à ou après chaque
échéance** est encodé, **à l'instant où il arrive**. Les autres sont sautés.
Rien n'attend jamais sur le thread de capture. La grille avance d'un intervalle
à chaque présent admis (jamais « maintenant + intervalle », sinon la cadence
dérive — mesuré 56 fps pour 60) ; un présent qui arrive un peu **avant**
l'échéance, dans le quart d'intervalle qui la précède, passe aussi, plutôt que
d'attendre une période d'écran entière pour le suivant. La grille n'est
**ré-ancrée** sur le présent admis que s'il est en retard de plus d'une
période d'écran, c'est-à-dire qu'un présent manquait là où la grille en
attendait un : écran resté fixe, boucle bloquée, ou jeu tournant au fps du
stream mais déphasé — auquel cas la grille se verrouille sur lui au lieu de
battre contre lui (une image sautée une fois, pas à chaque intervalle).

**Première version, abandonnée le 02/09 au soir.** Elle faisait l'inverse :
retenir le *dernier* présent de chaque intervalle et l'encoder à l'échéance,
réveil par le timeout d'`AcquireNextFrame` sous `timeBeginPeriod(1)`, pour
une émission parfaitement régulière et l'image la plus fraîche possible *à
l'échéance*. Mesurée par Bruno à 30 km par Internet : cette attente faisait
**5,4 ms de moyenne et 17 ms au p99**, soit les deux tiers du temps hôte (8,1 ms
sur un pipeline qui en coûte 2,7) — un présent arrivé juste après une échéance
patientait l'intervalle entier. Une image qui attend sur l'hôte est de la
latence que le joueur sent ; une émission en avance ou en retard d'une période
d'écran (6 ms à 165 Hz, rien quand le jeu tourne lui-même au fps du stream) ne
l'est pas, et avec le *tearing* côté client elle est invisible. Latence
d'abord, régularité ensuite. L'étape `hold` (t₂ → t₂ᵇ, `dueUs`) a disparu des
stats, de l'overlay et du banc avec elle ; `encode` va désormais de t₂ à t₃.

Le Selector résout « 0 » en la fréquence arrondie de l'écran ; la garde n'est
construite que si le stream est **plus lent** que l'écran. À la cadence de
l'écran tout présent est encodé tel quel — une garde au même rythme retiendrait
encore un présent arrivé un peu tôt pour le jeter au suivant (mesuré : 23 sur
1 651 en 10 s).

Le budget de l'encodeur suit désormais le fps **effectif** (`m_EncodeFps`) et
non plus 60 quand le réglage était 0.

Banc du 02/09 (première version, avec l'étape `hold` qui n'existe plus),
1440p HEVC AMF sur le RX 7600, 20 Mbit/s, la même vidéo 4K jouant à l'écran :

| Réglage | présents | encodées | non portées | hold ms | encodage ms | présent→encodé ms | Ko/image |
|---|---|---|---|---|---|---|---|
| 60 (avant) | 1 641 | 1 641 | 0 | — | 4,28 / 5,12 / 5,63 | 4,75 / 6,66 / 7,68 | 21,7 moy., 48 p95 |
| **60 (après)** | 1 647 | 601 | 1 045 (104/s) | 0,99 / 5,63 / 6,14 | 4,10 / 5,12 / 5,63 | 5,52 / 9,22 / 10,24 | 36,0 moy., 48 p95 |
| 0 = 165 | 1 651 | 1 628 | 0 | — | 4,24 / 5,12 / 5,63 | 4,87 / 6,66 / 8,19 | 13,2 moy., 40 p95 |

(moyenne / p95 / p99.) Sur un vrai flux depuis l'instance dev (1080p HEVC
AMF, 20 Mbit/s, 37 s, bureau avec une vidéo qui joue) : 6 107 présents, 2 148
images envoyées, 105/s non portées ; `hold` 0,50 / 4,61 / 6,14 ms, encodage
3,31 / 4,10 / 4,61, **total présent → dernier octet 4,73 / 8,19 / 10,24 ms**.
Le gain : 36 Ko × 60 = 17 Mbit/s réels pour 20 réglés, au lieu de 28 à 63 ;
l'encodeur dépense enfin son budget par image sur du contenu.

**Version sans attente, mesurée le 02/09 au soir**, même instance dev, même
écran 165 Hz, 60 réglés, 1080p HEVC AMF, 63 s de bureau avec une vidéo qui
joue : 10 386 présents, 3 793 images envoyées (60/s exactement), 104/s non
portées ; hôte `acquire 0,11 / 0,21 / 0,29 · convert 0,18 / 0,35 / 0,51 ·
encode 3,38 / 4,10 / 4,61 · queue 0,16 / 0,38 / 0,96 · send 0,33 / 0,70 /
1,02 · total 4,19 / 5,63 / 6,66 ms` (moyenne / p95 / p99). La session
précédente, avec rétention, sur le même banc : total **7,70 / 20,48 / 24,58**.
Le p99 hôte a été divisé par presque quatre pour le même débit sur le fil.

### 9.7 Écran verrouillé : la session attend, le stream reste vivant (02/09/2026)

`AcquireNextFrame` rend `DXGI_ERROR_ACCESS_LOST` pour trois raisons de durées
très différentes : un changement de mode (quelques centaines de ms), une
invite UAC sur bureau sécurisé (le temps de la lire), un **verrouillage** —
Win+L, écran de veille, capot refermé — pendant lequel DXGI refuse d'ouvrir
une duplication du bureau sécurisé, des minutes durant. La première version de
`restartCapture` bornait l'attente à 10 s : verrouiller le PC depuis le stream
tuait le stream (bug B3 de l'audit).

Désormais la boucle réessaie **tant que la session tourne**, au pas de
`RestartBackoff.h` : 100, 200, 400, 800 ms puis 1 s (une tentative = création
d'un device D3D11 + `DuplicateOutput`, ce n'est pas gratuit, et un écran
verrouillé depuis une minute ne reviendra pas dans les 100 ms). Pendant
l'attente le stream **reste vivant** : sans ça le navigateur déclare la famine
à 1 s de silence et descend l'échelle de qualité sur une session qui n'est que
verrouillée. Ce qui part est une **image noire**, au plancher d'écran fixe
(500 ms, ou le plancher demandé par le client), encodée par l'encodeur que la
session a encore — convertisseur et encodeur gardent leurs propres références
au device et survivent à la perte de la duplication ; ils ne sont libérés
qu'une fois la duplication rouverte, juste avant la reconstruction, pour ne
jamais avoir deux sessions matérielles ouvertes à la fois. Noir plutôt que le
dernier bureau : un bureau figé ressemble à un gel, et celui qui vient de faire
Win+L le referait. L'input continue de passer tout le temps (`SendInput`
atteint le bureau sécurisé), donc un mot de passe tapé dans le noir
déverrouille l'hôte ; la duplication rouvre, la première image est une
keyframe. Le journal dit « display is away », « still away after 10 s » une
fois, puis « display is back after N s and M failed attempts ».

Vérifié le 02/09 sur l'instance dev : quatre changements de mode 1440p ↔ 1080p
pendant un stream, reprise au premier essai à chaque fois (≈ 50 ms, aucune
image noire nécessaire), image intacte. **Le verrouillage lui-même reste à
tester à la main** : bench-desk a UAC en « élever sans demander », donc pas de
bureau sécurisé provoquable depuis un script, et verrouiller le poste sans
l'accord de son utilisateur n'est pas une chose que l'agent fait.

### 9.8 Le budget de l'encodeur suit la cadence réelle (04/09/2026)

Le CBR est un budget **par image** : débit ÷ cadence. La cadence dont l'encodeur
est configuré est le réglage du stream — ou le rafraîchissement de l'écran quand
le réglage est 0 — et un jeu ne tourne presque jamais à l'une ni à l'autre. Un
jeu à 60 fps sur un écran 165 Hz sous un stream « 165 » produit 60 images par
seconde, chacune dotée d'un 165e du débit : le fil porte 60/165 de ce que le
joueur a autorisé, et l'image est quantifiée comme si le lien était 2,75 fois
plus petit. Mesuré au banc (Call of Duty à 60 fps, 40 Mbit/s) : 29 Ko et QP 25
par image avec le réglage à 165 ; 65 Ko et QP 12 avec le réglage à 60 — même
contenu, même lien, même encodeur.

`encode::EffectiveCadence` compte les images réellement encodées depuis une
capture sur des fenêtres d'une seconde et tient la cadence pour laquelle le
budget est dimensionné. Le budget est déplacé en **multipliant le débit
transmis à l'encodeur** par cadence configurée ÷ cadence réelle — pas en
reconfigurant la cadence, parce que les trois encodeurs ont un `setBitrate()`
à chaud et aucun ne promet un changement de cadence à chaud. Le fil porte
toujours le débit réglé : (débit × 165/60) × 60 images = débit. Le VBV grandit
avec, et c'est le but : une image plus grosse est permise parce qu'il en vient
moins. Le modèle de lien (`LinkOccupancy`) ne voit rien de tout cela — c'est le
débit du fil, et il ne bouge pas ; la rafale de raffinement multiplie par-dessus.

Sens de variation : une cadence qui **monte** est rattrapée dans le quart de
seconde (sous-fenêtre de 250 ms — chaque image est désormais trop grosse, et
une seconde à 2,75× le lien c'est le pointeur qui traîne) ; une cadence qui
**descend** attend deux fenêtres d'une seconde qui s'accordent (un à-coup ne
gonfle pas la seconde suivante) ; 15 % d'hystérésis ; jamais sous 30 fps (un
écran fixe ne capture presque rien, ses passes ont leur propre budget) ni
au-dessus du réglage (la parole du joueur sur son lien). Log : « frames arrive
at 59 fps for a 165 fps stream — encoder budget 111864 kbps per second of
frames (40000 on the wire) ».

Vérifié sur le clip : à 40 Mbit/s, 61 Ko et QP 16 par image en moyenne sur 20 s
(dont les deux premières à l'ancien budget) contre 29 Ko et QP 25 ; à 20 Mbit/s,
31 Ko et QP 22 contre 15 Ko et QP 31. Encode +0,3 ms pour des images deux fois
plus grosses. Le fil reste sous le réglage.

### 9.9 Le débit suit le lien, piloté par ce que voit le récepteur (04/09/2026)

Le §9.3 annonçait un rate control « piloté par le retour réel du client » ; le
voici, et il a fallu corriger le plan sur deux points. `clientstats` n'existe
que sur le transport media (compteurs RTP), et `bufferedAmount` ne voit rien :
il compte ce qui a débordé du méga-octet de tampon usrsctp, donc 420 ms de
retard à 20 Mbit/s se lisent zéro. Sur l'hôte, un lien qui s'étrangle ressemble
à des images qui partent à l'heure. **Le premier endroit où la file se mesure
est l'arrivée** : chaque image porte l'heure de présent de l'hôte, et le
récepteur sait de combien elle arrive plus tard que d'habitude — la file, en
millisecondes, des secondes avant la première perte. C'est l'idée du contrôle
de congestion par le délai de WebRTC, réduite à l'os : un flux, un sens, un
récepteur qui horodate tout, un encodeur qui change de débit entre deux images.

**Client** (`StreamView._startLinkReporting`, hôte natif sur DataChannel) : un
message `linkstats` toutes les 500 ms — `owdRiseMs` = minimum sur la fenêtre de
(arrivée − présent hôte) moins le minimum de session (référence glissante sur
30 s, pour qu'une dérive d'horloge ne se lise jamais comme une file) ; le
décalage entre les deux horloges s'annule dans la soustraction, il ne reste que
l'attente — `gaps` = trous de numérotation, `fps`. Sur media, les compteurs de
`clientstats` alimentent la même entrée (pertes RTP en trous, jitter en délai).
Côté hôte, les évictions du `FrameSender` s'ajoutent au rapport : c'est de la
perte que l'hôte s'inflige, et le signe le plus sûr d'un lien plein.

**Moteur** (`encode::RateGovernor`, pur, testé ; `Session::reportLink`,
`LinkFeedback` en en-tête public) : surutilisation — délai ≥ 30 ms (deux
intervalles à 60 fps), un trou ou une éviction — **coupe de 20 % à l'instant**
et gel de 2 s ; trois secondes de calme (délai < 10 ms, rien de perdu)
remontent de **5 % par rapport**, jamais au-dessus du réglage ; entre les deux
(file présente, pas croissante) on tient ; plancher 20 % du réglage et
2 Mbit/s ; quatre secondes sans rapport valent une coupe, une seule — et
**depuis le 05/09**, le premier rapport d'un récepteur revenu de l'arrière-plan
porte `resumed` : ses trous sont les images que notre propre émetteur a évincées
faute de drainage et son silence était celui du navigateur, donc ce rapport n'est
pas lu comme une surutilisation et **la coupe du silence est défaite à l'instant**
(la cible revient où le lien l'avait laissée, une coupe de vraie congestion
antérieure reste) au lieu de remonter en cinq rapports. Couper
vite et remonter lentement : une coupe coûte de la netteté une seconde, un
débordement coûte la main du joueur.

**Trois couches** dans la boucle de session, et un seul `setBitrate()` : le
plafond du joueur (le réglage, déplacé par l'échelle du front via
`setTargetBitrate`) → ce que le lien prend (le gouverneur ; c'est le débit du
fil, et celui du modèle de lien `LinkOccupancy`) → le budget par image de la
cadence réelle (§9.8), et la rafale de raffinement par-dessus.

**Le front s'efface** : `_onStreamCongested` ne reconstruit plus la session
pour l'hôte natif. Relancer pour −30 % de débit vingt secondes après le premier
signe jetterait une image que l'hôte adapte déjà en 500 ms. L'échelle garde ses
autres hôtes.

Vérifié en LAN (04/09) : « link reports flowing from the receiver (first:
owdRise 0 ms, gaps 0) », gouverneur muet, 6,1 ms. La réaction en vraie
congestion se lit dans le log — « [native] link: delay rising — encoding at
32000 kbps of the 40000 set » — et **reste à observer sur un lien qui
souffre** (4G, hôtel).

### 9.10 Une image perdue se répare par un delta (04/09/2026)

Le §9.2 promis depuis le début. Jusqu'ici un trou dans la numérotation côté
client fermait la porte aux deltas et réclamait une IDR : ~70 Ko d'un coup sur
un lien déjà en peine, et une image figée le temps de l'aller-retour.

NVENC garde désormais un **DPB de quatre images** (`maxNumRefFrames` /
`maxNumRefFramesInDPB`, les trois codecs) et chaque image est estampillée de
**son propre numéro** (`inputTimeStamp = frameNumber`). Quand le récepteur nomme
celle qu'il n'a pas reçue, `NvEncInvalidateRefFrames` la retire des références
et l'image suivante est prédite depuis celles que le récepteur possède : un
delta ordinaire, le flux se répare sans rien de plus gros. Le DPB est un repli,
pas une recherche plus large — l'encodeur prédit toujours depuis la dernière
image, et le temps d'encode ne bouge pas : mesuré A/B au banc (`dpb=1` contre 4,
clip FPS 1440p, 40 Mbit/s, deux passes chacun) 3,69 / 3,79 ms contre
3,75 / 3,77 ms, même taille, même QP.

Le chemin : `/start` répond `ref_invalidation` quand l'encodeur de la session le
fait vraiment (`SessionInfo::referenceInvalidation`, faux sur AMF et oneVPL) ;
sur un trou de numérotation le client envoie `invalidateref {from, to}` (ids de
fil) et **continue de décoder** au lieu de jeter les deltas jusqu'à la keyframe ;
le relais DC traduit les ids de fil en numéros de moteur — un anneau des 512
derniers, parce que les deux divergent à chaque image que le relais jette avant
d'attribuer un id — et appelle `Session::invalidateReference` ; la session le
garde pour le thread de capture, qui le dit à l'encodeur juste avant la
prochaine image. Un trou plus large que le DPB (16 ids), un id oublié, un
encodeur sans la fonction : keyframe comme avant, le récepteur a toujours une
réparation. Un décodeur qui n'accepterait pas de décoder par-dessus le trou
tombe dans `_handleDecoderError`, qui demande une keyframe.

Vérifié avec le crochet de debug `localStorage.mw_drop_test = N` (le client
jette un delta sur N) : « Frame gap: lost 120..120 — naming them to the host,
decoding on », « reference invalidated: frame 222 never reached the receiver,
healing with a delta », aucune IDR demandée, aucune erreur de décodeur, 53
images/s et 5,2 ms pendant l'exercice. L'éviction C5 du `FrameSender` garde
la keyframe (l'émetteur ne dit pas laquelle il a jetée) — à brancher ici plus
tard.

### 9.11 La cadence s'aligne sur le rafraîchissement du client (04/09/2026)

Un client qui peint sur son vsync — *tearing* coupé, ou un navigateur qui ne
sait pas déchirer — affiche au plus **une image par rafraîchissement**, et
seulement au rafraîchissement. Un stream 60 sur un écran client 144 Hz tombe
alors sur des tics espacés de 2,4 rafraîchissements : certaines images tiennent
deux rafraîchissements, d'autres trois, et l'œil lit l'alternance comme un
à-coup alors qu'aucune image n'a été perdue. Le même stream sur 120 Hz est
parfaitement régulier — 60 divise 120.

Donc quand le client peint sur son vsync, le stream tourne à un **diviseur
entier du rafraîchissement client** plutôt qu'au réglage exact, dans une fenêtre
de ±20 % autour de lui : 144 Hz et 60 réglés donnent **72** (un rafraîchissement
sur deux), 165 Hz donne **55** (un sur trois), 120 Hz garde 60 (un sur deux).
Le réglage est un vœu sur la fluidité et le budget du lien, pas un contrat ; une
cadence à un cinquième de lui qui tombe sur la grille du client est l'image la
plus lisse pour le même coût. Le diviseur le plus proche gagne, le plus rapide
en cas d'égalité (72 plutôt que 48 pour 60 sur 144 Hz : un joueur qui a demandé
60 et peut avoir 72 pour la même fluidité est mieux servi).

`AlignedCadence` (`core/CadenceAlign.h`, pur) fait ce choix. La grille de
`FrameCadence` est tenue en **nanosecondes** : 55 fps sur 165 Hz, c'est trois
périodes de 6060,6 µs = 18181,8 µs, et une grille tronquée au microseconde
glisserait d'une image toutes les deux minutes (mesuré en test sur 99 000
présents : zéro dérive, chaque écart de trois présents exactement). Le budget
de l'encodeur suit par `EffectiveCadence::retarget()` — l'encodeur garde le
débit par image pour lequel il a été construit, et le débit transmis est
multiplié à l'instant où la grille change, jamais une seconde d'images
sur-dimensionnées pendant qu'elle passe de 60 à 72.

**Quand la règle ne s'applique pas**, la garde de cadence ordinaire (§9.6)
reprend telle quelle : le client *déchire* (Chromium desktop, le défaut) — il
n'y a pas de grille contre laquelle battre, l'image est peinte dès qu'elle est
décodée, et le réglage brut est la plus basse latence ; le réglage est 0 (le
rythme de l'hôte) ; aucun diviseur ne tombe dans la fenêtre (le réglage tient) ;
ou le diviseur demande plus que l'écran hôte ne produit (hôte 60 Hz, client
144 Hz, 60 réglés → 72 : l'hôte n'a pas 72 présents à donner, le réglage tient).

**Le chemin.** Le client mesure son rafraîchissement par `requestAnimationFrame`
(`util/RefreshRate.js`) : la **moyenne** des bons deltas donne la période au
dixième de pour-cent (les horodatages sont grossis à la milliseconde sans
isolation cross-origin, donc un delta seul ne distingue pas 165 de 166,7 ; une
médiane trie d'abord les images que le navigateur a sautées). La valeur part
dans `/start` (`client_refresh_mhz`, `client_vsync = !tearing`) ; l'hôte choisit
la cadence à l'ouverture et la journalise (« 55 fps stream for a 165 Hz client
presenting on vsync (60 set, every 3rd refresh) »). Quand la fenêtre change
d'écran en cours de stream, le nouveau taux part en `clientrefresh {mhz, vsync}`
sur le canal d'input et l'hôte re-choisit **entre deux images**, sans rien
relancer (`Session::setClientRefresh`, natif seulement, `qobject_cast` sur les
deux relais). Le message est traité comme `framefloor` : accepté par
`InputPolicy`, inerte pour tout host GameStream.

Vérifié le 04/09 sur l'instance dev (client Chrome dédié, *tearing* coupé, écran
165 Hz) : `/start` porte 164 972 mHz, l'hôte ouvre à **55 fps, un
rafraîchissement sur trois**, NVENC configuré `2560x1440@55`, budget par image
tenu ; fin de session 4 771 présents, 797 non portés, arrêt propre. **Limite
mesurée** : Chrome garde son horloge `requestAnimationFrame` à 165 Hz même
lorsque la fenêtre est physiquement sur l'écran 60 Hz, donc le re-choix en cours
de session ne se déclenche pas sur cette machine — le navigateur ne distingue
pas le taux par moniteur. Le chemin de re-choix est couvert par test unitaire
(`retarget`) ; sa démonstration en vrai demande un client qui rapporte
effectivement deux taux (à voir chez Bruno, multi-moniteurs de fréquences
différentes).

### 9.12 Gels et récupération côté navigateur (04/09/2026)

Ce que le navigateur fait quand l'image s'arrête ou se casse a été écrit pour
Sunshine : une keyframe demandée à chaque doute, parce que c'était la seule
réparation qui existait. Avec l'hôte natif, §9.10 en a apporté une autre — la
perte nommée, réparée par un delta — et les cinq réflexes ci-dessous ont été
relus à cette lumière. La règle : **le chemin GameStream ne change pas d'un
octet** ; tout ce qui suit est conditionné à `ref_invalidation` (la réponse
`/start`) ou à l'hôte natif.

**Famine (G1).** Le transport déclarait une famine après 1 s sans image et
demandait une IDR. Pour l'hôte natif, cette demande est toujours de trop : une
image qui n'est pas venue n'est pas une image perdue. Si des images ont été
jetées en route, la première qui arrive porte un trou de `frameId` et le
gestionnaire de trous les nomme à l'hôte (§9.10) ; si aucune ne l'a été, le flux
reprend simplement. Dans les deux cas la keyframe aurait été la plus grosse image
qui soit, envoyée sur un lien qui vient de prouver qu'il souffre. Donc, quand
l'hôte répare par invalidation, la famine **compte et chronomètre** (`stalls`,
ligne « Stream resumed after N ms ») et **ne demande rien**. ⚠️ Le plan
prévoyait un seuil adaptatif de « 4 intervalles d'image » (67 ms à 60 fps) :
**impossible**. Un écran fixe se tait légitimement — le plancher de vivacité de
l'hôte est à **500 ms** (§9.1, mesuré : 2 fps dans l'overlay sur un bureau
immobile), Sunshine n'envoie rien du tout — et un seuil de 67 ms lirait chaque
bureau au repos comme un flux mort. La seconde est la marge au-dessus de ce
plancher, et c'est sur elle que l'hôte a dimensionné son plancher.

Deux autres demandes du transport tombent sous la même règle sur l'hôte natif :
l'image incomplète et l'image périmée (`FRAME_TIMEOUT_MS`). Et un **bug** trouvé
à la lecture : `onFrameLoss` invalidait la référence côté vue **sans condition**,
donc sur l'hôte natif une image dont un fragment manquait était d'abord nommée à
l'hôte et réparée par un delta (§9.10), puis, 500 ms plus tard, le nettoyage des
images périmées la déclarait perdue, invalidait la référence, jetait tous les
deltas et demandait la keyframe que la réparation venait d'éviter. Sur l'hôte
natif `onFrameLoss` ne fait plus rien : le trou de `frameId` est la seule voie.
Et comme le canal vidéo est **ordonné** (`unordered = false`, 3 retransmissions),
une image qu'une plus récente a dépassée ne se complétera jamais : elle est
déclarée perdue **à l'instant** où la suivante s'assemble, au lieu d'attendre les
500 ms de l'horloge — c'est le temps pendant lequel l'image restait en retard
d'une image qu'elle aurait pu montrer.

**Ride-out (G2).** Le chien de garde qui laisse la vague d'intra-refresh
travailler avant de réclamer une keyframe comptait **2,5 s** d'horloge. Or la
vague avance **par image encodée** : `intraRefreshPeriodFrames` vaut 2 s de
la cadence de l'encodeur, mais un jeu qui présente à 30 sous un stream 165
étire une période de 330 images sur **11 s**, et l'horloge de 2,5 s l'aurait
déclarée en échec quatre fois de suite. `/start` porte désormais
`intra_refresh_frames` (`SessionInfo::intraRefreshFrames`, le nombre exact que
les trois encodeurs ont reçu) et le chien de garde compte les **images reçues**
contre 1,2 période — les images non reçues ont fait avancer la vague aussi, mais
ne compter que les siennes est le côté prudent — avec l'horloge gardée en
garde-fou (15 s) pour un flux qui s'arrête. Le message « Ride-out did not
recover » devient un compteur (`rideOutFailed`), montré dans le détail de
l'overlay avec les deux autres. Sans période connue (hôte qui ne la dit pas),
l'horloge de 2,5 s reste seule.

**Erreur du décodeur (G3).** `VideoDecoder` est à usage unique : sur erreur,
la vue le ferme, en crée un autre, remet le parseur NAL à zéro et demande une
keyframe. Relu : rien n'est à garder — les deltas en attente prédisent depuis
une référence que le nouveau décodeur n'a pas, et le parseur remis à zéro se
reconfigure à l'arrivée de la keyframe, qui porte ses SPS/PPS de toute façon.
Ce qui manquait, c'est la **mesure** : la récupération est chronométrée de
l'erreur à la première image que le nouveau décodeur sort, et journalisée avec
les images jetées entre-temps (« Decoder recovered in N ms, M frames dropped
meanwhile »), sur le thread principal comme dans le worker ; compteur
`recoveries`, même ligne d'overlay.

**Garde keyframe (G4).** Côté hôte, `sendBufferedKeyframe` ne jette une
keyframe tamponnée que si une plus récente est **déjà partie** — la mémoire
`webrtc-media-freeze-diagnosis` est respectée telle quelle. Côté natif, une
chose changeait : une keyframe demandée (`requestKeyframe`) n'était encodée qu'au
prochain `emit`, et sur un **écran fixe** le prochain `emit` est le tic du
plancher — jusqu'à **500 ms** pendant lesquelles un navigateur en récupération,
ou qui vient d'ouvrir son canal vidéo, regardait le vide alors que l'image
convertie attendait. La boucle encode maintenant **à l'instant** une keyframe
demandée sur un écran immobile (même image que le plancher aurait envoyée, un
encodage). C'est ce que Sunshine ne peut pas faire (§9.1 : il n'encode que sur
dommage) et que l'hôte natif peut.

**Onglet caché, retour (G5).** Mesuré le 04/09 sur l'instance dev : un onglet
**caché** continue de décoder — seuls ses timers passent à une seconde,
`linkstats` compris, ce qui reste sous les 4 s de silence que le gouverneur
(§9.9) lit comme un lien mort — et il n'y a rien à réparer. Une page **gelée**
(onglet d'arrière-plan sur téléphone, ou Chrome qui gèle un onglet caché depuis
longtemps ; `Page.setWebLifecycleState frozen` au banc) est différente : le
gouverneur coupe à 16 000 kbps pour « no report from the receiver » et remonte
en cinq rapports, et au dégel les images qui attendaient dans le canal arrivent
en rafale. Toute mesure qui lit un temps d'arrivée prendrait cette rafale pour
le fait du lien — la référence de délai aller simple lirait des secondes de
« montée » et l'hôte couperait son débit au moment même où le spectateur
revient ; le détecteur d'arrêts périodiques marquerait un événement ; le pacer
épinglerait sa réserve. Au retour (`visibilitychange` → `_resyncAfterHidden`)
ils sont ré-amorcés, le transport apprend que le silence était le nôtre, et les
manettes — dont la scrutation `requestAnimationFrame` s'est arrêtée avec
l'onglet, pendant que le chien de garde de l'hôte centrait ce qu'il n'entendait
plus — sont **redites une fois**, au repos ou non (`GamepadManager.resendAll`).
Les images que l'hôte a évincées pendant l'absence arrivent comme un trou de
`frameId` et prennent le chemin ordinaire : nommées, réparées par un delta.
Rien n'est demandé au retour — sauf, depuis le 05/09, que le premier
`linkstats` porte `resumed` (§9.3) : le gouverneur ne lit pas ses trous comme
ceux du lien et défait à l'instant la coupe que notre silence lui avait fait
faire, au lieu de la remonter en cinq rapports.

---

## 10. Host natif dans l'UI

- **Aucun pairing** : ni PIN, ni certificat, ni association. Il n'y a pas deux
  parties à authentifier.
- **Aucun appel réseau** au lancement : `launch()` renvoie un descripteur
  nommant un display.
- **Les displays SONT la liste d'apps** : une carte par écran, titrée du seul
  numéro que montrent les réglages Windows (`Display 1`) — le modèle du moniteur
  et le mode restent dans le log de sonde, pas sur la carte. La grille existante
  devient le sélecteur, zéro nouvelle UI, et avec un seul écran c'est une carte
  et un clic.
- Host nommé `<hostname> — MoonlightWeb Host`, non persisté (recalculé au
  démarrage d'après ce que la machine sait faire).

---

## 11. Repli vers Sunshine

`probe()` répond `unavailable` avec une raison machine — jamais affichée telle
quelle — dans exactement ces cas :

| Condition | Détail |
|---|---|
| Pas d'API de capture | DDA **et** WGC échouent |
| Aucun display attaché | machine headless |
| Pas d'encodeur utilisable | aucun GPU avec encodeur **et** codec |
| Pas de session interactive | service Windows en session 0 |
| OS trop ancien | Windows < 10 2004 |
| Architecture non supportée | Windows ARM64 en v1 |

Message unique, non technique :

> « Le streaming natif MoonlightWeb n'est pas disponible sur cette
> configuration. Installez Sunshine pour utiliser cette machine comme host. »

Sur la page Hosts, ce message n'apparaît **que** si aucun host n'est visible
**et** le natif est indisponible.

---

## 12. Licences

| Dépendance | Licence | Commercial | Note |
|---|---|---|---|
| nv-codec-headers (NVENC) | MIT (notice NVIDIA sur l'en-tête) | OK | rien du SDK n'est redistribué ; `nvEncodeAPI64.dll` vient du pilote |
| SDK Windows (DXGI, D3D11, WASAPI) | licence SDK | OK | — |
| AMF, oneVPL | MIT | OK | en-têtes seulement, les runtimes viennent des pilotes |
| **ViGEmClient** (manette) | **MIT** | OK | ⚠️ noté BSD-3 dans le plan d'origine — c'est faux, l'amont livre du MIT. Vendoré tel quel en v1.16.18.0, jamais modifié |
| ViGEmBus (le pilote) | BSD-3 | OK | **pas redistribué** : installé par l'installeur depuis l'amont |
| **libopus** (audio) | **BSD-3** | OK | livré le 04/09/2026 : sous-module `native-host/third_party/opus` épinglé v1.5.2, bibliothèque statique, sans programmes ni tests ni installation |
| libva, libdrm, EGL/GLES, GBM | MIT | OK | livrés le 05/09/2026 (§19), liés dynamiquement, trouvés par pkg-config |
| **libpipewire-0.3** (audio Linux) | **MIT** | OK | livré le 05/09/2026 (§19.7) ; **libpulse écarté** (LGPL) — d'où la limite « PipeWire doit être le serveur audio » |
| OpenH264 | BSD | OK | à venir (repli logiciel, pas encore nécessaire) |
| ❌ FFmpeg / libavcodec | LGPL/GPL | **écarté** | Sunshine l'utilise (156 appels `av_*`) ; nous non |
| ❌ x264 / x265 | GPL-2.0 | **interdit** | — |
| ❌ moonlight-common-c | GPL-3.0 | **jamais lié au module** | — |

**Brevets codec** — à arbitrer à la commercialisation, pas maintenant : AVC
(Via LA), HEVC (Access Advance + Via LA, le plus complexe), **AV1 (AOMedia,
libre de redevance)**. D'où la préférence AV1 quand les deux bouts suivent.

---

## 13. État

| Livré et mesuré | Reste |
|---|---|
| Module isolé + garde de licence | HDR (P010 + PQ) |
| **Audio** : WASAPI loopback → cadenceur 5 ms → libopus, thread « Pro Audio » (04/09/2026 ; le son reste audible sur l'hôte, à couper en v0.3.0) | |
| `IMediaEngine`, relais découplés | |
| Sonde displays/GPU + association | WGC en repli |
| Capture DXGI (0,06 ms) | Lanceur de session console (service Windows) |
| Conversion NV12 + AYUV 4:4:4 | UI (grille d'écrans, ligne GPU/encodeur) |
| NVENC (3,46 ms), AMF (3,70 ms), oneVPL (écrit, non exécuté) | Retrait de Sunshine de l'installeur |
| Intra-refresh sur les trois encodeurs + ride-out client | Linux, macOS |
| Curseur composé, plancher sur écran immobile choisi par le client (§9.1) | Benchmarks : NVENC et AMF (iGPU) mesurés le 04/09/2026 (`docs/bench-native-host.md`) ; RX 7600 et Intel attendent leur GPU ; l'application du réglage attend l'A/B |
| Copie inter-GPU (§5, 04/09/2026) : un écran dont le GPU n'encode pas streame quand même | |
| Six étapes mesurées par frame, p95/p99 dans les stats et le log (§4, point 4) | |
| Clavier/souris (`SendInput`), manette (ViGEm) + rumble | |
| Installeur : ViGEmBus en silencieux | |

**Le chemin est complet côté serveur**, et jouable : le host natif apparaît sans
pairing, un clic sur un écran construit un `NativeMediaEngine` qui alimente le
relais WebRTC existant, et clavier/souris/manette reviennent par le
DataChannel d'entrée. Le chemin Sunshine/Wolf/MultiSeat est intact.

**L'audio est là depuis le 04/09/2026** (`src/audio/`) : la sortie par défaut de
l'hôte, capturée en loopback WASAPI, cadencée à une trame Opus toutes les 5 ms
exactement (le fil avance l'horloge RTP d'une trame par paquet, donc le
cadenceur remplit de silence quand rien n'a été capturé et jette au-delà de
20 ms de file), encodée par libopus en CELT bas délai à 128 kbit/s VBR. Ce qui
reste : l'hôte continue d'entendre son propre son — le loopback capture ce qui
part vers les haut-parleurs — là où Sunshine le coupe (`localAudioPlayMode`).
Périphérique audio virtuel ou équivalent à étudier pour la v0.3.0.

### Input — ce qui a été tranché

| Point | Décision |
|---|---|
| Clavier | **Scancodes**, pas codes virtuels : jeux et DirectInput ne voient que ça. VK direct sur `NON_NORMALIZED` |
| Souris absolue | Mappée sur l'écran capturé puis sur le bureau **virtuel** (0..65535), avec le rectangle DPI-virtualisé — l'inverse du correctif de §6.1, et c'est voulu |
| Souris relative | Passe par l'accélération du pointeur de l'hôte, comme une vraie souris |
| Manette | **Xbox 360 via ViGEmBus**. Les bits étendus Sunshine (paddles, touchpad, Share) sont jetés : un pad X360 n'a pas ces boutons |
| Bitmask de modificateurs | **Ignoré** : le navigateur envoie déjà un vrai keydown/keyup pour Maj/Ctrl/Alt/Meta |
| Fin de session | Tout ce qui est encore enfoncé est relâché, et les pads débranchés |
| Pilote absent | Dégradation silencieuse : clavier/souris intacts, pas de manette |
| Saut de thread | ⚠️ **Toujours présent** : le relais marshale vers le thread Qt (`DataChannelRelay.cpp:764`) parce que le même handler pilote presse-papier, politique et stats. Le sink est prêt, le relais non |

---

## 14. Le banc : `--native-bench`

L'instrument des benchmarks d'encodeur. Il fait tourner le moteur — capture,
conversion, encodage — sur un écran pendant N secondes **vers un puits** : pas de
réseau, pas de navigateur, rien en aval de l'encodeur n'entre dans la mesure.

```
MoonlightWeb.exe --native-bench display=1,seconds=10,codec=hevc,fps=0,bitrate=20000,out=bench.csv
MoonlightWeb.exe --native-bench ""          # liste les écrans (display=<id>)
```

Clés : `display`, `seconds`, `codec` (hevc|h264|av1), `fps` (0 = celui de
l'écran), `bitrate` (kbps), `width`/`height` (0 = ceux de l'écran), `yuv444`,
`intra` (intra-refresh), `out`.

**Depuis le 04/09/2026, les réglages d'encodeur eux-mêmes** (`EncoderTuning`,
en-tête public, défaut = le choix du moteur, jamais rempli par une session
navigateur) : `preset=1..7`, `tuning=ull|ll`, `multipass=off|quarter|full`,
`aq`, `taq`, `preanalysis`, `quality=speed|balanced|quality`, `tu=1..7`,
`vbv=<frames>` ; et `gpu=<id>` pour encoder sur un autre GPU que celui de
l'écran (copie inter-GPU, §5) — c'est ainsi que l'iGPU AMD de bench-desk, qui ne
pilote aucun écran, a pu être mesuré. Sans `display=`, le banc liste écrans **et
GPU**. Chaque encodeur écrit dans son log ce que le preset ou l'usage active de
lui-même (multipass, AQ, lookahead ; qualité, pré-analyse, VBAQ) puis la
configuration effective. Le lookahead n'est pas exposé : il retient N images par
construction, disqualifié avant toute mesure. La variable d'environnement
`MW_NATIVE_TUNING` accepte les mêmes clés sur une **vraie session** (log
« MW_NATIVE_TUNING in effect »), pour l'A/B à l'œil que le banc ne peut pas
faire. Campagne du 04/09/2026 et recommandation : `docs/bench-native-host.md`.

Par frame, une ligne CSV : numéro, keyframe, capturée ou ré-émise, octets,
**QP moyen** (`frameAvgQP` côté NVENC, `StatisticsFeedbackAvgQP` côté AMF — un
q-index 0–255 en AV1), t₀ présent, t₁ acquis, t₂ converti, t₃ encodé, et les
durées dérivées. Le résumé sur la sortie standard donne cadence de capture
effective, moyenne/p95/p99 par étape, taille par frame (toutes, puis deltas
seuls) et QP moyen.

Premier passage le 02/09/2026 sur bench-desk : NVENC (RTX 5060 Ti) rend le QP —
première keyframe 1080p à **36 Ko / QP 36**, puis la rafale de raffinement à
248 Ko / QP 20 → 15, encode 5,8 ms de moyenne ; AMF (RX 7600, 1440p) encode en
5,5 ms mais **ne rend aucune statistique** sur ce pilote (`GetProperty` échoue
sur le buffer de sortie) — à élucider quand la campagne AMF commence.

Le QP est le **proxy objectif de qualité** du protocole de bench (plan v2 §5) : à
débit fixe, un réglage plus rapide qui coûte plus de 2 points de QP moyen n'est
pas plus rapide, il est plus flou. Les trois contenus (bureau fixe, défilement
de texte, séquence de jeu rejouée plein écran) sont affaire d'opérateur : le banc
mesure ce qui est à l'écran.

Pas un service : Desktop Duplication exige le bureau interactif, donc une
commande de terminal.

### ✅ Vérifié de bout en bout (31/08/2026)

Une **image réelle du bureau, dans un navigateur**, par le moteur natif :

- host `bench-desk — MoonlightWeb Host` **READY**, aucun pairing demandé ;
- une seule carte d'app, `Display 1 — 2560×1440 · 60 Hz` — la grille d'apps
  existante EST le sélecteur d'écran, comme prévu ;
- un clic → stream ; côté navigateur :
  `First video frame: isKeyframe=true size=39946 codec=hevc`, rendu en 1920×1080 ;
- côté worker : duplication 2560×1440 → conversion + mise à l'échelle 1920×1080
  → NVENC HEVC intra-refresh ;
- arrêt propre au bouton.

Trois bugs ont été trouvés en poussant ce test, et aucun n'était visible en
test unitaire : le crash du préchargement de jaquettes (§6.5), la validation
d'URL RTSP appliquée au natif, et `serverCodecModeSupport` à zéro.

### ✅ Multi-GPU vérifié (31/08/2026)

Deux cartes dédiées dans la même machine — RTX 5060 Ti et RX 7600 — avec un
écran sur l'AMD et un dummy HDMI sur la NVIDIA. C'est le scénario que §5
(association display → GPU) existe pour couvrir.

DXGI attribue chaque sortie sans ambiguïté, y compris entre constructeurs :

| Display | GPU | Encodeur retenu |
|---|---|---|
| Display 1 — 2560×1440 · 60 Hz | AMD RX 7600 | **AMF** |
| Display 2 — 800×600 · 30 Hz | AMD RX 7600 | *(écran virtuel Parsec/VDD)* |
| Display 3 — 1920×1080 · 60 Hz | NVIDIA RTX 5060 Ti | **NVENC** |

Chaque display encode donc sur **le GPU qui le scanne**, en zéro-copie, sans la
moindre table de correspondance à maintenir. Les capacités le confirment aussi
par adaptateur : la RX 7600 rapporte AV1/HEVC/H.264, l'iGPU AMD du même pilote
seulement HEVC/H.264.

**Limite constatée, et voulue** : deux sessions propriétaires simultanées ne
coexistent pas — la seconde démolit la première. C'est le take-over délibéré du
`/start` existant, pas une propriété du moteur natif.

### ⚠️ Intel (oneVPL) — écrit, jamais exécuté

Le chemin Intel est implémenté (sonde de capacités + encodeur) mais **aucun GPU
Intel ne l'a jamais exécuté**. Tous les autres encodeurs de cet arbre ont été
mesurés sur la machine qu'ils visent ; celui-ci non. À considérer comme non
prouvé jusqu'à ce qu'il encode une frame sur du vrai matériel.

Ce qui EST vérifié, sans matériel :

- **le calcul des paramètres**, qui est la partie la plus facile à se tromper
  en silence. `TargetKbps` est un `mfxU16` : au-delà de 65535 kbps il faut
  `BRCParamMultiplier`, sans quoi une demande à 100 Mbps se replierait sur une
  fraction d'elle-même et ressemblerait à un réglage de débit ignoré. Testé :
  150000 kbps → 50000 × 3 ;
- **l'alignement** : surface 16-alignée, crop à la taille réelle (1080 → 1088
  de surface). Se tromper donne quelques pixels de rebut au bord ;
- **l'absence du runtime**, qui doit répondre proprement — c'est le cas sur ce
  banc, sans GPU Intel.

Choix assumés, notés pour qui reprendra :

| Point | Décision |
|---|---|
| Runtime | oneVPL 2.x (`libvpl.dll`) seulement, pas le Media SDK historique |
| Implémentation | filtrée sur HARDWARE — sinon oneVPL sert son repli logiciel en silence |
| Choix du GPU | par `MFX_HANDLE_D3D11_DEVICE` sur NOTRE device, comme AMF, plutôt qu'en appariant à la main les énumérations Intel et DXGI |
| 4:4:4 | non revendiqué : la passe de conversion produit de l'AYUV, qu'oneVPL ne prend pas en entrée d'encodeur |
| Intra-refresh | demandé par `mfxExtCodingOption2` (`IntRefType = VERTICAL`), avec repli explicite sur les keyframes si `EncodeInit` le refuse — le refus est journalisé, jamais avalé |

### Vérification restante

**Un stream Wolf réel.** Le refactor `IMediaEngine` est purement typologique —
aucun corps de méthode modifié — mais il touche les trois relais. Sunshine a
été revérifié le 31/08 après le chantier intra-refresh (HEVC, 132 fps, 9,9 ms,
aucun changement de comportement) ; Wolf reste à repasser.

## 15. Le service : capturer depuis la session console (04/09/2026)

L'installation standard de MoonlightWeb est un service Windows (NSSM, session 0).
La session 0 n'a pas de bureau : Desktop Duplication n'y duplique rien, `SendInput`
n'y atteint aucune fenêtre, et la sonde du moteur le dit déjà en une ligne
(`hasInteractiveSession()` = faux, « no interactive desktop session »). Sans ce
chantier, le host natif n'existe donc **pas** pour l'installation la plus
courante — il n'apparaît qu'en instance dev, lancée à la main sur un bureau.

Tout ce que le natif fait doit se passer dans la **session console** — celle qui a
le moniteur et le clavier — dans un processus qui tourne sous l'utilisateur qui y
est connecté. Deux choses en découlent : **sonder** le moteur, et **lancer** le
worker de stream, tous deux ailleurs que dans le processus service.

### 15.1 `--native-probe` : les yeux du service sur le bureau

`MoonlightWeb --native-probe` fait tourner `NativeHost::probe()` là où il est
lancé, imprime **un objet JSON** sur stdout (`NativeCapabilitiesJson`, schéma
versionné : les deux bouts sont le même binaire, mais un schéma inconnu — un
fichier remplacé sous un service qui tourne — est refusé en entier plutôt que
cru à moitié) et sort ; le log du moteur part sur stderr, où le parent le récupère
pour le sien. Les énums voyagent en valeurs numériques, la poignée d'adaptateur
64 bits en hexadécimal (un `double` JSON perdrait ses bits au-delà de 2⁵³).

`NativeProbeService` en fait un **instantané** : lancer un processus à chaque
requête de la liste des hosts serait cher, donc la dernière réponse est gardée,
rendue tout de suite, et renouvelée en arrière-plan quand elle a plus de 20 s et
que quelqu'un demande, quand l'utilisateur console change (ouverture/fermeture de
session, surveillée toutes les 5 s **sans** rien lancer), et au démarrage.
`changed()` lève la carte du host quand l'utilisateur se connecte et la baisse
quand il se déconnecte. Avant la première réponse, l'instantané dit
« indisponible, en attente de la sonde de session console » : un host qui
apparaît une seconde après le démarrage du service est correct, un host offert
qui échoue ne l'est pas. Sur un bureau (instance dev, lancement manuel), rien de
tout cela : `snapshot()` appelle directement le moteur, à chaque fois.

### 15.2 `ConsoleProcess` : lancer sous l'utilisateur connecté

`WTSGetActiveConsoleSessionId` + `WTSQueryUserToken` donnent le jeton du shell de
l'utilisateur ; sous UAC c'est le jeton **filtré** d'un administrateur, alors le
jeton **lié** (le complet) est demandé ensuite et préféré quand il existe — le
worker tape alors dans les fenêtres élevées comme l'utilisateur le pourrait, et
tourne quand même **sous l'utilisateur, pas sous SYSTEM**. Un processus réseau qui
décode des inputs venus d'un navigateur est précisément celui à qui laisser le
moins de privilèges possible ; le prix — image noire pendant qu'une invite UAC est
à l'écran, que la boucle de capture gère déjà comme un display perdu — est celui
que les utilisateurs Sunshine paient sous une autre forme (l'invite est capturée,
mais le stream ne peut pas y répondre non plus).

`CreateProcessAsUserW` sur `winsta0\default`, avec le bloc d'environnement de
l'utilisateur (son `%APPDATA%`, là où Qt met ensuite le log et l'identité du
worker) et trois tubes dont les bouts enfant sont les **seuls** handles hérités
(`PROC_THREAD_ATTRIBUTE_HANDLE_LIST` : le parent peut être SYSTEM et l'enfant
l'utilisateur, aucun socket ni fichier du service ne doit franchir cette ligne).
Les tubes sont drainés par des threads à eux ; la sortie n'est signalée qu'une
fois les deux lecteurs finis, si bien qu'une ligne `response` écrite juste avant
de mourir n'est jamais doublée par la sortie du processus.

`StreamWorkerHost` choisit le lanceur en une ligne : un worker **natif** démarré
par le service va en session console (`ConsoleProcess`), tout autre backend parle
à un host par le réseau et tourne très bien depuis la session 0 (`QProcess`,
inchangé). Le même protocole de lignes JSON, les mêmes trois tubes ; rien après
`start()` ne voit la différence. `MW_CONSOLE_LAUNCH=force` prend ce chemin depuis
un bureau ordinaire, pour l'éprouver sans service.

### 15.3 `/api/native/status` et la ligne d'overlay

`GET /api/native/status` répond `{available, reason, remote_session, ...}`. La
disponibilité et la raison (l'énum) vont à tout appelant : c'est ce qui explique
pourquoi la carte « <hôte> — MoonlightWeb Host » est ou n'est pas dans sa liste.
Le reste — noms d'écrans, GPU, encodeur, codecs — ne va qu'à un appelant **assis à
la machine** (même raisonnement que `/api/internet/status` qui masque sa topologie
à distance). En service, `remote_session` est vrai et `user_present` distingue
« personne n'est encore connecté » d'« aucun encodeur ». Côté client, une ligne
**« Encodeur : NVENC »** dans le détail de latence nomme enfin le bloc de silicium
qui encode ce stream (`describeEncoder()`, portée dans `/start` par
`native_encoder`).

⚠️ **corrigé le 04/09 au soir sur retour de Bruno** : la première version mettait
`describeSession()` en entier — « NVIDIA GeForce RTX 5060 Ti · NVENC HEVC
intra-refresh » — dans le bloc toujours visible. Soixante caractères de valeur
élargissent la carte au-delà d'un écran de téléphone, et **toutes les autres
lignes se retrouvaient coupées** (« 1920× », « 0.8 M »). La chaîne longue reste ce
qu'elle a toujours bien fait, une ligne de log ; le codec a déjà sa ligne, le GPU
est sur la page admin, et ce qui manquait vraiment à l'overlay tient en un mot.
Il est rangé dans le détail plutôt que dans le bloc compact : il explique les
étapes listées sous lui et ne change jamais en cours de session.

### 15.4 Vérifié / à valider par Bruno

Vérifié le 04/09 en mode `MW_CONSOLE_LAUNCH=force` (bureau, `ConsoleProcess` en
`CreateProcess` même session) : le service voit « no desktop (session 0) »,
affiche d'abord « waiting for the console session probe », relaie le stderr de la
sonde en `[native-probe]`, puis « console probe: available — 2 display(s),
5 GPU(s) » et lève la carte du host ; `/api/native/status` en loopback rend les
deux écrans avec GPU/encodeur/codecs. **Reste à valider par Bruno en vrai
service** (installer le binaire en service, se connecter, streamer) : un stream
natif complet passé par `CreateProcessAsUser` sous le jeton console, et la
neutralité du chemin `QProcess` pour Sunshine/Wolf/MultiSeat (inchangé, mais
`StreamWorkerHost` a bougé).

### 15.5 L'installeur Windows n'installe plus Sunshine (04/09/2026)

Conséquence produit de 15.1–15.3, faite sur le feu vert de Bruno **avant** la
validation en vrai service (le plan la faisait attendre) : l'installeur Inno ne
détecte plus Sunshine, ne le télécharge plus, ne lui écrit plus d'identifiants et
n'appaire plus rien. Il reste une page de question, le lien Internet, et une
checklist d'une ligne. Ce qui disparaît du script : la page Sunshine et ses trois
formes, le bouton Skip, la sonde d'identifiants Basic-Auth (avec son encodeur
Base64 maison), le téléchargement `/S` + `--creds`, et l'objet `sunshine` de
`provisioning.json` — donc **plus aucun mot de passe en clair écrit sur le
disque**. `Provisioning::applyOnce` lit un objet absent comme `auto_pair=false` et
marque l'étape « skipped », si bien qu'un serveur plus ancien recevant ce fichier
se comporte exactement comme si l'utilisateur avait cliqué sur Ignorer.

Ce qui **ne** bouge **pas** : `SunshineInstaller` en entier, et la page Sunshine
du `SetupView`. ⚠️ **le plan disait « retrait de Sunshine de l'installeur *et* de
`SetupView` », c'est faux** : `SetupView` ne s'affiche jamais sous Windows
(`/api/setup/status` y répond `setup_completed: true` en dur, et `app.js` sort sur
`os === 'Windows'`) — c'est le premier lancement de **macOS et Linux**, les deux
plates-formes où le moteur natif répond « no backend for this platform in this
build » jusqu'à la phase I. L'y retirer ne complèterait pas H4, ça priverait ces
machines de tout hôte. Sunshine y reste donc la voie normale, et il reste partout
un **hôte** de plein droit : une machine qui le fait déjà tourner se découvre et
s'appaire depuis la page des hôtes comme avant.

Effet de bord corrigé au passage, sans quoi le retrait aurait été une
régression : `NvComputer::isLocalMachine()` répondait faux pour le host natif.
Elle compare des adresses, et le natif n'en a aucune — il *est* ce processus. Or
la mise à jour en un clic ne s'offre que si la machine a un hôte local appairé
(`_canSelfUpdate`), et depuis ce chantier la carte native est le seul hôte d'une
installation Windows fraîche : la bannière serait retombée à jamais sur « mettez à
jour le PC hôte ». `isNativeEngine()` répond maintenant vrai en premier ; au
passage `hostOs()` en profite (il court-circuite sur « local »), et
`/api/setup/status` exclut explicitement la carte native de son `sunshine.paired`,
qui doit continuer de vouloir dire ce qu'il dit.

## 16. HDR réel : scRGB FP16 → P010 BT.2020 PQ (04/09/2026)

Premier morceau de la phase I. Jusqu'ici le HDR était négocié puis **rabattu en
SDR** : le convertisseur ne savait faire que du 8 bits BT.709, et la session
ouvrait donc la capture en 8 bits pour recevoir le bureau déjà tone-mappé par
DXGI. Le chemin complet existe maintenant.

### 16.1 La chaîne, et l'ordre des étapes

DXGI livre un bureau HDR en **scRGB** : lumière linéaire, primaires BT.709, et
1.0 = le blanc SDR, soit 80 nits par définition (le curseur « luminosité du
contenu SDR » de Windows est déjà appliqué par le compositeur). Les valeurs
au-dessus de 1.0 sont les hautes lumières ; celles **en dessous de 0** sont les
couleurs hors BT.709, que scRGB exprime en négatif.

Quatre étapes, et l'ordre n'est pas négociable :

1. **primaires** BT.709 → BT.2020, en lumière linéaire — une matrice n'est
   valide que là ;
2. **échelle absolue** : ×80/10000, PQ étant défini contre un pic de 10 000 nits ;
3. **courbe PQ** (SMPTE ST 2084), avec un `max(0)` juste avant : `pow()` d'un
   négatif donne NaN, et un NaN se propage à toute la trame ;
4. **matrice YCbCr** BT.2020 non-constant luminance, qui est définie **sur le
   signal PQ**, pas sur la lumière.

Faire PQ après la matrice YCbCr est la façon classique d'obtenir une image
presque juste et subtilement fausse dans chaque dégradé.

Sortie : **P010**, 4:2:0 10 bits, plage limitée (luma 64..940, chroma 64..960
autour de 512). Les vues de plan sont les mêmes que NV12 d'un cran plus large,
`R16_UNORM` et `R16G16_UNORM`, parce que P010 range ses 10 bits dans des mots de
16. Le code est **arrondi à l'entier** avant d'être écrit : les 6 bits bas sont
définis comme nuls, le matériel n'en lit que les 10 hauts et s'en moque, mais une
surface seulement valide par accident est une surface que personne ne peut
vérifier — et c'est exactement ce que le test relit.

Le curseur est linéarisé depuis sRGB avant d'être composé (le mélanger tel quel
donne un pointeur bien trop sombre sur un bureau HDR), et le masque d'inversion
est borné contre le blanc SDR : `1 - rgb` sur une haute lumière à 10.0 vaudrait
-9, et ce négatif rencontrant le `max(0)` transformerait le curseur texte en trou
noir.

### 16.2 Ce que l'encodeur doit dire, pas seulement faire

NVENC : `NV_ENC_BUFFER_FORMAT_YUV420_10BIT` (c'est P010), profil **Main10 nommé
explicitement** pour le HEVC — laissé sur Main, NVENC accepte la surface P010 et
encode 8 bits dedans, donc le HDR part à la poubelle en silence — et
`inputPixelBitDepthMinus8`/`pixelBitDepthMinus8` pour l'AV1, qui porte sa
profondeur dans sa config et non dans un GUID de profil.

Surtout, la **description couleur dans le flux** : primaires 9 (BT.2020),
transfert 16 (SMPTE 2084), matrice 9 (BT.2020 NCL), plage limitée. C'est par elle
que le navigateur sait qu'il doit inverser la courbe PQ. Sans elle il suppose du
BT.709 sRGB et peint une image plate et grisâtre : le classique « le HDR est
délavé » qui se lit comme un bug de shader et n'est en fait que trois entiers
manquants.

### 16.3 Deux pièges de type B7 fermés au passage

**AMF et oneVPL annonçaient `supports10Bit`** depuis une vraie requête matérielle
— la silicium l'a — alors qu'aucun des deux n'a de chemin P010. Le Selector aurait
donc accordé le HDR sur une Radeon et la session serait morte à `init()` : c'est
mot pour mot le bug B7, où un `supports444` par GPU tuait le flux au clic. Les
deux capacités sont désormais **fausses par construction**, avec la ligne de code
à restaurer écrite en commentaire, dans le même commit que le chemin encodeur et
jamais avant. Une capacité dit « ce pipeline sait le porter », jamais « cette puce
le pourrait ».

**HDR et 4:4:4 ne peuvent pas voyager ensemble.** Le 10 bits 4:4:4 existe (Y410,
profil HEVC 4) et aucun navigateur ne l'affiche : Chrome 152 accepte
`hvc1.4.156`, le décode en matériel et rend un rectangle vert (F0f). Le Selector
les accordait tous les deux ; il garde maintenant le HDR et rend la chroma, avec
une ligne de log. Le 4:4:4 continue de **choisir le codec** — le HEVC est retenu
parce que c'est lui qui a un chemin 4:4:4 — et n'est repris qu'ensuite.

### 16.4 ⚠️ Le bug qui rendait tout stream SDR impossible depuis un bureau HDR

Trouvé en écrivant ce chapitre, sur un écran mis en HDR pour l'occasion, et
antérieur à lui : `DxgiDuplication` prenait son format dans
`duplDesc.ModeDesc.Format`, qui décrit le **mode d'affichage**, pas la sortie de
la duplication.

`DuplicateOutput1` convertit le bureau vers le premier format de la liste qu'il
peut honorer : une session SDR, dont la liste ne contient que BGRA8, reçoit donc
vraiment du BGRA8. `ModeDesc`, lui, continue d'annoncer FP16. Le convertisseur
construisait alors une vue FP16 sur une texture 8 bits, `CreateShaderResourceView`
la refusait, et la session mourait à la première image sur « could not view the
captured frame ».

C'était **tout stream SDR depuis une machine avec le HDR Windows activé** — le cas
exact que B1 devait régler et n'a réglé qu'à moitié : B1 a cessé de *demander* du
FP16, ceci cesse de *mal lire* ce qui revient. Le correctif est étroit : une liste
à une seule entrée est le seul cas où la réponse est connaissable sans acquérir
une trame, et c'est aussi le seul où `ModeDesc` se trompe (avec la liste HDR à
deux entrées DXGI garde le format du bureau, que `ModeDesc` rapporte fidèlement,
et le repli `DuplicateOutput` ne convertit rien).

### 16.5 Vérifié, et ce qui reste

Vérifié sur bench-desk, écran mis en HDR par `scratchpad/Set-DisplayHdr.ps1` :
`--native-bench display=0,hdr=1` donne « duplication started: (HDR, FP16) » →
« colour conversion: FP16 scRGB -> P010 4:2:0 (BT.2020 PQ, limited) » →
« NVENC ready: HEVC Main10 (BT.2020 PQ) », 30 trames encodées. Le banc a une
option `hdr=0|1`.

Tests (`test_capture`, branche qui ne s'exécute que sur un écran réellement en
mode HDR, et se déclare sautée sinon) : sortie **P010** confirmée, plage de luma
relue **64..588 en codes 10 bits** — dans les bornes légales, donc ni biais oublié
ni échelle oubliée —, 6 bits bas nuls, et une keyframe **HEVC Main10 de 37 Ko**.
La branche SDR relit 16..235 sur le même bureau HDR, ce qui est la non-régression
de 16.4. 1977 vérifications natives HDR allumé, 1966 éteint.

**Reste à Bruno** : le rendu à l'œil sur un vrai client. Le HDR de bout en bout
dépend du présentateur navigateur (F0e/F0f) : AV1 10 bits passe par WebGPU en mode
`linear`, HEVC 10 bits par l'élément `<video>`. Personne n'a encore regardé une
image HDR **native** sur un écran HDR — seulement des chiffres qui disent que les
octets sont dans les bonnes bornes.

## 17. Windows.Graphics.Capture, le repli (04/09/2026)

Deuxième morceau de la phase I. Desktop Duplication répond
`DXGI_ERROR_UNSUPPORTED` quand l'écran n'est pas balayé par l'adaptateur auquel on
la demande : c'est l'état ordinaire d'un portable hybride, dont la dalle pend à
l'iGPU pendant que le dGPU fait tourner le jeu. Aucun contournement n'existe côté
DXGI. WGC passe par le compositeur et ne se soucie pas de qui balaye.

### 17.1 Pourquoi c'est un repli et pas le défaut

DDA réveille l'appelant **sur le présent** — la meilleure propriété de latence de
tout ce moteur. WGC livre par une file que le compositeur remplit : c'est un
réveil derrière une queue, pas le présent lui-même. Le choix est donc DDA
d'abord, à chaque ouverture **et à chaque redémarrage** : un changement de mode ou
un redémarrage de pilote est exactement le moment où le bon backend change, et
rejouer le choix ne coûte qu'une tentative DDA ratée.

⚠️ **Pas de chiffre de comparaison ici.** Deux mesures au banc sur le même écran
se sont contredites (DDA 0,19 ms puis 2,19 ms de moyenne d'`acquire`, WGC 1,81
puis 1,05), parce qu'un bureau immobile ne présente presque rien : avec 16 trames
capturées en 6 secondes, `acquire` mesure surtout **quand l'écran a présenté**, pas
l'API. L'argument structurel tient, le chiffre n'est pas acquis — il demande un
écran en mouvement continu, comme la campagne E s'en était donné un.

### 17.2 Le curseur, que WGC ne donne pas

WGC ne rapporte aucun pointeur : il sait le composer **dans** l'image, et c'est
tout. Insuffisant ici pour deux raisons — un client peut demander à dessiner le
sien (il lui faut la forme en données, pas en pixels) et un téléphone l'agrandit
(il lui faut être séparable du bureau).

Le chemin WGC lit donc le pointeur directement dans Win32 : `GetCursorInfo` pour
la position et le `HCURSOR`, `GetIconInfo` pour la forme. Les trois encodages
Windows s'y retrouvent à l'identique — monochrome (deux masques 1 bit empilés,
AND au-dessus de XOR), couleur avec vraie couverture alpha, et couleur masquée
quand le bitmap n'a pas d'alpha du tout. Le décodeur qui les réduit tous les trois
en « image RGBA + drapeau d'inversion » a été **sorti de `DxgiDuplication` dans
`CursorShape`** et sert désormais les deux backends : ce sont soixante lignes de
manipulation de bits subtile qu'il aurait fallu corriger deux fois.

La position est mise à l'échelle entre le rectangle de bureau (virtualisé DPI) et
la texture capturée (en vrais pixels), sans quoi le pointeur dessiné se retrouve à
une fraction d'écran du vrai sur tout affichage mis à l'échelle.

Vérifié (`test_win32_cursor`, sans matériel particulier) : flèche standard lue en
canevas 32×32 dont **12×19 d'encre**, hotspot dans la forme, aucun pixel à la fois
dessiné et inversant, cache de forme qui ne re-décode pas un pointeur immobile,
mise à l'échelle vérifiée à un pixel près, et pointeur d'un autre écran déclaré
invisible.

### 17.3 ⚠️ L'horodatage de WGC n'est pas celui de DDA

DDA rapporte `LastPresentTime` : l'instant où la trame **a été** mise à l'écran,
toujours dans le passé. `SystemRelativeTime` de WGC est l'estampille du
compositeur et le **devance** — mesuré ici jusqu'à ~9 ms en avance sur l'instant
où on tire la trame, parce que c'est la présentation pour laquelle elle est
planifiée, pas une qui a eu lieu.

Laissé tel quel, c'est une latence de capture **négative**, et ça ne reste pas une
curiosité cosmétique : le gouverneur de lien (E3) dérive le délai aller simple du
client de (arrivée − présent), donc un présent dans le futur gonfle la montée
qu'il lit et fait couper le débit sur un lien qui va bien. L'estampille est donc
plafonnée à « maintenant ». La latence de capture sur ce chemin est un **plancher,
pas une mesure** — ce qui mérite d'être dit, et fait une raison de plus de garder
DDA en premier.

### 17.4 Choix d'implémentation et vérification

Pas de C++/WinRT : sa projection est fondée sur les exceptions, et ce module n'en
lie aucune. Les trois interfaces nécessaires sont du COM ABI ordinaire, activées
par `RoGetActivationFactory`. Le gestionnaire `FrameArrived` **doit** être agile
(`FtmBase`) — le pool libre-thread appelle depuis l'apartment du compositeur, et
sans le marshaleur libre l'abonnement est refusé d'emblée, ce qui a été la
première panne rencontrée. Le pool a **deux** tampons, pas plus : une file est de
la latence, et une plus profonde laisserait le compositeur prendre de l'avance sur
l'encodeur.

`put_IsCursorCaptureEnabled(false)` pour garder le pointeur séparable, et
`put_IsBorderRequired(false)` pour la bordure jaune (Windows 11 seulement ; sur
Windows 10 elle reste).

**`MW_CAPTURE=wgc` force le repli** sur une machine où DDA marche parfaitement.
Sans ça le chemin WGC n'est atteignable que sur du matériel que personne ici ne
possède — et c'est ainsi qu'un repli pourrit : écrit une fois, jamais exécuté,
trouvé cassé sur la seule machine qui en avait besoin. Même intention que
`MW_CONSOLE_LAUNCH=force`.

Vérifié : flux au banc sur le chemin forcé (80 trames, keyframe, arrêt propre), et
dans les tests la relecture des pixels — **luma NV12 19..234**, donc une vraie
image et pas un écran noir, ce que « la session a démarré » n'aurait jamais
prouvé. La branche WGC des tests s'exécute sur **toute** machine, pas seulement
sur celles qui en ont besoin.

## 18. oneVPL : audit sans matériel (04/09/2026)

Troisième morceau de la phase I, et le seul qui ne pouvait pas être exécuté : il
n'y a pas de GPU Intel ici. Le travail utile était donc de relire ce chemin
contre les invariants que NVENC et AMF respectent, et un vrai défaut en est
sorti.

### 18.1 ⚠️ `setBitrate` éteignait l'intra-refresh

`setBitrate` reconstruisait le bloc de paramètres de zéro avec
`fillEncodeParams`, puis appelait `EncodeReset`. Ce bloc neuf jette deux choses
que personne ne verrait partir :

- **la chaîne d'extension**, donc l'intra-refresh s'arrête — pendant que
  `intraRefreshEnabled()` continue de répondre vrai depuis le drapeau posé à
  l'init. Le récepteur est alors informé que le flux se répare seul, encaisse
  une perte en attendant une vague qui ne viendra jamais, et abandonne sur le
  garde-fou de 15 s de G2 ;
- **les corrections du runtime**, appliquées par `EncodeQuery` à l'init et
  absentes de tout bloc fraîchement construit.

Et ça tourne en permanence : le gouverneur de lien change le débit environ deux
fois par seconde sur un lien qui bouge, donc l'intra-refresh survivait à peu près
une demi-seconde de streaming réel.

C'est exactement la forme du bug **B4** sur AMF — une ré-application en cours de
session qui laisse tomber en silence ce que l'init avait mis en place. Le fait
que le même piège se soit tendu deux fois, sur deux encodeurs écrits à des
moments différents, dit que la faute est structurelle et pas d'inattention.

**Correctif** : `applyRateControl()` écrit les champs de débit — et rien d'autre —
dans un bloc **existant**, et `setBitrate` mute `m_Params` au lieu de le
reconstruire. C'est ce que NVENC fait depuis toujours (il réutilise `m_Config`) et
ce qui rend l'erreur impossible plutôt que corrigée. `fillEncodeParams` appelle le
même helper, donc l'arithmétique n'a qu'un seul endroit.

Vérifié sans matériel (`test_vpl_params`) : après un changement de débit, la
chaîne d'extension est toujours attachée, la période d'intra-refresh intacte, une
correction simulée du runtime préservée, et les invariants de latence
(AsyncDepth 1, pas de B-frames, GOP infini) inchangés.

### 18.2 Ce qui reste vrai : ce chemin n'a jamais encodé une image

NVENC et AMF ont été mesurés sur les machines qu'ils visent. oneVPL, non — et la
ligne « ready » le dit encore à chaque session. Ni le HDR ni le 4:4:4 n'y sont
implémentés, et leurs capacités sont **fausses par construction** (§16.3) pour que
le Selector n'y route jamais une session qui mourrait à l'init.

Bruno aura du matériel Intel plus tard ; à ce moment-là, l'ordre est : lever les
capacités une par une, dans le même commit que le chemin qu'elles annoncent, et
jamais avant.

## 19. Linux : les entrées (04/09/2026)

Quatrième morceau de la phase I, entamé par la seule pièce qui pouvait l'être
sans machine Linux : **uinput ne demande aucune bibliothèque**, ce sont des ioctl
noyau. Elle compile donc pour de vrai, à `-Wall -Wextra` sous WSL Debian 13, ce
qui est un cran au-dessus de « écrit ».

### 19.1 Pourquoi uinput et pas le serveur d'affichage

XTEST marche sous X11 et nulle part ailleurs ; Wayland n'a aucun protocole
d'injection et n'en aura pas. uinput crée un **vrai périphérique** dans le noyau,
sous les deux : le même code sert X11, tous les compositeurs Wayland et une
console nue, et le compositeur applique la disposition clavier de l'utilisateur
par-dessus exactement comme pour un clavier physique. C'est aussi ce que la
manette virtuelle utilise déjà, donc un hôte Linux présente trois périphériques
ordinaires plutôt que trois cas particuliers.

**Deux périphériques, pas un** : un appareil qui déclare à la fois des axes
relatifs et absolus est lu différemment selon le compositeur — certains en
ignorent un, d'autres y voient une tablette.

### 19.2 La table clavier, et pourquoi elle est écrite en chiffres

Le navigateur envoie la **position** de la touche, exprimée comme la touche
virtuelle que cette position porte sur un clavier US. Les codes evdev sont des
positions aussi : c'est donc une correspondance position → position, et la
disposition de l'hôte ne doit pas y entrer. Un hôte français tape français depuis
un client AZERTY sans que cette table sache rien de l'un ni de l'autre.

Les valeurs sont des **littéraux** et non des macros `KEY_*`, pour que l'en-tête
compile — et soit **testable** — sur une machine sans en-tête Linux, ce qu'est
toute machine sur laquelle ce moteur a été développé. Les codes sont une ABI
noyau, donc figés. C'est une affirmation, donc elle est vérifiée et non crue :
sous Linux, `UinputInput.cpp` compare 22 entrées couvrant chaque groupe de la
table à `<linux/input-event-codes.h>` par `static_assert`. Une dérive casse la
compilation au lieu de taper la mauvaise lettre sur le bureau de quelqu'un.

Vérifié aussi sous Windows (`test_evdev_keymap`, 104 codes distincts) : rangée
des chiffres qui ne commence pas à zéro, pavé numérique en ordre inverse de ses
touches virtuelles, F11/F12 non contigus à F10, modificateurs gauche et droite
distincts — les confondre casserait AltGr, donc `@` et `#` sur un clavier AZERTY —,
et **injectivité** de la table hors les trois alias voulus.

⚠️ La comparaison avec la table Windows a trouvé une divergence, et elle est
**intentionnelle des deux côtés** : `usScanCode` répond 0 pour Pause parce que
l'appelant Windows retombe alors sur `MapVirtualKey`, cette touche étant
indépendante de la disposition. Linux n'a pas de repli — uinput prend le code ou
rien — donc `KEY_PAUSE` y est nommé. Le test l'exige à **exactement une**
divergence, pour qu'une seconde, elle, échoue.

### 19.3 ⚠️ La route de capture : KMS d'abord, portail en repli

Écrit le 04/09 quand aucune machine Linux n'existait ; **caduc le soir même** :
Bruno a redémarré l'bench-mini sous Ubuntu 22.04 avec sa Radeon 780M, et a tranché
« les deux, KMS puis portail ». Ce qui suit est mesuré sur cette machine.

Le plan disait PipeWire. La reconnaissance a pesé autrement : GNOME en
**Wayland**, et le portail `ScreenCast` demande un clic d'autorisation sur
l'écran de l'hôte à la première session, puis exige de tourner **dans la session
D-Bus de l'utilisateur** — l'exact problème de la session 0 Windows, en miroir.
KMS/DRM lit la sortie écran directement, sans dialogue et sans session, au prix
de `cap_sys_admin` posée ~~sur le binaire~~ par le paquet : Sunshine porte cette
capacité sur cette machine même (⚠️ **corrigé le 05/09 au soir** : pas sur le
binaire, sur un *lanceur* — une capacité sur `MoonlightWeb` lui-même casse son
rpath `$ORIGIN`, §19.8). La symétrie avec Windows s'impose — **KMS =
DDA** (image au scan-out, réveil sur le vblank), **portail = WGC** (file du
compositeur, repli).

Les licences ne bloquent pas : libpipewire, libva, libdrm, EGL, GBM sont MIT.

### 19.4 La chaîne zero-copy Linux, prouvée sur la 780M (04-05/09/2026)

Le zero-copy a **failli ne pas exister**, et l'endroit où il a plié est instructif.

`GETFB2` sur le plan primaire réussit pour tout le monde mais rend des poignées
GEM **nulles** sans `cap_sys_admin` ; avec, elles apparaissent et
`drmPrimeHandleToFD` exporte le tampon (8,9 Mo pour du 1080p, plus que 1920×1080×4
: il est tuilé). Son modificateur `0x200000010467b04` se décode en GFX11, tuile
64K_R_X, **DCC activé avec retile**. Et là : **radeonsi refuse l'import VA-API de
ce tampon** (`invalid parameter`), alors qu'il accepte un modificateur linéaire ou
invalide. L'attribut `VASurfaceAttribDRMFormatModifiers` qu'il annonce sert à
l'allocation, pas à l'import. EGL, sur le même pilote, liste ce modificateur parmi
ses six formats d'import XRGB8888 — et son import a d'abord échoué aussi, en
`EGL_BAD_MATCH`, parce qu'un tampon DCC porte **trois plans** (les pixels, puis deux
de métadonnées de compression aux offsets 8 847 360 et 8 896 512) et que je n'en
passais qu'un. Tous les plans passés, l'import réussit.

La chaîne est donc celle de Sunshine, et elle calque le chemin Windows shader pour
shader : **plan KMS tuilé → EGLImage → deux passes GLES écrivant les plans d'une
surface NV12 allouée par VA-API → encodeur**. La surface est exportée par
`vaExportSurfaceHandle` en deux couches, R8 et GR88, importées chacune comme sa
propre EGLImage et attachée à son propre framebuffer — le même tour que les vues
`R8_UNORM`/`R8G8_UNORM` de D3D11 sur un NV12. La propriété est inversée par rapport
à Windows : c'est **l'encodeur qui possède la surface** et le convertisseur qui
rend dedans, parce que VA-API alloue les siennes.

Le vertex shader **ne retourne pas Y**, contrairement au HLSL : un framebuffer GL
adossé à un DMA-BUF a sa première ligne mémoire en NDC y = −1, et écrire uv (0,0)
en (−1,−1) fait correspondre première ligne source et première ligne cible.
**Vu le 05/09** : le flux d'une session complète décodé par ffmpeg sur la machine
donne le bureau GNOME droit, barre en haut, dock à gauche, texte lisible, orange
Ubuntu et icône Chrome aux bonnes couleurs (donc l'ordre B/R du XRGB est juste).

Vérifié (`test_linux_pipeline`, qui tourne sur toute machine et se déclare sauté
sans écran actif ou sans capacité) : `KmsCapture` liste les connecteurs et leur
mode exact (clock/htotal·vtotal, pas le vrefresh arrondi), capture le plan
primaire en 3 plans, lit le **plan curseur** (256×256 ARGB linéaire — présent ici,
absent sur la VM Debian) ; `GlConvert` convertit et la relecture donne **luma
25..235** ; `VaapiEncoder` produit une **keyframe H.264 de 50 Ko avec SPS, PPS et
IDR** puis un delta de 33 Ko. 1938 vérifications vertes sous Linux, 2013 sous
Windows après l'extraction des types partagés (`CaptureTypes.h`, `CursorDraw.h`,
`EncoderOutput.h`) hors des en-têtes D3D11.

Deux faits mesurés valent d'être retenus :

- **L'horodatage du vblank est prédit**, pas lu : le noyau le calcule depuis la
  position de balayage et il devance le réveil de ~400 µs. Plafonné à
  « maintenant » pour la même raison que sur WGC — le gouverneur de lien lit le
  délai aller simple sur (arrivée − présent).
- **radeonsi 23.2 n'offre pas d'intra-refresh** par colonnes
  (`VAConfigAttribEncIntraRefresh` non supporté) : sur Linux AMD la récupération
  reste par keyframe, et `intraRefreshEnabled()` le dit.

Les en-têtes SPS/PPS sont **écrits par le pilote** depuis les paramètres de
séquence, VUI comprise (`bitstream_restriction` — la leçon B8 — et timing). Les
en-têtes empaquetés que FFmpeg envoie ne le sont pas tant qu'une mesure ne montre
pas qu'il y manque quelque chose.

### 19.5 La couture plateforme : `LinuxProbe` et `LinuxSession` (05/09/2026)

`Unimplemented.cpp` ne sert plus sous Linux dès que les bibliothèques graphiques
sont là (`MW_NATIVE_PLATFORM "linux"`). La sonde répond aux trois questions de
`WindowsProbe` depuis trois autres endroits : les GPU depuis `/dev/dri/card*`
(PCI vendor/device et nom de pilote par libdrm, nom lisible extrait de la chaîne
vendeur VA-API — « AMD Radeon Graphics (gfx1103_r1) »), les écrans depuis KMS
(connecteurs branchés, mode exact, le primaire = celui à l'origine du bureau), les
encodeurs depuis VA-API sur le render node. **Seul H.264 est annoncé** tant que
`VaapiEncoder` n'a pas de chemin HEVC/AV1 — la leçon B7 : une capacité que
l'encodeur n'honore pas est une session morte à l'init ; le silicium HEVC et AV1
est dit dans le log seulement. `hasInteractiveSession()` n'a plus le sens Windows
(« ce processus atteint-il un bureau ? ») mais « y a-t-il un CRTC allumé ? » —
KMS capture qui que ce soit et sans serveur d'affichage, c'est le point. La
capacité `cap_sys_admin` est vérifiée une fois dans la sonde pour que la carte
d'hôte dise pourquoi, plutôt qu'un échec au clic.

`LinuxSession` est le portage étage par étage de `WindowsSession` — même thread
unique sans file, même garde de cadence, même plancher écran fixe et rafale de
raffinement, mêmes trois couches de débit, même redémarrage sur écran perdu — avec
les différences propres à la plateforme marquées là où elles vivent : l'image que
le chemin pointeur-seul reconvertit est **le dernier tampon KMS tenu par son fd**,
pas une copie (`KmsCapture::acquire` ne ferme le tampon précédent qu'au moment
d'exporter le suivant) ; l'encodeur possède la surface ; pas d'invalidation de
référence ni d'intra-refresh sur radeonsi (une image perdue coûte une keyframe, et
`SessionInfo` le dit) ; clavier/souris uinput et manette uinput construits comme
`Win32Input`/`VigemGamepad` ; le son par PipeWire depuis le 05/09 au soir (§19.7) ;
pas de HDR.

**Le piège qui a coûté la première image** : la session a d'abord produit un flux
de 1,5 Ko pour 13 images — du noir pur, luma 0, pas 16. Le contexte EGL est rendu
courant par `init()` sur le thread qui construit la session, et `convert()` tourne
sur le thread de capture : sans contexte courant **tout appel GL est un no-op
silencieux**, `glGetError()` compris, et l'encodeur lit une surface que rien n'a
écrite. Le test pipeline, sur un seul thread, ne pouvait pas le voir. Le
convertisseur lie maintenant le contexte au thread qui l'appelle
(`makeCurrent`), le relâche à la fin de `init()`/`bindTarget()` et le thread de
capture le rend avant de finir (`detachThread`), sinon `stop()` sur l'autre thread
se heurte à `EGL_BAD_ACCESS`.

Vérifié (`test_linux_session`, une session par `NativeHost::probe()` →
`createSession()` → 2,7 s → fichier) : 13 images, 2 keyframes (la première et
celle demandée), ordre des numéros tenu, latence hôte au pire 8,1 ms, 560 Ko, et
l'image décodée décrite en 19.4. 1952 vérifications vertes sous Linux, 2005 sous
Windows (les deux tests Linux s'y déclarent sautés). ffmpeg n'intervient que sur le
banc, pour regarder le flux : le paquet n'en dépend pas.

### 19.6 Ce qui reste

HEVC et AV1 VA-API, le portail PipeWire en repli — et avec lui l'**AppImage**, qui
ne peut porter aucune capacité (§19.8) et n'aura de capture que par le portail —,
et le premier flux vers un vrai navigateur depuis un hôte Linux, image et son.
~~Le paquet~~ : traité en §19.8 le 05/09 au soir (constaté le même jour : le job
Linux de `release.yml` n'installait aucune des `-dev`, le `.deb` et le `.rpm`
publiés embarquaient le stub).

### 19.7 Le son : le moniteur de la sortie par défaut, par PipeWire (05/09/2026)

Le choix de la bibliothèque est une décision de licence avant d'être une décision
technique : **libpulse est LGPL et hors de la liste blanche** de `LICENSE.md`,
**libpipewire est MIT et dedans**. Ça tombe bien : PipeWire est le serveur audio
de tous les bureaux actuels (Fedora depuis 34, Ubuntu depuis 22.10, Debian depuis
12, Arch, SteamOS, Bazzite), et les applications PulseAudio y tournent par
`pipewire-pulse`. L'inverse n'est pas vrai, et c'est **la limite à connaître** : sur
une machine où PulseAudio tient encore la carte son — Ubuntu 22.04, notre banc
même —, le démon PipeWire tourne (pour les portails) mais son graphe n'a **aucune
sortie** ; le flux de capture est refusé (« no node available »), la session
streame en silence et le log dit en clair qu'il faut `pipewire-pulse`. Le tap
réessaie toutes les deux secondes : une sortie peut apparaître (HDMI branché,
serveur basculé).

La source (`src/audio/linux/PipeWireCapture`) est un `pw_stream` en capture avec
`stream.capture.sink = true` — **le moniteur d'une sortie, jamais un micro** — et
sans cible nommée, pour que le gestionnaire de session l'accroche à la sortie par
défaut et **la déplace** quand l'utilisateur change de sortie en cours de session
(le « suit le périphérique par défaut » de WASAPI, fait par le serveur). On demande
du float32 entrelacé, 48 kHz, deux canaux : l'adaptateur de PipeWire convertit
depuis ce que la sortie fait tourner, donc une sortie 44,1 kHz ou 5.1 coûte un
resampler dans le graphe et rien chez nous. Les tampons sont lus contre le format
**négocié**, jamais contre le format demandé ; un mono ou un 5.1 qui passerait
quand même est ramené en stéréo par `interleavedToStereo` (`AudioInterleave.h`,
mêmes trois décisions que le planaire, testé partout). PipeWire appelle sur son
propre thread, quand le graphe tourne : c'est une API *push* comme le tap de
ScreenCaptureKit, donc la cadence vit dans `PacedOpusSink` (§20.8), et libopus se
construit maintenant sous Linux aussi.

**Mesuré sur l'bench-mini** (Ubuntu 22.04 basculé sur `pipewire-pulse` 0.3.48 pour
l'occasion, sink nul `mw_null` en sortie par défaut, une sinusoïde 440 Hz à −12 dBFS
en boucle dedans ; `test_linux_session`, 2,8 s) : format négocié **F32 entrelacé,
2 canaux, 48 000 Hz** ; **128 échantillons par tampon** (2,7 ms — le graphe a donné
moins que les 240 demandés) ; **135 168 échantillons en 1 056 tampons**, soit
48 kHz à l'échantillon près sur la durée ; **575 paquets, 0 jeté, 13 trames de
silence** — les 65 ms entre le départ du pacer et l'état *streaming* du flux, pas
une perte en régime ; et le signal **décodé en retour par libopus dans le test** :
crête **0,261**, RMS **0,175** pour une sinusoïde d'amplitude 0,25 (79 octets par
paquet ; le même test sans tonalité lit crête 0,000 et 3 octets par paquet).
Avant `pipewire-pulse`, sur le même banc : cadence parfaite, 3 octets par paquet,
crête 0,000 — exactement le symptôme de §20.8, cette fois pour une vraie raison
(pas de sortie dans le graphe).

Deux pièges de banc, retenus dans `mac-m1-bench`/la mémoire Linux : `pgrep -f` et
`pkill -f` attrapent **le shell SSH lui-même** dont la ligne de commande contient le
motif (la première tentative a tué son propre script avant de lancer la tonalité) ;
et libopus construit depuis un tarball sans `.git` s'annonce « libopus unknown » —
la version vient de `git describe`, rien à corriger côté module.

Le test de session décode désormais les paquets avec le même libopus et imprime la
crête : c'est la seule preuve, ce côté-ci d'un navigateur, qu'un tap capture du son
et pas un silence parfaitement cadencé. Au passage, **les tests de session étaient
muets depuis le 04/09** : `test_capabilities` remettait le puits de log à `nullptr`
pour prouver qu'il est optionnel et ne le restaurait pas — corrigé
(`installTestLogSink()`), les lignes « [native] » des sessions Linux et macOS
réapparaissent dans la sortie de `mw-native-tests`.

Reste : le premier flux Linux vers un vrai navigateur (image **et** son), le
`node.latency` que le graphe n'honore pas forcément (la file du pacer, 40 ms,
absorbe un quantum par défaut de 21 ms), et le paquet (§19.8).

### 19.8 Le paquet : la capacité, le lanceur, et ce que linuxdeploy ne doit pas embarquer (05/09/2026)

Le constat du matin : le job Linux de `release.yml` n'installait aucune des
bibliothèques de développement du backend — le CMake du module avertit et
construit le stub, et c'est le stub que chaque `.deb`, `.rpm` et AppImage publié
portait. Corrigé en ajoutant `libdrm-dev libva-dev libegl1-mesa-dev
libgles2-mesa-dev libgbm-dev libpipewire-0.3-dev` (tous présents sur l'image
`ubuntu-22.04` du job, vérifié sur le banc qui est la même distribution), et
surtout en rendant la régression **impossible en silence** : le job lit la sortie
de configuration et refuse de paquetiser si « Linux graphics backend ON » ou
« Linux audio (PipeWire » n'y sont pas, puis vérifie par `readelf` que le binaire
assemblé a bien `libdrm`, `libva`, `libEGL`, `libgbm` et `libpipewire-0.3` en
`NEEDED` — le stub n'en lie aucun. ⚠️ **Corrigé le 05/09 au soir, en rejouant le job
sur le banc** : la première version de la garde exigeait aussi `libGLESv2.so.2`, et
le binaire ne l'a **pas** — le module passe bien `-lGLESv2` et 33 symboles `gl*` sont
référencés, mais Qt6Gui tire `libOpenGL.so.0` (glvnd) plus tôt sur la ligne de lien,
qui exporte les mêmes points d'entrée GLES, et `--as-needed` écarte libGLESv2 comme
redondante. La répartition vers le GLES du pilote passe par glvnd dans les deux cas ;
la garde ne nomme plus libGLESv2, la dépendance `libgles2` du paquet reste (inoffensive,
et c'est ce que le module demande).

**Le piège, mesuré avant d'écrire une ligne.** §19.3 supposait « `setcap` sur le
binaire, comme Sunshine ». Un binaire qui *gagne* une capacité à l'exec est lancé
par glibc en **mode sécurisé** (`AT_SECURE`), et dans ce mode `$ORIGIN` n'est
développé que si le binaire vit dans un répertoire système (`/usr/lib`…). Notre
`MoonlightWeb` vit sous `/opt/moonlightweb/bin` et trouve son Qt embarqué par
`RUNPATH=$ORIGIN/../lib`, réécrit par linuxdeploy. Reproduit sur l'bench-mini avec
un programme de trois lignes et sa bibliothèque à côté :

```
$ ./bin/app                          # answer 42
$ sudo setcap cap_sys_admin+p bin/app
$ ./bin/app
./bin/app: error while loading shared libraries: libanswer.so: cannot open shared object file
```

Le paquet aurait installé une application qui ne démarre plus. Sunshine ne le
rencontre pas : son binaire est dans `/usr/bin` et lie les bibliothèques du système.
Le même mode sécurisé ignore aussi `LD_LIBRARY_PATH`, donc aucun contournement par
l'environnement.

**Le lanceur.** `backend/packaging/linux/moonlightweb-launch.c`, 16 Ko compilés,
lié à la seule libc (qui vit, elle, dans un répertoire de confiance). C'est *lui*
qui porte `cap_sys_admin+p` — **permitted seulement**, la posture exacte de
Sunshine (`getcap /usr/bin/sunshine` sur le banc : `cap_sys_admin,cap_sys_nice=p`).
Il met la capacité dans son ensemble *inheritable* (`capset`, permis puisqu'il la
tient permitted), la lève dans l'ensemble **ambiant** (`prctl(PR_CAP_AMBIENT,
RAISE)`), et `execv` le `MoonlightWeb` **à côté de lui** (résolu par
`/proc/self/exe`, jamais par `PATH` : une capacité ne doit pas suivre un nom dans
un répertoire que quelqu'un d'autre écrit). Un exec qui ne gagne rien que son
parent n'avait déjà n'est pas un exec sécurisé : l'application démarre
normalement, rpath compris, avec `CAP_SYS_ADMIN` permitted, effective et ambiant.
Sans capacité sur le fichier (arbre construit à la main, AppImage), la levée échoue
et le lanceur exec simplement — la sonde dit ensuite que la capture est
indisponible et pourquoi. Mesuré sur le banc : avec la capacité sur le lanceur,
l'application charge sa bibliothèque `$ORIGIN` **et** lit
`CapPrm/CapEff/CapAmb = 0x200000` (bit 21) ; par un lien symbolique aussi (ce que
`/usr/bin/moonlightweb` devient) ; sans capacité, tout à zéro et `answer 42`.

**Ce que l'application en fait** (`common/LinuxCapabilities.cpp`, première ligne
de `main()`, quand le thread principal est encore seul — les capacités sont *par
thread* et les threads héritent de celui qui les crée) : elle **garde** permitted
et inheritable, **retire** effective, et **abaisse** l'ambiant — `LOWER` sur
`CAP_SYS_ADMIN` seulement, pas `CLEAR_ALL`, parce qu'une unité systemd peut avoir
donné `CAP_NET_BIND_SERVICE` par le même mécanisme et celle-là doit rester. Le
serveur HTTP, la signalisation, les relais tournent donc **sans** la capacité
effective, et **rien de ce que le processus lance** ne l'hérite — `xdg-open` et le
navigateur derrière, `gio`, un installeur — sauf un seul enfant : le worker natif,
auquel `StreamWorkerHost` la rend par `QProcess::setChildProcessModifier` (dans
l'enfant forké, avant l'exec, en appels bruts). Le worker, même binaire, même
`main()`, se confine à son tour ; et `KmsCapture` lève la capacité dans l'ensemble
effectif **de son thread** le temps d'un `GETFB2` — `ScopedSysAdmin`, quatre
sites, `capget`/`capset` bruts, pas de libcap (rien à lier, et le `capset` de
libcap synchronise tous les threads, l'inverse du but). La séquence a été rejouée
en C sous le vrai lanceur avant d'être écrite en C++ :

| Étape | CapPrm | CapEff | CapAmb |
|---|---|---|---|
| à l'entrée, tel que le lanceur le donne | ✔ | ✔ | ✔ |
| après le confinement | ✔ | — | — |
| enfant A, `fork`+`exec` ordinaire (= `xdg-open`) | — | — | — |
| enfant B, levée ambiante puis `exec` (= le worker) | ✔ | ✔ | ✔ |
| un thread pendant `ScopedSysAdmin` | ✔ | ✔ | — |
| le thread principal au même instant, et le thread après | ✔ | — | — |

Et la preuve sur le module lui-même : `mw-native-tests` avec `cap_sys_admin+p`
**seulement** (le banc posait `+ep` jusqu'ici) — capture KMS, conversion, encodeur
et session complète, **2133/2133**.

**Ce qui doit rester au système.** linuxdeploy embarque tout ce qui n'est pas sur
sa liste d'exclusion, et une bibliothèque de pilote embarquée est pire qu'absente :
`libva` charge `radeonsi_drv_video.so` et `libpipewire` ses modules de protocole
depuis des chemins **compilés dans la bibliothèque** — une copie faite sur Ubuntu
cherche sous `/usr/lib/x86_64-linux-gnu` et ne trouve rien sur le `/usr/lib64` de
Fedora. La liste de pkg2appimage (lue le 05/09) contient `libdrm.so.2`,
`libEGL.so.1`, `libgbm.so.1`, `libpipewire-0.3.so.0` mais **pas** `libva.so.2`,
`libva-drm.so.2` ni `libGLESv2.so.2` : ces trois-là sont exclues explicitement, et
le job vérifie qu'aucune des sept n'a atterri dans `AppDir/usr/lib`, pour qu'un
changement de la liste amont ne puisse pas en embarquer une sans bruit. En face,
les paquets les **déclarent** : `libdrm2 libva2 libva-drm2 libgles2 libgbm1
libpipewire-0.3-0` (+ `libcap2-bin` pour `setcap`) côté `.deb`, les sonames côté
`.rpm`, `libdrm libva mesa pipewire libcap` côté AUR. `libpipewire` n'est que la
bibliothèque cliente : une machine encore sous PulseAudio l'a aussi, et l'hôte y
streame muet avec son log (§19.7) plutôt que de ne pas démarrer.

**Le reste du paquet** : `/usr/bin/moonlightweb`, l'entrée `.desktop`, l'unité
systemd et le `systemd-run` du postinst pointent le lanceur ; le postinst pose
`setcap cap_sys_admin+p` sur lui **à chaque installation et mise à jour** — ni
dpkg ni rpm ne restaurent une capacité de fichier depuis la charge utile —, avec
`/usr/sbin:/sbin` ajoutés au `PATH` du gestionnaire de paquets, et un avertissement
lisible sinon (Fedora a `setcap` dans `libcap`, toujours présent ; openSUSE dans
`libcap-progs`) ; l'AUR a son `.install` pour la même raison (pacman non plus). Le
lanceur est aussi dans l'AppImage, où il n'est qu'un exec : une AppImage est un
montage FUSE `nosuid`, ce qui désactive aussi les capacités de fichier — **pas de
capture depuis une AppImage**, et `install.sh` le dit au moment de l'installer
plutôt que de laisser chercher une carte d'hôte qui n'apparaît pas ; le portail
PipeWire sera sa route. L'image Docker n'est pas touchée : sans écran ni GPU, le
stub y est le bon backend.

**Vérifié le 05/09 au soir, en rejouant le job Linux étape par étape sur le banc**
(bench-mini, Ubuntu 22.04 — la distribution de l'image `ubuntu-22.04` du job —, Qt
6.6.3 local, fpm 1.18, linuxdeploy `continuous` extrait sans FUSE) : configure « ON »
× 2, build sans warning, ctest 3/3, AppDir avec `libqoffscreen.so`, aucune des sept
bibliothèques système embarquée (66 libs dans `usr/lib`, `RUNPATH=$ORIGIN/../lib`),
`.deb` et `.rpm` de 44 Mo avec les quinze dépendances attendues. Puis `dpkg -i`
**par-dessus le 0.2.1 déjà installé** — le vrai chemin de mise à jour : `prerm` a
arrêté l'ancien `--autostart`, `postinst` a posé `cap_sys_admin=p` sur le lanceur
(rien sur `MoonlightWeb`), `/usr/bin/moonlightweb` et l'entrée `.desktop` pointent
le lanceur, et `systemd-run --user` a relancé l'app dans la session GNOME Wayland de
l'utilisateur (Qt en `xcb` par XWayland, tray créé, page admin ouverte). Le
processus final s'appelle `/opt/moonlightweb/bin/MoonlightWeb` (argv0 = chemin,
parent = systemd utilisateur) et porte exactement l'état de la table ci-dessus :
`CapInh 200000 / CapPrm 200000 / CapEff 0 / CapAmb 0`. Le log dit « `[caps]
CAP_SYS_ADMIN held (permitted): the screen can be captured through KMS; dropped
from the effective set; withheld from child processes except the native worker »
puis « Native host available: bench-mini — MoonlightWeb Host », et
`/api/native/status` répond `available`, `DRM/KMS`, `HDMI-A-1 — 1920×1080 · 60 Hz`,
`VA-API`, `AMD Radeon Graphics (gfx1103_r1)`, codecs `H.264` — la sonde a donc bien
levé la capacité par thread dans le serveur, sans qu'elle soit effective ailleurs.
`ldd` : Qt depuis `/opt/moonlightweb/lib`, libva/libOpenGL depuis le système.

**Reste non vérifié** : le job GitHub lui-même (au premier `workflow_dispatch` de
`release.yml`, plateforme `linux`, après le push) — la seule différence avec le
banc est Qt 6.11 à la place de 6.6.3 ; et le **premier flux navigateur** depuis cet
hôte (§19.6), qui est la prochaine étape.

---

## 20. macOS : ScreenCaptureKit → VideoToolbox, sans étage de conversion (05/09/2026)

Le troisième backend, écrit sur bench-desk et construit sur le Mac M1 Pro de test
(macOS 15.6.1, SDK 15.5, Apple clang 17). Cinq fichiers Objective-C++ :
`SckCapture.mm`, `VtEncoder.mm`, `CgInput.mm`, `MacProbe.mm`, `MacSession.mm`, plus
la table clavier `MacKeyMap.h` (C++ pur, testée partout). Même couture plateforme
que Windows et Linux (`Probe.h`, `Session.h`), même boucle de session portée étage
par étage.

### 20.1 La chaîne, et pourquoi il n'y a pas de convertisseur

Sous Windows et Linux la capture livre le bureau tel qu'il est scanné (BGRA, ou un
DMA-BUF tuilé) et un shader le transforme en NV12 pour l'encodeur. ScreenCaptureKit
est la sortie du compositeur lui-même, et le compositeur sait écrire du NV12 : on
demande `'420v'` (4:2:0 bi-planaire, video range, matrice BT.709) à la résolution du
flux, et chaque image arrive comme un `CVPixelBuffer` sur IOSurface que VideoToolbox
lit en place — mise à l'échelle, conversion couleur et, si on le demande, pointeur
composé, déjà faits par WindowServer sur le GPU. La chaîne est donc **capture →
encodeur**, rien entre les deux ; la seule copie restante est le bitstream qui sort
de la VRAM (`copiesPerFrame = 1`, comme sur les deux autres OS).

SCK pousse ses images sur une file dispatch ; la boucle est écrite contre un
`acquire(timeout)` bloquant — c'est ainsi que DXGI et KMS répondent — donc la
dernière image est **parquée sous un mutex et remplacée** par la suivante si elle
n'a pas été prise : aucune file, la règle « dernière image, jamais d'arriéré ». Les
images `Idle` (rien n'a changé) sont comptées et ignorées ; `Complete` et `Started`
portent la sortie du compositeur. L'horodatage de présentation vient de l'attache
`SCStreamFrameInfoDisplayTime` (mach time), ramené sur l'horloge stable par « il y a
combien de temps ». La fréquence demandée à SCK est celle du panneau (120 Hz sur ce
M1 Pro, ProMotion) : la garde de cadence de la session voit chaque présent et décide
lesquels le flux porte, exactement comme sous Windows.

Pas de statut `PointerOnly` : avec le pointeur dans l'image, un mouvement de souris
**est** une nouvelle image (c'est ce que veut le mode composé) ; pointeur exclu, le
client dessine le sien, et la **forme** lui vient d'AppKit
(`NSCursor.currentSystemCursor`, rastérisé à l'échelle du panneau, haché ; nommé —
`default`, `text`, `pointer`, `ew-resize`… — quand son hachage est celui d'un
curseur standard appris une fois au démarrage). Le pointeur agrandi pour téléphone
(`cursorFramePx`) n'a pas de route ici : SCK dessine le pointeur à sa taille, et la
session le dit une fois dans le log.

### 20.2 VideoToolbox : ce qu'il a, ce qu'il n'a pas

Les mêmes décisions de latence que les quatre autres encodeurs : `RealTime`,
`AllowFrameReordering = false` (pas de B, une image en vol), `MaxKeyFrameInterval` et
sa durée poussés à « jamais » (les keyframes sont à la demande), `ExpectedFrameRate`,
`MaxFrameDelayCount = 0`, `PrioritizeEncodingSpeedOverQuality`, High/CABAC en H.264,
Main sans open-GOP en HEVC, matériel exigé
(`RequireHardwareAcceleratedVideoEncoder`). Le VBV est celui de `RateControl.h`
exprimé dans le vocabulaire de VideoToolbox : `DataRateLimits = [octets, secondes]`
avec une image de budget sur une image de temps. Le débit se change à chaud par
`AverageBitRate` + `DataRateLimits`, sans reconstruction. Chaque `encode()` **bloque**
jusqu'au bitstream (`CompleteFrames` puis attente du callback) : VideoToolbox est
asynchrone par construction, et l'attente de l'image que l'on vient de soumettre est
ce qui rend vraie la « une image en vol » de la boucle.

Ce qu'il n'a pas : ni intra-refresh, ni invalidation de référence, ni QP par image.
Une image perdue coûte une keyframe et `SessionInfo` le dit ; la rafale de
raffinement converge sur la taille et son plafond de passes seul (`RefineConvergence`
était déjà écrit pour un encodeur muet sur le QP — AMF fut le premier). Pas d'AV1 :
aucun encodeur Apple n'en produit. HEVC et H.264 seulement, dans cet ordre.

**AVCC → Annex B en place.** VideoToolbox sort des NAL préfixés de leur longueur, les
paramètres (SPS/PPS, VPS en HEVC) à part dans la description de format ; le
navigateur et tous les relais veulent de l'Annex B avec les paramètres devant chaque
keyframe. Un préfixe de longueur fait 4 octets, un start code aussi : une image
delta est **réécrite dans le tampon même de l'encodeur** et livrée sans copie ; une
keyframe — rare — est assemblée dans un tampon de travail, paramètres devant.

### 20.3 Les entrées : Quartz, et les deux choses que macOS laisse à l'émetteur

`CGEventPost` au HID tap, thread-safe et à la microseconde : la seule route. Deux
choses que Windows et Linux font pour nous et que macOS non :

1. **Les modificateurs sont des drapeaux, pas des touches.** Un appui sur Shift se
   poste en `kCGEventFlagsChanged` avec le nouvel état, et chaque événement clavier
   ou souris qui suit doit porter les modificateurs tenus dans son propre champ —
   WindowServer ne s'en souvient pas pour nous. `CgInput` tient le masque et
   l'estampille sur tout ce qu'il poste.
2. **Le double-clic se déclare, il ne se détecte pas.** Un second appui dans
   l'intervalle doit dire `clickState = 2`, sinon ce sont deux clics simples ; un
   déplacement bouton enfoncé est un `…Dragged`, pas un `MouseMoved`.

La table clavier `MacKeyMap.h` est écrite en chiffres (codes ADB, inchangés depuis le
premier Macintosh) et non en `kVK_*` pour être testée sur toute machine ; même
raisonnement position → position que les deux autres tables, la disposition de
l'hôte étant appliquée par l'OS. Win → Command, Alt → Option, Ctrl → Control ;
Impr. écran / Arrêt défil. / Pause vont là où un clavier Apple met F13–F15 ; la
touche menu et les touches média n'ont pas de place et sont écartées plutôt que
devinées. Caps Lock est un **état**, réglé par IOKit (`IOHIDSetModifierLockState`),
pas une frappe. Pas de manette : tranché le 02/09 (extension DriverKit signée,
entitlement Apple), `probeVirtualGamepad` répond `supported = false`.

### 20.4 La sonde : trois pièges, tous rencontrés le premier jour

- **La session graphique.** Un binaire lancé par SSH est dans une autre session
  d'audit : `CGSessionCopyCurrentDictionary` répond quand même « sur la console »
  (il parle de l'*utilisateur*), puis `CGGetActiveDisplayList` ne trouve aucun écran
  et la sonde aurait dit « Mac sans écran ». `hasInteractiveSession()` pose donc une
  seconde question, celle du *processus* : `SessionGetInfo` et son bit
  `sessionHasGraphicAccess`. Sous SSH la réponse est maintenant « pas de session
  interactive » ; les tests se lancent par `launchctl bootstrap gui/<uid>`, qui est
  la session de l'agent `com.moonlightweb.agent`.
- **L'écran endormi.** Un panneau en veille sort de la liste *active* : le Mac laissé
  dix minutes disparaissait de la liste des hôtes (« no active display », capot
  ouvert, écran noir). La sonde énumère la liste *online* (branché et utilisable) et
  note « (asleep) » dans le détail ; la session **réveille** le panneau au départ
  (`IOPMAssertionDeclareUserActivity`) et tient une assertion
  `PreventUserIdleDisplaySleep` tant qu'elle tourne — quelqu'un qui streame ce Mac
  l'utilise, quoi qu'en pense son minuteur.
- **La permission.** Screen Recording (TCC) est la seule chose que le programme ne
  peut pas s'accorder : la sonde répond `CapturePermission`, demande une fois par
  processus l'invite système (`CGRequestScreenCaptureAccess`), et dit dans quel
  panneau des Réglages Système est l'interrupteur — l'octroi ne vaut que pour les
  processus lancés *après*. Les entrées ont le même mur, Accessibility, vérifié dans
  `CgInput::start()` (`CGPreflightPostEventAccess`) avec la même phrase.

### 20.5 Ce que le banc a appris avant même la première image

- **TCC accroche l'octroi au *designated requirement* de la signature.** Une
  signature ad hoc (ce que fait l'éditeur de liens sur Apple Silicon, et ce que fait
  `codesign -s -` dans `release.yml`) a pour exigence le hachage du binaire lui-même :
  **chaque rebuild reperd l'autorisation et redemande**. Le banc signe tests et app
  avec une identité auto-signée stable (« MoonlightWeb Dev », trousseau dédié) dont
  l'exigence est `identifier … and certificate leaf = H"…"` ; un octroi, tous les
  builds. ⚠️ **Conséquence produit** : tant que le `.pkg` est signé ad hoc, chaque
  mise à jour de MoonlightWeb sur un Mac redemandera Screen Recording et
  Accessibility. Sunshine a le même problème sans Developer ID. À arbitrer avant la
  release macOS du host natif.
- **`FrameSender(Options options = {})` ne compile pas chez Apple clang** (« default
  member initializer needed within definition of enclosing class ») alors que MSVC
  et GCC l'acceptent : `Options` porte des initialiseurs de membre et le défaut est
  analysé avant qu'ils soient complets. Le seul appelant passe ses options ; le
  défaut est retiré. C'était une casse latente du job macOS de la CI depuis C4.
- **Le Mac de test n'a ni Homebrew utilisable ni sudo** : CMake et Ninja depuis leurs
  archives officielles sous `~/tools`, OpenSSL statique compilé sur place, Qt 6.10.3
  par `aqt` sous `~/Qt`, et l'app livrée sous `~/Applications` avec le LaunchAgent
  repointé, `/Applications/MoonlightWeb.app` appartenant à root.
- `.clang-format` n'avait pas de section Objective-C : les `.mm` n'étaient jamais
  formatés (« configuration does not support Objective-C »). Ajoutée, même style.

### 20.6 Le premier flux (05/09/2026)

Chrome 152 sur bench-desk → rendez-vous (`stream.moonlightweb.top/<id>`, ICE en LAN
`10.0.0.34:48010`) → l'app complète construite sur le Mac (Qt 6.10.3, OpenSSL
statique, signée avec l'identité de banc). Session `Built-in Retina Display
2560x1440@60 HEVC via VideoToolbox on Apple M1 Pro`, VBV 666 kbit, décodeur
Chrome `hev1.1.176.L153.B0` matériel, première image décodée en NV12, écran de
verrouillage du Mac **droit et aux bonnes couleurs** dans le navigateur. Cadence
lue : « 120 Hz display, 60 fps stream — 83 presents in 9.1 s, 1 not carried » ;
SCK : 84 images, 459 idle, 1 remplacée avant d'être prise ; première keyframe
123 Ko ; le gouverneur de lien a coupé une fois (« delay rising », 25,6 Mbit/s) puis
remonté par pas de 5 %.

⚠️ **Ce qui a été faux d'abord** : la première session a livré dix secondes de noir
(keyframes de 1,5 Ko), puis SCK a arrêté le flux (« Failed to find any displays »,
-3815) et la session a bouclé 45 tentatives jusqu'à ce qu'un `caffeinate -u`
externe rallume le panneau. Cause : `IOPMAssertionDeclareUserActivity` **relâchée
aussitôt déclarée** ne réveille rien. L'assertion est maintenant gardée pour la
session et redéclarée à chaque tentative de redémarrage ; rejoué avec l'écran
endormi par `pmset displaysleepnow` : image dès la première seconde, écran
rallumé, 123 Ko de première keyframe.

Autres constats de ce flux : `MaxFrameDelayCount = 0` est refusé par l'encodeur
matériel (-12900, en debug, sans conséquence visible) ; la sortie SCK suit le format
demandé par le client (2560×1440 puis 2218×1440 quand le front a réaligné le
rapport d'aspect sur l'écran 3600×2338).

### 20.7 Les entrées, validées — et le piège TCC qui les bloquait (05/09/2026)

Session de 213 s depuis Chrome : **10 233 présents pour un flux à 60 fps sur un
écran 120 Hz, 9 non portés** par la garde de cadence ; **12 768 images livrées par
SCK, 0 « idle », 2 534 remplacées avant d'être prises** — la règle « dernière image,
jamais d'arriéré » à l'œuvre ; 48 événements d'entrée injectés ; première keyframe
114 Ko ; fin propre.

Souris et clavier vérifiés **à l'œil, pas seulement dans le log** : un clic a mis les
Réglages Système au premier plan puis a actionné des boutons de dialogue, et
« clavier ok » s'est écrit dans leur champ de recherche. `CgInput` est donc juste de
bout en bout : position absolue mise à l'échelle du panneau, modificateurs portés
par chaque événement, texte par `CGEventKeyboardSetUnicodeString`.

⚠️ **Le piège, et il coûtera cher à un utilisateur** : TCC n'accroche pas une
autorisation à un identifiant de bundle mais à une **exigence de code** (le
*designated requirement*). Sur ce Mac, l'entrée « Accessibilité » de MoonlightWeb
avait été créée par l'ancienne application de `/Applications`, signée ad hoc, donc
enregistrée comme `cdhash H"9d8aba5f…"`. La nouvelle build, signée par certificat
(`identifier "com.moonlightweb.server" and certificate leaf = H"d88095f4…"`), ne
correspond pas : l'interrupteur était bien coché dans les Réglages, et macOS jetait
quand même tous les événements — silencieusement, ce qui donne un flux qui s'affiche
et ne répond pas. `CGPreflightPostEventAccess()` est ce qui le détecte, et
`kTCCServicePostEvent` est la ligne à regarder dans la base.

La réparation ne peut pas se faire à la main dans la base : **SIP la rend illisible
en écriture même pour root** (« attempt to write a readonly database »). La séquence
qui marche est `tccutil reset Accessibility com.moonlightweb.server`, puis laisser
l'application redemander (elle réapparaît dans la liste avec la bonne exigence),
puis cocher. Les trois lignes portent ensuite la même exigence que le binaire qui
tourne.

Conséquence produit, à trancher avant la release macOS : tant que le `.pkg` est
signé ad hoc, **chaque mise à jour change le cdhash et reperd Screen Recording et
Accessibility**, sans le dire. Un Developer ID, ou n'importe quelle identité stable,
supprime le problème d'un coup. C'est le même constat qu'en §20.5, mais mesuré cette
fois sur ses conséquences réelles.

Enfin, un fait de banc qui n'est pas de notre code : **Sunshine ne finit pas son
démarrage quand l'écran du Mac est en veille** — il s'arrête après le test des
encodeurs et n'ouvre aucun port. La veille écran est désormais désactivée sur cette
machine (`pmset -a displaysleep 0 sleep 0`).

### 20.8 Le son : le tap de ScreenCaptureKit, mis en cadence (05/09/2026)

macOS n'a pas de périphérique de boucle à ouvrir. Le seul moyen supporté
d'enregistrer ce que le Mac joue est le **tap audio de ScreenCaptureKit** (macOS
13+), qui n'est pas une capture à part mais **une seconde sortie du même flux** :
`capturesAudio = YES`, `sampleRate = 48000`, `channelCount = 2` sur la
configuration, et un `SCStreamOutputTypeAudio` ajouté sur une file qui lui est
propre (une image garée ne doit jamais retarder un paquet). C'est pourquoi
`setAudioSink()` vit sur la capture d'écran : un flux, une autorisation — celle
de « Screen & System Audio Recording », pas de micro —, et le son qui s'arrête et
repart avec l'image au lieu de dériver quand l'écran s'en va.

**La cadence, elle, n'a pas de plateforme.** Le relais avance l'horloge RTP d'une
trame par paquet : un paquet manquant n'est pas un paquet en retard, c'est une
horloge fausse. Windows tient ce contrat dans le fil WASAPI lui-même — le
périphérique signale, le même thread encode. Les API *push* (SCK ici, PipeWire
plus tard) appellent quand elles veulent, sur leur file : le tic doit vivre
ailleurs. C'est `PacedOpusSink` (`src/audio/`), neutre de plateforme : `push()`
depuis la capture, un thread à nous qui sort une trame Opus toutes les 5 ms,
silence compris.

Deux pièges, tous deux mesurés sur le banc :

- **`CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer` refuse une
  structure de taille fixe.** Une `AudioBufferList` dimensionnée pour huit
  canaux — largement de quoi tenir le stéréo — reçoit
  `kCMSampleBufferError_ArrayTooSmall` (-12737) : la taille que CoreMedia veut
  couvre plus que les tampons eux-mêmes. Il faut la **forme en deux appels** (le
  premier demande la taille). Le symptôme, sinon, est propre et trompeur : la
  chaîne entière tourne, 200 paquets/s exactement, 0 perdu, **3 octets par
  paquet** — du silence numérique parfaitement cadencé.
- **Core Audio livre du planaire** : 2 tampons d'1 canal, 960 échantillons (20 ms)
  à la fois. D'où `AudioInterleave.h`, où sont écrites les trois décisions qui
  comptent (mono dupliqué dans les deux oreilles, pas panoramiqué à gauche ; au-delà
  du stéréo on garde les deux frontaux ; un plan absent est du silence, pas une
  lecture par un pointeur nul) — arithmétique pure, donc testée partout.
- **La file du pacer était calibrée pour WASAPI.** Quatre trames = 20 ms, soit
  exactement une rafale SCK : chaque rafale remplissait la file à ras bord et la
  moindre gigue la débordait. Mesuré : **1 170 images capturées jetées ET 1 214
  trames envoyées en silence dans la même minute**, la file pleine et vide tour à
  tour, dix pour cent du flux dans chaque sens. Huit trames (40 ms) → **121
  jetées et 165 silences sur 16 536 paquets**, soit un dixième de ce qu'elle
  perdait. Le plafond ne borne que le pire cas : en régime établi la file se vide
  à chaque rafale, elle n'ajoute pas de latence.

Vérifié de bout en bout depuis Chrome sous Windows, un son bouclé sur le Mac :
**16 536 paquets en 82,6 s — 200,0/s exactement**, 3 958 080 échantillons captés
(61,8 s à 48 kHz sur la session précédente, sans un trou), et le signal **mesuré à
la sortie du décodeur du navigateur** : crête 0,235, RMS moyen 0,011 sur 413
blocs. Le son du Mac est audible dans le navigateur.

Reste à améliorer : le pour-cent de trames encore jeté ou envoyé en silence — de
la gigue d'ordonnancement, pas une erreur de débit (les deux horloges tiennent
48 kHz).

### 20.9 Ce qui reste

Le HDR (P010 en entrée, Main10 en sortie — le silicium le fait), le pointeur
agrandi, et la signature du `.pkg` (§20.5, §20.7).

Hors de ce module, vu au passage et **corrigé depuis** : Internet Access se
désactivait entièrement quand l'enregistrement PowerDNS échouait, rendez-vous
compris, alors que le rendez-vous n'a aucun besoin du sous-domaine. La
rétro-compatibilité DNS côté client a été retirée le 05/09/2026 — plus aucune
installation n'écrit dans PowerDNS ni ne lance ACME, et ce chemin de coupure
n'existe plus.
