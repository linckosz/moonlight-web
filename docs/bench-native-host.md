# Banc d'encodeur du host natif — campagne du 04/09/2026

> Phase E du plan v2. Instrument : `MoonlightWeb.exe --native-bench` (design
> §14), qui capture, convertit et encode un écran vers un puits — pas de réseau,
> pas de navigateur — et donne par frame l'heure de chaque étape, la taille et
> le QP moyen. Ce document est le **livrable avant décision** : les chiffres,
> ce qu'ils disent, une recommandation. Rien n'est appliqué au produit avant
> confirmation (règle 0.2.2 du plan).

## 1. Banc

| | |
|---|---|
| Machine | DualRTX : 2× RTX 5060 Ti (pilote 32.0.15.9636), iGPU AMD Radeon (Ryzen, AMF, pilote 32.0.21045), Windows 11 |
| Écran capturé | Display 3 = écran virtuel VDD 2560×1440 @ 165 Hz sur la RTX 5060 Ti n° 1 |
| Contenus | **défilement** : page de texte serif qui défile à 600 px/s, pilotée par `requestAnimationFrame` (une image par rafraîchissement, ~160 présents/s) · **jeu** : clip Super Mario Galaxy 1080p 60 fps étiré plein écran, relancé de zéro avant chaque passe (démarrage 3 s après chargement, banc lancé 3,5 s après) · **fixe** : la même page de texte sans mouvement |
| Réglage de référence | 2560×1440, fps = celui de l'écran (165), CBR 40 Mbit/s, HEVC 4:2:0, keyframes à la demande, 10 s par passe |
| Reproductibilité | réglage courant répété en tête et en queue de chaque matrice : encode 4,67 / 4,65 / 4,67 / 4,69 ms (défilement), 6,55 / 6,52 ms (jeu) |
| Colonnes | `encode` = t₂ converti → t₃ bitstream lisible, moyenne / p95 / p99 ms · `total` = t₀ présent → t₃ · `Ko` = octets par frame delta, moyenne · `QP` = quantificateur moyen rapporté par l'encodeur (plus bas = plus net ; q-index 0–255 pour l'AV1, pas comparable) |

Ce que le réglage courant vaut, tel que le pilote le livre — lu dans le log, pas
supposé : **NVENC P4 / ultra-low-latency active le multipass quart de résolution,
AQ spatial et temporel éteints, pas de lookahead** ; **AMF ultra-low-latency =
qualité « speed », pré-analyse éteinte, VBAQ éteint**.

## 2. NVENC (RTX 5060 Ti) — défilement de texte, 1440p à 165 présents/s

