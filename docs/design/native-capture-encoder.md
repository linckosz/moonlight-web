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
fait vraiment (`SessionInfo::referenceInvalidation`, faux sur oneVPL) ; sur un
trou de numérotation le client envoie `invalidateref {from, to}` (ids de fil) et
**continue de décoder** au lieu de jeter les deltas jusqu'à la keyframe ; le
relais DC traduit les ids de fil en numéros de moteur — un anneau des 512
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
images/s et 5,2 ms pendant l'exercice (NVENC).

### 9.10.1 AMF : la même réparation par références long terme (06/09/2026)

⚠️ **plan corrigé** : le plan disait « pas d'équivalent AMF (AMF n'a pas
d'appel) ». AMF n'a en effet pas le `NvEncInvalidateRefFrames` de NVIDIA, mais
il a les **références long terme** (LTR) — de quoi faire la même réparation par
l'autre bout. Au lieu de *retirer* l'image perdue, on *nomme une survivante* :
les images sont marquées dans des slots LTR au fil de l'encodage
(`MarkCurrentWithLTRIndex`), et quand le récepteur nomme une perte, l'image
suivante est forcée à ne prédire que du slot le plus récent *antérieur* à la
perte (`ForceLTRReferenceBitfield`) ; en mode `RESET_UNUSED`, le pilote lâche
les slots non nommés — précisément ceux qui portaient les images gâtées. Le flux
est propre à partir de cette image, sans rien de plus gros qu'un delta.

`ReferenceSlots` (`encode/ReferenceSlots.h`, pur, 37 checks) tient
l'arithmétique : quel slot marquer, quel slot est propre avant une perte,
lesquels oublier. **Portée** : 4 slots marqués à chaque image ne reculent que de
3 images — 18 ms à 165 fps, moins qu'un aller-retour Internet. Donc on marque
tous les `stride` images, le pas choisi pour que les slots couvrent ≥ 125 ms
quelle que soit la cadence (60 fps → pas 2 → 8 images ; 165 → pas 6 → 24). Le
prix est que la référence forcée peut être de `stride` images plus vieille que
la dernière propre — un delta un peu plus gros, une fois, au lieu d'une keyframe.
Ce que la table enregistre est ce que le **buffer de sortie confirme** avoir été
marqué, jamais ce qui a été demandé : un pilote qui ignore le marquage
(`MarkedLTRIndex` absent) ou la référence forcée (`ReferencedLTRIndexBitfield`
sans le bit) dégrade en keyframe et le dit, il ne fabrique pas une table fausse.

⚠️ **Deux corrections du 06/09, trouvées en observant la réparation en vrai.**

**Le sentinelle `-1` de `MarkedLTRIndex` arrive en 32 bits.** La documentation
dit « default = -1 » ; le pilote le range dans 32 bits et la propriété rend
**4294967295**. Le test `markedIdx >= 0` lisait donc « je n'ai pas marqué »
comme le slot quatre milliards, appelait `marked()` avec un cast qui retombe sur
−1, et la table gardait **un trou là où elle croyait avoir une référence**. Le
trou est invisible jusqu'à une perte, où la réparation nomme un slot que le
pilote n'a jamais rempli. Tout ce qui sort des slots accordés est un refus,
quelle que soit sa forme binaire — et le refus est dit avec la valeur reçue.

