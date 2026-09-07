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
| Machine | bench-desk : 2× RTX 5060 Ti (pilote 32.0.15.9636), iGPU AMD Radeon (Ryzen, AMF, pilote 32.0.21045), Windows 11 |
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

## 6b. Le contenu cible — un FPS en plein écran (Call of Duty, 1440p60)

Ajouté à la demande de Bruno après le premier rapport : le texte qui défile
coûtait +5 de QP à P1, que vaut une scène d'action ? Séquence de gameplay
Call of Duty (vidéo YouTube `yaNr1hHAg2M`, flux 1440p60 VP9 récupéré en local,
lue de 37 s à 52 s, plein écran sur l'écran capturé, relancée de zéro avant
chaque passe — répétabilité du réglage courant 7,67 / 7,69 ms). C'est le cas
le plus dur de la campagne : QP 25 à 40 Mbit/s là où le clip de plateforme
tenait 18.

| Réglage | encode ms (moy / p95 / p99) | total moy | Ko/frame | QP |
|---|---|---|---|---|
| **P4/ULL (courant), 40 Mbit/s** | **7,67 / 10,24 / 11,26** | 8,32 | 29,5 | **25** |
| P1 | 3,40 / 4,61 / 5,12 | 4,02 | 29,5 | **25** |
| P1, AQ spatial | 4,33 / 6,14 / 6,62 | 4,95 | 29,5 | 23 |
| P1, multipass off | 2,62 / 4,10 / 4,61 | 3,21 | 29,0 | 25 |
| P2 | 5,73 / 7,68 / 7,68 | 6,30 | 29,5 | 25 |
| P3 | 6,61 / 8,19 / 8,86 | 7,05 | 29,5 | 25 |
| P4, AQ spatial | 8,32 / 11,26 / 12,29 | 8,87 | 29,5 | 22 |
| P4, AV1 | 5,39 / 7,17 / 7,68 | 5,96 | 28,2 | q 91 |
| P1, AV1 | 3,28 / 4,61 / 5,12 | 3,83 | 28,2 | q 92 |
| P4, H.264 | 5,79 / 7,68 / 7,68 | 6,32 | 29,5 | 27 |
| P1, H.264 | 4,07 / 5,63 / 6,14 | 4,58 | 29,5 | 27 |
| **P4, 20 Mbit/s** | 7,36 / 10,24 / 11,26 | 7,90 | 14,8 | 31 |
| P1, 20 Mbit/s | 3,08 / 4,61 / 4,61 | 3,59 | 14,8 | 32 |
| P1 + AQ, 20 Mbit/s | 4,11 / 5,63 / 6,14 | 4,69 | 14,8 | 29 |

**Sur l'action, P1 ne coûte rien en QP** : 25 contre 25 à 40 Mbit/s, 32 contre
31 à 20 Mbit/s, pour 3,4 ms au lieu de 7,7 (et 3,1 au lieu de 7,4 à 20 Mbit/s).
Le +5 du texte défilant est propre au texte : contours nets à fort contraste,
que P4 sait mieux prédire ; une scène de jeu est faite de textures et de flou de
mouvement, où l'estimation de mouvement plus fine de P4 n'achète rien que le
rate control ne rende sous forme de QP identique.

**Et à l'œil.** Le banc encode vers un puits ; pour juger l'image il faut le
flux décodé. Protocole : instance dev avec `MW_NATIVE_TUNING`, client Chrome
dédié piloté par CDP (clics réels, donc plein écran accordé), flux 1440p60
HEVC 40 Mbit/s du même écran, capture `PrintWindow` de la fenêtre plein écran
en 2560×1440 physiques à 9, 12, 15 et 18 s après le relancement du clip — les
mêmes secondes à ±0,1 s pour P4, P1 et P1 + AQ (`bench/shots/`, montages
`compare-*.png` recadrés au centre et `zoom-*.png` agrandis ×2). Constat : aucun
des trois ne montre de blocs, de fourmillement ni de « bouillie » ; les
contours du décor, les débris, l'arme au premier plan et le HUD sont
également nets ; les différences entre vignettes sont celles du mouvement entre
deux images à un dixième de seconde d'écart, pas du réglage. À égalité de QP,
c'est le résultat attendu. La comparaison à 20 Mbit/s (QP 31–32) reste à faire
à l'œil si Bruno le souhaite ; l'écart d'un point de QP la rend peu probable.