| Réglage | encode ms (moy / p95 / p99) | total moy | Ko/frame | QP |
|---|---|---|---|---|
| **P4/ULL (courant)** | **4,67 / 5,63 / 6,00** | 5,32 | 28,1 | **26** |
| P1 | 2,71 / 4,10 / 4,61 | 3,14 | 29,4 | 31 |
| P2 | 4,36 / 5,63 / 5,63 | 4,88 | 29,4 | 31 |
| P3 | 4,65 / 5,63 / 6,14 | 5,29 | 28,5 | 27 |
| P5 | 4,65 / 5,63 / 6,14 | 5,28 | 28,3 | 26 |
| P6 | 4,67 / 5,63 / 6,14 | 5,31 | 28,2 | 26 |
| P7 | 4,89 / 5,63 / 6,14 | 5,60 | 28,2 | 26 |
| P4, tuning LL (multipass éteint par le preset) | 4,11 / 5,12 / 5,63 | 4,58 | 27,8 | 26 |
| P4, multipass off | 4,28 / 5,12 / 5,63 | 4,75 | 28,0 | 26 |
| P4, multipass full | 5,08 / 6,14 / 6,14 | 5,94 | 28,6 | 26 |
| P4, AQ spatial | 5,00 / 6,14 / 6,66 | 5,76 | 25,6 | 21 |
| P4, AQ temporel | 4,86 / 6,14 / 6,66 | 5,53 | 28,1 | 26 |
| P4, VBV = 1 frame (29 Ko, sans plancher) | 4,62 / 5,63 / 6,14 | 5,11 | 24,2 | 27 |
| P4, VBV = 2 frames (59 Ko) | 4,67 / 5,63 / 6,14 | 5,26 | 23,7 | 27 |
| P4, H.264 | 4,38 / 5,63 / 5,63 | 4,92 | 28,4 | 27 |
| P4, AV1 | 4,26 / 5,12 / 6,14 | 4,81 | 29,6 | q 90 |
| P4, HEVC 4:4:4 | 4,81 / 5,63 / 6,14 | 5,56 | 28,1 | 26 |
| P4, intra-refresh | 4,74 / 5,63 / 6,14 | 5,44 | 28,2 | 26 |
| P4, fps réglé 60 | 5,56 / 6,66 / 7,17 | 6,03 | 68,2 | 20 |
| **P1, multipass off** | **2,21 / 3,58 / 3,84** | 2,78 | 28,4 | 31 |
| P1, AQ spatial | 3,07 / 4,61 / 5,12 | 3,53 | 29,2 | 25 |
| P1, AV1 | 2,88 / 4,10 / 4,61 | 3,38 | 29,6 | q 138 |
| P1, H.264 | 3,49 / 4,61 / 5,12 | 4,03 | 28,6 | 27 |
| P1, VBV 2 frames | 2,80 / 4,10 / 4,61 | 3,33 | 26,6 | 31 |
| P2, multipass off | 4,03 / 5,12 / 5,63 | 4,67 | 28,2 | 30 |

## 3. NVENC — clip de jeu, 1440p, 60 présents/s

| Réglage | encode ms (moy / p95 / p99) | total moy | Ko/frame | QP |
|---|---|---|---|---|
| **P4/ULL (courant)** | **6,55 / 9,22 / 10,24** | 7,10 | 26,8 | **18** |
| P1 | 3,11 / 4,61 / 4,61 | 3,71 | 27,1 | 19 |
| P2 | 5,30 / 7,17 / 7,17 | 5,86 | 27,4 | 19 |
| P3 | 6,19 / 8,19 / 9,06 | 6,79 | 26,9 | 18 |
| P5 | 6,54 / 9,22 / 10,24 | 7,16 | 27,2 | 18 |
| P6 | 6,64 / 9,22 / 10,24 | 7,22 | 27,1 | 18 |
| P7 | 7,25 / 9,22 / 11,26 | 7,84 | 27,1 | 18 |
| P4, tuning LL | 6,12 / 9,22 / 11,25 | 6,75 | 26,8 | 19 |
| P4, multipass off | 6,12 / 9,22 / 11,26 | 6,69 | 26,7 | 19 |
| P4, multipass full | 8,10 / 12,29 / 13,31 | 8,69 | 27,5 | 19 |
| P4, AQ spatial | 7,17 / 10,24 / 11,26 | 7,74 | 27,4 | 17 |
| P4, AQ temporel | 6,74 / 10,24 / 11,26 | 7,25 | 27,5 | 18 |
| P4, VBV 1 frame | 6,52 / 9,22 / 10,24 | 7,12 | 22,4 | 19 |
| P4, VBV 2 frames | 6,62 / 9,22 / 10,24 | 7,17 | 24,4 | 19 |
| P4, H.264 | 5,58 / 7,68 / 7,68 | 6,11 | 27,1 | 20 |
| P4, AV1 | 5,19 / 7,17 / 7,17 | 5,79 | 25,6 | q 45 |
| P4, HEVC 4:4:4 | 6,90 / 9,22 / 10,24 | 7,49 | 27,4 | 19 |
| P4, intra-refresh | 6,66 / 9,22 / 10,24 | 7,27 | 27,5 | 19 |
| P4, fps réglé 60 | 6,95 / 10,24 / 11,26 | 7,59 | 65,5 | 12 |
| **P1, multipass off** | **2,52 / 3,84 / 4,10** | 3,19 | 26,6 | 19 |
| P1, AQ spatial | 4,14 / 6,14 / 6,66 | 4,79 | 27,2 | 18 |
| P1, AV1 | 3,37 / 4,61 / 5,12 | 4,08 | 25,4 | q 45 |
| P1, H.264 | 3,64 / 5,63 / 5,63 | 4,25 | 27,3 | 20 |
| P1, fps réglé 60 | 3,78 / 5,12 / 5,63 | 4,47 | 64,8 | 13 |
| P1, VBV 2 frames | 3,12 / 4,61 / 4,61 | 3,73 | 24,6 | 19 |
| P2, multipass off | 4,71 / 6,66 / 7,17 | 5,27 | 26,7 | 19 |