**Une référence se juge sur l'image, pas sur l'index.** Le pilote référence
souvent un autre slot que celui demandé, et il a le droit : ce qui rend une
référence propre est **l'image qu'elle porte**, pas son numéro. `allBefore()`
répond « toutes les images nommées précèdent-elles la perte ? » ; `describe()`
nomme les images derrière un bitfield, pour que le log soit lisible. Quand la
réponse est oui, c'est une réparation, même si ce n'est pas le slot demandé.
Quand c'est non — une image *postérieure* à la perte, ou aucune référence long
terme — le delta prédit de ce que le récepteur n'a pas, et **l'image suivante
est forcée en keyframe** : c'est ce que le design promettait et que le code ne
faisait pas (il se contentait d'un avertissement, une fois).

Vérifié sur la RX 7600 réelle le 06/09 (les trois codecs) : « AMF ready : … 4
LTR slots every N frames with reference invalidation (reach M frames) », et
`dpb=1` (le « avant » du banc) éteint proprement les slots (« no reference
invalidation »). Coût mesuré nul (bench §8c, point 5).

✅ **Réparation observée en vrai le 06/09** (l'angle mort du banc est levé : le
Chrome piloté décode ce flux depuis le correctif des paramètres AMF, §9.10.3, et
`mw_drop_test` s'arme donc). Flux HEVC AMF, un delta jeté toutes les 60 images,
**12 pertes** : le client les nomme et continue de décoder — *zéro* « Requesting
IDR », *zéro* erreur de décodeur, image nette de bout en bout. Sur l'hôte,
**une perte sur deux est réparée par un delta** :

```
AMF healed frame 71 with a delta from long-term slot 2 = frame 68
                                  (driver referenced slot 1 = frame 66)
```

et l'autre moitié dégrade en keyframe, avec sa raison :

```
AMF ignored the forced long-term reference (asked slot 3 = frame 126,
    referenced slot 0 = never marked, slot 1 = frame 130) for a loss at 129
```

✅ **L'alternance avait une cause, traitée le 06/09 : le pilote se réserve
l'index long terme 0.** Première hypothèse — « il ne marque pas la keyframe » —
**fausse** : en laissant parler l'avertissement cinq fois, les refus tombent sur
les frames 0, 8, 16, 24, 32 — **toutes celles qui demandaient l'index 0**,
keyframes comme deltas. Le pilote répond −1 à toute demande de marquage en 0, et
référence cet index de lui-même quand une image est forcée ailleurs (c'est le
« slot 0 = never marked » de chaque dégradation). La table ne lui parle donc
plus qu'en indices **1..N** (`kLtrReservedIndices`) : slot *s* de la table =
index *s* + 1 pour le pilote, un index de plus demandé à l'init pour garder
quatre places utiles (5 accordés → 4 slots), bitfield rapporté décalé d'un cran
avant jugement — le bit 0 du pilote tombe, et s'il a référencé cela seul, le
bitfield vide vaut « rien à garantir », donc keyframe plutôt qu'un pari sur ce
qu'il garde là. Rejoué : **7 pertes, 7 réparations par delta**, aucun refus de
marquage, le slot 0 de la table (index 1) porte enfin des images (« driver
referenced slot 0 = frame 128 »), zéro IDR, zéro erreur de décodeur.

### 9.10.2 L'éviction du FrameSender nomme enfin l'image jetée (06/09/2026)

Le §9.10 laissait un reste : quand l'émetteur (`FrameSender`) évince un delta de
sa file parce que le lien est plein, il « ne dit pas lequel il a jeté » et le
relais demandait donc une keyframe (ou, en intra-refresh, laissait le client
nommer le trou un aller-retour plus tard). L'émetteur **est** pourtant la seule
partie qui connaît le numéro de l'image jetée avec certitude. `enqueue` /
`enqueueFragments` prennent maintenant un `std::vector<uint32_t>* evicted`
optionnel, rempli du `frameNumber` de chaque delta écarté (dans les deux chemins
d'éviction : le plafond dur et la profondeur 1 du natif). Le relais, pour un
moteur qui répare par invalidation (`referenceInvalidation()` vrai), les passe
aussitôt à `invalidateReference` — un aller-retour **avant** que le récepteur ne
voie le trou et le nomme lui-même ; la session traduit un refus en keyframe
comme toujours. Pour tout autre moteur, le comportement d'avant est intact
(demande de keyframe sauf ride-out), garanti par le drapeau `nameEvictions`.
Ceci vaut pour NVENC comme pour AMF depuis que ce dernier a l'invalidation.

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
| Souris absolue sous Linux | Même chemin, en deux temps explicites : le device uinput rapporte une fraction de son axe que le compositeur étale sur tout le bureau, donc l'origine du display **et** les bornes du bureau entrent dans le calcul (§23.3) |
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

### Intel (oneVPL) — ⚠️ **exécuté pour la première fois le 07/09/2026**

> ⚠️ Cette section disait « écrit, jamais exécuté ». Elle est corrigée : le
> chemin Intel a streamé pour de vrai sur le banc `bench-intel` (N95 / UHD
> Graphics, pilote 32.0.101.7088), et il n'en est **rien sorti d'intact** — cinq
> défauts, dont trois fatals, décrits au §21. Ce qui suit reste vrai de la
> conception ; le tableau des choix a été corrigé là où le matériel a tranché.

Ce qui était vérifié **avant** tout matériel, et l'est resté :

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
| Choix du GPU | par notre device D3D11, comme AMF, plutôt qu'en appariant à la main les énumérations Intel et DXGI. ⚠️ **mais pas par `MFXVideoCORE_SetHandle`** : sur le dispatcher 2.x il faut le donner à la CRÉATION (`mfxHDL` + `mfxHandleType`) — §21.1 |
| Moteur | `LowPower = ON` (VDENC, le bloc fixe) avec repli automatique sur le moteur général si `Query` le refuse — §21.4 |
| Débit variable | possible seulement **vers le bas** : `Reset` refuse toute cible au-dessus de celle de l'init — §21.5 |
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

> ⚠️ **Périmé depuis le 08/09/2026 — voir §25.** Ce qui suit était vrai le 04/09
> et a cessé de l'être en deux jours : macOS (§20) et Linux (§19) ont eu leur
> moteur natif les 05 et 06, si bien que « ces plates-formes n'ont pas de moteur »
> ne justifie plus rien. Sunshine est sorti de l'assistant et de l'installeur
> macOS le 08/09, sur la même règle qu'ici : seulement là où la machine peut se
> diffuser elle-même.

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

**✅ B1 vérifié en vrai le 06/09/2026** (le test « HDR Windows actif → session
SDR » que le plan laissait à Bruno) : HDR activé sur le M27Q **pendant** un
stream SDR natif (HEVC 2560×1440@60, client Chrome sur l'écran virtuel voisin).
La session survit — « duplication lost (mode change or desktop switch) — will
restart » puis « duplication started: 2560x1440 (SDR, BGRA8) » en 90 ms, deux
fois de suite (le basculement HDR change le mode deux fois) — et l'image reçue par
le client, capturée à l'écran, est **identique** à celle du même bureau en SDR :
DXGI livre le rendu SDR du compositeur de Windows lui-même, ni délavé ni
sur-exposé (le « bureau HDR délavé en SDR » de Sunshine ne se produit pas ici).
Conséquence pour le client : un hôte Windows en HDR streamé en SDR n'a besoin
d'aucun tone-map côté navigateur — la piste « ACES WebGL2 pour hôte HDR → client
SDR » (F0d(2)) ne vaut que pour une capture qui ne sait pas rendre le SDR d'un
bureau HDR, ce qui n'existe sur aucune plateforme livrée.

**✅ F0d(2) fermée le 07/09/2026 — sans objet, et pour trois raisons qui se
recoupent.** L'item restait ouvert « à rouvrir avec un banc hôte macOS/Linux
HDR » ; ce banc existe depuis le 06/09 (§20.10), et la relecture des trois
plateformes le referme :

1. **Aucun hôte natif ne produit le cas.** C'est la négociation qui décide, pas
   l'état de l'écran. Windows : capture `BGRA8` et DXGI livre le rendu SDR du
   compositeur (mesuré juste au-dessus). macOS : `SckCapture` demande
   `kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange` + `kCGColorSpaceSRGB` +
   matrice BT.709 quand la session est SDR — c'est le compositeur qui rend le
   SDR, même mécanisme que Windows. Linux : il n'y a pas de HDR du tout
   (`LinuxProbe` pose `hdrActive = false`, la capture lit du XRGB 8 bits).
2. **Le client ne demande jamais le HDR sur un écran SDR.** `app.js` efface
   `hdr_enabled` au lancement quand `hdrClientCapability()` refuse — écran en
   mode HDR **et** adaptateur WebGPU **et** décodeur 10 bits. La combinaison
   « flux HDR sur écran SDR » n'est donc pas atteignable par le chemin normal.
3. **Forcée en debug (`mw_hdr_request=1`), elle est déjà servie** : HEVC prend
   le mode `browser` (le tone-map du navigateur), AV1 le mode `tonemap` (ACES
   sur WebGPU, livré en F0e). Il ne resterait à couvrir que « AV1 HDR + écran
   SDR + pas de WebGPU » — or la garde du point 2 exige justement WebGPU, donc
   ce triplet ne s'atteint qu'en contournant volontairement la garde sur une
   machine sans WebGPU. Pas de shader à écrire pour ça.

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
un client AZERTY sans que cette table sache rien de l'un ni de l'autre. (La seule
entrée qui échappe à cette règle est le **texte** d'un clavier tactile, qui n'est
pas une position et doit connaître la disposition de l'hôte — §19.10.)

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

#### 19.9 Le plan curseur ne se lit qu'en client atomique

Le paragraphe ci-dessus disait que le plan curseur était lu ; il ne l'était pas.
L'état d'un plan — `FB_ID`, `CRTC_X`, `CRTC_Y` — vit dans ses **propriétés**, et
le noyau les rapporte à **zéro** à un client qui n'a pas demandé
`DRM_CLIENT_CAP_ATOMIC`, quoi que le compositeur ait réellement posé dessus.
`KmsCapture::start()` ne demandait que `UNIVERSAL_PLANES` — qui suffit à *lister*
les plans, d'où l'illusion : le plan curseur était trouvé et annoncé dans le log,
et `updateCursor()` lisait ensuite `FB_ID = 0` à chaque tour, donc « pas de
framebuffer », donc **pointeur invisible, pour toujours et en silence**.

Conséquence pour le spectateur : **aucun client n'avait de souris** sur un hôte
Linux. Rien à composer dans l'image pour un téléphone (où le pointeur ne peut
être que gravé, `_sendCursorMode`), et aucune forme à envoyer à un navigateur de
bureau pour qu'il la dessine. Mesuré sur la 780M sous GNOME le 07/09 : la même
lecture donne `FB_ID = 0` sans le cap et `FB_ID = 162` (256×256, encre 18×24)
avec. La correction est la ligne `drmSetClientCap(m_Card,
DRM_CLIENT_CAP_ATOMIC, 1)` : on ne fait **jamais** de modeset, le cap ne change
donc que ce qu'on a le droit de *lire*, et un pilote sans atomic le refuse sans
rien empirer.

Ce que la vérification d'origine avait manqué, et qui est maintenant dans
`test_linux_pipeline` : le test relit le plan curseur **par son propre fd**, cap
atomique compris, et exige que les deux réponses concordent — si le compositeur
a un framebuffer de curseur sur ce CRTC, la capture doit le voir (`visible`,
taille non nulle, encre non nulle). Sans le correctif il échoue en 4 points,
avec il passe ; sauté honnêtement si le compositeur n'a pas de plan curseur ou a
caché le pointeur. La leçon vaut au-delà de ce bug : **une capture qui « trouve »
un plan ne prouve rien tant que son contenu n'a pas été relu par un second
chemin.**

#### 19.10 Le texte : la seule entrée qui doive connaître la disposition (08/09/2026)

Le §19.2 dit que la disposition de l'hôte ne doit pas entrer dans la table des
touches, et c'est vrai — pour les **positions**. Il existe une entrée qui n'est
pas une position : `Type::Utf8Text`, ce qu'envoie le clavier tactile d'un
téléphone, qui n'a aucune position à envoyer. Elle tombait dans le `break` vide
de `UinputInput::inject`, avec pour commentaire « needs a layout-aware path that
does not exist on Linux yet ». Résultat pour le spectateur : **sur un hôte Linux,
un mobile ne tapait rien** — pas un caractère — pendant que les flèches, Échap et
Retour arrière du bandeau passaient, eux, parce que ce sont des positions.

Windows et macOS injectent le **caractère** (`KEYEVENTF_UNICODE`,
`CGEventKeyboardSetUnicodeString`) : l'hôte n'a besoin d'aucune touche capable de
le produire. Linux n'a pas d'équivalent. uinput rapporte une position, et c'est
le compositeur qui la lit à travers la disposition de l'utilisateur. Pour faire
apparaître un `a` il faut donc savoir quelle touche produit un `a` **ici** — sur
l'hôte AZERTY de référence, celle qu'un clavier US appelle Q. Une table US aurait
tapé `q`.

`XkbTextMap` compile la disposition avec **libxkbcommon** et parcourt une fois
chaque touche, niveau par niveau (`xkb_keymap_key_get_syms_by_level`), pour bâtir
`caractère → touche + modificateurs`. Trois décisions valent d'être écrites :

- **Quels modificateurs on accepte de tenir** : Shift et Mod5 (AltGr), rien
  d'autre. `xkb_keymap_key_get_mods_for_level` peut proposer un masque contenant
  Lock ; atteindre une majuscule en basculant le Verr. Maj. laisserait le clavier
  de l'hôte dans un état que le spectateur n'a pas demandé et ne voit pas. Un
  masque qu'on refuse est un niveau qu'on n'utilise pas.
- **D'où vient la disposition** : `XKB_DEFAULT_*` si la session la pose, sinon
  `/etc/default/keyboard`, sinon le défaut de libxkbcommon. Les noms sont passés
  explicitement plutôt que laissés à libxkbcommon, qui lit l'environnement par
  `secure_getenv` — vide pour un processus porteur d'une capacité ambiante
  (§19.8). Le résultat est **journalisé** (« fr+azerty (from /etc/default/keyboard),
  115 caractères atteignables ») : c'est une supposition sur le bureau de
  quelqu'un, et si un hôte tape la mauvaise lettre, la ligne dit en un coup d'œil
  quelle disposition a été crue. ⚠️ GNOME garde sa propre copie du réglage dans
  dconf ; un utilisateur qui change de disposition **après** l'installation peut
  la faire diverger du fichier. La lire voudrait dire lancer `gsettings` en fils
  d'un processus qui porte `CAP_SYS_ADMIN` — pire échange que de se tromper sur
  un hôte qui peut poser `XKB_DEFAULT_LAYOUT`.
- **Le repli pour ce qui n'est sur aucun niveau** : les touches mortes. Un `ê` n'a
  pas de touche sur un clavier français, et une personne le tape en deux temps —
  accent circonflexe, puis `e`. La carte fait pareil : les keysyms morts, que le
  parcours ignore puisqu'ils ne portent aucun caractère, sont gardés à part, et
  une table des précomposés Latin-1 les recompose. Les deux moitiés doivent être
  atteignables, sinon on ne tape rien : la moitié d'un caractère est pire que
  rien. Pour ce qui reste hors d'atteinte — un emoji, un idéogramme — il n'y a
  **pas** de repli : uinput n'a pas de mode Unicode, et la séquence
  Ctrl+Maj+U d'IBus n'existe que dans certaines applications, où la manquer
  écrirait `u` suivi de chiffres dans le champ visé. Le caractère est abandonné,
  et une ligne de log le dit une fois.

libxkbcommon est chargée par `dlopen`, jamais liée — la propriété du §19.1 tient
donc toujours : le moteur compile sur un Linux sans le moindre paquet `-dev`, et
un hôte sans la bibliothèque garde exactement le comportement d'avant (le texte
est ignoré, les touches marchent). Le garde-fou « chargée mais n'exporte pas les
appels » n'est pas décoratif : il a attrapé, à la première exécution,
`xkb_keymap_min_key_code` — qui s'appelle en réalité `xkb_keymap_min_keycode`.

⚠️ `Type::LockKeySync` reste ignoré sous Linux, **sciemment** : aligner les
verrous de l'hôte reviendrait à basculer Verr. Maj. et Verr. Num. sur un vrai
bureau depuis un état que le client croit connaître.

Vérifié sur l'bench-mini (GNOME Wayland, `fr+azerty`) : `a` → touche 16 (le Q d'un
clavier US), `q` → 30, `1` → touche 2 + Shift (les chiffres sont en niveau haut
sur AZERTY), `@` → touche 0 + AltGr, `é` en direct sur la touche 2, `ê` = touche
morte 26 **puis** touche 18. `test_xkb_text_map` tient les deux moitiés : le
décodage UTF-8 et la lecture de `/etc/default/keyboard` sont testés partout
(octets tronqués, séquence à quatre octets, valeurs non guillemetées), la carte
réelle seulement là où il y a une libxkbcommon — et elle exige l'alphabet dans
les deux casses, les chiffres, l'espace, la majuscule **sur la même touche** que
la minuscule plus un modificateur, et aucun modificateur hors Shift/AltGr.

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

~~Le portail PipeWire en repli~~ : **fait, câblé et vérifié en flux navigateur
réel (§19.15)** — une machine sans capacité streame, et le jeton de consentement
est rangé, donc **un clic par installation**.
~~Le paquet~~ : traité en §19.8 le 05/09 au soir (constaté le même jour : le job
Linux de `release.yml` n'installait aucune des `-dev`, le `.deb` et le `.rpm`
publiés embarquaient le stub). ~~HEVC~~ : §19.11. ~~Le premier flux navigateur,
image et son~~ : §19.12. ~~L'invalidation de référence~~ : §19.14.

**Ce qui reste pour une prochaine version**, par ordre de ce que l'utilisateur
sent — le détail et le pourquoi de chacun sont en §19.16 :

| # | Manque | État |
|---|---|---|
| 1 | **Couper le son côté hôte** | rien d'écrit ; `HostMute` est Windows seulement, donc `mute_host_audio` — coché par défaut chez le client — est ignoré en silence et le son joue dans la pièce |
| 2 | **Le clic de consentement rejoué** | le sens serveur → worker est prouvé en flux réel ; le sens retour (un grant **neuf** jusqu'à `settings.json`) attend un dialogue humain |
| 3 | **AV1** | écrit, ⛔ bloqué par le pilote (§19.13) ; à rouvrir sur un Mesa plus récent, ce qui demande de mettre à jour le banc |
| 4 | **Le multi-écran absolu** | corrigé et testé unitairement (§23.3), **jamais exécuté sur un vrai hôte Linux à deux écrans** |
| 5 | **HDR** | inexistant : `LinuxProbe` pose `hdrActive = false` en dur, la capture est XRGB 8 bits, rien en aval n'existe |
| 6 | **Wayland et le pointeur relatif** | ⛔ sans solution par conception — aucun client Wayland ne peut lire ni déplacer le pointeur d'un autre |

### 19.11 HEVC par VA-API (08/09/2026)

`renderHevc()` était un refus écrit d'avance, et `LinuxProbe` n'annonçait donc que
H.264 en le disant — « HEVC (silicon, not yet driven) ». Le silicium du 780M
encode HEVC depuis toujours ; il manquait le jeu de paramètres. Écrit en miroir
des choix déjà mesurés sur H.264 (pas de B-frames, une référence, GOP infini,
CBR au VBV d'une image), avec cinq différences qui ne sont pas cosmétiques :

| Point | H.264 | HEVC | Pourquoi ça compte |
|---|---|---|---|
| Unité de bloc | macrobloc 16 | **CTB 64** | la bande d'intra-refresh se compte dedans |
| Horloge VUI | tick = un **champ** (`time_scale = 2·fps`) | tick = une **image** (`= fps`) | un facteur 2 sur la cadence annoncée au décodeur |
| `slice_type` | I 2, P 0 | **I 2, P 1** | la numérotation est inversée entre les deux specs |
| Recadrage | fenêtre de crop dans la séquence | **rien** | la taille codée doit être un entier de blocs minimaux |
| MV temporels | — | éteints, `collocated_ref_pic_index = 0xFF` | prédire depuis l'image collocated casse de plus quand une référence se perd, or ce flux répare en **nommant** l'image perdue (E2) |

Le recadrage absent est le seul point qui change un comportement visible : le
buffer de séquence VA-API HEVC n'a pas de fenêtre de crop **et** c'est le pilote
qui écrit le SPS, donc `init()` aligne la taille codée à 8 **vers le bas** pour ce
codec et le journalise. Toute résolution courante en est déjà un multiple —
1920×1080 compris — donc ça ne coûte rien là où ça ne coûte rien, et ça perd au
pire 7 colonnes ou lignes là où l'alternative serait de donner au décodeur une
taille que le flux ne sait pas exprimer.

La VUI porte `bitstream_restriction` comme sur H.264 : c'est la leçon B8 (200 ms
de latence de décodage sur NVENC faute de ce drapeau) et elle vaut pour tout codec
remis au décodeur matériel d'un navigateur.

**Mesuré sur l'bench-mini** (Radeon 780M, Mesa 23.2.1, libva 1.14) : test de session
84 images / 2 keyframes / première image clé 26 Ko, relues par `ffprobe` en
`hevc / Main / 1920×1080 / 84 images` ; **flux navigateur réel** depuis
Chrome/Windows, « Negotiated video codec: hevc », décodage matériel, 1920×1080 à
60 fps, **8,4 ms**, image juste. Le pilote émet VPS/SPS/PPS à chaque IDR et le
correctif HEVC du relais les trouve sans avoir à les reconstruire.

Voir §19.13 pour AV1, et §19.14 pour l'invalidation de référence.

### 19.15 Le portail ScreenCast : la poignée de main, et ce que coûte le consentement (08/09/2026)

Le portail est la **seule** route de capture d'une AppImage, qui ne peut porter
aucune capacité (§19.8). Il n'est joignable que par **D-Bus**, ce qui a imposé
une décision de licence avant toute ligne de code — voir `native-host/LICENSE.md`
§ « L'exception sd-bus » : les trois façons de parler D-Bus en C sont copyleft ou
pires, et **sd-bus (LGPL-2.1+) a été accepté le 08/09 comme exception bornée**, à
ce seul chemin, plutôt que d'écrire 600 à 1000 lignes de protocole.

**Le piège de la conversation.** Un appel au portail ne rend pas la réponse : il
rend un **chemin d'objet**, et la réponse arrive plus tard en signal dessus.
Chaque étape doit donc dériver ce chemin — depuis notre propre nom de bus unique,
« : » retiré et points en underscores —, s'y abonner, **puis** appeler. Un
abonnement posé après l'appel rate la réponse et attend indéfiniment.

Ce qui est négocié, et pourquoi : `types=1` (un moniteur, jamais une fenêtre —
c'est un hôte de bureau) · `cursor_mode=4` **METADATA**, donc le pointeur arrive
*à côté* de l'image et le client continue de dessiner le sien, comme sur toutes
les autres plateformes · `persist_mode=2`, qui est ce qui achète le silence.

**Le consentement, mesuré** (bench-mini, portail ScreenCast v4, GNOME 42) :

| Passage | Résultat |
|---|---|
| premier, dialogue accepté à la main | nœud 67, 1920×1080, **restore token de 37 octets** |
| rejoué avec le jeton | **aucun dialogue** |
| rejoué encore | aucun dialogue — le jeton survit à son usage |
| **sans** le jeton | **le dialogue revient** |

Donc : **un clic par installation, pas par session**, à condition de ranger le
jeton. C'est la différence entre un repli acceptable et un produit qui demande la
permission à chaque lancement — et c'est pour ça que `persist_mode=2` n'est pas
un détail.

**Où le jeton est rangé, et par quel chemin (08/09/2026).** Le worker n'a pas de
fichier de réglages : il en est un processus séparé, et sur une install en
service il ne tourne même pas sous le même jeton. Le jeton fait donc l'aller-
retour que fait déjà le TTL de l'hôte, à ceci près qu'il est **persisté** :

```
AppSettings["portal_restore_token"]
  → cfg["portalRestoreToken"]            (la ligne de configuration du worker)
  → StreamSession::setPortalRestoreToken
  → NativeMediaEngine::StartParams
  → SessionConfig::portalRestoreToken
  → PortalCapture::setRestoreToken       → aucun dialogue

et au retour, seulement si le portail a VRAIMENT demandé :
  Session::setPortalGrantCallback        (posé AVANT start(), voir plus bas)
  → NativeMediaEngine::portalGrantReceived
  → StreamSession::portalGrantReceived
  → événement JSON {"event":"portalGrant"} sur stdout
  → StreamWorkerHost::portalGrantReceived
  → AppSettings::setPortalRestoreToken
```

⚠️ **Le rappel se pose avant `start()`**, pas après comme tous les autres :
demander un screencast **est** ce qui lève le dialogue, donc le consentement
revient de l'intérieur de `start()`. Un écouteur posé ensuite n'est pas en retard
d'un peu, il a manqué le seul appel qu'il y aura jamais.

⚠️ **Et le grant n'est signalé que s'il est nouveau.** Mesuré le 08/09 : GNOME 42
**ne fait pas tourner le jeton** — rejouer un jeton valide rend exactement la même
chaîne (md5 identique avant/après). Sans la garde `granted != stocké`, chaque
session réécrirait `settings.json` pour rien. Un jeton identique est donc le cas
**normal** d'une machine qui marche, et `setPortalRestoreToken` ne réécrit pas le
fichier quand la valeur ne change pas (vérifié par un test qui réécrit le fichier
en JSON compact et regarde s'il a été ré-indenté).

Le consentement est rangé **où qu'il apparaisse** : la session du propriétaire et
celle d'un joueur invité le remontent toutes les deux, parce qu'il appartient à
la **machine** et non au spectateur — sans quoi l'écran de l'hôte lèverait un
dialogue que personne n'est là pour répondre.

**La route complète, et ce qu'elle décide en chemin.** `IScreenCapture` sépare
les deux sources — le lecteur de scanout et le portail — parce que la session
pose les mêmes questions aux deux ; tout ce qui est propre à une route (chemin
de carte et connecteur pour KMS, jeton de consentement pour le portail) reste
sur la classe concrète. La sonde décide (`caps.capture`), `ResolvedTarget` le
porte, la session obéit : une règle, un endroit.

⚠️ **La liste d'écrans se réduit à une entrée sur la route portail**, et ce n'est
pas une simplification : le portail ne laisse pas l'application choisir un
moniteur, c'est l'utilisateur qui le fait dans le dialogue. Offrir les trois
écrans énumérés serait offrir trois boutons qui font la même chose.

⚠️ **Et une décision qui ne peut pas être prise avant la négociation** : le
compositeur peut donner de la **mémoire partagée** plutôt qu'un DMA-BUF, et EGL
ne sait pas importer ça. La paire GPU devient alors impossible quoi que le
Selector ait choisi, et la session bascule sur la paire CPU en le disant — donc
**H.264 seulement**. GNOME 42 fait exactement ça sur le banc. `SessionInfo`
rapporte l'encodeur **réel**, pas celui choisi, pour que le client ne se voie pas
promettre un codec qu'il ne recevra pas.

#### 19.15.1 ⚠️ Le codec doit suivre la paire (08/09/2026)

Trouvé au **premier vrai flux navigateur** par le portail, et c'est le genre de
défaut qu'aucun test unitaire n'aurait attrapé parce que le test choisissait son
codec : Chrome préfère HEVC, le Selector le lui accorde — le GPU offre bien HEVC
—, puis la paire CPU répond « OpenH264 encodes H.264 only, not HEVC » et la
session meurt avant la première image. Sur une AppImage, c'est **toute** première
session de tout utilisateur.

Le correctif est en deux endroits, et le second est le vrai :

1. `buildPipeline` abaisse le codec **avec** la paire : si la mémoire partagée
   force le CPU et que le codec choisi n'est pas H.264, la session encode en
   H.264 et le journalise. `SessionInfo::codec` rapporte alors H.264 — le client
   apprend ce qu'il va recevoir, jamais ce que le GPU aurait pu faire.
2. ⚠️ **`NativeHost::createSession` ne réduit plus `clientCodecs` au codec
   choisi.** Cette ligne (`resolved.clientCodecs = {selection.codec}`) était une
   normalisation bien intentionnée — « le backend ne rejoue pas la politique » —
   mais le choix voyage déjà par `ResolvedTarget::codec`, donc elle n'achetait
   rien et coûtait la vérité : elle faisait dire à la configuration que le client
   ne décode qu'un seul codec. Avec la liste réduite, la seule réponse
   disponible à « ce client prendrait-il du H.264 ? » était « il a demandé du
   HEVC », et la session mourait sur une machine dont le navigateur décode le
   H.264 parfaitement.

La règle générale, qui vaut au-delà de Linux : **une contrainte découverte tard
doit pouvoir être arbitrée tard**, et pour ça les faits sur le client (ce qu'il
décode) doivent survivre jusqu'au backend — seules les *décisions* se
normalisent. Un client qui n'aurait nommé que HEVC est refusé explicitement,
avec la raison ; il n'est pas servi un flux qu'il ne peut pas décoder.

**Mesuré, les deux routes** :

| Route | Comment | Résultat |
|---|---|---|
| KMS | binaire avec la capacité | 3587/3587, session VA-API HEVC inchangée |
| **Portail** | **une copie du binaire, donc sans capacité — l'AppImage exactement** | bascule automatique, nœud ouvert **sans dialogue**, mémoire partagée détectée, paire CPU, codec abaissé, **3535/3535** dont la descente HEVC→H.264 et le refus d'un client HEVC-seul |
| **Portail, vrai navigateur** | Chrome/Windows → l'app complète sur le banc, jeton lu dans `settings.json` | ouverture **sans dialogue**, `CODEC: H264`, **1920×1080, 8,1 ms**, le bureau GNOME du banc à l'écran |

⚠️ **Le piège AT_SECURE**, qui a d'abord fait croire à l'absence de portail : une
capacité de **fichier** met le processus en `AT_SECURE`, et libsystemd refuse
alors l'adresse du bus venue de l'environnement (`secure_getenv`). Mesuré sur les
trois cas — sans capacité `AT_SECURE=0`, portail v4 ; capacité sur le binaire
(le cas de `mw-native-tests`) `AT_SECURE=1`, « No medium found » ; **à travers
`moonlightweb-launch`, comme le paquet livre, `AT_SECURE=0`, portail v4**. L'app
livrée est du bon côté parce que le lanceur passe la capacité par un exec qui ne
gagne rien, ce qui n'est pas un exec sécurisé (§19.8).

### 19.13 AV1 : écrit, et bloqué par le pilote (08/09/2026)

Le jeu de paramètres AV1 est écrit — séquence, image, groupe de tuiles, sur le
même modèle que les deux autres. Il n'est **pas annoncé** par la sonde, et ce
n'est pas de la prudence : c'est mesuré.

Un profil avec un point d'entrée d'encodage n'est pas un encodeur configurable.
L'encodeur AV1 se décrit par des attributs à lui — `VAConfigAttribEncAV1` (52)
et ses deux extensions — et **Mesa 23.2.1 sur gfx1103 répond « non supporté » à
ces attributs tout en annonçant le profil**. Les deux bouts le confirment :

| Implémentation | Où elle s'arrête |
|---|---|
| la nôtre | `vaEndPicture` → « invalid VAContextID » |
| **FFmpeg 7.1.1**, complète, écriture des OBU comprise | « Driver does not support some wanted packed headers (wanted 0xb, found 0x3) » puis « **Attribute type:52 is not supported** » |

Qu'une implémentation de référence échoue sur le même pilote est ce qui tranche :
le manque est **du côté du pilote**, et annoncer le codec serait le bug B7. La
sonde le dit dans le log en distinguant les deux cas — « profile advertised, but
this driver has no AV1 encode attributes — unusable » ici, « silicon and
attributes, never driven here » sur une machine où ils existeraient.

Ce qui resterait à écrire le jour où un pilote les décrit : les **OBU d'en-tête**
de séquence et d'image en packed headers, avec les décalages de bits
(`bit_offset_qindex` et ses voisins) pointant dedans pour que le contrôle de
débit y écrive ce qu'il décide. C'est un écrivain de flux binaire, pas un
paramètre — et c'est la seule partie qui manque.

### 19.14 Invalidation de référence : une perte coûte un delta (08/09/2026)

`LinuxSession::invalidateReference` forçait une keyframe. Windows répare par un
delta depuis le 06/09 sur les trois encodeurs (E2) ; Linux était le seul à payer
une image clé entière à chaque perte — au moment précis où elle coûte le plus
cher, un lien qui souffre. Or **VA-API donne la liste de références à
l'application, image par image** : il n'y avait rien à demander au pilote, juste
un DPB à tenir et un choix à faire.

Cinq surfaces de reconstruction au lieu de deux (quatre références + la
courante), `max_num_ref_frames = 4`. Chaque slot retient l'image sous **ses deux
noms** : celui que le récepteur connaît (`EncodedFrame::frameNumber`, le seul
avec lequel il peut nommer ce qu'il n'a pas reçu) et celui du flux (`frame_num`
H.264 / POC HEVC, qui repart de la dernière IDR). Toute l'astuce est là :
l'invalidation parle la première langue, les buffers de paramètres parlent la
seconde. Invalider `n` invalide **`n` et toute la suite** — chaque image encodée
après `n` a pu prédire depuis elle.

**Mesuré** (HEVC réel, `mw_drop_test=120`) : 25 pertes nommées, 25 réparations
par delta, **0 repli sur keyframe**, 0 « Requesting IDR » du client, 0 erreur de
décodage, **une seule image clé dans toute la session** — celle d'ouverture.

Et la question qui compte vraiment — le pilote honore-t-il la liste, ou fait-il
comme AMF qu'il fallut juger sur l'image et non sur l'index (§9.10.1) ? Après ces
25 réparations, l'image du client est **identique à celle d'une IDR fraîche** :
signature de luma 16×9 en pleine résolution, écart moyen **0,043 niveau**, pire
cellule 0,2. Aucune dérive.

⚠️ La paire CPU (OpenH264) répond `false` : elle écrit sa propre liste de
références, et `SessionInfo` le dit au client comme avant.

### 19.12 Le premier flux navigateur depuis un hôte Linux : le son (08/09/2026)

L'image était prouvée deux fois (VM Debian par la chaîne CPU, bench-mini par
VA-API) ; **le son ne l'avait jamais été dans un navigateur**, seulement en test
unitaire par libopus. Relevé le 08/09 sur l'bench-mini, tonalité 440 Hz d'amplitude
0,25 jouée dans le sink par défaut, mesure par `AnalyserNode` sur le `MediaStream`
que la page joue réellement :

| Tonalité côté hôte | Crête | RMS | Fondamentale |
|---|---|---|---|
| jouée | **0,2538** | 0,1732 | **445 Hz** (bin de 11,7 Hz) |
| **coupée** | **0** | **0** | — |
| rejouée | 0,2539 | 0,1764 | 445 Hz |

Une sinusoïde d'amplitude 0,25 a une RMS de 0,177 : ce qui sort du décodeur du
navigateur est le signal de l'hôte, pas un artefact de mesure — et l'A/B/A le
prouve mieux qu'une seule lecture, parce qu'une chaîne qui invente du bruit ne
sait pas se taire sur commande.

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

### 19.16 Ce qui manque encore à la plateforme Linux (08/09/2026)

Écrit après le premier flux navigateur complet par le portail, quand la
plateforme est utilisable de bout en bout : image, son, clavier, souris, manette,
réparation sans keyframe, et une route pour les machines qui ne peuvent pas lire
leur scanout. Ce qui suit n'est pas une liste de bugs, c'est ce qu'un hôte Linux
ne sait **pas encore** faire, et pourquoi.

**1. ~~Couper le son côté hôte — rien n'est écrit.~~** ✅ **livré le 08/09, voir
§19.17.** Ce paragraphe est conservé parce qu'il se trompait, et sur le point qui
décidait de tout : « couper le sink par défaut au **volume** couperait aussi la
capture, puisque le moniteur entend ce que le sink joue ». C'est la sémantique de
**PulseAudio**. PipeWire fait l'inverse par défaut, et la mesure le dit ; le
sink nul, annoncé ici comme la seule voie, n'est en fait que le repli.

**2. Le consentement du portail, dans le sens du retour.** Le trajet
`settings.json` → worker → portail est prouvé en flux navigateur réel : le jeton
est relu, rejoué, et la session s'ouvre sans dialogue (§19.15). Le trajet inverse
— un grant **neuf** qui remonte jusqu'à `settings.json` — est écrit, testé
unitairement, et ne peut être exercé qu'en levant un vrai dialogue, donc en
cliquant à la main sur la machine. C'est une vérification, pas un doute : sans
elle, le pire cas est que le dialogue revienne, ce qui est le comportement
d'avant.

**3. AV1 — bloqué ailleurs que chez nous.** §19.13. Rien à corriger ici ; il faut
un pilote qui décrive ses attributs d'encodage AV1, donc un banc plus récent
qu'Ubuntu 22.04 / Mesa 23.2.

**4. Le multi-écran absolu, jamais vu tourner.** `displayPointToDesktop()` et
`desktopToAbsoluteRange()` (§23.3) sont justes par construction et couverts par
`test_absolute_map.cpp`, mais aucun hôte Linux à deux écrans n'a jamais exécuté
ce code — le banc n'en a qu'un. Un test unitaire prouve l'arithmétique, pas la
convention du compositeur.

**5. HDR : inexistant, et ce n'est pas un oubli.** `LinuxProbe` pose
`hdrActive = false` en dur, la capture est XRGB 8 bits, et rien en aval ne sait
produire du P010 ni du Main10 par VA-API. C'est le chantier de §16 à refaire
entièrement côté Linux. ⚠️ Et le banc ne pourra rien en juger sans un écran HDR
branché dessus.

**6. Wayland et le pointeur relatif — ⛔ sans solution.** `X11Pointer` est chargé
en `dlopen` ; sur une session Wayland il ne trouve rien, et le rapatriement du
pointeur entre écrans n'a pas lieu. Ce n'est pas un manque à combler : **aucun
client Wayland ne peut lire ni déplacer le pointeur d'un autre**, par conception
du protocole. L'injection, elle, marche partout — uinput est un périphérique
noyau, que le compositeur voit comme une vraie souris.

**Et une contrainte assumée, à ne pas relire comme un manque** : PipeWire est
**requis** pour le son. Sur une machine encore sous PulseAudio pur, le graphe n'a
aucune sortie, le flux est refusé, la session streame en silence avec un journal
explicite et réessaie toutes les 2 s. C'est une décision de licence — libpulse
est LGPL, hors de la liste blanche de `backend/native-host/LICENSE.md` — pas un
défaut.

### 19.17 Couper le son côté hôte : le moniteur est en amont du volume (08/09/2026)

Troisième et dernière plateforme à recevoir `HostMute`, et la troisième réponse
différente à la même question — *où est le tap, par rapport au réglage qui fait
taire les haut-parleurs ?* Windows le prélève sur le moteur audio (§24), macOS sur
le flux applicatif (§20.14), Linux sur le **moniteur d'un sink** (§19.7). Aucune
des deux réponses précédentes ne se transporte, et celle que §19.16 avait écrite
d'avance était fausse.

#### La mesure, avant d'écrire

Banc bench-mini, WirePlumber 0.4.8 sur PipeWire 0.3.48, une tonalité 440 Hz à 0,25
jouée en continu, RMS de ce que rend le moniteur, 2 s par état. Deux sinks
mesurés côte à côte, le vrai et un nul :

| état | vraie sortie ALSA | sink nul |
|---|---|---|
| départ | 0,1755 | 0,1755 |
| sink **muet** | **0,1755** | **0,0000** |
| volume 0 % | **0,1755** | **0,0000** |
| restauré | 0,1755 | 0,1755 |
| `monitor.channel-volumes` | *absent* | `true` |

Une sinusoïde d'amplitude 0,25 a une RMS de 0,177 : le moniteur de la vraie
sortie rend le signal **entier**, muet ou pas.

La propriété est toute l'explication. `monitor.channel-volumes` décide si
l'adaptateur applique le volume et le mute du nœud à ses ports moniteur ; elle
vaut **false par défaut**. Sur toute sortie réelle — ALSA, HDMI, USB, Bluetooth —
le moniteur est donc pris **en amont** du volume, et un mute n'atteint jamais la
capture. Les seuls sinks qui la posent à `true` sont les sinks virtuels créés par
la couche de compatibilité PulseAudio, qui gardent exprès la sémantique de
Pulse — celle que §19.16 avait prise pour la règle générale.

#### Deux stratégies, choisies en lisant cette propriété

1. **SinkMute** — `monitor.channel-volumes` n'est pas vrai : on coupe la sortie
   par défaut. Rien ne bouge dans le graphe de l'utilisateur, son niveau de
   volume est intact, et ce qu'il voit est l'icône de haut-parleur barrée. C'est
   le cas de tout bureau ordinaire.
2. **NullSink** — le moniteur porte bien le volume, donc couper tuerait la
   capture avec (mesuré ci-dessus). Une sortie qui ne joue nulle part est créée
   (`support.null-audio-sink`, nœud `moonlightweb-host-muted`) et devient la
   sortie par défaut de la session : le gestionnaire de session **déplace les
   flux en cours** dessus, la pièce se tait, et le tap — qui suit la sortie par
   défaut — atterrit sur son moniteur.
3. **None** — l'hôte s'entend, et le journal dit pourquoi.

`engage()` avant l'ouverture du tap (la stratégie 2 déplace la sortie à laquelle
le tap s'attache), `release()` après sa fermeture (sinon le gestionnaire de
session ramènerait un tap encore vivant sur la vraie sortie).

#### Le piège qui a coûté deux passes : où s'écrit un mute

Le premier jet écrivait `SPA_PROP_mute` sur le **nœud** du sink. Vérifié de
l'extérieur pendant une vraie session, le résultat était : `node.mute=True`
pendant, `False` après — et `pactl get-sink-mute` répondait **`no`** tout du
long. Un mute que le bureau ne voit pas.

Ce que fait le bureau, lu au même endroit :

| | `pactl` | `node.mute` | `node.softMute` |
|---|---|---|---|
| au repos | no | False | False |
| après `pactl set-sink-mute 1` | **yes** | True | True |
| notre 1ʳᵉ version | no | True | False |
| notre version livrée | **yes** | True | True |

Un mute vit sur la **route de la carte** (`SPA_PARAM_Route`, avec l'index et le
`card.profile.device` du sink), pas sur le nœud : c'est là que les réglages du
système l'écrivent, là que l'icône le lit, là qu'une carte munie d'un mute
matériel l'applique en matériel. Le démon le répercute ensuite **lui-même** sur
le nœud, `softMute` compris — d'où la dernière ligne du tableau, obtenue par une
seule écriture. Un sink sans carte (virtuel) n'a pas de route : celui-là est
coupé sur son nœud, en écrivant les deux propriétés à la main.

⚠️ Ce détour n'est pas cosmétique. `softMute` est l'étage qui retire réellement
les échantillons envoyés au périphérique ; `mute` seul annonçait une sourdine que
personne n'appliquait. Et écrire là où le bureau écrit donne la seule preuve
disponible sur une machine sans oreilles : **l'état obtenu est identique, propriété
par propriété, à celui que produit le mute de l'utilisateur**. Le silence des
haut-parleurs n'est pas observable en logiciel — le seul consommateur de la sortie
d'un sink est le matériel — donc l'équivalence est la preuve, et c'est pour ça
qu'elle vaut le code qu'elle coûte.

#### Ce qui a été vérifié, et comment

- **Stratégie 1, vraie session** (sonde de banc, session KMS + VA-API complète) :
  `hostMuted=true`, `pactl` passe à `yes` pendant et revient à `no` après, le
  moniteur reste à **0,1767** pendant la sourdine, et l'audio encodé porte
  **80,5 octets/paquet** contre 3,0 en silence — la capture entend tout.
- **Stratégie 2, vraie session**, en forçant le cas (sortie par défaut = un sink
  nul, donc `monitor.channel-volumes = true`) : `moonlightweb-host-muted`
  apparaît, devient la sortie par défaut, **le flux déjà en cours migre dessus**
  (sink 44 → 2802), le son continue de partir à 80,4 o/paquet, et à l'arrêt la
  sortie par défaut est rendue **et notre sink a disparu**.
- Tests : **3610/3610** sur le banc Linux, **3431/3431** sous Windows.
- Ce que ni l'un ni l'autre ne prouve : que la pièce se tait. Le banc n'a pas
  d'enceinte branchée, et aucun logiciel ne peut écouter la sortie d'un sink.
  C'est le seul point qui attend une oreille, comme sur macOS (§20.14).

#### Deux propriétés qui tombent en prime

Le sink nul est créé avec `object.linger = false` : il meurt avec notre
connexion, donc **un worker tué ne laisse pas la machine sur une sortie
silencieuse** — ce que la version Windows, elle, ne garantit pas (§24). En
revanche un mute de stratégie 1 survit à un worker tué, exactement comme sous
Windows ; l'utilisateur le défait d'un clic, puisque c'est son propre mute.

#### Ce dont ça dépend

D'un gestionnaire de session qui publie l'objet metadata `default` — c'est ce qui
nomme la sortie par défaut. WirePlumber le fait, sur tous les bureaux actuels. Le
banc tournait encore sous `pipewire-media-session` 0.4.1, retiré depuis, qui ne
le fait pas : là, `pactl set-default-sink` sort en erreur, il n'y a aucune
metadata à lire, et `HostMute` répond `None` avec ces mots plutôt que de couper
un sink dont il ne peut pas prouver que c'est celui que l'utilisateur écoute. Le
banc a été basculé sur WirePlumber pour cette raison.

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

### 20.9 Ce qui restait — fermé le 06/09/2026

Le HDR (§20.10), le pointeur agrandi (§20.11), le résidu audio (§20.12), la
signature stable et les tests avec Screen Recording (§20.13). La plateforme est
au niveau des deux autres, à une exception près qui n'est pas du code : le
`.pkg` n'est pas notarié (Developer ID payant), donc un double-clic sur le
téléchargement affiche « développeur non identifié » ; le chemin Homebrew n'en
souffre pas.

Hors de ce module, vu au passage et **corrigé depuis** : Internet Access se
désactivait entièrement quand l'enregistrement PowerDNS échouait, rendez-vous
compris, alors que le rendez-vous n'a aucun besoin du sous-domaine. La
rétro-compatibilité DNS côté client a été retirée le 05/09/2026 — plus aucune
installation n'écrit dans PowerDNS ni ne lance ACME, et ce chemin de coupure
n'existe plus. **Vérifié en vrai le 06/09** : l'app complète à jour déployée
sur le Mac de banc (`.env` réduit à `MW_DOMAIN` + `MW_PDNS_TOKEN`, les seules
clés que le client lit encore), le consentement v2 redonné par
`MoonlightWeb --enable-internet --yes`, et la ligne de rendez-vous levée sans
qu'aucun enregistrement DNS ne soit écrit.

### 20.10 HDR : le 10 bits PQ de ScreenCaptureKit → HEVC Main10 (06/09/2026)

macOS n'a pas d'interrupteur HDR. Un panneau capable d'aller au-dessus du blanc
SDR l'est toujours — c'est l'*Extended Dynamic Range*, et
`NSScreen.maximumPotentialExtendedDynamicRangeColorComponentValue` dit de
combien (le Liquid Retina XDR du banc répond 5 ; un panneau SDR répond 1). La
sonde traduit donc `hdrActive` par « headroom > 1 **et** macOS 15 », parce que
c'est la capture qui gate la plateforme : `SCStreamConfiguration.captureDynamicRange`
n'existe que depuis macOS 15, et sans elle le compositeur ne livre que du 8 bits.
`supports10Bit` suit la même règle (HEVC matériel **et** capture 10 bits), jamais
« la puce le pourrait » (§16.3).

La chaîne reste sans étage de conversion : on demande au compositeur du
`x420` (4:2:0 10 bits dans des mots de 16, la disposition P010) avec
`SCCaptureDynamicRangeHDRCanonicalDisplay` — la référence fixe à 1 000 nits,
pas le headroom local, pour que le flux ne suive pas l'état de luminosité du
panneau — et l'espace `ITUR_2100_PQ`. ⚠️ CGDisplayStream n'a jamais eu de
constante de matrice BT.2020 ; la propriété prend les mêmes chaînes que
CoreVideo attache aux tampons, et `kCVImageBufferYCbCrMatrix_ITU_R_2020` est
acceptée. La première image est **relue et journalisée** (format, primaires,
transfert, matrice) : `x420 2218x1440, primaries ITU_R_2020, transfer
SMPTE_ST_2084_PQ, matrix ITU_R_2020` — c'est la ligne qui prouve que la
description couleur de l'encodeur dit la vérité.

VideoToolbox : profil **Main10 nommé** (un profil Main accepte la surface 10
bits et encode 8 bits dedans, §16.2) et les trois propriétés couleur posées sur
la session — `ColorPrimaries ITU_R_2020`, `TransferFunction SMPTE_ST_2084_PQ`,
`YCbCrMatrix ITU_R_2020` — pour qu'elles atterrissent dans le VUI. En SDR rien
n'est posé : VideoToolbox lit alors les attaches du tampon, et une description
qui différerait d'elles déclencherait une conversion couleur silencieuse.

**Vérifié depuis Chrome/Windows sur le M27Q en HDR** : session « HEVC HDR
(Main10, BT.2020 PQ) », première keyframe 98 Ko, overlay client « HEVC HDR »
(présentateur `<video>`, le chemin F0e), image aux bonnes couleurs — les
séquoias de l'économiseur du Mac, pas délavés. Le pointeur agrandi (§20.11)
passe par le même chemin 10 bits (courbe PQ, matrice BT.2020 sur le signal PQ,
blanc SDR à 203 nits).

### 20.11 Le pointeur agrandi : dessiné par le moteur (06/09/2026)

Le §20.1 disait « pas de route » : ScreenCaptureKit compose le pointeur à sa
taille, et il n'y a pas de convertisseur où le grossir. La route est celle-ci :
quand le client demande une taille (`cursorFramePx`, le téléphone), la capture
est mise en `showsCursor = NO` et le moteur **dessine lui-même** le curseur
système — l'image `NSCursor` déjà rastérisée pour le mode client-dessiné — dans
le tampon du compositeur, juste avant l'encodage. `convert/CursorBlend.h`, pur
et testé partout : la forme est préparée **une fois** par changement dans les
valeurs de code de la cible (luma/chroma × couverture, BT.709 8 bits ou BT.2020
PQ 10 bits), puis chaque image coûte un échantillonnage bilinéaire à l'échelle
voulue et un multiplier-ajouter par plan sur l'empreinte du pointeur — quelques
centaines de pixels dans une image 4K, verrouillage `CVPixelBufferLockBaseAddress`
compris.

Trois détails qui ont une raison :

- **Le tampon est le compositeur's, et il est ré-encodé.** Le plancher écran
  fixe et la rafale de raffinement ré-encodent la dernière image ; un pointeur
  brûlé dedans laisserait une traînée. Le blend sauve d'abord le rectangle
  qu'il couvre (`PlanePatch`) et le **restaure avant le blend suivant**, tant que
  le tampon est le même (numéro de série de l'image tenue).
- **Un mouvement de pointeur redevient une image.** Pointeur hors capture, un
  déplacement sur un écran fixe ne produit plus d'image ; la boucle regarde
  alors le pointeur (position à chaque tour, forme toutes les 50 ms) et
  ré-encode l'image tenue quand il a bougé, au plus à la cadence du flux —
  l'équivalent du statut PointerOnly de DXGI, obtenu autrement.
- **L'échelle** : taille naturelle dans l'image = raster × (image / pixels de
  l'écran) ; cible = taille demandée / côté long de l'encre ; plafond ×2,5 comme
  le convertisseur Windows.

**Vérifié en vrai** : « drawing the pointer at x2.50 of the frame (96 px asked,
shape 34x46, ink 34x46) », le pointeur suit la souris injectée depuis le client,
en SDR comme en HDR. ⚠️ Fausse alerte du banc : il est sorti **vert** — la
couleur de remplissage personnalisée du pointeur de ce Mac (Accessibilité ›
Pointeur, `cursorFill` dans `com.apple.universalaccess`), fidèlement reproduite,
ce qu'une sonde de relecture (raster → contributions → plans, en debug) a
tranché en une ligne : `BGRA 0 255 0 255` à la source.

### 20.12 Le résidu audio : la grâce du pacer (06/09/2026)

Le pour-cent du §20.8 avait une cause, pas une gigue. ScreenCaptureKit livre
20 ms de son à la fois, à son heure ; avec `pop()` seul, une rafale arrivée 3 ms
après le tic qui en avait besoin envoyait une trame de **silence** — et ce
silence n'était pas gratuit : l'horloge avançait sans consommer la file, qui
gardait dès lors une trame **de plus**, pour toujours, jusqu'à ce que le plafond
en jette une. Chaque rafale tardive ajoutait une trame, chaque trame jetée était
cet ajout qui ressortait — d'où deux compteurs jumeaux (165 silences, 121
jetées sur 83 s) sur un hôte dont les deux horloges étaient exactes.

`AudioPacer::take()` : une trame due que la file ne peut pas remplir est
**différée** jusqu'à deux périodes (10 ms) avant que le silence parte ; le
`PacedOpusSink` dort alors jusqu'à la fin de la grâce **ou** jusqu'au `push()`
suivant, qui le réveille. Le récepteur tient 35 ms de tampon de gigue : il ne
voit rien. `pop()` est conservé tel quel pour WASAPI (un seul fil, pas de rafale
à attendre). Test : 20 ms de rafales avec 9 ms de gigue pendant 2 s → zéro
silence, zéro jetée.

**Mesuré sur 325 s de flux réel (tonalité jouée sur le Mac)** : 65 138 paquets,
**7 jetées, 57 silences** (0,01 % et 0,09 %, dont 4 et 50 dans la première
minute — la rafale de démarrage de SCK), 2 344 attentes. Les minutes suivantes :
1 / 1, puis 2 / 2. Le dixième de pour-cent restant est la rafale initiale et
quelques rafales à plus de 10 ms de retard ; il n'y a plus d'accumulation.

### 20.13 Signature stable, tests avec Screen Recording, banc à jour (06/09/2026)

**La signature.** Le §20.5 et le §20.7 disaient le problème : TCC accroche ses
octrois à l'*exigence de code*, et une signature ad hoc en change à chaque
build, donc chaque mise à jour reperdait Screen Recording et Accessibility (la
seconde en silence). `release.yml` signe désormais l'app avec un certificat
**auto-signé stable** (« MoonlightWeb », valable jusqu'en 2041 ; identité
générée le 06/09/2026, conservée hors dépôt dans `.secrets/` chez le mainteneur,
publiée dans les secrets d'environnement `MACOS_SIGN_P12` (base64) et
`MACOS_SIGN_PASSWORD`). Exigence désignée résultante :
`identifier "com.moonlightweb.server" and certificate root = H"d051d7d8…"` —
« root » parce qu'un auto-signé est sa propre racine ; constante d'une release
à l'autre. Gatekeeper n'y voit aucune différence avec l'ad hoc (« développeur
non identifié » au double-clic, rien via Homebrew) ; le `.pkg` lui-même reste
non signé, une signature d'installeur n'ayant de sens qu'avec un Developer ID
Installer. Une installation qui vient d'une build ad hoc redemandera les deux
autorisations **une dernière fois**. Répété sur le banc avant d'être écrit dans
le workflow, avec deux pièges : `security import` refuse un `.p12` moderne
(PBES2/AES-256, ce qu'OpenSSL 3 produit par défaut — « MAC verification
failed ») et veut du SHA-1/3DES ; et `codesign` répond `errSecInternalComponent`
tant que le trousseau temporaire n'est pas dans la **liste de recherche**, même
nommé par `--keychain`. Secret absent → ad hoc comme avant, avec un avertissement.

**Les tests.** `mw-native-tests` n'avait pas Screen Recording et l'octroi à la
main ne survivait pas au rebuild (§20.5). TCC identifie un exécutable nu par
son **chemin**, un bundle par son identifiant + son exigence : enveloppé dans un
`.app` minimal qui porte l'identifiant de l'app (`com.moonlightweb.server`) et
signé de la même identité, le binaire de tests satisfait **l'octroi existant de
l'app** — même ligne dans `TCC.db`, aucune nouvelle. `scripts/mac-native-tests.sh`
fait l'enveloppe, la signature et le lancement par `launchctl` dans la session
graphique (la seule où SCK voit un écran) ; mesuré : les tests de session
capturent au premier essai, 2 254/2 254.

**Le banc.** Le clone du Mac est passé au HEAD de la machine Windows par
`git bundle` (les commits ne sont pas poussés), l'app complète rebâtie et
déployée : c'est la première fois que le Mac tourne le code du jour et non un
`native-host` superposé à un serveur vieux d'une semaine.

### 20.14 Couper le son côté hôte : le tap n'est pas sur le chemin (08/09/2026)

Le réglage `mute_host_audio` — coché par défaut chez le client, envoyé depuis
toujours — n'était lu que par Windows (§24). Sur un Mac, la case ne faisait
**rien, en silence** : le spectateur entendait le jeu, et la pièce aussi.

**La mesure d'abord, parce que la réponse de Windows ne se transporte pas.** Là-bas
le loopback WASAPI prélève la sortie du *moteur*, donc le volume principal
atteint la capture et seul un mute fait par le pilote lui échappe. Le tap de
ScreenCaptureKit est ailleurs : c'est une seconde sortie du même flux (§20.8),
alimentée par les applications. Une sonde de banc (une tonalité 440 Hz jouée en
continu, RMS du tap sur 2,5 s par état) tranche :

| État | RMS du tap |
|---|---|
| départ | 0,2997 |
| point de sortie **muet** | 0,3027 |
| volume 0,5 | 0,3028 |
| volume **0** | 0,3029 |
| restauré | 0,3030 |

Identique au bruit près : **ni le mute ni le volume du périphérique de sortie
n'est sur le chemin de la capture**. Deux conséquences, et elles simplifient le
code par rapport à Windows :

- le **volume** est utilisable ici, alors qu'il ne l'a jamais été là-bas ;
- il n'y a **pas besoin** de la stratégie « router vers un périphérique qui ne
  pilote aucun haut-parleur » : rien, dans le périphérique de sortie, ne peut
  retirer le son du flux, donc rendre muet celui que l'utilisateur écoute suffit.

⚠️ La première passe de la sonde avait écrit 0 sur un volume qui **lisait déjà
0,000** : elle ne prouvait rien du volume, et le tableau ci-dessus est la
seconde, qui monte à 0,5 avant de redescendre.

**Ce qui est livré.** `audio/macos/HostMute.{h,cpp}` — même forme et même
contrat que la classe Windows du même nom, deux stratégies : `EndpointMute`
(`kAudioDevicePropertyMute` sur la sortie par défaut, élément maître ou, à
défaut, sa paire stéréo) et, pour une sortie qui n'a pas de mute (certains HDMI,
AirPlay), `VolumeZero`. Déjà muet ou déjà à zéro = revendiqué **sans rien
sauvegarder**, pour que `release()` ne relève pas un mute qu'il n'a pas posé. Au
relâchement, un réglage que l'utilisateur a changé entre-temps est laissé tel
quel. `MacSession` engage avant la capture et relâche après — sur macOS l'ordre
n'a aucune importance, il n'est là que pour que les deux plateformes se lisent
côte à côte.

⚠️ Piège attrapé en câblant : `MacSession::start()` remet `m_Info` à zéro
**après** le bloc audio (l'ordre inverse de Windows), donc le drapeau
`hostMuted` posé à l'engagement était effacé sans bruit. Il est relu de
`m_HostMute.strategy()` là où `m_Info.audio` est rempli.

**Vérifié.** `mw-native-tests` 3179/3179 sur le Mac, 3385/3385 sur Windows (le
test est commun aux deux plateformes depuis ce chapitre). Le test *s'arrange*
sa précondition : sur une machine déjà muette, il lève le mute pour que la
branche qui écrit soit celle qui est exercée, et repose ce qu'il a trouvé.
Et en vrai, sur l'app déployée, l'état lu **de l'extérieur** (`osascript`,
une fois par seconde) : `muted=false` avant, `true` de la première à la douzième
seconde de session, `false` à l'instant de l'arrêt et ensuite — pendant que
l'audio continuait de partir (2 961 paquets, 0 jeté, 29 trames de silence de
démarrage), une vidéo YouTube jouant sur le Mac.

**Confirmé à l'oreille par Bruno, devant la machine** (« le son est bien coupé,
ça fonctionne ») : c'est la seule moitié de ce chapitre qu'aucune sonde ne peut
produire. Un `kAudioDevicePropertyMute` à 1 dit ce que l'OS a enregistré, pas ce
que la pièce entend.