## 6c. Présentateurs client en plein écran 1:1 — clic → drapeau (06/09/2026)

Le 04/09, en fenêtre (1080p → 996×935, donc en réduction), Canvas2D « Off »
mesurait 54 ms de clic → image contre 36 pour FSR1 WebGL2 — un écart assez gros
pour remettre le défaut SDR en question, et assez surprenant pour exiger d'être
recoupé dans le cas qui compte : plein écran, un pixel du flux pour un pixel de
l'écran.

Banc : sonde clic → drapeau (`docs/design/glass-to-glass.md` §5 bis, build
debug), hôte natif **AMF RX 7600, HEVC 2560×1440 @ 60, 20 Mbit/s**, bureau
quasi fixe ; client **Chrome dédié en kiosque** sur l'écran virtuel 1440p de la
même machine (canvas 1707×960 CSS à 1,5 = **2560×1440 physiques**, page visible,
`document.fullscreenElement` vrai), transport `webrtc-dc-udp`. Trois séries de
10 clics par présentateur, **en alternance** (Off, FSR1, Off, FSR1, Off, FSR1),
un clic de chauffe écarté avant chaque série, 60 clics mesurés sur 60.

| Présentateur | séries (médiane ms) | 30 clics : médiane | p90 | min–max |
|---|---|---|---|---|
| Off — Canvas2D `desynchronized` | 27,0 · 36,3 · 31,4 | **34,5** | 62,7 | 20,9–73,5 |
| Auto — FSR1 WebGL2 (EASU + RCAS, ×1) | 32,3 · 35,7 · 29,9 | **34,7** | 67,7 | 20,5–89,2 |

Indiscernables : 0,2 ms de médiane d'écart pour une dispersion de 15 ms entre
séries du même mode. La distribution est bimodale (≈ 25–38 ms ou ≈ 55–70), ce
qui est la quantification de la capture à 60 présents/s sur un bureau immobile
— le drapeau tombe avant ou après l'échéance suivante —, pas le présentateur.
**Verdict : le défaut SDR reste Canvas2D Off.** L'écart vu en fenêtre était
propre à la réduction dans une fenêtre (un `drawImage` qui rééchantillonne vers
le bas contre un passage GL au même coût quelle que soit l'échelle) ; il ne dit
rien du plein écran.

Deux pièges de banc, pour la prochaine fois :

- **L'extension Chrome ne fait pas de plein écran réel.** Elle émule un viewport
  fixe (2048×1017 CSS ici) quel que soit l'écran, `requestFullscreen` répond
  « not granted » à ses clics, et l'onglet reste `hidden`. Toute mesure de
  présentation passe par un Chrome dédié piloté en CDP (`scratchpad/bench/cdp.py`,
  port 9333) où le clic sur le bouton Fullscreen de l'app vaut activation.
- **Un clic injecté qui active une autre fenêtre gèle la sonde.** Le pointeur
  hôte parqué sur la barre des tâches est tombé sur le chevron des icônes
  masquées : le Chrome plein écran perdu l'activation → page `hidden` → rAF
  gelé → `_waitUntil` ne rend jamais la main. Cible de clic = une petite fenêtre
  topmost `WS_EX_NOACTIVATE` sur l'écran capturé (`click-target.ps1`), qui
  encaisse les clics sans rien activer ; `Page.bringToFront` avant chaque série.

## 7. Ce que les chiffres disent

1. **Le preset est le levier, et il est grand.** Sur ce NVENC (Blackwell), P1
   encode en **2,7 ms** (texte) et **3,1 ms** (jeu) contre 4,7 et 6,5 pour P4 :
   −2 à −3,4 ms de moyenne, −2 à −5,6 ms de p99, à débit égal. Le plan supposait
   « moins d'une milliseconde d'écart entre P1 et P4 » : c'était faux d'un
   facteur trois. P2 est le pire des deux mondes (lent **et** QP de P1) ; P3, P5,
   P6 valent P4 ; P7 coûte 0,2–0,7 ms de plus pour le même QP.
2. **Le prix de P1 en qualité dépend du contenu.** FPS en action : **0** de QP
   (25 vs 25 à 40 Mbit/s, 32 vs 31 à 20), et rien à l'œil sur le flux décodé
   (§6b). Jeu de plateforme : +1 (19 vs 18). Texte qui défile : +5 (31 vs 26),
   au-dessus de la tolérance ; **c'est le seul cas où P1 paie**. Écran fixe :
   identique (la rafale de raffinement converge pareil à QP 8).
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

## 8. Recommandation — **appliquée le 04/09/2026** (E1)

Bruno a confirmé après le contenu FPS (§6b) : `kDefaultPreset = 1` dans
`NvencEncoder.cpp`, multipass quart conservé, rien d'autre ne bouge. Vérifié sur
un flux réel sans `MW_NATIVE_TUNING` : la ligne « NVENC ready » dit `P1/ULL
multipass=quarter` sans crochet `[bench]`.

**NVENC : passer de P4 à P1, multipass quart conservé, tout le reste inchangé.**

- FPS en action (la cible) : **−4,3 ms de moyenne et −6 ms de p99** sur l'étape
  encode (7,7 → 3,4 ms), pour **0** de QP et rien de visible sur le flux décodé.
  Plateforme : −3,4 ms pour +1 de QP. C'est le gain le plus grand mesuré sur
  toute la chaîne hôte depuis la phase C — l'étape encode était les deux tiers
  du temps hôte.
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

**AMF** : rien à appliquer, ni sur l'iGPU ni sur la RX 7600 discrète (§8c,
mesurée le 06/09) — le réglage courant (speed, sans pré-analyse) est aussi le
plus rapide sur les deux. **oneVPL** : pas de GPU Intel, la matrice `tu=1..7`
attend.