## 4. NVENC — écran fixe : la première keyframe et la rafale de raffinement

Ce que l'on regarde ici n'est pas l'encode moyen (rien ne bouge) mais la
**trajectoire des passes de raffinement** (§9.1 du design) : la keyframe, puis
les passes au budget ×3 jusqu'à convergence. Octets et QP des dix premières
frames, 40 Mbit/s, 1440p, VBV 81 Ko (plancher 1/60 s).

| Réglage | keyframe | passes (Ko → QP) | verdict |
|---|---|---|---|
| P4 (courant) | 64 Ko, QP 45 | 163 → 32 · 91 → 25 · 88 → 19 · 77 → 14 · 33 → 12 · 35 → 10 · 33 → **8** | converge en 8 passes |
| P1 | 64 Ko, QP 45 | 149 → 33 · 34 → 31 · 96 → 25 · 63 → 20 · 86 → 15 · 79 → 10 · 19 → **8** | idem, encode 2,2–2,7 ms par passe |
| P1, AQ spatial | 59 Ko, QP 40 | 164 → 26 · 94 → 19 · 89 → 13 · 72 → 9 · 21 → 7 · 33 → **4** | converge, plus bas encore |
| **P1, multipass off** | 89 Ko, QP 45 | 53 → 39 · 9 → 38 · 19 → 36 · 13 → 34 · 25 → 33 · 12 → 31 · 27 → 30 · 12 → **29** | **ne converge pas** : les passes n'utilisent pas le budget, l'image reste à QP 29 |
| P4, VBV 1 frame (29 Ko) | 32 Ko, **QP 50** | 84 → 38 · 88 → 32 · … · 38 → 9 | la première image molle que le plancher de RateControl.h corrige |
| P4, fps réglé 60 | 76 Ko, QP 44 | 184 → 25 · 170 → 15 · 114 → 8 · 63 → 5 · 26 → 4 | converge en 6 passes |

## 5. AMF (iGPU AMD Radeon, par copie inter-GPU)

L'iGPU ne pilote aucun écran : l'encodeur y est atteint par le pont inter-GPU
livré avec ce banc (`gpu=2`, `CrossGpuBridge`), soit une trame de 14 Mo qui
traverse la mémoire système avant la conversion. Le coût du pont est **dans
l'étape convert** (≈ 2 à 5 ms ici) ; la colonne `encode` mesure l'encodeur seul.
**Ce pilote ne rapporte aucun QP** (`GetProperty` échoue sur le buffer de sortie,
comme sur la RX 7600 le 02/09) : la qualité n'a pas de mesure objective côté AMD.