**Reste** : Linux (rien — `HostMute` n'est inclus que par les sessions Windows
et macOS), et un worker tué de force laisse le mute posé, comme sous Windows.

## 21. Intel Quick Sync : la première exécution, et ce qu'elle a cassé (07/09/2026)

Le banc `bench-intel` (Intel N95, UHD Graphics 24 EU, pilote 32.0.101.7088,
Windows 11) est le premier GPU Intel de la flotte. Le chemin oneVPL y a été
exécuté pour la première fois. Le §14 du plan v2 le disait « écrit, jamais
exécuté » ; il n'a rien fonctionné du premier coup, et chacun des cinq défauts
était invisible sans matériel.

Ordre des symptômes, tel que la machine les a donnés — c'est aussi l'ordre dans
lequel un autre vendeur les redonnera :

### 21.1 « no usable encoder » sur une machine qui a Quick Sync

La sonde répondait `available:false`, « no video encoder this engine can drive
on any GPU », sur les **trois** adaptateurs Intel que DXGI énumère (un vrai, et
un par pilote d'écran indirect — Parsec VDD et Virtual Display Driver).

`MFXVideoCORE_SetHandle(MFX_HANDLE_D3D11_DEVICE)` répondait
`MFX_ERR_UNDEFINED_BEHAVIOR` (-16), documenté « the same handle is redefined …
or an internal handle has been created before this function call ».

Deux causes empilées, et il fallait les deux :

1. **Le device n'avait pas `D3D11_CREATE_DEVICE_VIDEO_SUPPORT`.** Sans ce
   drapeau un device D3D11 n'expose pas d'`ID3D11VideoDevice`, et le runtime
   Intel ne peut rien en faire. Aucun autre encodeur de cet arbre ne l'exigeait,
   donc personne ne l'avait posé — ni la sonde (`VplCapabilities`) ni la capture
   (`DxgiDuplication`, qui est le device que l'encodeur reçoit en session
   réelle). Les deux le posent maintenant ; c'est gratuit sur un GPU qui s'en
   moque.
2. **Le device doit être donné au dispatcher, pas à la session.** Sur oneVPL 2.x
   le dispatcher crée le device lui-même en créant la session, et un `SetHandle`
   ultérieur arrive trop tard. Les deux propriétés qui le lui donnent à temps
   — `mfxHDL` et `mfxHandleType` — appartiennent au dispatcher et non à
   `mfxImplDescription`, donc elles n'apparaissent nulle part dans les en-têtes.

`VplSession::open` essaie les deux routes dans cet ordre et **demande ensuite au
runtime quel device il utilise** (`GetHandle`), puis compare le LUID de son
adaptateur à celui demandé. C'est ce qui départage les trois « Intel(R) UHD
Graphics » de la liste, et c'est ce qui empêcherait une session de tourner en
silence sur un device dont nos textures ne sont pas.

⚠️ **`MFXVideoCORE_GetHandle` n'incrémente pas le compteur COM**, contrairement à
ce que sa documentation promet. Relâcher la référence qu'on n'a jamais reçue
libère le device sous son propriétaire : mesuré comme une violation d'accès dans
`d3d11!CDevice::Release` à la seconde où la sonde lâchait son propre `ComPtr`.
Le handle est emprunté, jamais possédé.

### 21.2 La première frame tuait le processus

Corruption de pile (`0xC0000409`), sans log, dès le premier `EncodeFrameAsync`.

En `MFX_IOPATTERN_IN_VIDEO_MEMORY` une surface ne porte pas de pixels : elle
porte un `Data.MemId`, que le runtime traduit en texture **par l'allocateur de la
session**. Sans allocateur enregistré, le runtime utilise le sien et lui présente
notre MemId, qu'il relit comme une de ses propres structures. Ce n'est pas un
chemin d'erreur, c'est un pointeur sauvage.

`VplFrameAllocator` (nouveau) pose le contrat : **dans ce moteur, un MemId est
toujours un `mfxHDLPair` {ID3D11Texture2D\*, sous-ressource}**, et `GetHDL` est
la ligne qui le dit. L'allocateur sert aussi les surfaces que l'encodeur alloue
pour lui-même (images reconstruites), en `D3D11_BIND_DECODER` comme le fait
l'allocateur des samples Media SDK, avec repli en RT+SRV.

### 21.3 Le débit ne bougeait jamais

`MFXVideoENCODE_Reset` répondait `MFX_ERR_INCOMPATIBLE_VIDEO_PARAM` (-14) à
**chaque** changement du gouverneur de lien — soit environ deux fois par seconde
sur un lien qui souffre. L'encodeur Intel ignorait donc complètement le lien,
et la seule trace était un avertissement qu'un log de session fait défiler.

Trois causes, toutes réelles :

1. **Le modèle HRD.** Avec `NalHrdConformance` actif, oneVPL traite un changement
   de débit comme une nouvelle séquence et refuse tout ce qui n'est pas une IDR.
   Il est désormais explicitement à `OFF` (`mfxExtCodingOption`, chaîné à chaque
   session, intra-refresh ou pas). L'alternative — forcer la nouvelle séquence —
   achèterait la conformité au prix d'une keyframe deux fois par seconde, c'est-
   à-dire exactement le pic de débit qu'un lien congestionné ne peut pas encaisser.
2. **Le VBV.** Rebâtir `BufferSizeInKB` au nouveau débit est une réallocation, et
   `Reset` refuse l'appel entier pour ça. `applyBitrateOnly()` ne touche donc que
   `TargetKbps`/`MaxKbps` ; le VBV reste où l'init l'a mis.
3. **Le bloc de paramètres.** `Reset` compare au bloc réellement en vigueur, y
   compris les champs que le runtime a remplis lui-même à l'init (profil, niveau,
   nombre de références). `init()` relit maintenant ce bloc par
   `EncodeGetVideoParam` et c'est lui que `Reset` reçoit.