## 8b. Après la campagne — ce que E4 et E2 ont changé aux chiffres (04/09 après-midi)

- **E4, le budget suit la cadence réelle** (design §9.8) : le même clip FPS à
  60 images/s sous un stream 165, 40 Mbit/s, passe de 29 Ko / QP 25 par image à
  **63 Ko / QP 18** — l'encodeur reçoit enfin le débit que le joueur a autorisé
  au lieu de 60/165 de celui-ci. Encode 3,4 → 3,75 ms pour des images deux fois
  plus grosses. Le fil reste à 38,5 Mbit/s mesurés sur un flux réel.
- **E2, DPB de 4 images pour l'invalidation de référence** (design §9.10) :
  `dpb=1` contre `dpb=4`, deux passes chacun : 3,69 / 3,79 ms contre
  3,75 / 3,77 ms, même taille, même QP. Gratuit.
- Piège de banc rencontré : l'écran virtuel devenu **écran principal** (session
  Parsec de Bruno) a reçu ses fenêtres ; le kiosque passait dessous et le banc
  capturait un bureau à moitié figé (14 Ko / QP 10, faux). `kiosk.ps1` épingle
  désormais la fenêtre TOPMOST sur le rectangle physique de l'écran et attend
  qu'elle existe.

## 8c. AMF sur la RX 7600 discrète (06/09/2026)