| Contenu · réglage | encode ms (moy / p95 / p99) | total moy | Ko/frame | cadence capturée |
|---|---|---|---|---|
| jeu · speed (courant) | 7,92 / 10,24 / 10,24 | 9,98 | 27,0 | 59,9 fps |
| jeu · balanced | 8,01 / 10,24 / 10,24 | 10,11 | 27,2 | 59,9 |
| jeu · quality | 9,24 / 11,26 / 11,26 | 11,68 | 27,0 | 59,8 |
| jeu · pré-analyse | — | — | — | **la session meurt** (« the AMD encoder stopped producing frames ») |
| jeu · VBAQ | 8,08 / 10,24 / 10,24 | 10,31 | 26,9 | 59,8 |
| jeu · VBV 1 frame | 8,30 / 10,24 / 10,24 | 10,82 | 27,3 | 59,9 |
| jeu · VBV 2 frames | 8,14 / 10,24 / 10,24 | 10,43 | 27,2 | 59,9 |
| jeu · H.264 | 7,92 / 10,24 / 10,24 | 10,18 | 24,3 | 59,9 |
| jeu · intra-refresh | 7,98 / 10,24 / 10,24 | 10,11 | 27,0 | 59,9 |
| jeu · fps réglé 60 | 8,21 / 10,24 / 10,24 | 10,40 | 73,0 | 55,5 |
| jeu · **1920×1080** | 5,94 / 7,68 / 8,19 | 8,05 | 28,0 | 59,9 |
| défilement · speed (courant) | 7,84 / 10,24 / 10,24 | 12,83 | 29,6 | **102 fps** (sur 160 présentés) |
| défilement · balanced | 7,84 / 10,24 / 10,24 | 12,65 | 29,6 | 102 |
| défilement · quality | 9,12 / 11,26 / 11,26 | 14,14 | 29,6 | 89,5 |
| défilement · VBAQ | 7,75 / 10,24 / 10,24 | 12,56 | 29,6 | 104 |
| défilement · H.264 | 7,68 / 9,22 / 10,24 | 12,61 | 29,6 | 104 |
| défilement · 1920×1080 | 5,50 / 7,17 / 7,68 | 9,67 | 29,6 | 136 |
| fixe · speed | keyframe 132 Ko, passes 87 · 77 · 65 · 42 · 80 · 24 Ko puis 0 | | | |

Lecture : sur cet iGPU, les presets AMF ne bougent presque rien (« speed » et
« balanced » sont le même chiffre, « quality » coûte 1,3 ms) ; le VBAQ et le VBV
sont neutres en temps ; la **pré-analyse tue l'encodeur** en ULL/CBR (l'en-tête
AMF la documente pour le VBR à pic contraint seulement). À 1440p l'iGPU **ne
tient pas 165 fps** (7,8 ms d'encode + le pont > 6 ms de période : 102 images/s
capturées sur 160) ; à 1080p il en tient 136. C'est un iGPU : le résultat vaut
pour la classe « portable AMD sans carte », pas pour une RX 7600, qui reste à
mesurer quand elle sera rebranchée.

## 6. Côté client — latence de décodage par codec (mesure A3)

Flux réels depuis l'instance dev (`--dev`), host natif sur Display 3, clip de
jeu, 1440p60 40 Mbit/s, client Chrome sur la même machine, transport
`webrtc-dc-udp`, overlay de stats après 30 s. Moyenne / p99 ms.

| Codec | décodage | rendu | host total | latence affichée |
|---|---|---|---|---|
| HEVC (`hvc1.1.144.L150`) | 1,1 / 2,5 | 0,6 / 2,4 | 6,8 | 9,2 ms |
| AV1 | 0,9 / 2,4 | 0,4 / 0,8 | 5,5 | 7,4 ms |
| H.264 (`avc1.640033`) **avant** correctif | **200,8 / 206,3** | 0,4 / 1,4 | 6,3 | **208 ms** |
| H.264 **après** correctif | 0,9 / 2,7 | 0,4 / 0,8 | 6,1 | 8,3 ms |

Deux bugs trouvés par cette mesure, corrigés dans la foulée (règle 0.2.6 : les
bugs vus au passage se corrigent) :