### 21.4 Trop lent de moitié

Premier chiffre mesuré : **16,3 ms** par frame en HEVC 1080p60, TU7. Un stream à
60 fps ne tient pas dans ça.

`mfx.LowPower = MFX_CODINGOPTION_ON` — le moteur à fonction fixe (VDENC) plutôt
que celui à shaders — le ramène à **10,5 ms**, et le 1440p de 21,4 à 13,4 ms.
C'est le même arbitrage que partout ailleurs dans ce moteur : moins de passes,
moins de latence, quelques bits de plus. Une génération sans VDENC pour ce codec
le dit à `Query`, et `init()` refait la demande sans — journalisé, jamais avalé.

### 21.5 La session mourait au bout de douze frames

`SyncOperation` répond `MFX_WRN_IN_EXECUTION` (1) quand son délai passe avec la
frame encore dans l'encodeur — « redemande », exactement comme
`MFX_WRN_DEVICE_BUSY`. Le code en faisait une erreur fatale : premier flux
navigateur réel, douze frames, puis `session ended: waiting for the encoded
frame failed: still executing (1)`. Sur un N95 qui encode, décode et fait tourner
le navigateur sur les mêmes quatre cœurs, une frame sur quelques centaines
dépasse 100 ms.

Le délai reste court — un encodeur vraiment mort doit être vu vite — mais il est
redemandé jusqu'à dix fois, et la première lenteur est dite une fois par session.