La RX 7600 est revenue dans la machine (`gpu=0`), à côté des deux RTX. Elle ne
pilote aucun des écrans du banc, donc mêmes conditions que l'iGPU du 04/09 :
atteinte par le pont inter-GPU depuis Display 3 (VDD sur la RTX), soit une trame
de 14 Mo à travers la mémoire système comptée dans `convert` (2,4 ms mesurés),
la colonne `encode` mesurant l'encodeur seul. Contenu Call of Duty 1440p60, la
même séquence relancée avant chaque passe qu'au §6b. **Ce pilote ne rapporte
toujours aucun QP** (même constat que l'iGPU et que le 02/09) : la qualité AMD
n'a pas de mesure objective ici.

| Réglage (CoD 1440p, 40 Mbit/s sauf mention) | encode ms (moy / p95 / p99) | Ko/frame | cadence |
|---|---|---|---|
| **speed (courant)** | **4,66 / 7,17 / 12,29** | 61,4 | 59,8 fps |
| dpb=1 (pas d'invalidation) | 4,77 / 7,68 / 11,26 | 58,8 | 59,9 |
| balanced | 4,53 / 7,68 / 11,26 | 60,1 | 59,7 |
| quality | 5,04 / 7,17 / 11,26 | 59,8 | 60,0 |
| aq (VBAQ) | 4,68 / 6,66 / 11,26 | 60,0 | 59,9 |
| **pré-analyse** | — | — | **la session meurt** (« stopped producing frames ») |
| VBV 1 frame (29 Ko) | 4,84 / 7,68 / 12,29 | 51,5 | 59,9 |
| VBV 2 frames (59 Ko) | 4,95 / 9,22 / 12,29 | 59,1 | 59,9 |
| H.264 | 4,66 / 7,68 / 11,90 | 59,5 | 59,9 |
| AV1 | 5,38 / 9,22 / 11,26 | 53,0 | 59,8 |
| AV1, dpb=1 | 5,41 / 9,22 / 12,29 | 53,1 | 59,9 |
| intra-refresh | 4,74 / 7,68 / 11,26 | 59,7 | 59,9 |
| fps réglé 60 | 4,88 / 10,24 / 11,26 | 60,2 | 55,5 |
| 20 Mbit/s | 4,79 / 9,22 / 11,26 | 30,3 | 59,9 |
| **1920×1080** | **3,71 / 7,68 / 10,24** | 59,9 | 59,9 |
| défilement de texte 1440p (162 présents/s) | 3,78 / 4,61 / 5,12 | 25,1 | **162,2 fps** |
| défilement · AV1 | 4,51 / 5,63 / 6,14 | 26,7 | 144,3 |
| défilement · 1080p | (voir CSV) | | |

Lecture, et ce qui confirme l'iGPU comme ce qui l'infirme :

1. **La carte discrète tient 1440p60 sans effort et 1080p à pleine cadence.**
   4,66 ms d'encode à 1440p (l'iGPU était à 7,9 sur un clip *plus facile*), 3,71
   à 1080p, et **162 fps** sur le texte défilant 1440p là où l'iGPU plafonnait à
   102. La classe « carte AMD dédiée » n'a pas le problème de cadence de l'iGPU.
2. **Les presets AMF ne bougent presque rien**, exactement comme sur l'iGPU :
   speed 4,66 / balanced 4,53 / quality 5,04 ms. Le « quality » coûte 0,4 ms
   pour aucune mesure de gain (pas de QP). Rien à gagner à quitter « speed ».
3. **La pré-analyse tue encore l'encodeur** en ULL/CBR, sur la carte dédiée comme
   sur l'iGPU — le verrou de `AmfEncoder::init` (§18 du design) est justifié sur
   les deux silicium AMD, pas un hasard de l'iGPU.
4. **AV1 coûte ~0,7 ms de plus que HEVC** (5,38 vs 4,66) ; H.264 = HEVC (4,66).
   Même hiérarchie que NVENC, à ceci près qu'AMF n'a pas l'avance d'AV1 de NVENC.
5. **L'invalidation de référence AMF est gratuite en temps.** `dpb=1` (qui
   l'éteint) contre le défaut : 4,77 vs 4,66 ms HEVC, 5,41 vs 5,38 AV1 — dans le
   bruit, comme le DPB de 4 sur NVENC. Les slots LTR ne coûtent rien à porter.
6. **VBV** : le plancher tient ; 1 ou 2 frames font des images plus petites (51
   au lieu de 61 Ko à VBV 1) sans gagner de temps — du débit non dépensé, même
   verdict que NVENC.

**Recommandation AMF (RX 7600 comme iGPU) : rien à appliquer.** Le réglage par
défaut (usage ultra-low-latency = « speed », pré-analyse interdite, VBAQ au
choix du pilote) est déjà le plus rapide mesuré, et le pilote ne rapporte pas de
QP qui permettrait d'aller chercher un compromis qualité. La seule variable qui
compte sur AMD est la résolution, et elle est le choix de l'utilisateur.

**Le dernier bouton AMD jamais touché : `LowLatencyInternal`** (06/09, au soir).
Le seul réglage AMF que ce moteur n'avait ni posé ni mesuré, et le seul dont
l'en-tête d'AMD annonce « **default = false** » au lieu du « depends on USAGE »
de tous les autres — celui que Sunshine pose explicitement. Il vaut donc une
mesure, pas une supposition. `AmfEncoder::init` le **relit** maintenant, après
avoir posé l'usage et avant toute surcharge, et la réponse tient en un mot :
`lowlatency=1` **déjà**, sur les trois passes par défaut comme sur les forcées.
L'usage ultra-low-latency l'allume lui-même, l'en-tête est trompeur. L'A/B le
confirme, HEVC puis H.264, en alternance, CoD 1440p 40 Mbit/s :

| Passe | encode ms (moy / p95 / p99) | Ko/frame | cadence |
|---|---|---|---|
| HEVC défaut | 4,65 / 9,22 / 11,26 · 4,82 / 9,22 / 12,29 | 59,8 · 59,7 | 59,9 |
| HEVC `lowlatency=1` forcé | 4,93 / 10,24 / 12,29 · 4,83 / 10,24 / 12,29 | 59,6 · 59,9 | 59,9 |
| H.264 défaut | 4,68 / 10,24 / 12,29 · 4,67 / 10,24 / 11,26 | 59,5 · 54,5 | 59,9 |
| H.264 `lowlatency=1` forcé | 4,63 / 9,22 / 11,26 · 4,83 / 10,24 / 12,29 | 60,0 · 57,6 | 59,8 |

Rien à appliquer, et pour la meilleure des raisons : c'était déjà appliqué. Ce
qui reste du travail, c'est la **ligne de log** — l'état effectif du mode est
désormais écrit à côté de `quality` et `preanalysis`, donc un pilote qui
changerait d'avis se verrait au lieu de se deviner. Clé de banc `lowlatency=0|1`
pour rejouer, AV1 excepté (il a son propre `ENCODING_LATENCY_MODE`, déjà au plus
bas).

**Invalidation de référence AMF** — livrée le 06/09 (R4 du plan), mesurée ici
gratuite (point 5) et **configurée sur les trois codecs** : la ligne « AMF
ready » porte « 4 LTR slots every N frames with reference invalidation (reach M
frames) », et `dpb=1` la retire (« no reference invalidation »). Détail du
mécanisme : design §9.10. ⚠️ **Reste à observer sur un vrai lien** : la ligne
« AMF healed frame … from long-term slot bitfield » à la réparation effective —
le banc encode vers un puits (pas de récepteur pour nommer une perte), et le
client Chrome piloté par CDP du banc ne décode pas ce flux (il redemande une
IDR sans jamais monter la vue), donc le test de perte (`mw_drop_test`, qui vit
dans `StreamView`) ne s'arme pas. Même angle mort que l'effet visuel de la
réparation NVENC laissé à l'œil de Bruno au §8b.

## 8d. Intel Quick Sync sur l'UHD Graphics d'un N95 (07/09/2026)

Premier GPU Intel de la flotte (banc `bench-intel`, Intel N95, UHD Graphics 24 EU,
pilote 32.0.101.7088). Le chemin oneVPL n'avait **jamais encodé une frame** avant
ce jour ; il a fallu cinq corrections pour qu'il en encode une, puis pour qu'il
tienne une session. Le détail des cinq est au design §21 — ici, seulement les
chiffres.

**Ce qui a été mesuré.** Bureau fixe (le seul contenu disponible sur ce banc :
pas de clip, 5 Go de libre sur le disque), 1 passe de 8 s par ligne, 20 Mbit/s,
intra-refresh actif, capture DXGI de l'écran 2560×1440 de la machine.

| Codec | TU | Taille | fps | encode moy / p95 / p99 (ms) | convert moy | Ko/frame |
|---|---|---|---|---|---|---|
| HEVC | 1 | 1920×1080 | 59,6 | 13,33 / 18,43 / 24,58 | 1,15 | 36,4 |
| HEVC | 4 | 1920×1080 | 59,8 | 12,59 / 18,43 / 20,48 | 1,37 | 36,4 |
| HEVC | 7 | 1920×1080 | 59,7 | **11,46** / 15,36 / 18,43 | 1,64 | 36,4 |
| H.264 | 1 | 1920×1080 | 58,1 | 15,53 / 20,48 / 26,62 | 0,74 | 39,1 |
| H.264 | 4 | 1920×1080 | 59,8 | 12,61 / 16,38 / 22,53 | 1,49 | 36,5 |
| H.264 | 7 | 1920×1080 | 59,7 | 13,38 / 18,43 / 18,43 | 1,11 | 36,5 |
| HEVC | 7 | 2560×1440 | 59,6 | 13,43 / 18,43 / 20,48 | 1,17 | 36,4 |
| HEVC | 1 | 2560×1440 | 58,3 | 16,47 / 20,48 / 26,62 | 0,43 | 36,4 |

**Verdict : rien à appliquer, et pour la troisième fois.** Le `TargetUsage`
d'Intel se comporte comme les presets d'AMD : 1,9 ms d'écart entre le bout
qualité et le bout vitesse, du même ordre que la dispersion entre deux passes du
même réglage, et le défaut du moteur (TU7) est déjà le bord rapide. Le débit par
frame ne bouge pas d'un octet (36,4 Ko partout) : en CBR sur un bureau fixe, le
contenu ne discrimine rien.

⚠️ **Aucune mesure objective de qualité sur ce banc non plus.** Le pilote Intel ne
rapporte pas de QP moyen, exactement comme AMD (§8c). Un jugement de qualité
Intel resterait un A/B à l'œil.

**Le seul réglage qui a compté ne se règle pas — il se pose.** `LowPower`
(le moteur à fonction fixe, VDENC) : HEVC 1080p60 est passé de **16,3 à 10,5 ms**
et le 1440p de **21,4 à 13,4 ms**. Il est désormais demandé à chaque session,
avec repli automatique sur le moteur général si la génération ne l'a pas.

**Ce que ça vaut, honnêtement.** Un N95 est un SoC à 4 cœurs sans SMT et 24 EU :
ces chiffres disent que le chemin **tient 60 fps en 1080p et en 1440p** sur le
plus petit Intel qui existe, pas ce que vaut un Arc ou un Core de bureau. Et la
capture, la conversion, l'encodage, le décodage du navigateur et la page
tournaient tous sur la même machine.

**Flux réel.** Chrome 152 sur la machine elle-même, hôte natif Intel :
HEVC `hvc1.1.144.L123.B0`, `descLen=111` (VPS/SPS/PPS extraits de la keyframe
servie), première image décodée 1920×1080 NV12 en matériel, 65,7 s de session,
**1689 présents tous portés**, audio 13 142 paquets / 0 jeté, zéro erreur de
décodeur, arrêt propre. Le gouverneur de lien descend le débit à chaque montée
de délai (20000 → 4196 kbps) et **toutes** ses baisses sont appliquées — c'était
l'objet de la correction du HRD.

**Puis depuis une autre machine, ce qui est la vraie mesure.** Chrome sur bench-desk
→ hôte Intel, en LAN, appairage par PIN, `webrtc-dc-udp` : **latence affichée
11,4 à 14,2 ms**, deux sessions de 289 s et 320 s, 2327 puis 2558 présents **tous
portés**, 4831 frames émises, encode 7,05 / 11,26 / 14,34 ms, total hôte
8,84 / 13,31 / 18,43 ms, 57 800 paquets audio / 0 jeté, **63 événements d'entrée
injectés** (souris et clavier), arrêt propre. Les 41 à 55 ms du loopback étaient
ceux d'un N95 qui encodait, décodait et servait la page à la fois — à ignorer.

Et c'est là seulement que le gouverneur a pu **remonter** : 16000 → 20000 kbps en
cinq paliers, toutes les hausses appliquées. En loopback la machine était saturée
et il ne faisait que descendre, donc la moitié montante du correctif de `Reset`
n'y était pas prouvée.

⚠️ **Le banc était injoignable en LAN, et ce n'était pas le pare-feu tiers.** La
boîte Windows « autoriser cette application ? » s'était ouverte dans la session
console sans que personne ne la voie, et Windows en avait fait deux règles de
**blocage** pour le binaire — un blocage l'emporte sur toute règle de port, donc
les autorisations ajoutées à la main ne servaient à rien. SSH marchait pendant ce
temps et masquait le problème.

⚠️ Deux limites propres à Intel, mesurées ici : le débit ne peut pas **monter**
au-dessus de celui de l'init (`Reset` refuse), donc la moitié montante du budget
par cadence réelle (E4) est plafonnée ; et il n'y a **pas d'invalidation de
référence** sur oneVPL (`NumRefFrame = 1`), donc une perte se répare par
keyframe.

## 8e. Intel : la matrice de paramètres sur du vrai contenu (07/09/2026)

Le §8d mesurait sur un bureau fixe et concluait que le `TargetUsage` ne se voit
pas. ⚠️ **C'était vrai du bureau fixe et faux du reste** : sur le clip Call of
Duty, TU1 coûte **trois fois** le temps d'encodage de TU7. Le défaut du moteur ne
change pas — il était déjà au bon bout de l'échelle — mais le raisonnement, si.

**Protocole.** Clip CoD 1440p60 relancé en kiosque plein écran sur l'écran
capturé avant **chaque** passe (mêmes secondes du même métrage), stream HEVC
1080p60, 20 Mbit/s, intra-refresh, 10 s par passe, banc `--native-bench` vers un
puits.

⚠️ **Ce banc est saturé, et il faut le dire avant les chiffres.** L'N95 décode le
clip 1440p60 *et* encode 1080p60 sur les mêmes 24 EU. Quatre passes du réglage
par défaut donnent 51,4 / 60,9 / 70,2 / 77,8 ms — **±20 % de dispersion**. Rien
en dessous d'un facteur ~1,5 n'est mesurable ici.

| Réglage | fps | encode moy / p95 / p99 (ms) | Ko/frame |
|---|---|---|---|
| **(défaut)** | 16,8 | **51,40** / 81,92 / 147,46 | 67,3 |
| `extbrc=1` | 17,1 | 49,40 / 90,11 / 134,49 | 64,8 |
| `dpb=1` | 14,4 | 51,72 / 98,30 / 294,91 | 63,5 |
| `gaming=1` | 13,5 | 54,44 / 106,50 / 196,61 | 62,1 |
| `vbv=1` | 14,5 | 55,40 / 114,69 / 196,61 | 66,2 |
| `tu=4` | 14,8 | 57,16 / 114,69 / 183,39 | 65,6 |
| `mbbrc=1` | 13,1 | 60,65 / 106,50 / 360,45 | 67,8 |
| `vbv=2` | 12,0 | 67,26 / 122,88 / 524,29 | 65,4 |
| `lowdelaybrc=1` | 9,3 | 71,94 / 196,61 / 648,94 | 66,6 |
| `mbbrc=0` | 11,1 | 72,02 / 212,99 / 360,45 | 65,9 |
| `winbrc=60` | 11,4 | 72,36 / 180,22 / 267,47 | 66,7 |
| `tu=1` | 6,1 | **141,35** / 245,76 / 965,98 | 69,2 |
| `lowpower=0` | 4,5 | **185,90** / 393,22 / 633,88 | 65,3 |

**Deux réglages sortent du bruit, et ce sont les deux que le moteur pose déjà.**

- `lowpower=0` — le moteur à shaders au lieu du bloc fixe : **3,6× plus lent**,
  4,5 fps. VDENC n'est pas une optimisation, c'est la condition d'existence du
  chemin Intel.
- `tu=1` — le bout « qualité » du TargetUsage : **2,8× plus lent**, 6 fps, et le
  débit par image ne bouge pas (69,2 Ko contre 67,3). On paie tout le temps pour
  rien de visible. TU7, le défaut, est le bon.

**Tout le reste est dans la dispersion du défaut lui-même** (49 à 72 ms, contre
51–78 pour quatre passes identiques). `extbrc=1` est nominalement le meilleur,
mais l'écart est plus petit que le bruit : rien à appliquer. Verdict identique à
NVENC après la campagne du 04/09 et à AMF au §8c — **le moteur était déjà réglé
juste**, et cette fois on sait aussi *pourquoi* : les deux seuls leviers qui
comptent sont ceux qu'il pose.

⚠️ `winbrc=60` a fait mourir une passe entière (`still executing`) avant que le
plafond d'attente ne soit relevé : la fenêtre glissante coûte assez cher pour
qu'une image dépasse la seconde sur ce matériel.

⚠️ **Le clic→photon n'a pas pu être mesuré sur ce banc.** Le drapeau
click-to-photon exige un build debug (`LatencyFlag` est gaté sur `QT_DEBUG`) ;
un vrai build Debug est dix fois trop lent ici (notre passe de conversion passe
de 0,4 à 10,6 ms), donc un arbre Release portant seulement `QT_DEBUG` a été bâti
pour la mesure. L'hôte confirme chaque clic (« [LatencyFlag] injected click at
1711,1056 ») mais la sonde du navigateur ne voit **jamais** le drapeau dans
l'image décodée — ni sur le clip, ni sur un bureau fixe où le pipeline est sain.
La contre-vérification par capture d'écran sur le banc ne tranche pas : `BitBlt`
ne voit pas une fenêtre *layered*, donc « rien vu » n'y prouve rien. Deux bugs
réels ont été trouvés en montant cette mesure (§21.8), mais **le chiffre lui-même
n'est pas acquis**, et il ne faut pas en inventer un.

