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
3. **L'époque du relais.** Le moteur date en absolu ; le relais, lui, attend le
   contrat du shim GameStream : un temps de présentation **relatif à la première
   frame**, qu'il rajoute à l'époque pour retrouver l'horloge. `NativeMediaEngine`
   fait la soustraction (époque = présent de la première frame). Livrer l'absolu
   comptait l'horloge deux fois : le `backendTs` du client avançait à 2×, et
   toute mesure de gigue en aval était fausse (bug B2, corrigé le 02/09/2026).

### Linux / macOS

PipeWire via le portail ScreenCast (DMA-BUF, `restore_token` pour ne demander
l'autorisation qu'une fois) et ScreenCaptureKit. Non implémentés.

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
| `ULTRA_LOW_LATENCY`, preset P4 | P1 serait plus rapide mais visiblement plus mou ; l'écart est sous la milliseconde sur un encodeur moderne |
| **Aucune B-frame** (`frameIntervalP = 1`) | elle référencerait une image pas encore envoyée → une trame entière retenue |
| **GOP infini + intra-refresh** | une keyframe est un pic de débit ; `MediaTrackRelay` documente ce que ces pics font à un lien congestionné (perte → tempête de PLI → effondrement) |
| **CBR, VBV = une frame** | c'est le VBV qui impose réellement la faible latence : aucune frame ne peut être si grosse qu'elle mette plusieurs temps de trame à passer |
| **SPS/PPS à chaque keyframe** | le décodeur du navigateur s'y configure ; un client qui arrive en retard doit pouvoir démarrer sur la suivante |

Le zéro-copie tient : NVENC enregistre directement la texture D3D11 écrite par
la passe de conversion, sur le même adaptateur.

`setBitrate()` reconfigure sans redémarrer la session — la base du rate-control
piloté par le retour réel du client.

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
puis la **rafale de raffinement** ré-encode cette image pendant 1 s à ×6 de
budget jusqu'à convergence. Un plancher plus haut ne ferait que continuer après
ça, au budget ordinaire, sur une image que l'encodeur a déclarée finie — donc en
mode bureau il n'apporte rien, quel que soit l'appareil. Une plateforme au
signal plus flou remonte ce nombre ici, et aucun client ne l'apprend.

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
| libopus, OpenH264, libva, PipeWire | BSD/MIT | OK | à venir |
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
| Module isolé + garde de licence | **Audio (WASAPI loopback → Opus)** |
| `IMediaEngine`, relais découplés | HDR (P010 + PQ) |
| Sonde displays/GPU + association | WGC en repli |
| Capture DXGI (0,06 ms) | Lanceur de session console (service Windows) |
| Conversion NV12 + AYUV 4:4:4 | UI (grille d'écrans, ligne GPU/encodeur) |
| NVENC (3,46 ms), AMF (3,70 ms), oneVPL (écrit, non exécuté) | Retrait de Sunshine de l'installeur |
| Intra-refresh sur les trois encodeurs + ride-out client | Linux, macOS |
| Curseur composé, plancher sur écran immobile choisi par le client (§9.1) | Benchmarks vs Sunshine |
| Clavier/souris (`SendInput`), manette (ViGEm) + rumble | |
| Installeur : ViGEmBus en silencieux | |

**Le chemin est complet côté serveur**, et jouable : le host natif apparaît sans
pairing, un clic sur un écran construit un `NativeMediaEngine` qui alimente le
relais WebRTC existant, et clavier/souris/manette reviennent par le
DataChannel d'entrée. Le chemin Sunshine/Wolf/MultiSeat est intact.

**Ce qui manque vraiment** : l'audio. Un stream muet reste utilisable, mais ce
n'est pas fini.

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

### ✅ Vérifié de bout en bout (31/08/2026)

Une **image réelle du bureau, dans un navigateur**, par le moteur natif :

- host `DualRTX — MoonlightWeb Host` **READY**, aucun pairing demandé ;
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