### 21.6 Faire monter le budget par image — trois routes, une seule marche

`Reset` refuse aussi toute cible **au-dessus** de celle de l'init (« requires
additional memory allocation »), et refuse l'appel entier. Or le budget par
cadence réelle (E4) demande légitimement plus que le débit réglé quand l'image
bouge moins vite que le flux : sur ce banc il demandait 32000 kbps pour un stream
réglé à 20000, deux fois par seconde. Ce n'est **pas** une demande d'envoyer plus
par seconde — le débit sur le fil ne bouge pas — c'est « cette seconde ne contient
que 30 images, chacune peut être deux fois plus grosse ».

Trois routes essayées sur l'N95, dans cet ordre :

| Route | Résultat |
|---|---|
| Déclarer un `MaxKbps` plus haut à l'init pour laisser de la place | ⛔ en CBR le runtime le rabat aussitôt sur `TargetKbps` — « ignored », comme la doc l'autorise. Vérifié par relecture : `budget up to 20000` alors qu'on avait demandé 120000 |
| Laisser le débit tranquille et dire à `Reset` que la cadence a baissé (arithmétiquement le même budget) | ⛔ refusé aussi (-14). `Reset` refuse **tout ce qui déplace le budget par image**, quel que soit le champ où c'est écrit |
| Dimensionner le tampon de bitstream à l'init pour la hausse | ✅ **c'est celle-là**. 20000 → 40000 refusé avec 85 Ko de tampon, accepté avec 250 Ko |

Le tampon est aussi le VBV (§9.x, RateControl.h), donc la marge n'est pas « tout
ce que quelqu'un pourrait demander » — ce serait six fois le débit, six temps
d'image pour une seule image — mais **exactement la plage où travaille
`EffectiveCadence`** : son plancher est 30 fps, donc au plus deux fois pour un
flux à 60. `budgetCeilingKbps()` le calcule, `budgetBufferKbps()` dit de combien
le tampon doit dépasser ce plafond pour que `Reset` l'accepte (trois fois,
mesuré), et `setBitrate` plafonne au lieu de se faire refuser.

⚠️ **Et cette marge a été RETIRÉE le jour même, après mesure.** Elle fait ce
qu'elle promet — 40,6 → 55,4 Ko par image, +37 % de bits pour le même débit sur
le fil — mais le tampon est aussi le VBV : le pic par image passe de 60-64 Ko à
104-155 Ko, soit **26 → 42 ms d'occupation du lien** pour une seule image à
20 Mbit/s (banc §8f). Règle de Bruno : « qualité légèrement moindre sur écran
fixe acceptable ; aucune augmentation volontaire de la latence pour gagner en
netteté ». Donc `kBudgetHeadroom = 1` : le VBV revient à la règle partagée, le
budget par image ne monte pas, et `setBitrate` **plafonne** au lieu de se faire
refuser — ce qui reste strictement meilleur que le point de départ, où un
`Reset` refusé laissait le débit là où il était.

L'arithmétique de la marge est conservée entière, parce que c'est un arbitrage et
non un fait : `kBudgetHeadroom` porte la mesure et ce qu'un changement coûte.
### 21.6b Invalidation de référence — Intel est le plus simple des trois

`NumRefFrame` valait **1**, ce qui rendait la réparation par delta impossible par
construction : sans image plus ancienne à laquelle se raccrocher, une perte ne
pouvait être répondue que par une keyframe. Il vaut maintenant 4, comme le DPB
NVENC et pour la même raison.

Le mécanisme est `mfxExtAVCRefListCtrl`, attaché **par image** au
`mfxEncodeCtrl` : `LongTermRefList` marque l'image courante comme référence long
terme, `RejectedRefList` refuse celle que la perte a gâtée, `PreferredRefList`
nomme celle sur laquelle prédire, et `NumRefIdxL0Active = 1` fait de cette
préférence une obligation.

Et c'est là qu'Intel est plus commode que les deux autres : **oneVPL nomme les
images par `FrameOrder`**, c'est-à-dire par le numéro que le récepteur connaît
déjà. Pas de traduction index de slot ↔ numéro d'image — la source de deux
allers-retours sur matériel AMD (§9.10.1). `ReferenceSlots`, écrit pour AMF, se
réutilise tel quel pour l'arithmétique de portée ; seule la façon de nommer
change. Le support est **demandé au runtime** (query mode 1 avec le buffer
attaché, comme son en-tête le prescrit) et non supposé : un runtime qui n'en veut
pas laisse la session exactement comme avant, keyframes comprises, et `/start`
répond `ref_invalidation:false`.

✅ **Vérifié en vrai le 07/09** : `mw_drop_test=120` depuis bench-desk sur un flux
HEVC 1080p de l'hôte Intel — cinq pertes nommées, cinq réparations
(« oneVPL healed frame 205 with a delta against frame 204 »), **zéro IDR
demandée, zéro erreur de décodeur**, flux vivant à 16,8 ms. Coût mesuré à
l'encodage : nul (10,99 ms contre 11,22 sans).

### 21.7 Ce qui est prouvé, et ce qui ne l'est pas

Prouvé sur le banc, le 07/09/2026 :

- sonde : `available:true`, **HEVC et H.264** en matériel, pas d'AV1 (Alder
  Lake-N décode l'AV1 mais ne l'encode pas) ;
- banc `--native-bench`, bureau fixe, 20 Mbit/s, intra-refresh, 8 s par passe :

  | Codec | TU | Taille | fps | encode moy / p95 / p99 (ms) |
  |---|---|---|---|---|
  | HEVC | 1 | 1920×1080 | 59,6 | 13,33 / 18,43 / 24,58 |
  | HEVC | 4 | 1920×1080 | 59,8 | 12,59 / 18,43 / 20,48 |
  | HEVC | 7 | 1920×1080 | 59,7 | **11,46** / 15,36 / 18,43 |
  | H.264 | 1 | 1920×1080 | 58,1 | 15,53 / 20,48 / 26,62 |
  | H.264 | 4 | 1920×1080 | 59,8 | 12,61 / 16,38 / 22,53 |
  | H.264 | 7 | 1920×1080 | 59,7 | 13,38 / 18,43 / 18,43 |
  | HEVC | 7 | 2560×1440 | 59,6 | 13,43 / 18,43 / 20,48 |
  | HEVC | 1 | 2560×1440 | 58,3 | 16,47 / 20,48 / 26,62 |

  **Le TargetUsage ne se voit pas** : 1,9 ms d'écart maximum, du même ordre que
  la dispersion entre passes, et le défaut du moteur (TU7) est déjà le bord
  rapide. Même verdict que sur AMD — rien à appliquer. ⚠️ le pilote Intel ne
  rapporte **pas** de QP moyen, donc ce banc n'a aucune mesure objective de
  qualité, exactement comme AMF (§8c du banc) ;
- **premier flux navigateur depuis un hôte Intel** : Chrome 152 sur la machine
  elle-même, HEVC `hvc1.1.144.L123.B0`, `descLen=111` (VPS/SPS/PPS extraits de
  la keyframe), première image décodée 1920×1080 NV12 en matériel, 65,7 s de
  session, **1689 présents tous portés**, audio 13 142 paquets / 0 jeté, aucune
  erreur de décodeur, arrêt propre ;
- **puis depuis une autre machine, en LAN** (Chrome sur bench-desk → hôte Intel,
  appairage par PIN, `webrtc-dc-udp`) : **latence affichée 11,4 à 14,2 ms**,
  deux sessions de 289 s et 320 s, 2327 puis 2558 présents **tous portés**,
  4831 frames émises, encode 7,05 / 11,26 / 14,34 ms et total hôte
  8,84 / 13,31 / 18,43 ms (moy/p95/p99), 57 800 paquets audio / 0 jeté,
  **63 événements d'entrée injectés** (souris et clavier depuis le navigateur
  distant), arrêt propre. ⚠️ c'est la mesure qui compte : les chiffres en
  loopback (41 à 55 ms) étaient ceux d'un N95 qui encodait, décodait et servait
  la page en même temps.

  C'est aussi la seule condition où le gouverneur de lien a pu **remonter** :
  16000 → 20000 kbps en cinq paliers, toutes les hausses appliquées. En loopback
  la machine était saturée et il ne faisait que descendre — la moitié montante
  du correctif de `Reset` n'y était pas prouvée.