## 8f. Ce que coûte le VBV sur Intel, et pourquoi la marge a été retirée (07/09/2026)

Le §21.6 du design explique comment le budget par image *peut* monter sur Intel :
en dimensionnant le tampon de bitstream à l'init, parce que c'est ce que `Reset`
valide. Reste à savoir ce que ce tampon coûte, puisqu'il est aussi le VBV.

**Mesure.** Texte défilant plein écran (mouvement sur toute l'image, à chaque
frame — le pire cas d'un flux de bureau), HEVC 1080p60, 20 Mbit/s,
intra-refresh. `vbv=<n>` demande exactement n images au débit du flux ; sans clé,
la règle du moteur.

| Réglage | VBV | Ko/frame moy | p95 | p99 | occupation du lien (p95) |
|---|---|---|---|---|---|
| `vbv=1` | 85 Ko | 40,6 | 60,0 | 64,0 | **24 ms** |
| `vbv=2` | 85 Ko | 40,8 | 64,0 | 66,2 | 26 ms |
| `vbv=3` | 125 Ko | 41,9 | 88,0 | 96,0 | 35 ms |
| `vbv=6` | 250 Ko | 56,2 | 112,0 | 156,5 | **45 ms** |
| marge ×2 (250 Ko) | 250 Ko | 55,4 | 104,0 | 154,6 | **42 ms** |

**Lecture.** La marge fait exactement ce qu'elle promet — 40,6 → 55,4 Ko par
image, soit **+37 % de bits pour le même débit sur le fil** quand l'écran bouge
moins vite que le flux. Et elle le fait payer là où ça compte : le pic par image
passe de 60-64 Ko à 104-155 Ko, c'est-à-dire de **~26 ms à ~42 ms** d'occupation
du lien pour une seule image à 20 Mbit/s.

**Décision de Bruno, appliquée** : « qualité légèrement moindre sur écran fixe
acceptable ; aucune augmentation volontaire de la latence pour gagner en
netteté ». `kBudgetHeadroom = 1` : le VBV revient à la règle partagée (85 Ko,
une image au débit du flux), le pic redescend à 64 Ko au p99, et le budget par
image ne monte pas. Une marge ×3 aurait demandé 375 Ko, soit ~150 ms de lien
pour une image — jamais envisagée.

⚠️ Ce qui est perdu est la moitié **montante** de E4 sur Intel seulement : une
image lente n'y dépense pas les bits que ses frames auraient valus. La moitié
descendante — celle qui compte quand un lien souffre — fonctionne exactement
comme ailleurs, et surtout **les demandes sont maintenant plafonnées au lieu
d'être refusées** : avant le 07/09 un `Reset` refusé laissait le débit là où il
était, warning à l'appui.

⚠️ La voie qui donnerait les deux — gros tampon pour autoriser la hausse,
`MaxFrameSizeP` pour borner l'image — **n'existe qu'en VBR** (« used in VBR based
bitrate control modes and ignored in others »). Elle demanderait de changer le
mode de contrôle de débit de ce chemin ; à instruire séparément si le sujet
revient.


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
`tu=1..7`, `vbv=<frames>`, `lowlatency=0|1`, `gpu=<id>`, et pour Intel
`lowpower=0|1`, `mbbrc=0|1`, `extbrc=0|1`, `lowdelaybrc=0|1`, `gaming=0|1`,
`winbrc=<frames>`. Le contenu est affaire d'opérateur : ici
un Chrome dédié en kiosque sur l'écran capturé (`--user-data-dir` séparé,
`--kiosk --window-position=<x>,<y>`), relancé avant chaque passe pour le clip.
