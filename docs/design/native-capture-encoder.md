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

Deux points de justesse invisibles hors exécution :

1. **Les horloges.** DXGI date en QPC, le reste du moteur en `steady_clock`.
   Même cadence, origines différentes : sans le couple de calibration pris au
   démarrage, la latence de capture serait un écart entre deux époques sans
   rapport — grand, stable, et vide de sens.
2. **Un présent à zéro n'est pas une frame.** DXGI réveille aussi sur un simple
   mouvement de pointeur ; le compter comme une frame enverrait un doublon
   horodaté n'importe comment.

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

---

## 10. Host natif dans l'UI

- **Aucun pairing** : ni PIN, ni certificat, ni association. Il n'y a pas deux
  parties à authentifier.
- **Aucun appel réseau** au lancement : `launch()` renvoie un descripteur
  nommant un display.
- **Les displays SONT la liste d'apps** : une carte par écran
  (`Display 1 — 2560×1440 · 144 Hz`). La grille existante devient le sélecteur,
  zéro nouvelle UI, et avec un seul écran c'est une carte et un clic.
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
| libopus, OpenH264, AMF, oneVPL, libva, PipeWire, ViGEmClient | BSD/MIT | OK | à venir |
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
| Module isolé + garde de licence | Audio (WASAPI loopback → Opus) |
| `IMediaEngine`, relais découplés | Input (`SendInput`, ViGEm) |
| Sonde displays/GPU + association | HDR (P010 + PQ) |
| Capture DXGI (0,06 ms) | AMF, oneVPL, WGC en repli |
| Conversion NV12 + AYUV 4:4:4 | Lanceur de session console (service Windows) |
| NVENC : capacités réelles, encodage (3,46 ms) | UI (grille d'écrans, ligne GPU/encodeur) |
| Boucle de session (1 thread, 0 file) | Installeurs, retrait de Sunshine |
| `NativeMediaEngine` + branche `Session.cpp` | Linux, macOS |
| Host « `<hostname>` — MoonlightWeb Host » dans la liste | Benchmarks vs Sunshine |
| 123 assertions, ctest 3/3 | |

**Le chemin est complet côté serveur** : le host natif apparaît, sans pairing,
et un clic sur un écran construit un `NativeMediaEngine` qui alimente le relais
WebRTC existant. Le chemin Sunshine/Wolf/MultiSeat est intact.

Ce qui manque pour une expérience finie : l'audio et l'input (un stream vidéo
seul n'est pas jouable), puis l'UI et les installeurs.

### Vérifications non faites

1. **Un stream Sunshine et un stream Wolf réels**, de bout en bout. Le refactor
   `IMediaEngine` est purement typologique — aucun corps de méthode modifié —
   mais il touche les trois relais, et cela demande un banc réel.
2. **Une image native dans un navigateur.** Chaque étage est vérifié
   séparément (la capture produit des pixels, l'encodeur un Annex-B conforme,
   le host apparaît), mais l'assemblage complet jusqu'au décodeur du navigateur
   n'a pas encore été observé.