Pas prouvé, et à ne pas supposer :

- ⚠️ **HDR : plus vrai depuis le §21.10** — le P010 Main10 BT.2020 PQ est
  livré et vérifié le jour même. Le **4:4:4** reste refusé, pour une raison
  qu'aucun matériel ne change : la conversion produit de l'AYUV, qu'oneVPL ne
  prend pas en entrée d'encodeur ;
- **les chiffres de latence** viennent d'un N95 à 4 cœurs qui encodait, décodait
  et servait la page en même temps. Ils disent que le chemin tient 60 fps en
  1080p et en 1440p ; ils ne disent rien d'un Intel de bureau ou d'un Arc.

### 21.8 Deux bugs trouvés en montant la mesure clic→photon

Aucun des deux n'est propre à Intel ; les deux étaient invisibles jusqu'ici.

**La vue Réglages plantait dans TOUT build debug.** `SettingsView.render()`
construisait `veAlgoHtml` — le sélecteur d'algorithme d'Enhancer, qui n'existe
qu'en debug — en lisant `veCheckboxDisabled`, un `const` déclaré dix lignes plus
bas. Lire un `const` avant sa déclaration est une exception (zone morte
temporelle), donc la vue entière mourait sur « Cannot access
'veCheckboxDisabled' before initialization ». En Release `veAlgoHtml` vaut `''`
et l'expression n'est jamais évaluée : le bug ne pouvait se voir que là où on
allait justement chercher la sonde de latence. Les trois déclarations sont
remontées avant leur usage.

**Une image lente tuait la session.** `SyncOperation` répond
`MFX_WRN_IN_EXECUTION` quand son délai passe sans que l'image soit prête ; le
§21.5 avait porté le plafond de 100 ms à une seconde. Avec le clip 1440p60 qui
joue sur l'hôte **et** le navigateur qui décode sur les mêmes quatre cœurs, des
images ont dépassé la seconde, puis trois. Le plafond est maintenant de dix
secondes : un flux à une image par seconde est inutilisable, mais l'arrêter est
pire que le laisser se dégrader — la cadence, le gouverneur de lien et le
réglage de résolution du spectateur sont là pour ça.

### 21.9 Le clic→photon n'a pas été obtenu sur ce banc

Demandé, monté, non acquis — et il ne faut pas en inventer un chiffre.

Le drapeau clic→photon (`LatencyFlag`) est gaté sur `Q_OS_WIN && QT_DEBUG`. Un
vrai build Debug est inutilisable comme banc ici : notre propre passe de
conversion passe de 0,4 à 10,6 ms et l'acquisition de 0,24 à 7 ms. Un arbre
Release ne portant que `-DQT_DEBUG` a donc été bâti pour la mesure (jamais
livré). L'hôte journalise bien chaque clic reçu — « [LatencyFlag] injected click
at 1711,1056 » — mais la sonde du navigateur ne voit **jamais** les trois bandes
dans l'image décodée : `timeout` sur toutes les tentatives, avec le clip **comme**
sur un bureau fixe où le pipeline tourne à 60 fps et 11 ms d'encodage.

Ce qui a été éliminé : la page cliente n'est pas gelée (les premières tentatives
l'étaient — Chrome dé-priorise une fenêtre occultée et `requestAnimationFrame`
s'arrête ; relancé avec `--disable-features=CalculateNativeWinOcclusion`) ; le
clic part et arrive ; la session vit.

Ce qui n'est pas tranché : le drapeau est-il dessiné ? Une vérification par
capture d'écran sur le banc n'a rien vu, **mais elle ne prouve rien** — `BitBlt`
(ce qu'utilise `CopyFromScreen`) ne capture pas une fenêtre *layered*, alors que
Desktop Duplication, elle, la capturerait. La prochaine étape utile est donc de
regarder l'écran du banc autrement (Desktop Duplication, ou l'œil), pas de
recommencer la même mesure.

⚠️ **Complément du même jour, et il déplace la question.** La sonde a été
instrumentée : elle lit bien de vrais pixels, au bon endroit
(Canvas2DRenderer._readProbePixels échantillonne y = 2,5 % de la hauteur et
x = 46,5 / 50,5 / 54,5 % — exactement le rectangle du drapeau), 244 lectures sur
les 1,5 s qui suivent un clic. Elles rendent toutes **R = G = B autour de 140**,
alors que le haut de l'écran de l'hôte est à ce moment-là une page blanche. Ce
que la sonde échantillonne ne correspond donc pas au haut de l'image de l'hôte,
quel que soit le drapeau. C'est une piste **client**, indépendante d'Intel.

#### ⚠️ Élucidé le 07/09 au soir — et ce n'était ni Intel ni le client

Le drapeau n'était créé que sur l'écran **principal**. La session, elle, streame
l'écran que le spectateur a choisi : sur tout autre écran le drapeau n'était pas
dans l'image du tout. Le banc Intel a **deux adaptateurs d'écran virtuels** en
plus du sien, et un stream peut atterrir sur le mauvais — le gris uniforme lu par
la sonde était le haut d'un autre
écran que celui qu'on regardait, et la page blanche était sur le principal.

Prouvé sur bench-desk avec un harnais qui exerce le vrai `LatencyFlag` et relit
chaque sortie en Desktop Duplication : avant, l'écran secondaire rendait
`rgb(1,64,108) rgb(2,66,112) rgb(1,67,115)` — trois valeurs voisines et banales,
la même signature que le « 140 gris » du banc ; après, les deux écrans rendent
bleu/blanc/rouge. Détail dans `docs/design/glass-to-glass.md` §5 bis.

Le même harnais répond à la question laissée ouverte deux paragraphes plus haut :
**le drapeau est bien peint, et la Desktop Duplication le capture**. `BitBlt` ne
voyait rien parce qu'il ne voit pas une fenêtre *layered*, pas parce qu'il n'y
avait rien à voir.

Côté sonde, un échantillon écarté porte désormais `saw` et `via` : les pixels
lus et la surface qui les a rendus. Les trois causes possibles d'un `timeout`
(drapeau absent, mauvaise image, surface non dessinée) se lisaient toutes
« timeout » — c'est ce qui a coûté deux fausses pistes.

**Reste** : le relevé clic→photon sur le banc Intel lui-même, à refaire avec ce
correctif.

### 21.10 HDR sur Intel : FP16 scRGB → P010 → HEVC Main10 (07/09/2026)

Le §21 disait « HDR refusé par construction ». Corrigé le même jour : la chaîne
existait déjà des deux côtés — la passe de conversion sait produire du P010
BT.2020 PQ depuis la capture FP16 (§16), et le runtime Intel dit oui au 10 bits.
Il ne manquait que le chemin entre les deux.

**Trois choses, et pas une de plus** :

- `FrameInfo` en `MFX_FOURCC_P010`, `BitDepthLuma/Chroma = 10`, `Shift = 1` (le
  P010 range ses dix bits dans le HAUT de chaque échantillon 16 bits) ;
- `CodecProfile = MFX_PROFILE_HEVC_MAIN10`, nommé plutôt que laissé au runtime ;
- `mfxExtVideoSignalInfo` chaîné : `ColourPrimaries = 9` (BT.2020),
  `TransferCharacteristics = 16` (PQ), `MatrixCoefficients = 9`,
  `VideoFullRange = 0`. ⚠️ **Ces trois entiers ne sont pas un détail** : un flux
  10 bits dont la VUI dit encore BT.709 sRGB n'est refusé par personne, il est
  *affiché délavé* — ce qui se lit comme un bug de shader. Mêmes valeurs, même
  raisonnement, que les chemins NVENC et AMF.

La sonde de capacités demande maintenant le 10 bits **au runtime**, et seulement
pour HEVC : c'est le seul codec pour lequel ce chemin a un profil Main10, et
aucune puce Intel du banc n'encode l'AV1 (§21.7). `supports10Bit` répond `true`
sur l'UHD Graphics de l'N95.

**Vérifié en vrai, HDR Windows activé sur le M27Q du banc (HDMI)** :

- sonde : `[HDR]` sur le display, `hdrActive:true`, `supports10Bit:true` ;
- banc : `duplication started: 2560x1440 (HDR, FP16)` →
  `colour conversion: FP16 scRGB -> 1920x1080 P010 4:2:0 (BT.2020 PQ, limited)` →
  `oneVPL ready: HEVC HDR (Main10, BT.2020 PQ)`, keyframe 41 Ko, 131 images ;
- flux réel depuis bench-desk : le navigateur configure
  **`hvc1.2.144.L123.B0`** — profil 2 = Main10 — `descLen=115`, `hdr=true`,
  première image décodée 1920×1080. Le client sait donc que c'est du PQ BT.2020
  parce que le flux le lui dit.

⚠️ **Coût mesuré** : l'encodage 1080p60 passe de ~11 à **18,7 ms** par image sur
cette puce. Le 10 bits n'est pas gratuit sur un iGPU d'entrée de gamme.

⚠️ **Ce qui n'est PAS prouvé** : l'image sur un écran client HDR. Le M27Q est
partagé — HDMI vers le banc Intel, DisplayPort vers bench-desk — et n'affiche
qu'une entrée à la fois, donc le client était sur écran SDR : Chrome a annoncé
`hdrMode=browser` et a ramené le PQ à la main, ce qui donne l'image plate et
délavée attendue dans ce cas. Structure, couleurs et géométrie sont justes ; le
rendu HDR final demande de basculer l'entrée du moniteur et d'y activer le HDR,
ce qui est la manœuvre de Bruno, pas la mienne.

## 22. L'étage de repli : quand aucun GPU n'encode (07/09/2026)

Jusqu'ici une machine dont aucun GPU n'avait d'encodeur que nous savons piloter
était refusée (`Unavailability::NoEncoder`, « there is no software fallback »).
Deux machines de Bruno étaient dans ce cas : le portable **Windows on ARM**
(Snapdragon 7c, Adreno 618 — aucun SDK constructeur) et la **VM Debian sous
Hyper-V** (`hyperv_drm`). Demande : un repli automatique, avec la latence la plus
basse possible, ces machines étant du bas de gamme.

### 22.1 Un étage à côté des GPU, jamais devant

`Capabilities::fallbacks` est une liste de `FallbackEncoder` (API, codecs,
matériel ou non, nom), **consultée uniquement quand aucun GPU n'encode**
(`Capabilities::anyGpuEncodes()`, partagé entre `probe()` et `select()`). Elle
vit à côté de `GpuInfo::encoders` et non dedans, et c'est le point de
conception : la règle du Selector est « le GPU de l'écran, sauf s'il ne peut pas
encoder » — une entrée logicielle sur la liste d'un iGPU ferait choisir le CPU
alors qu'un NVENC dort dans la même machine. Tenu à part, le repli est
inatteignable tant qu'un GPU répond, et chaque sélection existante est identique
à l'octet près (six tests le verrouillent).

Garanties : matériel avant CPU quel que soit l'ordre de la sonde ; le GPU de
l'écran est **conservé** (capture et conversion y tournent, seul l'encodeur a
bougé — aucune copie inter-GPU introduite) ; ni HDR ni 4:4:4 ; H.264 en tête de
ce qui est offert (un hôte trop faible pour encoder en matériel ne doit pas
pousser le client vers un décodeur logiciel) ; et **jamais de suragrandissement**
— un client réglé en 1440p devant un écran 1080p aurait fait encoder 1,8× les
pixels pour aucune information (mesuré : 21 ms par image sur le Snapdragon).

Arbitrage de Bruno : « les deux, MF d'abord » — sur Windows, Media Foundation
matériel → Media Foundation logiciel → OpenH264 ; sur Linux, OpenH264. Chaque
descente est dite dans le log. Clés de banc `fallback=1|mf|mfsw|mfcpu|cpu`
(`EncoderTuning::Fallback`) : la machine est mise dans l'état exact où l'étage
sert — chaque GPU dépouillé de ses encodeurs — plutôt qu'un cas spécial.

### 22.2 Media Foundation : le silicium dont on n'a pas le SDK

`MfEncoder` est deux choses derrière une interface. Énuméré avec
`MFT_ENUM_FLAG_HARDWARE` c'est le transform d'un constructeur sur son silicium,
et les textures D3D11 du convertisseur y entrent telles quelles (DXGI device
manager) ; sans le drapeau c'est le transform logiciel de Microsoft, qui prend de
la mémoire système — chaque image traverse alors une texture de staging. La
classe les distingue en interrogeant le transform (`MF_SA_D3D11_AWARE`), jamais
son nom. `mfplat.dll` est chargé à l'exécution et jamais lié : absent des éditions
N de Windows, un import statique empêcherait MoonlightWeb de **démarrer**. Seuls
`mfuuid` et `strmiids` (tables de GUID sans DLL) sont liés.

Latence dans le vocabulaire MF : `AVLowLatencyMode`, aucune B-frame, CBR, VBV
d'une image (la règle de `RateControl.h`), GOP sans keyframe périodique, keyframe
à la demande ; chaque refus d'un transform est dit, pas fatal.

Trois pièges mesurés :