- **H.264 : 200 ms de décodage.** Le SPS NVENC ne portait pas de
  `bitstream_restriction` ; sans `max_num_reorder_frames`, le décodeur D3D11 de
  Chrome retient un DPB entier avant d'afficher — une douzaine d'images à
  1440p60, sur un flux sans aucune B-frame. `bitstreamRestrictionFlag = 1` dans
  les paramètres VUI : 208 → 8 ms. Le HEVC le portait déjà par défaut.
- **AV1 jamais sélectionnable en natif.** `NativeMediaEngine` testait le masque
  client avec `0x0200`, qui est **HEVC Main10**, pas AV1 (`0x1000`). Un client
  demandant AV1 obtenait du HEVC ; un client demandant du HEVC HDR aurait été lu
  comme demandant de l'AV1. Corrigé par masques par codec en entrée
  (`0xF000` / `0x0F00` / `0x000F`) et format négocié fidèle au profil en sortie.

Aucun codec ne coûte plus de 1 ms de décodage de plus qu'un autre sur ce
Chrome/RTX : le choix de codec peut se faire sur le coût hôte et la licence.

## 7. Ce que les chiffres disent

1. **Le preset est le levier, et il est grand.** Sur ce NVENC (Blackwell), P1
   encode en **2,7 ms** (texte) et **3,1 ms** (jeu) contre 4,7 et 6,5 pour P4 :
   −2 à −3,4 ms de moyenne, −2 à −5,6 ms de p99, à débit égal. Le plan supposait
   « moins d'une milliseconde d'écart entre P1 et P4 » : c'était faux d'un
   facteur trois. P2 est le pire des deux mondes (lent **et** QP de P1) ; P3, P5,
   P6 valent P4 ; P7 coûte 0,2–0,7 ms de plus pour le même QP.
2. **Le prix de P1 en qualité dépend du contenu.** Jeu : +1 de QP (19 vs 18),
   indiscernable par construction (le protocole §5 tolère 2). Texte qui défile :
   +5 (31 vs 26), au-dessus de la tolérance ; **c'est le seul cas où P1 paie**.
   Écran fixe : identique (la rafale de raffinement converge pareil à QP 8).
3. **Multipass.** Le quart de résolution que le preset ULL active coûte 0,4 ms
   (P4) à 0,6 ms (P1) sur le mouvement, pour le même QP. Mais l'éteindre **casse
   la rafale de raffinement de l'écran fixe** : sans première passe, le rate
   control d'un écran qui vient de s'arrêter n'ose pas dépenser le budget et
   l'image reste à QP 29 au lieu de 8. Ce cas est celui pour lequel §9.1 existe,
   et c'est celui de chaque pause de souris sur un bureau. **Garder le multipass
   quart.** Le tuning LL n'est que ULL avec multipass éteint : même verdict.
4. **AQ spatial** : −5 de QP moyen sur le texte à P1 (25 vs 31), +0,4 ms à P1 /
   +0,3 à P4 ; sur le jeu +1 ms à P1 pour −1 de QP. Le QP moyen sous AQ n'est plus
   tout à fait la même grandeur (l'AQ le redistribue), mais la rafale de
   raffinement converge et le texte gagne. **Candidat sérieux pour compenser P1
   sur le texte**, à trancher à l'œil. AQ temporel : rien, +0,2 ms. À écarter.
5. **VBV.** Le plancher 1/60 s (RateControl.h) tient : sans lui la keyframe sort
   à 32 Ko / QP 50 ; le VBV 1 frame et 2 frames ne changent rien au temps
   d'encode et font des frames plus petites que le budget (22–24 Ko sur 30
   possibles), c'est du débit non dépensé. **Ne rien changer.**
6. **Codec.** À preset égal : AV1 < H.264 < HEVC en temps d'encode (jeu, P4 :
   5,2 / 5,6 / 6,5 ms ; P1 : 3,4 / 3,6 / 3,1). Le décodage client est équivalent.
   La préférence AV1 (licence libre) du moteur est confortée, à condition du
   correctif de masque ci-dessus.