- **l'énumération matérielle est machine-entière** : sur l'écran NVIDIA de
  bench-desk elle rendait `AMDh264Encoder`, qui refusait ensuite tout type de sortie
  (son device n'est pas le nôtre). `MFTEnum2` + `MFT_ENUM_ADAPTER_LUID` demande
  le transform de l'adaptateur des images ; NVIDIA n'ayant pas de MFT, la session
  retombe proprement sur le transform logiciel ;
- **le transform AMD (H.264 et HEVC) accepte une image puis se tait** — un seul
  `METransformNeedInput`, jamais de sortie, texture ou mémoire système, 1 s
  d'attente. Ce n'est pas le pompage d'événements. Il est donc **mis à l'épreuve à
  l'init** sur une image noire ; muet, il coûte à la machine l'étage suivant au
  lieu d'une session morte. AMD n'est pas la cible (AMF le sert) ; le garde-fou
  protège le cas Qualcomm, qui est un asynchrone du même genre ;
- **le transform Qualcomm annonce 18 `NeedInput` avant sa première sortie** (la
  profondeur de sa file) ; l'attente de la première image (1 s) l'absorbe.

**Prouvé sur le banc ARM** : `QCOM Hardware Encoder - H264` / `- HEVC` trouvés,
matériels, asynchrones, D3D-aware. Banc 720p60 8 Mbit/s : 131 images / 8 s,
12,5 ms de moyenne, p99 16,8 ms. **Flux navigateur réel** depuis bench-desk par le
rendez-vous : le client préfère HEVC, le transform HEVC répond, 2 650 images
entrées / 2 650 sorties, encode 20,9 ms de moyenne à 1440p (suragrandi — d'où la
règle du §22.1), 29,5 ms de latence affichée, bureau du Snapdragon à l'écran.
Une machine où Sunshine encode en x264 logiciel streame en **matériel**.

### 22.3 OpenH264 : le dernier étage, sur le CPU

Sous-module `cisco/openh264` épinglé v2.6.0 (BSD-2, déjà sur la liste blanche),
et un CMake écrit par nous — upstream n'a que Makefile et meson —, encodeur seul
(`third_party/openh264.cmake`, listes de `codec/*/targets.mk`). Noyaux NASM sur
x86-64 (nasm cherché sur le PATH, dans `MW_NASM` et dans le cache d'outils de
vcpkg), `.S` NEON sur AArch64 avec gcc/clang, **C pur sous MSVC ARM64** — la
raison de l'arbitrage « MF d'abord » : sur le Snapdragon on n'aurait eu que le
C, et l'assembleur GAS d'upstream ne passe pas `armasm64`. Le configure dit fort
quand il compile sans noyaux.

`OpenH264Encoder` est neutre : il prend une image **I420** en mémoire système
(pas NV12, seul encodeur ici) et ne sait rien des textures ; chaque plateforme
possède la copie qui l'y amène. Latence : threads **par tranches** (une image sur
les cœurs, jamais un pipeline d'images), **aucun saut d'image** — OpenH264
avertit que sans saut « le débit ne peut pas être contrôlé » : il veut dire qu'une
image trop grosse dépasse au lieu de disparaître, ce que le gouverneur de lien
absorbe, alors qu'une image disparue se lit comme un gel —, CAVLC,
`LOW_COMPLEXITY`, denoise/scène/arrière-plan/AQ éteints, CBR, GOP sans keyframe
périodique, VUI BT.709 limité.

Deux manies mesurées : le plafond doit être **strictement** supérieur à la cible
(+1 %, au moins un kilobit) ; et `SPATIAL_LAYER_ALL` n'écrit que le chiffre
global alors que le contrôle lit la couche 0 — quatre appels pour un nombre, et
l'ordre dépend du sens (plafond d'abord à la hausse, cible d'abord à la baisse).
Sans cela la rafale de raffinement et le gouverneur étaient refusés.

`SoftwareEncoder` (Windows) : NV12 texture → staging → Map → trois plans I420 en
une passe, le chroma entrelacé séparé pendant la lecture. Mesuré sur bench-desk :
3,3 ms/image synthétique 1080p sur 4 threads, 10,7 ms/image bureau réel
relecture comprise ; flux navigateur réel 2560×1440 en `avc1.42c033`, 16,3 ms.

### 22.4 Linux sans render node : KMS → DMA-BUF mmap → CPU

La VM Debian n'a que `card0` : pas de VA-API, mais **pas d'EGL non plus** — ni
conversion ni encodage GPU. ⚠️ Le plan disait « capture X11/XShm » ; c'était
faux. Mesuré avec une sonde C (`kmsdump`) : le scanout de `hyperv_drm` est
**XR24 linéaire (modifier 0)**, l'export PRIME passe et le `mmap` du dma-buf rend
les vrais pixels (8 Mo en 4,2 ms à froid). Sur l'bench-mini le même mmap est refusé
(amdgpu, tuilé) — la voie CPU est bien celle des machines sans GPU, et seulement
d'elles. Donc `KmsCapture` reste tel quel — aucun serveur d'affichage requis, la
même propriété « capture avant le login » que la voie GPU — et ce qui change est
qui lit le buffer : `CpuConvert` (mmap du premier plan, `DMA_BUF_IOCTL_SYNC`, une
passe BGRA→I420 BT.709 en bandes de lignes sur 4 threads, `BgraToI420.h` testé
sous Windows aussi), puis `OpenH264Encoder`. Dans `LinuxSession` la conversion et
l'encodage deviennent un objet, `VideoPipeline`, parce que les deux paires
inversent la propriété de l'image (la surface de l'encodeur pour VA-API, les
plans du convertisseur pour OpenH264).

**Le premier flux a reconstruit la chaîne 5 440 fois en 50 s.** `hyperv_drm` n'a
pas de vblank (`drmWaitVBlank` → `EOPNOTSUPP`, lu comme « le CRTC s'en va » →
`Lost`) et n'a **qu'un framebuffer**, dans lequel le compositeur dessine sur
place : son id ne change jamais. Aucun des deux signaux qu'`acquire()` lit
n'existe. **Mode scruté** : un refus du vblank au premier `acquire` bascule la
capture en scrutation à la cadence de l'écran, et c'est le **contenu** qui
témoigne — le buffer tenu est mappé pour la durée de la tenue et replié en un
nombre (XOR × premier, chaque mot compte) ; empreinte nouvelle = image nouvelle,
même empreinte = `Timeout`. ~1 ms par scrutation en 1080p. Le contrat de la boucle
tient sans qu'elle bouge.

**Prouvé sur la VM** (`.deb` 0.3.0.i10, lanceur avec la capacité) : hôte natif
levé là où l'ancien build disait « operating system predates… » ; flux navigateur
depuis bench-desk par le rendez-vous : 1920×1080 `avc1.42c02a` décodé en matériel,
**8,1 ms** de latence affichée, bureau XFCE à l'écran ; un clic dans le flux
déplace le pointeur (dans l'image sur ce pilote), le dock apparaît, l'horloge
avance — l'empreinte détecte le mouvement. Étages hôte sur 291 images : convert
0,41 / 4,10 / 6,66 ms, encode 5,36 / 15,4 / 18,4 ms (moy./p95/p99).

### 22.5 Ce qui est prouvé, et ce qui ne l'est pas

Prouvé : les trois machines nommées streament en natif (Snapdragon en matériel,
VM et bench-desk-sans-GPU en CPU) ; aucune machine qui encodait déjà n'a changé
d'un octet ; la descente MF matériel → MF logiciel → OpenH264 et ses raisons dans
le log.

Non prouvé, ou non fait : le pointeur n'est pas composé dans l'image sur la voie
CPU (le client le dessine — défaut bureau ; en mode jeu il manque) ; aucun
plafond automatique quand le CPU ne suit pas (E4 réduit le budget par image et le
gouverneur le débit, mais rien ne baisse la résolution — à mesurer sur l'N95) ;
`/api/native/status` n'affiche pas l'encodeur de repli (codecs vides sur le GPU) ;
une édition N de Windows sans `mfplat.dll` n'a pas été essayée ; le transform AMD
muet n'est pas élucidé (sans conséquence : AMF le sert).

---

## 23. Glisser une fenêtre d'un écran streamé à l'autre (07/09/2026)

Deux displays de l'hôte natif streamés en parallèle, un panneau navigateur
chacun : déplacer la souris de l'un à l'autre donne déjà la sensation de deux
écrans. La demande était la suite naturelle — prendre la barre de titre d'une
fenêtre sur le display 1, bouton gauche tenu, la déposer sur le display 2, comme
on le fait sans y penser sur deux écrans physiques.

Le geste voulu, dans le détail qui compte : hors des deux panneaux **rien ne
bouge**, les images restent figées ; au survol du second panneau la fenêtre
reprend sa course ; et un bouton relâché entre les deux panneaux doit être connu
avant même que le pointeur ne revienne sur une image.

### 23.1 Trois des cinq étapes ne demandaient aucun code

- **Hors image, rien ne part.** `_absoluteMouseMessage()` (`StreamView.js`)
  renvoie `null` dès que le point sort du rectangle de l'image. Aucune position
  n'est jamais envoyée depuis un endroit que le spectateur ne regarde pas.
- **Le bouton tenu survit à la traversée.** Un bouton est un état du *bureau*,
  pas d'une session : `SendInput` a posé un `LEFTDOWN` global et rien ne le
  relâche. Le chien de garde (`InputWatchdog`, `kStaleMs = 250`) ne lâche que sur
  **silence** du client, et le panneau d'origine bat toutes les 100 ms tant qu'un
  bouton est tenu (`_sendInputState`).
- **Les deux sessions ne se marchent pas dessus.** Chacune a son `Win32Input`,
  son `m_HeldButtons` et son watchdog ; un panneau qui pose une position
  n'enregistre aucun bouton, et son `releaseAll()` de fin de session ne touche
  pas celui du voisin. La dé-duplication de `Win32Input::sendMouseButton` nommait
  déjà le cas « deux sessions qui se recouvrent ».

### 23.2 La mesure, et le mur

La seule inconnue était le navigateur, pas nous : une page autonome qui compte
les événements, ouverte dans deux fenêtres Chrome côte à côte, y répond
fidèlement et sans build. Elle a répondu trois choses d'un coup :

1. La fenêtre de départ reçoit bien son `mouseup` — 25 s après l'appui, alors que
   le bouton a été relâché sur le bureau, entre les deux fenêtres. **L'étape 5
   fonctionne**, et un bouton ne peut pas rester collé.
2. Survoler l'autre fenêtre ne fait **pas** perdre le focus à celle qui tient le
   drag. On croyait devoir empêcher `_onWindowBlur` de relâcher les boutons
   souris en plein vol : faux problème, rien à corriger.
3. La fenêtre survolée ne reçoit **rien du tout**. Zéro `mousemove`, zéro
   `mouseover`, pendant toute la durée du drag.

Le troisième point est le mur. Chrome pose une capture souris au niveau de l'OS
sur la fenêtre où l'appui a eu lieu, et **aucune API web ne permet de la rendre**.
Le geste est donc inatteignable entre deux fenêtres navigateur indépendantes —
non par un choix de conception de notre côté.

Deux routes ont été écartées explicitement, et il vaut la peine de dire pourquoi
plutôt que de les redécouvrir : **prolonger la position hors image** ferait
apparaître le curseur sur le second écran avant que le pointeur du spectateur n'y
soit arrivé, ce qui se voit ; **faire dialoguer les onglets** (origines d'écran
publiées dans les capacités, `BroadcastChannel` entre panneaux) est de la
machinerie fragile pour ce qu'elle rend.

Reste **une seule forme exacte** : les deux panneaux dans un *même document*. La
capture reste alors sur le document, le panneau qui tient le drag reçoit les
positions même au-dessus de son voisin, et il lui délègue la seule position —
chaque panneau calculant contre son propre `_mediaRect()`. Aucune extrapolation,
aucun canal entre onglets, aucune origine d'écran à publier, et au-dessus de rien
il n'y a pas de panneau donc rien n'est envoyé. Décision produit **non prise** :
cette forme impose les deux flux dans une seule fenêtre navigateur.

### 23.3 ⚠️ Ce que la mesure a trouvé au passage : l'absolu Linux ignorait l'origine

`UinputInput` mettait la position à l'échelle du device absolu — l'espace fixe
`0..32767` de la convention tablette — **sans jamais ajouter l'origine du
display**. L'en-tête l'assumait : « an absolute position is expressed in the
display's own space and needs no offset ».

C'est faux, et le raisonnement l'était de la même façon qu'un curseur de tablette
l'est : un périphérique noyau ne rapporte qu'une **fraction de son propre axe**,
et le compositeur l'étale sur le **bureau entier**, exactement comme une tablette
couvre tout le sous-main. Viser le display capturé revenait donc à viser le
bureau : correct sur un hôte à un seul écran — d'où l'invisibilité — et faux
partout ailleurs. Sur un hôte Linux à deux écrans, streamer le second plaçait
déjà le pointeur n'importe où, sans qu'aucun glissement soit en jeu.

Le correctif est celui que Windows applique depuis toujours, en deux temps
(`X11Pointer.h`, deux fonctions libres à côté de `clampIntoRect` — arithmétique
pure, donc testée sur les trois plateformes) :

1. `displayPointToDesktop()` — la surface de référence du client mise à l'échelle
   du display, **origine comprise**, et clampée dedans pour qu'un client dont le
   ratio diffère d'un pixel ne marche pas sur l'écran voisin.
2. `desktopToAbsoluteRange()` — ce point bureau exprimé en fraction du bureau,
   bornes sur bornes, la convention même de `Win32Input::desktopToAbsolute`
   contre `SM_CXVIRTUALSCREEN`.

Le bureau vient de l'union des sorties **actives** de la carte
(`LinuxSession::readInputRects()`, poussée par `IInputSink::setDesktopRect()` au
démarrage et à chaque redémarrage de capture). Lue **hors du verrou d'entrée** :
énumérer les connecteurs ouvre le périphérique DRM, et `inject()` attend sur ce
même verrou. Bureau inconnu = hôte à un écran, où le display *est* le bureau et
le calcul se réduit à ce qu'il était.

La plage du device reste `0..32767` : elle n'a pas à changer, ce qui évite de
recréer le périphérique quand la disposition des écrans bouge. Windows et macOS
n'avaient rien à corriger — `toAbsolute()` ajoute déjà l'origine, et `CgInput`
interpole entre les bornes du display en coordonnées globales.

Limite assumée : seules les sorties de la carte capturée sont comptées. Un bureau
étalé sur deux GPU compterait un bureau trop petit — et se tromperait alors du
même pointeur mal placé qu'on vient de corriger, pas de pire.

## 24. Couper le son de l'hôte sans couper la capture (07/09/2026)

Depuis D4 (§13) l'hôte natif capture sa sortie par défaut en loopback WASAPI —
et continue de l'entendre. Sunshine coupe les haut-parleurs quand le client le
demande (`localAudioPlayMode=0`), et MoonlightWeb a ce réglage depuis toujours
(`mute_host_audio`, coché par défaut) : il partait vers les hôtes GameStream et
n'était **pas lu** par le natif. Le chapitre consistait à savoir *comment* le
lire, parce que la réponse évidente est fausse.

### 24.1 La mesure qui tranche

Le loopback WASAPI prélève la **sortie du moteur audio**, avant l'endpoint. Tout
ce que le moteur fait à cette sortie atteint donc la capture ; tout ce que le
pilote fait après, non. Sonde écrite pour le mesurer (une tonalité 440 Hz jouée
par un autre processus, RMS du loopback sur 2 s par état), sur la sortie par
défaut de bench-desk — le HDMI du M27Q, pilote AMD :

| État de l'endpoint | RMS loopback | Ce que ça dit |
|---|---|---|
| Rien | 0,1726 | référence |
| `IAudioEndpointVolume::SetMute(TRUE)` | **0,1726** | le mute est fait **par le pilote**, après le prélèvement : haut-parleurs muets, capture intacte |
| `SetMasterVolumeLevelScalar(0)` | 0,0175 | le volume est appliqué **par le moteur** : la capture s'éteint avec les haut-parleurs |
| `Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE)` | — | `AUDCLNT_E_DEVICE_IN_USE` : impossible tant qu'un flux partagé (le jeu) existe |

Ce qui distingue les deux premières lignes est déclaré par le pilote :
`QueryHardwareSupport()` répond `ENDPOINT_HARDWARE_SUPPORT_MUTE` (0x2) sans le
bit volume sur cet endpoint. Un endpoint qui coupe en logiciel aurait une
première ligne à zéro — et l'on n'a **aucun** moyen de couper ses haut-parleurs
sans couper la capture. Le mode exclusif, parfois proposé pour « prendre » la
sortie, est écarté par construction : il refuse dès qu'une application partagée
joue, c'est-à-dire exactement pendant un stream.

### 24.2 Trois stratégies, dans l'ordre (`audio/windows/HostMute.h`)

1. **Mute matériel** — la sortie par défaut annonce le mute matériel : `SetMute`
   pour la session, remis à la fin. Aucune dépendance, un seul effet visible :
   l'icône du haut-parleur.
2. **Sortie virtuelle** — pas de mute matériel, mais un périphérique de lecture
   qui n'a pas de haut-parleur existe (Steam Streaming Speakers, VB-Cable,
   VoiceMeeter, Virtual Audio Cable, Virtual Desktop Audio, reconnus par leur
   nom) : il devient la sortie par défaut (rôles Console et Multimédia ;
   Communications n'est pas touché, un appel en cours n'a rien à faire sur un
   périphérique que personne n'entend) par la même interface `IPolicyConfig`
   non publiée que Sunshine et tous les commutateurs de sortie utilisent, et le
   loopback s'ouvre **dessus** — d'où l'ordre : `engage()` avant `WasapiLoopback`.
   La sortie d'avant est remise à la fin.
3. **Rien** — l'hôte continue de s'entendre, et le journal dit pourquoi en une
   phrase (« *mutes in software (the capture would go quiet too) and there is no
   virtual output to route to* »).

Deux règles de restitution : ce que l'utilisateur a changé pendant la session
lui appartient (un mute levé à la main n'est pas remis ; une sortie changée à la
main reste), et la destruction de l'objet relâche aussi — une session qui meurt
par une exception ne laisse pas la pièce muette. Ce qui n'est pas couvert : un
worker **tué** (crash, `taskkill`) laisse le mute ou la sortie en place, à
remettre dans le panneau Son.

`SessionConfig::muteHostAudio` porte le réglage (`Session.cpp` le prend dans
`m_Config.muteHostAudio`, la même source que le GameStream), `SessionInfo::
hostMuted` dit ce qui a été obtenu, la ligne « streaming … » du moteur porte
`[host muted]`.

### 24.3 Vérifié

- `test_host_mute.cpp` : la stratégie prévue a toujours une phrase ; aller-retour
  `engage()`/`release()` avec l'état de l'endpoint lu de l'extérieur avant, pendant
  (muet) et après (identique à l'avant) ; idempotence ; destructeur. 2671 checks.
- **Flux réel** Display 1 (AMF HEVC, RX 7600) depuis Chrome par le rendez-vous :
  « speakers muted on "4 - M27Q (2- AMD High Definition Audio Device)" (hardware
  mute — the capture keeps hearing the mix) », endpoint lu `muted=1` pendant le
  stream, **loopback RMS 0,274 avec la tonalité** — la capture entend ce que la
  pièce n'entend plus ; à l'arrêt, `muted=0`.

### 24.4 Ce qui reste

- ~~**macOS et Linux** : rien.~~ ✅ **macOS livré le 08/09 (§20.14)** — et la
  supposition écrite ici était fausse dans les deux sens : il n'a fallu ni
  changer de périphérique de sortie par défaut, ni s'inquiéter du volume. Le tap
  de ScreenCaptureKit n'étant sur le chemin ni du mute ni du volume (mesuré),
  rendre muette la sortie que l'utilisateur écoute suffit. ✅ **Linux livré le
  08/09 (§19.17)** — et là aussi la phrase écrite ici (« jamais un mute au
  volume, le moniteur entend ce que le sink joue ») était fausse : c'est vrai de
  PulseAudio, pas de PipeWire, dont les ports moniteur sont pris **en amont** du
  volume sauf sur les sinks virtuels. Le mute est donc la stratégie 1 et le sink
  nul le repli, à l'envers de ce qui était prévu. Les trois plateformes ont
  maintenant un `HostMute`, et **aucune des trois n'a la même réponse** — la
  seule règle qui se transporte est de mesurer avant d'écrire.
- La stratégie 2 n'a été vérifiée que par la sonde (Steam Streaming Speakers
  existe sur bench-desk mais le HDMI passe en stratégie 1) : `SetDefaultEndpoint`
  et la remise sont écrits, pas exercés en flux réel.
- Un endpoint qui **dit** matériel et refuse `SetMute` retombe sur la stratégie
  2 puis 3 — chemin écrit, jamais vu.

## 25. Sunshine sort aussi de l'installation macOS/Linux (08/09/2026)

Constat de Bruno sur une capture d'écran de l'assistant : « je pensais que
Sunshine ne faisait plus partie du processus d'installation ». Il avait raison,
et §15.5 avait cessé d'être vraie. Le 04/09, retirer Sunshine du seul installeur
Windows était le geste **complet** : macOS et Linux répondaient « no backend for
this platform in this build », et le leur retirer les aurait privées de tout
hôte. Le 05 et le 06, ces deux plates-formes ont eu leur moteur (§19, §20). La
raison est tombée, la page est restée.

### 25.1 Le verdict n'est pas « disponible », c'est « possible »

Trois endroits demandaient Sunshine : l'assistant in-app (`SetupView`), le pane
« Sunshine » du `.pkg` macOS, et le postinstall macOS qui téléchargeait un DMG et
écrivait `sunshine --creds`. Aucun ne pouvait simplement être supprimé : une
machine qui ne peut pas se diffuser doit continuer d'être aidée — **l'AppImage**
en premier, qui ne porte aucune capacité et n'a donc aucune capture (§19.8).

`/api/setup/status` gagne donc un objet `native`, et le champ sur lequel
l'assistant branche n'est **pas** `available` :

```json
"native": { "available": false, "reason": "…", "needs_permission": true, "possible": true }
```

`possible` = `available`, ou l'une des deux raisons qui veulent dire « le moteur
est là, il attend quelque chose que l'utilisateur peut donner » :
`CapturePermission` et `NoInteractiveSession`. C'est le piège que la première
version aurait eu : **macOS répond `available: false` jusqu'à ce que
l'enregistrement d'écran soit coché** (`CGPreflightScreenCaptureAccess`, §20.4),
et c'est exactement l'état d'un premier lancement. Un assistant qui branche sur
`available` enverrait chaque nouveau Mac installer un second serveur de streaming
au moment précis où il est à une case à cocher de se diffuser lui-même. Toutes
les autres raisons — pas d'API de capture, aucun encodeur, OS trop vieux, build
sans backend — veulent dire que cette machine a besoin d'un hôte à côté d'elle,
et Sunshine y est offert exactement comme avant.

### 25.2 Ce qui change pour l'utilisateur, écran par écran

| Où | Avant | Après |
|---|---|---|
| Assistant, section « Sunshine » | identifiants + case « installer automatiquement » | section « Diffuser cet ordinateur » : une ligne verte « cet ordinateur peut diffuser son propre écran » ; sur un Mac sans permission, la phrase qui dit quoi cocher et qu'il faut **relancer** l'app |
| Assistant, réapparition | revenait tant que Sunshine n'était pas installé | ne revient que si la machine n'a **aucun** hôte : `native.possible` compte autant que `sunshine.installed` |
| Assistant, écran final (macOS) | « ouvrez Sunshine et accordez-lui… » | la même phrase pour **MoonlightWeb**, et seulement quand la permission manque vraiment |
| `.pkg` macOS, pane latéral | « Sunshine » : identifiants, téléchargement du DMG, bouton Skip, sonde Basic-Auth | « Internet » : la seule question, et le texte de consentement récupère toute la bande que les identifiants occupaient |
| `.pkg` macOS, postinstall | montait un DMG, copiait `Sunshine.app`, lançait `--creds`, écrivait le mot de passe en clair dans `provisioning.json` et dans `/tmp` | plus rien de tout ça — **aucun mot de passe en clair n'est écrit sur ce disque** |
| `install.sh`, `.deb`/`.rpm` | « Sunshine n'a pas été installé : cet hôte n'a pas d'écran » | « cet hôte ne peut pas se diffuser lui-même » — la même chose, sans nommer un logiciel qui n'était pas en cause |

Ce qui **ne** bouge **pas** : `SunshineInstaller` en entier, `/api/setup/
sunshine-check`, et tout le chemin Sunshine/Apollo/Wolf. Sunshine reste un hôte
de plein droit partout, découvert et appairé depuis la page des hôtes ; l'AppImage
et les machines sans encodeur voient l'assistant d'avant, mot pour mot.

### 25.3 Vérifié, et ce qui attend la CI

- `/api/setup/status` sur bench-desk : `{"available": true, "needs_permission":
  false, "possible": true, "reason": "available"}`. Backend TNR 1082, sécurité
  405, build vert.
- Front : 5 tests neufs (`SetupNativeHost.test.js`) — la machine qui se diffuse
  ne montre aucun champ Sunshine ; un Sunshine installé **à côté** du moteur
  n'est plus ni installé ni appairé par l'assistant ; le cas permission macOS
  montre la phrase et pas l'offre Sunshine ; une machine sans moteur garde
  l'assistant d'avant ; un `status` **sans** objet `native` est traité comme
  « ne peut pas se diffuser » (un serveur plus ancien aide au lieu de se taire).
  584 tests front au total.
### 25.4 Vu à l'écran, sur le banc Mac (08/09/2026)

Les deux réserves du §25.3 sont levées, l'une entièrement, l'autre à moitié.

**L'assistant.** App au commit de l'assistant en deux pages, bâtie et déployée sur
le M1 (identité « MoonlightWeb Dev », donc les octrois TCC tiennent), puis parcourue
par Bruno : « l'assistant fonctionne bien ». Deux drapeaux ont dû être remis pour
qu'il y ait quelque chose à voir — `setup_completed`, évidemment, mais aussi
`internet_access_enabled` : **une machine dont le lien est déjà actif ne voit jamais
la page 1**, `_configPage()` l'envoie droit sur la seconde. Le raccourci est voulu ;
il cache simplement la moitié du parcours à qui veut le relire.

Ce que ce passage prouve au-delà du rendu : le consentement enregistré dans
`settings.json` est **mot pour mot ce qui était à l'écran** — 967 caractères, le
corps du texte suivi de `/ Allow the Internet link (recommended)`, la phrase même
que le bouton Accepter engage. C'était jusqu'ici la propriété d'un test unitaire
sur des clés `text:setup.*` ; elle est maintenant vérifiée de bout en bout, du
navigateur au fichier, sur une vraie machine.

**Le `.pkg`, à moitié.** `MWInternetPane.m` compile propre sur le banc en
`-Wall -Wextra` (bundle Mach-O arm64), `postinstall` passe le contrôle de syntaxe,
le `.xib` référence bien la classe renommée, et les seules occurrences de
« Sunshine » qui restent sous `installer/macos` sont de la prose (« client des hôtes
Sunshine, Apollo et Wolf ») et des commentaires d'historique — plus une ligne de
logique. ~~⚠️ **L'assemblage lui-même reste non fait**~~ ✅ **fait par la CI le
08/09** (run `34273892962`, job « Package macOS arm64 », 4 min 45) : `xcrun ibtool`
demande Xcode complet, que le banc n'a pas, mais le runner l'a. Le `.pkg` produit a
été ouvert et vérifié :

- `MWInternetPane.nib` **compilé** est dans le bundle, avec le binaire
  `MoonlightWebInstaller` (Mach-O arm64, signé), `NSMainNibFile = MWInternetPane`
  et `NSPrincipalClass = InstallerSection` ;
- `InstallerSections.plist` place le volet entre `PackageSelection` et `Install`,
  donc l'utilisateur le voit avant que quoi que ce soit ne s'installe ;
- l'app à l'intérieur est signée `com.moonlightweb.server` avec **l'exigence
  désignée attendue** — `certificate root = H"d051d7d8…"`, la constante de §20.13 —
  et `codesign --verify --deep --strict` passe : les octrois TCC survivront donc
  bien aux mises à jour, ce qui n'avait jamais été vérifié sur un artefact de CI ;
- le `.pkg` lui-même est **sans signature**, comme décidé (Developer ID Installer
  seulement) ;
- le `postinstall` ne contient aucun secret : les trois occurrences de
  « sunshine / password / creds » sont les commentaires qui expliquent leur
  disparition.

⚠️ **Piège de lecture, à ne pas refaire** : `pkgutil --expand` écrit le répertoire
des plugins comme un **fichier** `PlugIns` — un blob gzip+cpio, même forme que
`Payload`. Vu de loin il ressemble à un dossier vide, et j'ai d'abord conclu que le
volet manquait. `xar -tf` puis `gunzip -dc | cpio -i` montrent le contenu réel.

**Et installé pour de vrai, sur le banc** (08/09, artefact du run `34280361028`,
`sudo installer -pkg … -target /`) : « The upgrade was successful », la charge
utile arrive dans `/Applications` appartenant à root, `codesign --verify --deep
--strict` passe sur l'app posée, et le `postinstall` fait ce qu'il annonce —
`provisioning.json` écrit, LaunchAgent réécrit vers `/Applications`, données
utilisateur rendues à l'utilisateur, **règle de pare-feu ajoutée**. L'app démarre,
sert `/api/health` en 80 et 46152, et découvre les hôtes du LAN.

⚠️ **Ce que l'installation apprend sur TCC, et qu'il faut lire correctement** :
l'app installée répond `available: false — Screen Recording is not granted`. Ce
n'est pas une régression, c'est l'arithmétique des identités : le banc signe avec
« MoonlightWeb Dev » (`certificate leaf = H"d88095f4…"`) et la CI avec l'identité
de release (`certificate root = H"d051d7d8…"`). Deux exigences désignées
différentes, donc deux octrois différents — l'app installée en demande un, **une
fois**, et le garde ensuite d'une mise à jour à l'autre puisque la racine, elle,
ne bouge plus. Le message de la sonde dit exactement quelle case cocher.

#### ✅ Le volet Internet s'affiche — et deux défauts d'apparence (09/09/2026)

**Vu à l'écran par Bruno, sur le paquet livré tel quel** : la barre latérale
d'Installer.app affiche Introduction · License · Destination Select ·
Installation Type · **Internet** · Installation · Summary, et le volet montre sa
phrase et sa case cochée. Le volet fonctionne, sur macOS 15.6.1, sans correctif.

⚠️ **Ce paragraphe remplace une conclusion fausse, et la méthode qui l'a produite
mérite d'être retenue.** Piloter Installer.app par SSH avait donné quatre
« signaux » convergents — parcours trop court, bundle absent de `lsof`, pas de
fichier de transmission, journal muet — tous **artefacts du même blocage** : une
feuille modale de macOS (« this package will run a program… », bouton **Allow**)
arrêtait le parcours au premier écran. Le volet n'était donc jamais atteint : il
n'avait aucune raison d'écrire son fichier, et le processus interrogé n'y était
pas encore arrivé. Quatre observations tirées d'un même montage cassé ne sont pas
quatre preuves indépendantes — c'est une seule erreur, comptée quatre fois. La
seule mesure qui tranchait était l'œil d'un humain devant la machine.

**Sur `NSPrincipalClass`, qui n'était donc pas le problème.** Le `Info.plist`
déclare `NSPrincipalClass = InstallerSection`, la classe **de base** du
framework et non une sous-classe — ce qui s'écarte du contrat d'Apple, et ce que
la mesure ci-dessous confirme. Mais puisque le volet s'affiche, macOS l'accepte :
c'est une entorse sans conséquence, à laisser telle quelle plutôt qu'à
« corriger » sur du code qui marche. Relevé en chargeant les deux bundles dans le
runtime Objective-C (`NSBundle` + `principalClass`, hors de tout Installer.app) :

| | livré par la CI | patché |
|---|---|---|
| `NSPrincipalClass` déclaré | `InstallerSection` | `MWInternetSection` |
| le bundle se charge | oui | oui |
| classe principale résolue | `InstallerSection` | `MWInternetSection` |
| **est la classe de base elle-même** | **OUI** | non |
| **est une sous-classe** | **non** | **OUI** |
| `MWInternetPane` enregistrée | oui | oui |

⚠️ **L'expérience était viciée, et le montage l'a montré tout seul.** Le paquet
patché, ouvert par Bruno, ne montrait plus le volet — mais un second paquet,
assemblé de la même façon et dont le `Info.plist` n'avait **pas** été touché, ne
le montrait pas davantage. Le point commun n'est donc pas la classe principale,
c'est **la manière de réassembler le paquet** : `pkgutil --expand` → remplacer le
blob `PlugIns` → `pkgutil --flatten` perd la section des plugins, alors même que
le blob se relit correctement. Un paquet ne se rebricole pas à la main ; il
s'assemble avec `productbuild --plugins`, ce que fait `build-pkg.sh`, et ce qu'il
faut faire aussi pour tout paquet de banc (la recette sans Xcode : `pkgutil
--flatten` sur le *composant* seul pour le remettre à plat, puis `productbuild`
avec la Distribution, les Resources et le dossier de plugins).

Conclusion sur `NSPrincipalClass` : **non tranchée, et sans intérêt pratique**.
Ce qui est livré marche ; l'entorse au contrat d'Apple est réelle mais sans
conséquence observable, donc on n'y touche pas. Le paquet de test a par ailleurs
échoué à l'installation faute de privilèges — encore un symptôme du même
réassemblage bricolé, pas du paquet de la CI, qui s'installe.

**Deux pièges de banc à retenir**, qui ont coûté plusieurs passes chacun :
`screencapture` lancé depuis une session SSH n'a pas l'enregistrement d'écran, et
macOS lui rend alors le fond d'écran et la barre de menus **en effaçant les
fenêtres des autres applications** — les captures paraissent montrer un bureau
vide alors que la fenêtre est bien là (position et taille lues par l'API
d'accessibilité). Et le premier écran d'Installer est une **feuille modale** de
macOS (« this package will run a program… », bouton **Allow**, pas « Agree ») :
tant qu'elle n'est pas acquittée, aucun clic « Continue » n'avance.