7. **4:4:4** : +0,15 à +0,35 ms. **Intra-refresh** : +0,1 ms, même QP, même
   taille — gratuit. **fps réglé 60 sur écran 165** : frames plus grosses (68 Ko)
   donc encode plus long (+0,9 ms à P4, +0,7 à P1), c'est le budget par frame qui
   change, pas l'encodeur.
8. **AMF (iGPU)** : rien à régler qui compte ; le seul levier est la résolution.
   La pré-analyse est à ne jamais activer en ULL/CBR.

## 8. Recommandation

**NVENC : passer de P4 à P1, multipass quart conservé, tout le reste inchangé.**

- Jeu : −3,4 ms de moyenne et −6 ms de p99 sur l'étape encode, pour +1 de QP.
  C'est le gain le plus grand mesuré sur toute la chaîne hôte depuis la phase C
  (queue 0,22 → 0,16 ms) — l'étape encode était les deux tiers du temps hôte.
- Texte défilant : +5 de QP. Deux façons de le régler, à trancher **à l'œil**
  par Bruno (protocole §5 : A/B en aveugle, netteté du texte, fourmillement en
  mouvement, jank) :
  - **P1 seul** : 2,7 ms, QP 31 sur le texte en mouvement (le texte **fixe** est
    identique à P4 : la rafale converge à QP 8 dans les deux cas — ce qui se lit
    à l'arrêt ne change pas) ;
  - **P1 + AQ spatial** : 3,1 ms, QP 25 sur le texte en mouvement, +1 ms sur le
    jeu (4,1 ms, toujours −2,4 sur P4).
- Ma préférence : **P1 seul**. Le texte qui défile est le seul cas perdant, la
  perte n'est visible que pendant le défilement, et c'est le jeu qui fixe
  l'exigence (§0.1). Si l'A/B montre un fourmillement gênant sur le texte,
  P1 + AQ est la sortie, pour 0,4 ms.

**À ne pas toucher** : multipass (quart), VBV (plancher 1/60 s), tuning ULL,
AQ temporel éteint, lookahead éteint, préférence AV1 > HEVC > H.264.

**AMF** : rien à appliquer sur l'iGPU. La RX 7600 sera mesurée quand elle sera
rebranchée (même matrice, même script) ; d'ici là le réglage courant (speed,
sans pré-analyse) est aussi le plus rapide mesuré. **oneVPL** : pas de GPU
Intel, la matrice `tu=1..7` attend.

## 9. Pour l'A/B

Le banc encode vers un puits ; l'A/B se fait sur un vrai flux. Une session
native lancée avec la variable d'environnement `MW_NATIVE_TUNING` prend les
mêmes clés que le banc :

```
set MW_NATIVE_TUNING=preset=1
MoonlightWeb.exe --dev --log dev-p1.log
```

puis `preset=1,aq=1`, puis rien (P4). Le log dit « MW_NATIVE_TUNING in effect »
et la ligne « NVENC ready » porte `[bench: preset=P1]`. La variable n'est lue que
par le moteur natif, jamais posée par le produit.

## 10. Reproduire

```
# lister écrans et GPU
MoonlightWeb.exe --native-bench display=-1
# une passe
MoonlightWeb.exe --native-bench display=1,seconds=10,bitrate=40000,preset=1,out=p1.csv
# l'iGPU AMD par le pont inter-GPU
MoonlightWeb.exe --native-bench display=1,gpu=2,seconds=10,bitrate=40000
```

Clés d'encodeur : `preset=1..7`, `tuning=ull|ll`, `multipass=off|quarter|full`,
`aq=0|1`, `taq=0|1`, `preanalysis=0|1`, `quality=speed|balanced|quality`,
`tu=1..7`, `vbv=<frames>`, `gpu=<id>`. Le contenu est affaire d'opérateur : ici
un Chrome dédié en kiosque sur l'écran capturé (`--user-data-dir` séparé,
`--kiosk --window-position=<x>,<y>`), relancé avant chaque passe pour le clip.
