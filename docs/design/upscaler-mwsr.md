# Shader d'upscaling MoonlightWeb — plan de la v0.4.1

Plan écrit le 10/10/2026, à exécuter après la release v0.4.0 ; décisions de lancement et parc corrigé au
§13 (19/10/2026). À la date du plan rien n'était commencé : aucun fichier du dépôt touché, aucune mesure
prise, aucun shader écrit. Les
chiffres cités viennent soit du dépôt (chapitre 15 du wiki, mémoires de banc, commentaires du code),
soit des sources externes listées au §10 ; chaque gain annoncé est marqué **[démontré]** (mesuré par
une source nommée, dans des conditions nommées) ou **[attendu]** (hypothèse à vérifier au banc).

Nom de travail du shader : `mwsr` (MoonlightWeb Super Resolution) dans le code et les réglages ;
le libellé de l'interface est au choix de Bruno. Le nom n'engage rien.

---

## 0. Résumé et verdict de réalisme

**Ce que le client fait aujourd'hui.** Le client porte déjà trois upscalers en deux saveurs : FSR1
(EASU + RCAS), SGSR1 et NIS, sur WebGL2 et sur WebGPU, choisis par la matrice de septembre
(chapitre 15 du wiki) : en SDR, Canvas2D sans enhancer par défaut, FSR1 WebGL2 sur un ordinateur et
SGSR1 WebGL2 sur un téléphone quand l'enhancer est coché ; en HDR, tout passe sur WebGPU. Sur WebGPU
chaque upscaler paie une passe de plus que son algorithme : la « passe 0 », qui recopie la texture
externe de la vidéo dans une texture ordinaire parce que les trois shaders ont besoin de
`textureGather`, que WebGPU refuse sur une `texture_external`. FSR1 y coûte donc trois passes
plein écran, SGSR1 et NIS deux. À résolution native, l'enhancer tourne quand même (EASU puis RCAS
à l'échelle 1, ou SGSR1 avec son vote de contour), et le présentateur le plus rapide mesuré en
SDR reste Canvas2D.

**Verdict, objectif par objectif.**

| Objectif | Verdict | Pourquoi |
|---|---|---|
| Plus rapide que SGSR1 sur mobile | **Réaliste mais étroit** [attendu] | Qualcomm mesure SGSR1 à 0,30-0,48 ms contre 0,16-0,25 ms pour un bilinéaire pur sur Adreno 660 → 8 Gen 2 (entrée 1240×576, sortie 1080p-1600p) : l'écart à battre est de 0,14-0,23 ms, soit le coût du noyau au-dessus du plancher qu'est la passe elle-même. Les leviers sont connus et mesurés ailleurs [démontré sur iPhone 12 par Atyuwen] : 5 taps bilinéaires au lieu de gathers (3,6 → 1,8 ms), f16 (5,4 → 4,7 ms), passe de copie évitée (0,7 ms), sortie anticipée. Mais SGSR1 a déjà une sortie anticipée et un seul gather sur le chemin plat : le gain net sur le noyau seul sera de l'ordre de 10-30 %, celui de la chaîne complète (sans la passe 0) plus net. Aucun chiffre public de SGSR1 ni de FSR1 n'existe sur Mali ni sur Apple, et aucun chiffre indépendant nulle part : tout est à mesurer. |
| Plus économe que SGSR1 sur batterie | **Plausible, non démontrable sans banc** | L'énergie d'une passe plein écran suit son temps GPU et sa bande passante ; une passe de moins et des échanges mémoire plus courts vont dans le bon sens. Aucune source publique ne donne un milliwatt pour un upscaler mobile ; le protocole du §7 fixe comment on le mesurera (puissance GPU sur Mac M1, décharge de batterie sur iPhone, capteurs sur le banc ARM). |
| Plus rapide que FSR1 ailleurs, à qualité comparable | **Réaliste sur le temps, à vérifier sur la qualité** | FSR1 sur WebGPU = passe 0 + EASU (4 gathers × 3 canaux, 12 taps) + RCAS : trois passes et deux textures intermédiaires. Une passe unique fusionnant upscale et netteté, sans passe 0, retire deux passes plein écran [démontré par l'architecture, gain à chiffrer]. La qualité est l'inconnue : EASU reste le noyau le plus net sur le texte ; un noyau à 5-9 taps bilinéaires sans le déringing 2×2 d'AMD peut sonner sur le texte à ×2 (Atyuwen : « artefacts rarement visibles », mais sur un jeu, pas sur du texte). Le critère « comparable » est chiffré au §8. |
| Upscale, downscale, HDR, 4:4:4 | **Réaliste** | Les chemins existent déjà dans le renderer (float16 linéaire, tone-map ACES, 4:4:4 négocié par codec) ; le shader doit seulement être écrit sans supposer [0, 1] et porter un mode de réduction correct, ce qu'aucun des trois upscalers actuels ne fait vraiment (NIS refuse la réduction, SGSR1 et EASU ne la filtrent pas). |
| Passage direct à résolution native | **Réaliste, avec une limite d'architecture** | Un canvas ne change jamais de type de contexte : on ne peut pas passer de WebGPU à Canvas2D en cours de flux, et l'échelle change en cours de flux. Le contournement se fait donc dans le renderer, par image (aucune passe d'enhancer, aucune texture intermédiaire, une seule écriture de la surface) et, quand l'enhancer est éteint, par le choix de Canvas2D au lancement comme aujourd'hui. Le « zéro copie » absolu n'existe que par l'élément `<video>`, mesuré 35-45 ms plus lent au clic : exclu par la règle de latence. |
| Latence, mémoire, énergie minimales | **Réaliste** | Une passe au lieu de deux ou trois, pas de texture intermédiaire, un bind group par image : c'est déjà moins que tout ce qui tourne aujourd'hui sur WebGPU. Reste le surcoût de présentation de WebGPU sur le thread principal (4 ms Windows, 10 ms macOS mesurés le 03/09 en fenêtre ; inversé plein écran), que le shader ne change pas : d'où le jumeau WebGL2. |

**Trois décisions d'architecture que le plan propose d'emblée** (à confirmer par Bruno au lancement) :

1. **Un seul algorithme, deux jumeaux.** La version WGSL (WebGPU) est la référence et la plus
   optimisée ; la version GLSL ES 3.00 (WebGL2) suit le même noyau à coût égal, parce que le noyau
   de base n'utilise que des échantillons bilinéaires : aucun `textureGather`, qui n'existe pas en
   WebGL2 (GLSL ES 3.10 seulement) et que la texture externe de WebGPU refuse de toute façon. Les
   accélérations propres à WebGPU (f16, opérations de quad, lecture directe de la texture externe,
   variante compute) sont des couches activées par constantes `override` : le GLSL ne les a pas, et
   le WGSL ne porte aucun échafaudage de compatibilité.
2. **Pas de passe 0 en SDR.** Le shader WGSL lit la `texture_external` directement
   (`textureSampleBaseClampToEdge`, et `textureLoad` là où il est pris en charge). Les chemins HDR
   gardent leur passe 0 (lecture des plans YUV ou tone-map) et alimentent la variante
   `texture_2d<f32>` du même noyau.
3. **Code original, règle de salle blanche.** Les sources étudiées (FSR1 MIT, SGSR1 BSD-3, NIS MIT,
   gist d'Atyuwen MIT) sont toutes compatibles avec la GPL-3.0-or-later du projet, mais le shader
   de la v0.4.1 n'en recopie aucune ligne : il en reprend les idées (taps bilinéaires, sortie
   anticipée, clamp anti-sonnerie, netteté fusionnée) et ses propres constantes. Les ports
   existants restent dans le dépôt comme références de banc et comme crans de l'échelle.

---

## 1. Le client aujourd'hui : architecture et points d'intégration

### 1.1 Les quatre présentateurs et leur routage

`frontend/js/stream/renderers/createRenderer.js` choisit le renderer une fois par flux, et
`StreamView.js` (vers les lignes 636-845) décide avant lui de ce qu'on lui demande :

| Renderer | Quand | Ce qu'il fait de l'image | Coût mesuré au dépôt |
|---|---|---|---|
| `Canvas2DRenderer` | SDR, enhancer éteint (défaut), ou `smooth2d` | `drawImage(VideoFrame)` direct, contexte `desynchronized` ; tampon à la taille de l'image, étirement CSS par le compositeur (ou tampon à la taille d'affichage avec `imageSmoothingQuality` haute en `smooth2d`) | 0,2-0,6 ms de rendu (03/09) ; clic → photon 11,6 ms en fenêtre, 30,3 plein écran (ch. 15) |
| `WebGlRenderer` | SDR, enhancer coché : `gl-fsr1` (ordinateur), `gl-sgsr` (téléphone, tablette, tout Snapdragon), `gl-nis` (debug) | contexte `desynchronized` ; gathers émulés par quatre `texelFetch` ; FSR1 en deux passes par FBO intermédiaire ; barrières `fenceSync` pour signaler au gouverneur un GPU en retard | 0,3-0,4 ms de soumission ; clic → photon +2-4 ms en fenêtre, 34-37 ms plein écran |
| `WebGpuRenderer` | HDR (toujours), ou saveur WebGPU demandée ; `off` = blit seul | passe 0 obligatoire avant tout enhancer ; SGSR1 et NIS une passe ensuite, FSR1 deux ; attente `onSubmittedWorkDone` à chaque image (contre-pression) ; canvas `rgba16float` + `toneMapping: extended` + `display-p3` en HDR | 4,6 / 8,0 ms (moyenne / p99) pour le blit seul en fenêtre (03/09) ; plein écran, les saveurs WebGPU ont gagné de 5-10 ms au clic (27-29,5 ms contre 34-37) |
| `VideoElementRenderer` | HDR HEVC (seul chemin HDR matériel), TV, ou « Video » du menu debug | trame écrite dans un `MediaStreamTrackGenerator` → `<video>` ; aucun shader possible | 35-45 ms plus lent au clic que tout canvas |

Deux faits de cette matrice pèsent sur le plan :

- **En SDR, la production ne tourne pas sur WebGPU.** La décision du 03/09 (WebGPU = 4 ms de rendu
  sous Windows, 10 sous macOS, un vsync entier au p99, pas d'équivalent de `desynchronized`) a mis
  les enhancers SDR sur WebGL2. Le wiki (15.3) note que le plein écran inverse la hiérarchie en
  faveur de WebGPU et demande de le confirmer sur du contenu de jeu avant de toucher au défaut. Un
  shader WGSL seul ne toucherait donc qu'au HDR et au menu debug : d'où le jumeau GLSL (§3.6) et
  la porte P4 qui remesure WebGPU contre WebGL2 avec le nouveau shader.
- **Le renderer ne change pas en cours de flux, l'échelle si.** Un canvas est `2d` ou `webgpu`
  pour toujours ; l'échelle change à chaque instant (fenêtre, plein écran, pinch-zoom quantifié par
  `_outputZoomScale`, rotation, et l'échelle de qualité qui baisse la résolution du flux sous
  congestion avec le message « résolution réduite, upscaling activé »). Toute décision « à
  l'échelle 1 » se prend donc **par image, dans le renderer**, pas au lancement.

### 1.2 La chaîne WebGPU, image par image

`WebGpuRenderer.draw(frame)` :

1. taille de sortie = boîte CSS × `devicePixelRatio` × zoom quantifié, bornée à 4096, à l'aspect de
   l'image ; le canvas est redimensionné si besoin ;
2. en HDR « readback » (`tonemap`, `linear`) : `frame.copyTo()` des plans 10 bits (décodage logiciel
   AV1 seulement), `writeTexture` vers trois `r16uint`, passe 0 = shader YUV → tone-map ACES ou
   lumière linéaire ; sinon `importExternalTexture(frame)` + bind group **par image** (la texture
   externe expire à la fin de la tâche) ; sur WebKit, passe 0 = tone-map depuis la texture externe
   (`tonemap-ext`) ;
3. `off` : une passe vers le canvas (blit ou tone-map) ; sinon passe 0 vers `inputTex`
   (`rgba8unorm`, ou `rgba16float` en HDR, à la résolution de l'image) ;
4. SGSR1 / NIS : une passe `inputTex` → canvas ; FSR1 : EASU → `intermTex` (résolution de sortie,
   même format) puis RCAS → canvas ; les uniformes sont réécrits à chaque image ;
5. sonde clic → photon : trois texels copiés du canvas vers un tampon mappable ;
6. `submit`, `frame.close()`, puis `await onSubmittedWorkDone()` : la promesse ne se résout qu'une
   fois le GPU fini, ce qui alimente le gouverneur (`lastDraw.waitMs`) et borne la file (deux dessins
   en vol au plus, `RenderPacing`).

Ce que cette chaîne coûte de plus que le strict nécessaire, et que le shader de la v0.4.1 peut
retirer :

- **la passe 0** : une passe plein écran à la résolution d'entrée, une écriture puis une lecture de
  `inW × inH × 4` octets (8 Mo + 8 Mo à 1080p, 16 + 16 en HDR float16) ; elle n'existe que parce
  que les trois shaders font des gathers ;
- **la texture intermédiaire de FSR1** à la résolution de sortie (écrite par EASU, lue par RCAS) ;
- **trois `createBindGroup`** et plusieurs `writeBuffer` par image, dont certains pour des valeurs
  qui ne changent qu'au redimensionnement ;
- **le clamp à [0, 1]** dans SGSR1 (`saturate`) et NIS (`clamp`), faux en HDR linéaire (seul FSR1
  passe la lumière de scène sans l'écrêter, ce que le wiki 15.5 a vérifié).

Au passage, deux écarts du port SGSR1 du dépôt par rapport à la source Qualcomm, relevés en lisant
les deux : seuil de contour 6/255 au lieu de 8/255, `EdgeSharpness` 1,5 au lieu de 2,0, et pas de
clamp ±23/255 du delta de luma. Rien à corriger pour ce plan, mais le banc comparera le port tel
qu'il est (c'est lui qui tourne chez les utilisateurs) et le dira.

### 1.3 Le gouverneur et l'échelle de dégradation

`EnhancerGovernor.js` ne descend l'échelle que dans un sens, `fsr1 → nis → sgsr → off`, sur trois
signaux : le coût de service du dessin au-delà de 80 % de l'intervalle d'arrivée des images, une
file de décodage au-delà de 4, ou plus de la moitié des dessins encore sur le GPU quand le suivant
arrive (barrières WebGL seulement : WebGPU ne fournit pas ce signal aujourd'hui). Le verdict est
mémorisé par appareil (`mw_enhancer_rung`) et la remontée est frileuse (15 s, doublée à chaque pari
perdu). Un invité a un budget fixe (8 ms) et pas de remontée.

Points d'intégration : la place de `mwsr` dans `ENHANCER_LADDER` (en dessous de `sgsr` s'il est
moins cher, ce qui est l'objectif ; sinon au-dessus), `ladderRung` / `rendererAlgo` pour le préfixe
`gl-`, et, si les requêtes d'horodatage entrent, un quatrième signal de coût GPU réel sur WebGPU
(aujourd'hui `gpuBehind` y est nul). *[P4, 21/10 : `mwsr` est entré entre `sgsr` et `off` (moins cher
que SGSR1 sur tout GPU mesuré) ; la mémoire range des noms, rien à migrer ; `auto` le prend derrière le
drapeau `mw_mwsr` jusqu'à P6.]*

### 1.4 Les réglages, le menu, les chaînes

- `SettingsView.js` : une case « Video Enhancement » en production ; la liste des algorithmes
  (`auto`, `canvas2d`, `video`, `smooth2d`, `gl-sgsr`/`sgsr`, `gl-nis`/`nis`, `gl-fsr1`/`fsr1`)
  n'apparaît qu'en build debug ; `pickAutoEnhancer()` (`BrowserDetect.js`) résout `auto` ;
  `isSnapdragonGpu()` renifle la chaîne WebGL `UNMASKED_RENDERER_WEBGL`.
- i18n : trois catalogues (`en.json`, `fr.json`, `zh.json`), clés `settings.algo*`,
  `stream.statEnhancer`, `stream.enhancerOff` ; `npm run i18n` vérifie la complétude.
- Métriques de forme du flux : résolution, cadence, codec, HDR/4:4:4, bande de débit, backend,
  transport, LAN/Internet, classe d'appareil ; **pas** le nom de l'enhancer (à ajouter si on veut
  savoir ce qui tourne chez les utilisateurs ; décision de Bruno).
- Le worker vidéo (`VideoDecodeWorker.js`, opt-in) crée les mêmes renderers sur un
  `OffscreenCanvas`, sans `localStorage` ni `<video>` : tout réglage du shader lui est transmis par
  message, comme `algo` et `ceiling` aujourd'hui.

### 1.5 HDR et 4:4:4 tels qu'ils arrivent au shader

- **HDR** (StreamView `_hdrMode`) : `sink` (`<video>`, HEVC, pas d'enhancer), `linear`
  (AV1 logiciel, canvas float16, 1,0 = 203 nits, BT.2020 → P3, enhancer en lumière de scène),
  `tonemap` (AV1 logiciel, ACES vers un canvas SDR, puis enhancer), `tonemap-ext` (WebKit, ACES
  depuis la texture externe, puis enhancer), `browser` (tone-map du navigateur à l'import, puis
  blit). La contrainte de fond est documentée aux chapitres 13 et 15.5 du wiki : Chrome tone-mappe
  un `VideoFrame` HDR **à l'import** sur toute surface canvas ; seul `copyTo` d'une image décodée en
  logiciel garde le signal. Le shader ne peut rien à cela ; il doit juste accepter des valeurs
  au-delà de 1,0 et un seuil de contour qui n'est pas « 6/255 ».
- **4:4:4** : négocié par codec côté hôte (`GpuInfo::codecs444` : NVENC H.264 et HEVC, pas AV1 ;
  AMF et oneVPL rien), accepté par le client si `isConfigSupported` répond pour le profil RExt ou
  High 4:4:4 (`chroma444ClientCapability`), **SDR seulement** (le 10 bits 4:4:4 rend vert sur
  Chrome/Windows). Les images arrivent en I444 et `importExternalTexture` les convertit : le shader
  voit du RGB dont la chrominance porte autant de détail que la luminance. `this._yuv444` est connu
  de StreamView et peut descendre au renderer.

### 1.6 Les résolutions qu'on rencontre vraiment

- Ordinateur : flux Auto = la boîte de l'écran (ratio 1 en plein écran), « Match my screen »,
  taille fixée ; en fenêtre, **réduction** fréquente (×0,5-0,9) ; sous congestion, le flux descend
  d'un cran et l'upscale remonte à ×1,33-×2.
- Téléphone et tablette : Auto streame **jusqu'à 1440 lignes** quelle que soit la largeur de
  l'écran (commit `0201eaef`, pour la netteté du pinch-zoom) : sur un écran de 1080 lignes, le cas
  courant est une **réduction ×0,75**, et le zoom repasse au-dessus de 1. Un shader « upscale
  seulement » ne couvre donc pas l'usage mobile réel.
- Au-delà de ×2 : hôte 720p sur un 4K (×3) ; AMD ne documente rien au-delà de ×2 par dimension et
  le port mpv de FSR1 s'arrête à ×2 puis laisse mpv finir ; SGSR1 et NIS sont spécifiés pour ≤ ×2.

### 1.7 L'outillage de mesure existant

| Outil | Ce qu'il mesure | Réutilisable pour le plan |
|---|---|---|
| `PipelineDiag` + `[perf]` (console, `mw_perf_diag=1`) | soumission / attente du dessin, file de décodage, pertes par cause, images présentées contre décodées | oui : c'est le coût « vu de la page » ; il s'arrête avant le compositeur |
| Sonde clic → drapeau (`mwLatency.run()`, build debug, hôte Windows ; `probePixels` du renderer) | clic → pixel présenté, médiane et p90 sur dix clics | oui : seul instrument qui voit la présentation ; onglet visible obligatoire, Chrome dédié piloté par `scripts/bench/cdp.py` |
| `scripts/bench/` (campagne, `/bench`) | matrice codec × enhancer × 4:4:4 × HDR, clic → photon, rapport | oui pour la validation finale (P5) |
| `mw-scaler-bench` (hôte, D3D11) | temps GPU et PSNR des filtres de réduction, métrique de défilement (décalage d'1 px) | la méthode est reprise ; l'outil lui-même est côté hôte |
| `UltraPlayer.js` (POC Ultra) | `timestamp-query` quand l'adaptateur l'offre, `querySet` par passe, `gpuwait.py` | oui : le seul code du dépôt qui horodate des passes WebGPU ; à reprendre tel quel pour le labo |
| `hevc-psnr.py`, `scripts/bench/content/*.html` (texte fixe, défilement, clip) | PSNR par bande ; contenus reproductibles | oui |

Ce qui manque et que le plan ajoute : une **page de laboratoire** qui fait tourner N variantes de
shader sur les mêmes images, mesure le temps GPU par requêtes d'horodatage (ou par amortissement
sur un lot quand elles manquent) et relit les pixels pour les métriques de qualité ; une
**validation statique** des shaders en CI (aucun GPU n'y tourne aujourd'hui ; `npm run check` et
`vitest` seulement) ; des **tests dorés** sur un WebGPU logiciel.

### 1.8 Le parc de banc, et ses trous

| Machine | GPU | Rôle pour ce plan |
|---|---|---|
| DualRTX | iGPU du Ryzen 9 9900X (RDNA 2, 2 unités de calcul), Arc A380, RTX 5060 Ti | trois GPU d'ordinateur, trois écrans ; encodeur et décodeur jamais sur le même GPU |
| N95 (`mw-intel`) | Intel UHD (Alder Lake-N) | iGPU faible, client Wi-Fi ; f16 y a déjà aidé (Lanczos-2 14 → 11,3 ms côté hôte) |
| UM790Pro | Radeon 780M | Windows / Ubuntu, client Ethernet |
| Mac M1 (Léo) | Apple M1 | **seule machine où la puissance GPU se lit** (`powermetrics`) ; créneaux de Bruno |
| `mw-arm` | Snapdragon 7c, Adreno 618 (Windows ARM, Chrome) | **seul Adreno du parc** ; génération ancienne (SGSR1 est mesuré par Qualcomm sur 660 → 8 Gen 2) |
| AMD 610M (192.168.1.190) | Radeon 610M | bon client, iGPU minimal |
| iPhone / iPad | Apple A / M | Safari (WebKit) ; WebGPU selon la version d'iOS ; batterie |
| Mi TV, Freebox POP | Mali-G31 | exclus de l'enhancer (mémoire ≤ 1 Go, TV) : hors cible |

**Correction du 29/10/2026** : la ligne DualRTX disait « Radeon 780M (iGPU) ». C'est l'iGPU du
Ryzen 9 9900X (id 0x13C0, plan B §9). Dans ce plan, dans ses rapports P0 à P4 et dans le journal du
labo, le « 780M » du DualRTX désigne ce GPU. Seul l'UM790Pro a un vrai 780M. Le README de `mwsr/` et
/mwsr/ le nomment juste depuis le 29/10.

**Trous** : aucun téléphone Android, aucun Mali de téléphone, aucun Adreno récent. L'objectif
« plus rapide que SGSR1 sur mobile » sera donc **démontré sur Adreno 618 et sur Apple**, et
seulement **extrapolé** aux Adreno 7xx/8xx et Mali-G7xx. Le plan le dit tel quel dans ses critères
(§8) ; un téléphone Android de prêt (Snapdragon 8 Gen 1 ou plus, et un Mali) est listé comme
dépendance souhaitable, pas bloquante.

---

## 2. Ce que disent les sources

Revue faite le 10/10/2026 sur les sources primaires (en-têtes de shaders, dépôts, billets, PDF
des éditeurs, articles), avec la convention du §0 : **[démontré]** = chiffre mesuré et publié par
une source nommée dans des conditions nommées ; **[affirmé]** = chiffre d'éditeur sans méthode ;
**[attendu]** = notre hypothèse. Les URL sont au §10.

### 2.1 FSR1 : EASU et RCAS (AMD, MIT)

- **EASU** : 12 taps en croix (un bloc 4×4 moins les coins) lus par **4 `textureGather` par
  canal** (12 instructions texture), luma approchée `0,5·B + 0,5·R + G`, analyse direction /
  longueur en quatre appels pondérés bilinéairement, noyau « Lanczos-2 approché » sans sinus ni
  racine (polynôme en `d²`), fenêtre tournée et étirée selon le contour, puis **déringing** par
  clamp entre le min et le max des 4 texels les plus proches. AMD vise ≤ 64 registres (36 pour
  la version 32 bits) ; le shader est « borné par le VALU » sur les GPU d'ordinateur.
- **RCAS** : croix à 5 taps, résolution analytique du poids de netteté qui n'écrête pas, limite
  fixe 0,1875, netteté en « stops » (0 = maximum), option de détection de bruit ; AMD interdit
  RCAS sans EASU (CAS sinon). La v1.0.2 inclut le tap central dans la borne.
- **Entrée** exigée : RGB perceptuel (sRGB ou gamma 2) dans [0, 1], anti-aliasé, sans bruit.
  Pour le HDR, l'en-tête livre un tone-mapper réversible (`c /= max3(c) + 1`) à appliquer avant
  et défaire après.
- **Échelles** : modes 1,3× / 1,5× / 1,7× / 2,0× par axe ; « 1x to 4x area » ; rien de documenté
  au-delà de 2× par axe ; le port mpv s'arrête à 2× et laisse mpv finir.
- **Coûts** : AMD donne des bornes par classe de GPU d'ordinateur, « 0,40 ms ou moins » à 4K sur
  RX 6800 XT / RTX 3080, 1,0 ms sur RX 5700 XT / RTX 2060 S [démontré par AMD, configuration
  citée mais pas la méthode] ; **aucun chiffre FSR1 sur Adreno ni Mali dans aucune source
  primaire**.
- **Variante f16** (`FsrEasuH`) : deux taps par opération empaquetée, coordonnées en f32 ; AMD la
  recommande par défaut (« no visible quality impact ») sauf sur les GPU sans f16 empaqueté et
  sur certains pilotes D3D11.

### 2.2 L'optimisation mobile d'Atyuwen (iPhone 12, 2021 ; gist MIT, billet CC BY-NC)

Un seul appareil (iPhone 12, A14), pixel shaders, 1772×819 → 2532×1170, méthode de mesure non
décrite. Chaque étape est mesurée [démontré] :

| Étape | EASU |
|---|---|
| version AMD, f32 | 5,4 ms (+ RCAS 0,9) |
| f16 | 4,7 |
| sans le déringing 2×2 | 4,3 |
| une seule analyse de direction sur des lumas interpolées | 3,8 |
| sortie anticipée bilinéaire si la direction est quasi nulle | 3,6 (« le shader est entièrement borné par la texture ») |
| **5 échantillons bilinéaires** (croix) au lieu des gathers, gathers différés au chemin contour | **1,8** |
| passe de copie évitée | 0,7 de plus gagnés |

Enseignements repris ici : la texture, pas l'ALU, est la limite sur ce GPU ; les échantillons
bilinéaires battent les gathers ; la sortie anticipée seule ne gagne presque rien si les gathers
restent sur le chemin plat. Réserves : le déringing supprimé (« artefacts rarement visibles », sur
un jeu), luma sans le bleu, pas de réduction ni de HDR ; la version fusionnée EASU + RCAS de 2025
applique la netteté sur l'anneau bilinéaire avec un facteur égal au ratio de rendu, sans
paramètre : une source d'idées, pas un composant. Le gist dépend de l'en-tête AMD.

### 2.3 SGSR1 (Qualcomm, BSD-3)

- **Un seul fragment shader** : un échantillon bilinéaire (la couleur de base), un `textureGather`
  du canal vert, un **vote de contour** (`edgeVote > 8/255`, sinon sortie bilinéaire), puis sur
  les contours trois gathers de plus (12 taps, bloc 4×4 moins les coins), poids
  `(x − 1)(x − 4)³` avec `x = 0,55·r² + terme de rang`, reconstruction d'une correction de luma
  bornée au min/max du 2×2 central, multipliée par `EdgeSharpness = 2`, ajoutée aux trois canaux.
  La chrominance reste bilinéaire. Précision `mediump` (couleurs) / `highp` (coordonnées).
- **Coûts, par Qualcomm, à fréquence GPU maximale, entrée 1240×576** [démontré, méthode non
  décrite] : SGSR1 0,476 / 0,356 / 0,295 ms et bilinéaire 0,248 / 0,196 / 0,156 ms sur Snapdragon
  888 / 8 Gen 1 / 8 Gen 2 (sorties 2340×1080 et 2400×1600). Le coût « suit linéairement la
  surface de sortie » ; le seuil n'y change rien. L'affirmation « deux fois plus rapide que les
  autres upscalers mobiles » n'a pas de méthode publiée [affirmé].
- **Aucune mesure indépendante** (ms, mW, PSNR) de SGSR1 n'a été trouvée, sur aucun GPU. Aucune
  mesure d'énergie d'un upscaler mobile nulle part. Le seul chiffre tiers (Arm, concurrent) est
  un gain d'images par seconde sur Immortalis-G720, sans ms.
- Contraintes : entrée tone-mappée et anti-aliasée, ≤ 2× par axe, pas de réduction, pas de HDR
  (seuils en `/255`, clamp à [0, 1]) ; `textureGather` n'existe pas en GLSL ES 3.00 : le port
  WebGL2 du dépôt émule chaque gather par quatre `texelFetch`.
- Le port WebGPU du dépôt diffère de la source : seuil 6/255, netteté 1,5, pas de clamp ±23/255
  (§1.2).
- **SGSR2** est temporel (profondeur, vecteurs de mouvement, jitter) : hors sujet pour un client
  qui ne reçoit que des images décodées ; ses coûts (0,9-1,1 ms en deux passes fragment sur
  8 Gen 3) servent seulement d'ordre de grandeur de « ce que coûte un multi-passes bien optimisé
  sur un Adreno haut de gamme ».

### 2.4 NIS, CAS et les noyaux classiques

- **NIS** (NVIDIA, MIT) : compute à mémoire partagée, luma 6×6, carte de contours à quatre
  directions, 64 phases de filtres, netteté USM intégrée ; **refuse toute échelle hors
  [1, 2] par axe** ; modes HDR (linéaire, PQ) et **lecture NV12 native** ; **aucun chiffre de
  coût publié**. Son port fragment du dépôt recalcule les 36 lumas par pixel : c'est le plus cher
  des trois à ce titre.
- **CAS** (AMD, MIT) : 5 ou 9 taps, entrée **linéaire** exigée, surface ≤ 4× ; pas de coût
  officiel.
- **Noyaux classiques** : Catmull-Rom en 9 échantillons bilinéaires (ou 5 sans les coins), B-spline
  en 4 ; Lanczos-2/3 sonne sur les contours durs ; Mitchell et les EWA sont les meilleurs
  réducteurs du test en anneau de Robidoux ; mpv réduit en lumière linéaire par défaut
  (`--linear-downscaling`), **élargit le noyau à la réduction** (`--correct-downscaling`) et a
  pris Hermite comme réducteur par défaut pour sa vitesse ; pour l'agrandissement il préfère la
  courbe sigmoïde à la lumière linéaire (moins de sonnerie).
- **Familles dirigées** (NEDI, iNEDI, ICBI, DCCI) : schémas 2× entiers en deux étapes, cent à
  mille fois le coût d'un bilinéaire, aucune implémentation GPU permissive maintenue : des idées
  (choisir la direction de plus faible dérivée seconde), pas des composants.
- **Shaders mpv / libplacebo** : RAVU (LGPL-3), nnedi3 (LGPL/GPL, quatre passes), FSRCNNX
  (licence ambiguë), Anime4K (MIT, réseaux de neurones réglés pour l'animation ; son port WebGPU
  tient « sous 3 ms » à 720p sur une RTX 3070 Ti, soit très loin du budget mobile),
  adaptive-sharpen (BSD, 25 positions), SSimSuperRes et KrigBilateral (LGPL-3, plusieurs passes).
  Aucun ne tient dans une passe fragment sous la milliseconde sur un iGPU ; aucun n'entre au
  produit.
- **Neuronal léger** (ESPCN, FSRCNN-s, edge-SR) : aucune donnée publique sous la milliseconde sur
  un iGPU à 1440p ; extrapolation à plusieurs ms. Écarté (§3.5).
- **Upscale conscient de la compression** : rien de publié pour un upscaler spatial temps réel sur
  du jeu à 10-50 Mbit/s ; les deux mécanismes « conscients des artefacts » des noyaux bon marché
  sont la borne 2×2 d'EASU et le détecteur de bruit de RCAS : la conception du §3.2 en reprend
  l'esprit (cohérence `a`, marge locale `H`).

### 2.5 Métriques et protocoles de qualité

- **Aucun éditeur** (AMD, NVIDIA, Qualcomm, Apple, Arm) ne publie de métrique objective pour son
  upscaler spatial : images côte à côte seulement.
- Le banc académique le plus proche de notre cas (SR+Codec, BMVC 2024 : vidéos réduites,
  compressées, puis agrandies, classées par des humains) trouve que **PSNR, SSIM et VMAF corrèlent
  mal** avec le jugement humain sur de la vidéo compressée agrandie (corrélation de rang 0,15 à
  0,45) ; LPIPS 0,43, ERQA 0,62, et un modèle sans référence 0,64 [démontré]. PSNR et SSIM restent
  des garde-fous de régression, pas des juges.
- **ꟻLIP** (NVIDIA, BSD-3, `pip install flip-evaluator`) est conçu pour comparer deux rendus à une
  distance de vue donnée (pixels par degré), donne une carte et une moyenne : le juge principal du
  plan, avec la carte pour voir *où* (texte, interface) l'écart se fait.
- **VMAF** exige référence et candidat à la même résolution, a un modèle 4K, et **récompense la
  netteté** : son mode NEG (« no enhancement gain ») est obligatoire dès qu'un sharpener est dans
  la boucle ; à titre indicatif seulement.
- **SSIMULACRA 2** (BSD-3) a des termes explicites de blocs et de sonnerie : utile pour vérifier
  qu'un upscaler n'amplifie pas les artefacts du codec.
- Vérité terrain : le rendu natif, réduit par un filtre fixe et identique pour tous les candidats
  (et dit dans le rapport) ; comparer au flux haute résolution réel mélange le bruit du codec à la
  référence et ne sert qu'en contrôle.

### 2.6 Mesurer le temps GPU et l'énergie

- **`timestamp-query` de WebGPU** : disponible largement (Chrome 121+ ; Safari aussi), mais
  **quantifié à 100 µs** par défaut dans Chrome ; le drapeau `chrome://flags/#enable-webgpu-developer-features`
  lève la quantification. Nos passes font 0,3-2 ms : le drapeau est obligatoire au labo, ou il
  faut empiler N dispatches identiques et diviser.
- **PIX** capture Chrome sous Windows (lancement depuis PIX, `--disable-gpu-sandbox`, symboles HLSL
  de Dawn) et montre la traduction D3D12 de Dawn (barrières comprises) ; **PresentMon** donne
  `GPU Busy` par image mais « moins précis sous HAGS, parfois d'environ 0,5 ms » (notre banc garde
  HAGS actif) ; **Xcode** capture Chrome sur macOS seulement avec un Chrome non signé, et aucun
  document ne couvre le processus GPU de Safari ; **`powermetrics`** donne la puissance GPU en mW,
  « estimée et possiblement inexacte » selon sa page de manuel : bon pour des deltas A/B sur la même
  machine, pas pour des absolus.
- **Mobile** : le compilateur hors ligne Mali (`malioc`) estime les cycles par unité (arithmétique,
  chargement, texture), le chemin le plus long et les registres, mode Vulkan compris, « à confirmer
  sur appareil » ; le compilateur hors ligne Adreno accepte SPIR-V, HLSL et GLSL et donne des
  comptes d'instructions ; Android GPU Inspector exige une application débogable (pas le Chrome
  de production) ; Perfetto lit les rails de puissance (ODPM, Pixel 6 et plus) et la fréquence GPU ;
  la méthode publiée par un studio (Wuthering Waves) est celle du plan : fenêtres de 30 s, luminosité
  fixe, même scène, A/B, **différences en mW et non valeurs absolues**.
- **Fréquences** : les chiffres de Qualcomm sont pris « à fréquence GPU maximale » ; en régime
  établi, après quelques minutes, on peut attendre 1,5 à 3 fois ces durées [attendu] : le rapport
  donnera les deux conditions.

### 2.7 WebGPU et WGSL dans les navigateurs du parc (lu dans les sources de Chrome, de WebKit et de Firefox le 10/10/2026)

**La texture externe.** Trois opérations seulement : `textureDimensions`, `textureLoad` à
coordonnées entières, `textureSampleBaseClampToEdge` ; ni `textureSample`, ni `textureGather`.
Dans Chrome, l'import est **sans copie** seulement si l'image décodée est une NV12 partagée lisible
par WebGPU et si l'espace demandé est `srgb` ou `display-p3` (le cas SDR matériel courant) ; alors
**chaque échantillon coûte deux fetches** (plan Y en pleine résolution, plan UV en demi, avec notre
sampler : chroma bilinéaire) et une multiplication 3×4, sans `pow` pour un flux BT.709 SDR (Chrome
assimile sa courbe à celle du sRGB). Tout le reste (I420 logiciel, P010 et 10 bits, 4:2:2, 4:4:4,
RGBA) passe par **une copie faite par Chrome** vers une image RGBA 8 bits avant notre shader : une
passe de plus, invisible pour nous, et le 10 bits perd sa précision (c'est pourquoi le renderer lit
lui-même les plans YUV en HDR). `textureLoad` sur externe = deux fetches entiers, chroma au plus
proche ; `textureSampleBaseClampToEdge` = un clamp calculé dans le shader plus les deux
échantillons. Safari (WebKit) fait de même : deux textures Metal par `CVPixelBuffer`, matrice
choisie d'après les métadonnées, formats 4:2:0, 4:2:2 et 4:4:4 en 8 et 10 bits liés sans copie, HDR
importé sans perte ; Firefox a les textures externes depuis la version 144 (Windows, macOS). Depuis
Chrome 137, une vue de texture ordinaire (`rgba8unorm`, `bgra8unorm`, `rgba16float`, un niveau)
peut être liée à un emplacement `texture_external` : un seul module WGSL peut servir les deux
feeds sur Chrome. L'objet externe expire quand la `VideoFrame` est fermée (après le `submit`), et un
bind group par image reste nécessaire ; Chrome ne ralentit plus la lecture vidéo à l'import depuis
la version 116.

**HDR.** `toneMapping: { mode: 'extended' }` sur un canvas `rgba16float` : Chrome 129 (ordinateur et
Android ; Linux en 144) ; Safari 26 : iOS fonctionne, macOS renvoyait `extended` mais affichait du
SDR (bug WebKit d'août 2025) ; Firefox : pas de canvas `rgba16float`. « extended » = aucune courbe
du navigateur, écrêtage à ce que l'écran sait montrer ; pas de PQ ni de HLG en sortie, pas d'API de
marge HDR (proposition W3C ouverte depuis juin 2025). Chrome 155-156 ajoute `srgb-linear` et
`display-p3-linear` comme espaces du canvas. Côté import, Chrome ne tone-mappe ni n'écrête, mais
fait passer le P010 par la copie 8 bits : le constat du wiki 15.5 tient toujours. WebGL n'a
toujours aucune sortie HDR (la proposition Khronos est ouverte depuis 2024).

**Fonctionnalités.** `shader-f16` : Chrome 120 (D3D12 : DXC + Shader Model 6.2 + opérations 16 bits
natives ; Vulkan : f16 + int16 + accès 16 bits aux tampons ; Metal : toujours), Safari : toujours,
Firefox : non exposé ; relevés de terrain (web3dsurvey) : ~71 % des Qualcomm, ~29 % des Adreno 6xx
(donc sans doute pas l'Adreno 618 du banc), ~97 % des 7xx, 100 % d'Apple et d'Arm. `subgroups`
(opérations de quad et de sous-groupe, **fragment et compute**) : Chrome 134 (D3D12 SM 6.0, Metal
Apple6 / Mac2, Vulkan avec l'étage fragment) ; WebKit a le code (Metal3) mais pas Safari 26.0 selon
les tables ; Firefox non. `timestamp-query` : Chrome 121 partout, **quantifié à 100 µs** sauf
drapeau développeur ; Safari 26 si compteur ; Firefox non. `adapter.info` (`vendor`,
`architecture` : « adreno-6xx », « adreno-7xx », « rdna4 », « blackwell »…) : Chrome 127, Safari 26,
Firefox 141 partiel. Immédiats (push constants) : Chrome 149-150 seulement ; `writeBuffer` deux
fois plus rapide depuis Chrome 144. Mode compatibilité : Chrome 146 (Android GLES 3.1 ; 15 % des
Android n'ont pas de Vulkan convenable), textures externes prises en charge, dérivées fines
absentes (les grossières suffisent au vote), jamais dans Safari. Chrome Android tourne sur Vulkan
depuis Android 12 (Adreno, Mali, Intel ; PowerVR sur Android 16). WebGPU dans Firefox : Windows
141, macOS Apple Silicon 147.

**Le canvas en écriture par compute.** `STORAGE_BINDING` est légal pour `rgba8unorm` et
`rgba16float`, pour `bgra8unorm` avec la fonctionnalité ; mais Chrome **recopie** le canvas sur
Android pour tout canvas storage ou BGRA, sur macOS pour `rgba8unorm`, sur Linux pour BGRA ; le
format préféré est `rgba8unorm` sur Android et Linux, `bgra8unorm` ailleurs. La passe fragment
vers le canvas est donc le seul chemin sans copie sur toutes les plateformes. Qualcomm écrit
« préférer les fragment shaders aux compute shaders » (le compute désactive la compression UBWC)
et recommande R10G10B10A2 comme format de rendu ; Arm dit la même chose pour le post-traitement
(réductions, agrandissements, flous : « les compute shaders peuvent être plus lents et moins
économes »).

**`copyExternalImageToTexture(VideoFrame → canvas)`** : légal avec `RENDER_ATTACHMENT | COPY_DST`,
mais Chrome l'implémente par la même texture externe **plus son propre pipeline de rendu plein
écran** (Dawn `CopyTextureForBrowser`) : jamais moins cher que notre triangle ; Firefox ne
l'accepte pas depuis une `VideoFrame`, Safari en 32 bits par pixel seulement. L'option tombe.

**Promotion en plan de composition.** Sous Windows, seuls les quads vidéo et les canvas « basse
latence » (Canvas2D `desynchronized`) sont candidats ; un canvas WebGPU ne l'est pas. Sous macOS, le
swap chain WebGPU est une IOSurface que le serveur de fenêtres présente et redimensionne. La mise
à l'échelle CSS d'un canvas (bilinéaire) est gratuite par rapport à une passe de plus : c'est ce
que fait déjà Canvas2D « Off », et c'est le vrai plancher de l'« agrandissement ».

**4:4:4.** Les formats `I444`, `I444P10`, `I444P12` existent dans WebCodecs ; sous Windows le HEVC
RExt matériel sort en AYUV (8 bits), Y410 ou Y416, que Chrome fait passer par la copie RGBA : la
chroma pleine résolution est conservée (aucun sur-échantillonnage), et la passe 0 économisée par
notre shader est alors celle de Chrome, pas la nôtre ; macOS et Android non vérifiés ; Safari lie
les formats 4:4:4 sans copie.

**Matrice** (première version stable ; « — » = absent ; « ? » = non vérifié) :

| Capacité | Chrome Win | Chrome macOS | Chrome Android | Chrome Linux | Safari iOS 26 | Safari macOS 26 | Firefox |
|---|---|---|---|---|---|---|---|
| WebGPU | 113 | 113 | 121 (Android 12+) | 144 (Intel), 147 (NVIDIA) | 26 | 26 | 141 Win, 147 macOS |
| `shader-f16` | 120 (DXC, SM 6.2) | 120 | 121 selon pilote | 144 / 155 | 26 | 26 | — |
| `timestamp-query` | 121 (100 µs) | 121 | 121 selon pilote | 144 | 26 si compteur | 26 | — |
| `subgroups` (fragment compris) | 134 | 134 | 134 selon pilote | 144+ | code WebKit, 26.0 ? | idem | — |
| `toneMapping: extended` | 129 | 129 | 129 | 144 | fonctionne | SDR en 26.0 (bug) | — |
| canvas `rgba16float` | 113 | 113 | 121 | 144 | 26 | 26 | — |
| `importExternalTexture(VideoFrame)` | 116 (NV12 sans copie) | 116 | 121 | 144 | 26 (sans copie) | 26 | 144 |
| `copyExternalImageToTexture(VideoFrame)` | 116 (passe interne) | 116 | 121 | 144 | 26 (32 bpp) | 26 | — |
| `adapter.info` | 127 | 127 | 127 | 144 | 26 | 26 | 141 partiel |
| `bgra8unorm-storage` | 113 | 113 | selon Vulkan | 144 | 26 | 26 | — |
| Mode compatibilité | 146 (D3D11 à l'étude) | 146 | 146 (GLES 3.1) | plus tard | jamais | jamais | non engagé |
| Immédiats | 149-150 | 149-150 | 149-150 | 149-150 | — | — | — |

### 2.8 Superoptimisation, SweetOpt, génération assistée, f16, fusion de passes

- **SweetOpt** (CeeJayDK, GPL-3 pour l'outil, versions 0.6 et 0.7 d'octobre 2026) cherche par
  énumération « le programme le moins cher, du plus petit au plus grand », plus une bibliothèque de
  réécritures, pour des **expressions** (quatre instructions, 24 opérations par région par défaut,
  cinq secondes par région) de shaders ReShade FX, HLSL SM5 et, depuis la 0.7, GLSL fragment ;
  **pas de WGSL**. Vérification par échantillonnage dense (un million de points, exhaustif sous
  2²⁴ valeurs), budgets d'erreur (exact, 8 bits couleur, coordonnées, absolu, relatif). Modèles de
  coût **d'ordinateur seulement** (RDNA 3 par défaut, NVIDIA Ada par ptxas, Intel Gen 12 et 7.5 ;
  rien pour Mali, Adreno ni Apple), coûts statiques. Résultats documentés en instructions, pas en
  temps d'image. Son banc annexe TexBench donne des mesures précieuses pour nous [démontré, GTX
  1660, RTX 5080, UHD 630] : un bilinéaire contre trois gathers contre quatre `Load` = 11,4 / 29,1 /
  57,5 unités sur GTX 1660 (sur Blackwell, quatre `Load` battent trois gathers) ; point et
  bilinéaire coûtent pareil jusqu'à 32 bits par texel ; le RGBA16F filtre à demi-vitesse sur NVIDIA
  et Intel Gen 9 ; les lectures à moins de 8 texels sont gratuites, plus chères au-delà de 16 ; un
  étalement vertical coûte plus qu'horizontal sur la plupart des cartes ; **écarter des tuiles 8×8
  entières divise le temps d'une passe lourde par deux, écarter un pixel sur deux ne gagne rien** ;
  une passe de réduction 4K → 1080p a coûté le même temps avec un bilinéaire ou quatre `Load` sur
  GTX 1660 (0,25 ms, bornée par la mémoire). Emploi ici : §6.3, sur le chemin contour une fois la
  structure fixée, résultat reporté à la main.
- **Superoptimiseurs** : STOKE (x86, abandonné), Souper (LLVM, archivé en 2025), GreenThumb, GEVO
  (mutations évolutives de noyaux CUDA mesurées sur GPU, +49 % en moyenne sur Rodinia), Mirage
  (programmes tensoriels), spirv-opt (« aucune garantie d'améliorer une métrique »), Shader
  Minifier (taille) : aucun n'optimise du WGSL ni n'a de modèle de coût mobile ; la méthode qui
  transfère est « mesurer dans la boucle, tolérer une erreur bornée ». Les analyseurs statiques
  utiles sont ceux du §6.2 : `malioc` (cycles par unité ; exemple Arm : un flou gaussien passe de
  4,53 à 0,96 cycle arithmétique en f16 et devient borné par la texture à 2,50), Radeon GPU
  Analyzer (`vk-offline`, ISA RDNA et registres sans pilote AMD), compilateur hors ligne Adreno
  (SPIR-V : instructions 32 et 16 bits, lectures texture, registres).
- **Le projet le plus proche du nôtre** : `rudybear/shaderopt` (MIT, 2026) optimise des shaders
  plein écran par réécritures exactes et sémantiques, prédicteur CPU puis mesure A/B/A/B sur GPU
  avec intervalle de confiance, sur une RTX PRO 6000 et une tablette Mali-G720. Verdicts [démontré]
  : les réécritures « de compilateur » (pliage, CSE, déroulage, `select`) font **0,0 ± 0,2 %** sur
  les neuf shaders (« le pilote les fait déjà ») ; les gains sont **algorithmiques** (un flou à neuf
  taps réécrit en cinq bilinéaires : +11 %, un PCF +24 % sur la tablette) ; le f16 explicite fait
  ≤ +1,9 % sur l'ordinateur « et souvent moins à cause des conversions », mais **+12 à +40 % sur le
  Mali-G720** ; bruit de mesure 0,1-0,9 %, porte à `max(2 %, bruit)`, porte de qualité **FLIP moyen
  0,01 et p99 0,05** : ce sont les seuils que le §8 reprend.
- **Boucles « modèle + harnais »** : KernelBench (2025 : moins de 15 % des noyaux générés battent
  PyTorch d'un coup ; avec dix tours de génération, exécution et profil, le taux monte beaucoup) ;
  NVIDIA avec DeepSeek-R1 en boucle fermée sur H100 (15 minutes par problème, « résultats
  préliminaires ») ; AlphaEvolve (boucle évolutive, +23 % sur un noyau de Gemini, +32 % sur
  FlashAttention) ; Sakana « AI CUDA Engineer » (annonce « 100× », puis rétractation : le système
  avait trouvé des failles dans l'évaluateur) ; Kevin-32B et METR documentent les **tricheries de
  récompense** (copier la référence, désactiver la synchronisation, réécrire le chronomètre).
  Enseignement unique et ferme : **le vérificateur doit être hors de portée du générateur** :
  temps GPU par horodatage, images vérifiées contre une référence indépendante, processus de mesure
  séparé. C'est la règle du §6.3. La lignée d'avant les LLM dit la même chose depuis vingt ans :
  simplification automatique de shaders d'Olano (2003) et Pellacini (2005), programmation génétique
  de Sitthi-Amorn et al. (2011 : des dizaines de milliers de variantes évaluées sur GPU, front de
  Pareto vitesse / erreur), Wang et al. (2014), Spire de He, Foley et Fatahalian (2016, successeur
  Slang). Les travaux « LLM et shaders » proprement dits (ShaderMatch, ShadAR) jugent la
  correction ou la créativité, jamais la vitesse.
- **f16, les faits** : Arm (guide des bonnes pratiques 2025) : « 16 bits suffit normalement pour le
  calcul d'une couleur de sortie », `highp` « jusqu'à deux fois les ressources et la moitié de la
  vitesse », vectoriser le f16 (`f16vec4`), éviter les conversions répétées, samplers `mediump`,
  coordonnées `highp` au-delà de 512-2048 texels (11 bits de mantisse) ; Apple (A8 et après) :
  registres 16 bits, « conversions typiquement gratuites », `half` pour les lectures de texture et
  les interpolants ; NVIDIA : Pascal (GTX 10x0) exécute le FP16 à **1/64** du débit FP32, Ampère et
  Ada à parité (gain de registres seulement) ; AMD : « Rapid Packed Math » à double débit depuis
  Vega, `-enable-16bit-types` ; Intel Xe-LP : double débit (source secondaire). WGSL : les
  textures échantillonnées sont `f32` seulement, donc **chaque fetch rend du f32** et un noyau f16
  paie une conversion par échantillon.
- **Fusion de passes et tuiles** : Arm : ne jamais agrandir par `vkCmdBlitImage` (« aller-retour
  mémoire inutile ») ; « idéalement, utiliser des fragment shaders pour le traitement d'image » ;
  les tuiles font 16×16 pixels avec 128 à 1024 bits de stockage par pixel, plus le format est large,
  plus la tuile est petite ; formats HDR recommandés RGB10_A2 et B10G11R11, RGBA16F seulement si
  nécessaire ; branches : « minimiser la divergence entre threads voisins », `min`/`max`/`mix` pour
  les petites branches, mais « brancher est généralement la meilleure solution » pour plusieurs
  chemins coûteux, et les flots uniformes par constantes de spécialisation. Apple : une lecture de
  texture « prend typiquement quelques centaines de cycles » et une sortie anticipée après une
  première lecture « fait attendre deux fois ». Mesures de taps [démontré] : Catmull-Rom en 16
  `Load`, 9 ou 5 bilinéaires et 1 tap sur R9 380 à 1600×1200 : 0,58 / 0,53 / 0,47 / 0,40 ms (sur
  GTX 980 à 1080p : 0,34 / 0,32 / 0,32 / 0,30) : sur un GPU d'ordinateur la passe domine et le
  nombre de taps compte peu ; Rákos : un gaussien 9 taps en 5 fetches, « environ 60 % plus rapide »
  sur HD 5770 ; Bjørge (Mali-T760, 1080p) : flou gaussien 41,9 ms, boîte 21,4, Kawase 4,5, « dual
  filter » 2,8 : sur un tiler, les fetches et les passes comptent énormément.
- **Sortie anticipée, art antérieur** : NEDI (2001) n'applique son estimateur qu'aux « pixels de
  contour » (5 à 15 % de l'image, variance locale au-dessus d'un seuil de 8), le reste en
  bilinéaire ; SGSR1 teste un gather et sort en bilinéaire ; NIS n'a pas de sortie anticipée par
  pixel (il saute des directions) ; EASU aucune ; Meta Quest Super Resolution (dérivé de SGSR) :
  « coût dépendant du contenu ». Aucune de ces sources ne chiffre le gain de la sortie anticipée
  seule sur un GPU moderne : Qualcomm montre même un temps insensible au seuil sur sa scène.


---

## 3. Conception du shader `mwsr`

### 3.1 Les principes, tirés des mesures

1. **Une passe, à la résolution de sortie, qui écrit la surface finale.** Pas de texture
   intermédiaire, pas de passe 0 en SDR. C'est la seule différence structurelle qui ne dépend
   d'aucune finesse de noyau, et la plus grosse : chez Atyuwen la passe de copie seule valait
   0,7 ms sur un iPhone 12 [démontré] ; ici elle vaut une passe plein écran à la résolution
   d'entrée plus un aller-retour mémoire de la trame.
2. **Le plancher, c'est la passe elle-même plus un échantillon bilinéaire.** Sur Adreno, Qualcomm
   mesure le bilinéaire pur à 0,16-0,25 ms et SGSR1 à 0,30-0,48 ms [démontré] : tout ce que le
   noyau ajoute se paie au-dessus de ce plancher, et doit donc n'être payé que là où l'œil le
   voit (les contours) et pas ailleurs. SGSR1 dépense déjà deux instructions texture sur son
   chemin plat (un bilinéaire et un gather) ; pour faire mieux il faut en dépenser **une**.
3. **Aucun `textureGather` dans le noyau de base.** Les taps sont des échantillons bilinéaires à
   des positions calculées : le même noyau lit une `texture_external` (WebGPU SDR, sans passe 0),
   une `texture_2d<f32>` (chemins HDR, laboratoire) et un `sampler2D` WebGL2. Atyuwen a montré
   sur l'iPhone 12 que remplacer les 12 gathers d'EASU par 5 échantillons bilinéaires divise le
   temps par deux sur un shader borné par la texture [démontré] ; ici c'est en plus la condition
   du jumeau WebGL2 et de la suppression de la passe 0.
4. **Des mathématiques valables en HDR.** Aucun seuil absolu en « n/255 », aucun clamp à 1,0 ; une
   compression de dynamique réversible encadre le noyau quand l'entrée est en lumière de scène
   (§4.1).
5. **Un noyau qui connaît l'échelle.** Trois régimes : réduction (filtre de surface), identité
   (contournement, §5), agrandissement (noyau directionnel) ; la netteté suit l'échelle.
6. **Original.** Idées empruntées et citées ; constantes, polynômes et structure à nous (§10).

### 3.2 Le noyau de base : un échantillon sur le plat, cinq sur les contours

Notation : `s = taille de sortie / taille d'entrée` par axe ; pour le pixel de sortie `o`, la
position d'entrée est `p = (o + 0,5) / s − 0,5` en texels d'entrée ; `t = 1 / taille d'entrée`
est le pas d'un texel en coordonnées de texture.

**Chemin plat (toutes les sorties).**

1. `C = sample(p)` : un seul échantillon bilinéaire RGB. C'est la couleur de base, et la sortie
   finale sur une zone plate.
2. `lC = luma(C)` avec une luma à deux multiplications-additions (pondération propre au projet,
   proche de Rec. 709 arrondie à des puissances de deux ; à fixer en P2).
3. **Le vote de contour par les dérivées d'écran** : `gx = dpdx(lC)`, `gy = dpdy(lC)`,
   `contraste = |gx| + |gy|` (`fwidth`). Les dérivées sont calculées par le GPU sur le quad 2×2 de
   pixels de sortie, **sans aucun échantillon supplémentaire** : elles existent en WGSL (`dpdx`,
   `dpdy`, `fwidth`, cœur du langage) et en GLSL ES 3.00 (`dFdx`, `dFdy`, `fwidth`). À
   l'agrandissement, deux pixels de sortie voisins sont à moins d'un texel d'entrée l'un de l'autre,
   et la dérivée de la luma bilinéaire est une mesure fidèle du gradient local, lissée d'un
   demi-texel. La branche qui en découle est cohérente par quad par construction, ce qui est
   exactement la granularité à laquelle un GPU paie la divergence.
4. Si `contraste < τ`, la sortie est `C`. Fin. Coût : la passe, un échantillon, une dizaine
   d'opérations. **C'est une instruction texture de moins que le chemin plat de SGSR1**
   [attendu : la dérivée n'est pas gratuite partout, mais elle est de l'ordre d'une addition par
   composante]. *Variante en paramètres (J33, 23/10/2026) : `FLAT_TAPS` 5 ou 9 remplace `C` par le
   cubique de Mitchell-Netravali (B 0, C `FLAT_C`, 0,5 = Catmull-Rom) sur les 4×4 texels, lu en cinq
   ou neuf échantillons bilinéaires et borné par eux ; sur les pixels plats il rejoint FSR1 (journal
   J33), pour cinq ou neuf fois les instructions texture du chemin plat ; défaut 1, le temps réel à
   juger sur l'appareil.*

**Chemin contour (les quads où `contraste ≥ τ`).**

5. Quatre échantillons bilinéaires de plus, en croix à un texel : `A = sample(p + (0, −t))`,
   `B = sample(p + (−t, 0))`, `D = sample(p + (+t, 0))`, `E = sample(p + (0, +t))`. Chacun est
   déjà une moyenne 2×2 décalée d'un texel ; les cinq ensemble couvrent une fenêtre de 3×3 à
   4×4 texels selon la phase `f = p − floor(p)`, soit la même emprise que les 12 taps d'EASU ou de
   SGSR1, en cinq instructions texture au lieu de douze ou de quatre gathers.
6. Direction et cohérence du contour : `g = (lD − lB, lE − lA)` (différences centrées, déjà
   lissées), `d = g / |g|` avec un epsilon ; cohérence `a = |g| / (|lD − lC| + |lC − lB| + |lE − lC|
   + |lC − lA|)` dans [0, 1] : vaut 1 sur un contour net, tend vers 0 sur une texture ou du bruit
   de compression. C'est `a`, et non la seule force du gradient, qui dose l'anisotropie : un bloc
   de compression ou un grain n'est pas un contour.
7. Poids directionnels : pour chaque tap `i` d'offset `o_i` (en texels, relativement à `p`), on
   tourne l'offset dans le repère du contour, `v = R(d) · (o_i)`, on l'étire le long du contour et
   on le resserre en travers selon `a`, puis `x = |v|²` et `w_i = W(x)` avec une fenêtre
   polynomiale en `x` (sans racine ni fonction trigonométrique) : un lobe positif et un lobe
   négatif borné, dont la profondeur dépend de `a` et de l'échelle. Polynôme et constantes à fixer
   en P2 par le labo (§6), **pas ceux d'AMD ni de Qualcomm**.
8. Reconstruction : `R = Σ w_i · tap_i / Σ w_i` sur les trois canaux (les poids viennent de la luma,
   appliqués au RGB : le 4:4:4 est servi sans échantillon de plus, §4.2).
9. Anti-sonnerie : `R` est borné entre le minimum et le maximum des cinq échantillons (borne
   faible, gratuite). Variante `DERING_EXACT` : sur le chemin contour seulement, quatre
   `textureLoad` des quatre texels entourant `p` donnent la borne exacte 2×2 d'EASU (quatre
   instructions de plus, sur les contours seulement ; WebGPU, et WebGL2 par `texelFetch`). Le labo
   dira si la borne faible suffit sur du texte. **Verdict du 23/10/2026 (journal J35)** : la borne
   faible n'est pas seulement faible, elle nuit sur un trait d'un texel : l'échantillon central y
   est l'extrême des cinq, et la borne épingle le pixel à son mélange bilinéaire, plus pâle que le
   trait (ce que Bruno a vu sur l'iPhone : « une couleur plus pâle sur le noir »). `DERING 2` ne
   garde que la borne 2×2 des texels : sur `text` à ×1,33, +0,37 dB et SSIM 0,950 → 0,958, les
   noirs des traits à la profondeur de la référence, dépassements 0 % ; option de labo (`dr=2` du
   comparateur) jusqu'au 23/10 à 15:14, où Bruno en fait le défaut du produit (J37) ; `dr=1` du
   comparateur rend l'ancienne borne.
10. Netteté fusionnée, adaptative au contraste : `out = R + λ · (R − moyenne(A, B, D, E))`, avec
    `λ = netteté · S(s) · H`, où `S(s)` croît de 0 à l'échelle 1 (contournement de toute façon) à
    1 à l'échelle 2, et `H` est la marge locale (distance de `R` au min et au max du voisinage,
    normalisée, comme le font CAS et RCAS dans leur principe) qui empêche l'écrêtage et limite les
    halos. En 4:2:0 le terme de netteté s'applique à la luma seule (ajouté aux trois canaux) ; en
    4:4:4 par canal.

**Coût par pixel de sortie [attendu, à vérifier au labo]** : chemin plat = 1 échantillon et
~10-15 opérations ; chemin contour = 5 échantillons (9 avec `DERING_EXACT`) et ~60-90 opérations,
dont une `inverseSqrt` et une division. Sur une image de jeu ou de bureau, les quads « contour »
sont une minorité ; sur du texte dense ils dominent.

Ce décompte doit être lu avec **le modèle de coût de la texture externe** (§2.7, lu dans le code de
Chrome) : sur le chemin sans copie (NV12 matériel, SDR), **chaque échantillon vaut deux fetches**
(plan Y, plan UV) et une multiplication 3×4 ; WebKit échantillonne aussi deux textures. Donc, en
fetches : chemin plat `mwsr` sur externe = 2, contre 2 + une passe 0 entière pour le port SGSR1
actuel (son bilinéaire et son gather lisent la copie RGBA) ; chemin contour `mwsr` sur externe =
10, contre 5 + la passe 0 pour SGSR1. Sur une image de texte dense, la variante **sur RGBA après
passe 0** (5 fetches, et un gather possible) peut donc battre la variante sur externe : c'est
exactement ce que P0 mesure en premier (§6.4), et la réponse peut différer par classe de GPU. Le
plan ne tranche pas d'avance ; il garde les deux feeds dans la même source.

Deux points de la littérature pèsent sur la sortie anticipée elle-même : Apple rappelle qu'un
embranchement après un premier fetch fait attendre deux fois la latence texture (« souvent il faut
juste essayer »), et les mesures de Qualcomm montrent que le seuil de SGSR1 ne change pas son
temps sur leur scène. La sortie anticipée est donc une constante `EARLY_OUT` mesurée (branche
après le vote, ou cinq échantillons émis d'emblée et branche sur l'arithmétique seule), pas un
acquis. Dans les deux cas la branche est cohérente par quad, ce qui est la seule granularité qui
paie (les mesures de TexBench : écarter des tuiles entières divise le temps par deux, écarter un
pixel sur deux ne gagne rien).

### 3.3 Les variantes et leur sélection

Une seule source WGSL, spécialisée **par constantes `override`** (réglées à la création du
pipeline, sans remonter dans un uniforme ni dupliquer les chaînes) ; une seule exception, le type
de la texture d'entrée, qui n'est pas « overridable » en WGSL et se fait par substitution du
préambule (deux modules : `FEED_EXTERNAL`, `FEED_TEX2D`).

| Constante | Valeurs | Choisie par |
|---|---|---|
| `FEED` (préambule) | externe (SDR WebGPU) · `texture_2d<f32>` (HDR `linear`/`tonemap`/`tonemap-ext`, labo, et l'option « passe 0 gardée » si P0 la retient) | le chemin HDR de StreamView, comme aujourd'hui ; sur Chrome ≥ 137 une vue de texture ordinaire se lie à un emplacement `texture_external`, donc un seul module peut servir les deux feeds là où c'est accepté |
| `EARLY_OUT` | branche après le vote · cinq échantillons d'emblée, branche sur l'arithmétique | mesure P2 par classe de GPU (latence texture contre fetches économisés) |
| `MODE` | agrandissement · réduction | l'échelle de l'image, par image ; les deux pipelines sont construits une fois et choisis à chaque dessin |
| `TIER` | performance (5 taps) · qualité (5 taps + 4 diagonales sur le chemin contour, borne 2×2 exacte) | classe d'appareil et GPU (`adapter.info.architecture` quand il existe, sinon la détection actuelle) ; le gouverneur peut descendre de qualité à performance avant d'éteindre |
| `HALF` | f16 sur le chemin contour (coordonnées et accumulateurs de poids en f32) | `device.features.has('shader-f16')` ; jamais forcé : A/B au labo par GPU, car f16 ne gagne rien sur certains GPU d'ordinateur et a des pièges de précision sur Mali. **04/11/2026, décision de Bruno : le produit tourne en f32 partout** ; le module reste au labo et à la page de comparaison, dont les envois le chronomètrent |
| `QUAD_UNIFORM` | la décision contour prise pour tout le quad, voire le sous-groupe (`quadBroadcast`, `subgroupAny`) | `device.features.has('subgroups')` et prise en charge en fragment ; sans elle, les dérivées rendent déjà la branche cohérente par quad |
| `DERING_EXACT` | borne 2×2 par quatre chargements de texels | `TIER` qualité ; sur texture externe seulement si `textureLoad(texture_external)` est disponible (§2) |
| `HDR_LINEAR` | compression de dynamique réversible autour du noyau | chemin `linear` |
| `CHROMA444` | netteté par canal | `this._yuv444` |
| `LINEAR_LIGHT` (réduction) | décodage sRGB par tap avant la moyenne, ré-encodage après | ordinateur ; coût à mesurer (côté hôte, le bilinéaire en lumière linéaire gagnait 4,6 dB de PSNR pour +30 % de temps sur le N95) |

Uniformes (un seul tampon, réécrit **seulement quand une valeur change**, pas à chaque image) :
tailles d'entrée et de sortie, `s`, phase, seuil `τ`, netteté, drapeaux. Bind group mis en cache
pour les feeds `texture_2d` ; par image pour la texture externe (obligatoire, elle expire).

### 3.4 Les techniques récentes du langage, et ce qu'elles achètent

Statuts lus dans les sources des navigateurs le 10/10/2026 (détail et matrice au §2.7) ; P0 relève
ce que chaque machine du parc expose vraiment (`adapter.features`), car la version ne suffit pas
(f16 dépend du pilote, les sous-groupes de l'étage fragment, etc.).

| Technique | Statut | Usage ici | Gain |
|---|---|---|---|
| Lecture directe de `texture_external` (`textureSampleBaseClampToEdge` et `textureLoad`, cœur du WGSL) | Chrome 116+, Safari 26, Firefox 144 (Windows, macOS) ; sans copie seulement pour une NV12 matérielle SDR dans Chrome | supprime la passe 0 en SDR | une passe plein écran et un aller-retour mémoire [démontré par l'architecture] ; mais chaque échantillon = deux fetches (§2.7) : le net se mesure en P0 |
| Dérivées `dpdx` / `dpdy` / `fwidth` (cœur, dérivées grossières autorisées même en mode compatibilité) | partout, WebGL2 compris | vote de contour sans échantillon | une instruction texture de moins que SGSR1 sur le plat [attendu] |
| `override` (cœur WebGPU 1.0) | Chrome 113, Safari 26, Firefox 141 | une source, N pipelines spécialisés | aucune branche uniforme morte dans le code compilé ; pas de chaînes dupliquées |
| `enable f16` (`shader-f16`) | Chrome 120 (D3D12 : DXC + SM 6.2 + 16 bits natifs ; Vulkan : f16 + int16 + tampons 16 bits ; Metal : toujours) ; Safari 26 toujours ; Firefox non ; terrain : ~29 % des Adreno 6xx, ~97 % des 7xx, 100 % Apple et Arm | arithmétique du chemin contour, en vecteurs (`vec4h`), coordonnées en f32, conversions minimisées (chaque fetch rend du f32) | 5,4 → 4,7 ms chez Atyuwen (A14) ; +12 à +40 % mesurés par shaderopt sur Mali-G720 ; ≤ 2 % et parfois négatif sur NVIDIA Ampère et après ; **jamais sur Pascal** (f16 à 1/64 du débit) [démontré ailleurs] |
| `enable subgroups` (opérations de quad et de sous-groupe, **fragment et compute**) | Chrome 134 (D3D12 SM 6.0, Metal Apple6/Mac2, Vulkan avec l'étage fragment) ; WebKit a le code (Metal3) mais pas Safari 26.0 selon les tables ; Firefox non | branche contour uniforme par sous-groupe (`subgroupAny`), en flot de contrôle uniforme seulement ; lanes auxiliaires au comportement dépendant de l'appareil | supprime le coût de divergence résiduel ; option, pas fondation |
| `GPUAdapterInfo` (`adapter.info.vendor` / `architecture` : « adreno-6xx », « adreno-7xx », « rdna4 », « blackwell »…) | Chrome 127, Safari 26, Firefox 141 partiel | choix du `TIER`, de `HALF` (pas sur Pascal) et de `EARLY_OUT` par GPU, à la place du reniflage WebGL | une détection standard, qui ne crée plus de contexte WebGL jetable |
| `timestamp-query` | Chrome 121 partout, **quantifié à 100 µs** sauf `chrome://flags/#enable-webgpu-developer-features` ; Safari 26 si compteur ; Firefox non | labo (§6) et, si présent, quatrième signal du gouverneur | un coût GPU réel sur WebGPU, qui manque aujourd'hui |
| `requestAdapter({ powerPreference: 'low-power' })` + `navigator.getBattery()` | Chrome seulement (Safari n'a jamais eu `getBattery`) | sur batterie, préférer l'iGPU d'un portable à deux GPU | à mesurer : un décodeur sur l'autre GPU fait tomber l'import dans le chemin à copie |
| Canvas en `STORAGE_BINDING` + compute par tuiles | légal pour `rgba8unorm` et `rgba16float`, `bgra8unorm` avec la fonctionnalité ; mais Chrome **recopie** le canvas sur Android pour tout canvas storage, sur macOS pour `rgba8unorm` ; Qualcomm : « préférer les fragment shaders » | **expérience P2.4, ordinateur seulement** : une tuile de sortie 16×16 charge une fois ses texels d'entrée en mémoire partagée (0,5-0,6 chargement par pixel de sortie à ×2 au lieu de 5) | prometteur sur GPU d'ordinateur ; sur mobile la passe fragment est le seul chemin sans copie : expérience à porte, pas la base |
| Immédiats (`immediate_address_space`, push constants) | Chrome 149-150 seulement | non : un seul tampon uniforme, réécrit au changement seulement (`writeBuffer` deux fois plus rapide depuis Chrome 144) | — |
| Espaces de canvas `srgb-linear` / `display-p3-linear` | Chrome 155-156 ; WebKit a le code | le chemin HDR `linear` pourrait écrire la lumière linéaire sans encodage sRGB | quelques opérations par pixel ; mineur |
| `pack2x16float` / `unpack` ; `dot4U8Packed` | cœur / `packed_4x8_integer_dot_product` Chrome 123 | empaquetage éventuel des taps en f16 pour la variante compute | marginal ; seulement si la variante compute est retenue |
| Mode compatibilité WebGPU (`featureLevel: 'compatibility'`) | Chrome 146 (Android GLES 3.1 ; D3D11 à l'étude) ; jamais dans Safari ; Firefox non engagé ; textures externes prises en charge, pas de `dpdxFine`, pas de sous-groupes | étend la portée de la saveur WebGPU sur Android ; ne remplace pas le jumeau WebGL2 (Safari, Firefox, iOS anciens) | portée, pas vitesse |

Ce que le shader **ne fait pas**, et pourquoi : pas de `discard` (coûteux sur les tuiles) ; pas
de boucle à bornes dynamiques ni d'indexation dynamique de tableaux (débordements de registres
sur Adreno) ; pas de lecture dépendante (toutes les coordonnées sont calculées depuis `p`) ; pas
de `textureSample` avec mipmaps (il n'y en a pas sur une trame vidéo) ; pas de deuxième passe
séparable (deux écritures plein écran coûtent plus que quatre taps de plus sur un GPU à tuiles).

### 3.5 Ce que le plan écarte

- **Temporel** (SGSR2, FSR2, ASR) : il faut profondeur, vecteurs de mouvement et jitter de
  caméra ; un client qui ne possède que des images décodées, déjà tone-mappées, anti-aliasées et
  compressées ne peut que les falsifier (fantômes à chaque mouvement). Écarté pour la résolution ;
  une mémoire de *stabilisation* sans vecteurs (question de Bruno du 21/10) est traitée au §14 :
  étude courte, hors v0.4.1, derrière une porte chiffrée.
- **Neuronal** (FSRCNN, ESPCN, Anime4K « upscale ») : plusieurs passes et des milliers
  d'opérations par pixel ; aucune donnée publique ne les place sous la milliseconde sur un iGPU à
  1440p. Écarté pour la v0.4.1, noté au §12 comme piste de labo si le harnais existe.
- **NIS** comme base : son chemin par pixel recompte 36 lumas et quatre cartes de contour ; sa
  force est le compute à mémoire partagée, pas le fragment. Reste un cran de l'échelle.
- **Reprendre le gist d'Atyuwen tel quel** : il dépend de l'en-tête AMD (MIT, compatible), mais
  il supprime le déringing, n'a pas de réduction, pas de HDR, et sa version fusionnée applique la
  netteté sur l'anneau bilinéaire sans paramètre : c'est une source d'idées, pas un composant.

### 3.6 Le jumeau WebGL2 : même noyau, même coût, sans alourdir la version WebGPU

Réponse à la question posée pendant la rédaction (« une version optimisée WebGPU et une version
WebGL2 ? le même shader avec repli, sans alourdir le compute ? ») :

- **Même algorithme, deux textes.** Le noyau de base (§3.2) n'emploie que des échantillons
  bilinéaires et des dérivées d'écran : il s'écrit à l'identique en GLSL ES 3.00 (`texture`,
  `dFdx`, `dFdy`, `texelFetch` pour la borne exacte). Rien n'est émulé, rien n'est dégradé : le
  GLSL n'a pas quatre `texelFetch` par gather comme les ports actuels du dépôt.
- **Les couches WebGPU sont additives**, derrière des `override` : f16, quad uniforme, lecture
  directe de la texture externe, variante compute. Le WGSL compilé sans ces couches est le même
  programme que le GLSL ; avec, il est plus rapide. Aucun code de compatibilité ne pèse sur lui.
- **Le GLSL a sa propre demi-précision gratuite** : `precision mediump float` sur les couleurs et
  les poids (les coordonnées restent `highp`) donne du f16 sur Adreno, Mali et Apple en WebGL2,
  là où `enable f16` le donne en WGSL. C'est la seule différence de texte entre les deux.
- **Un seul générateur, deux cibles ?** Un petit module JS tient les deux textes côte à côte,
  avec les constantes partagées (polynôme, seuils, pondération de luma) injectées dans les deux
  depuis une seule table : pas de mini-langage, mais une garantie que les chiffres sont les mêmes.
- **Un test de conformité** (P4) rend les deux textes sur les mêmes images (labo, §6) et exige
  une différence maximale de 2/255 par canal en SDR ; au-delà, c'est un bug de port, pas une
  tolérance. *[P4, 21/10 : tenu là où aucune dérivée n'intervient (réduction : 1/255). Sur le noyau,
  la ligne de référence des dérivées d'écran « coarse » est le choix de l'implémentation et diffère
  entre les espaces fenêtre de GL (y vers le haut, `dFdy` = −`dpdy`) et de WebGPU : les jumeaux ne
  peuvent pas être identiques au pixel sur un contour (17 % des pixels du texte au-delà de 2/255, autant
  d'un côté que de l'autre de la bilinéaire). Le critère devient « même qualité face à la vérité
  terrain » : FLIP 0,071 / 0,019 / 0,029 des deux côtés, PSNR à 0,025 dB. Rapport
  `upscaler-mwsr-p4.md` §3, journal J18.]*
- **Qui tourne où** : la saveur WebGL2 reste la production SDR tant que la porte P4 n'a pas
  tranché WebGPU contre WebGL2 avec le nouveau shader sur chaque classe d'appareil (le wiki 15.3
  l'a laissé ouvert) ; WebGPU porte le HDR et, si P4 le confirme, le plein écran. *[Porte P4, 21/10 :
  sur ordinateur (RTX, 780M) WebGL2 reste la saveur SDR — à shader égal sur le GPU, la chaîne WebGPU
  (import, soumission, attente) finit son image 2 × plus tard sur le RTX et 1,1-1,2 × sur le 780M,
  quelle que soit la passe ; exception, la réduction 4K → 1440p sur le 780M (vues sRGB : WebGPU 0,82 ×).
  Appareils de Bruno le 22/10 : iPhone 13 Pro → WebGPU (0,25-0,53 ×, l'envoi `texImage2D` de WebKit coûte
  3-24 ms de fil principal) ; Mac M1 Pro dans Chrome → égalité à 60 i/s, WebGL2 reste ; realme (Mali-G57)
  → égalité selon la règle, WebGL2 reste (à la taille de son écran, WebGPU 0,76-0,90 ×, réduction
  0,50 ×) ; `mw-arm` (Adreno 618) → WebGL2 seul, `gl-mwsr` 0,63-0,77 × SGSR1. Clic-photon des
  téléphones le 22/10 (J28), le labo s'arrêtant à la fin du GPU (J27) : iPhone → WebGPU confirmé, sans
  régression au photon et à cadence plus stable ; realme → l'échelle coupe la passe dans les deux saveurs
  à ×1,33, il reste le présentateur simple (égal à 960×540, WebGPU 53-55 i/s contre 30 à 1080p). Rapport
  P4 §5, journaux J20 et J23-J28. Décision par classe de Bruno le 22/10 (§9) : `gl-mwsr` sur PC et Mac,
  `mwsr` sur iPhone, `gl-mwsr` à la place de SGSR1 sur Snapdragon, aucun upscaler automatique sur les
  Mali d'entrée de gamme, dont le présentateur simple passe à WebGPU ; appliquée en P6.]*

---

## 4. HDR, 4:4:4, agrandissement et réduction

### 4.1 HDR

Les trois feeds HDR existent déjà (§1.5) ; le shader doit seulement s'y comporter :

- **`tonemap`, `tonemap-ext`, `browser`** : l'entrée est déjà SDR après la passe 0 ou l'import ;
  rien de particulier, le noyau tourne tel quel.
- **`linear`** (AV1 logiciel, canvas `rgba16float` étendu, 1,0 = 203 nits, lumière de scène
  BT.2020 → P3) : le noyau reçoit des valeurs au-delà de 1,0 et une luminance très étirée. *[P3,
  21/10 : la passe 0 écrit en réalité des valeurs encodées sRGB prolongées au-delà de 1,0 (1000 nits
  ≈ 2,0) ; la compression ci-dessous est sans objet, le noyau tourne tel quel et tient C8 en qualité,
  rapport `upscaler-mwsr-p3.md` §2.]* Le
  plan applique **une compression de dynamique réversible** autour du noyau : `y = x / (x + k)`
  par canal à l'entrée (k = 1 au blanc de référence), le vote, les poids, la borne et la netteté
  en `y`, puis `x = k · y / (1 − y)` à la sortie. Le noyau étant une combinaison convexe bornée
  plus un terme de netteté borné par la marge locale, `y` reste dans [0, 1) et l'inverse est
  sûre. Le principe est celui du tone-mapper réversible qu'AMD livre dans son en-tête (SRTM) pour
  le même usage ; la fonction et la constante sont les nôtres. Les seuils (`τ`, netteté)
  s'appliquent ainsi à des valeurs comparables à du display-referred, et un reflet spéculaire
  n'est ni écrêté ni rendu gris.
- Rien d'autre : pas de tone-mapping dans le shader (ce n'est pas son rôle), pas de conversion de
  gamut (le noyau est linéaire par canal), format intermédiaire inexistant (une passe).
- **Test** (P3) : une rampe PQ synthétique et un texte à fort contraste sur fond à 1000 nits,
  comparés à FSR1 `linear` : maximum conservé, aucun pixel au-dessus du maximum local d'entrée,
  écart de teinte borné (métrique ΔE dans le labo), et le temps GPU ajouté par le shader inférieur
  à celui de FSR1 sur le même feed (le gros du coût HDR reste le décodage logiciel AV1, 31 ms de
  p99 au wiki 15.5 : le shader ne le changera pas, et le plan le dit).

### 4.2 4:4:4

- En 4:2:0, la chrominance arrive déjà lissée par l'import (sur-échantillonnage bilinéaire du
  navigateur) : le noyau directionnel appliqué au RGB ne lui fait aucun mal, et la netteté sur la
  luma seule évite d'amplifier les franges de chroma.
- En 4:4:4 (texte, code, tableurs : la raison d'être du mode), la chrominance porte autant de
  contour que la luma : SGSR1 et NIS ne reconstruisent **que la luma** et laissent la chroma
  bilinéaire, d'où des franges colorées sur du texte coloré ; EASU filtre les trois canaux au prix
  de trois fois plus de gathers. Ici les cinq taps portent déjà le RGB : poids directionnels
  appliqués aux trois canaux sans coût de plus, et netteté **par canal** quand `CHROMA444` est
  levé (le renderer le reçoit de StreamView).
- Toujours SDR seulement (décision du wiki 15.6) : pas de chemin HDR 4:4:4 à prévoir.
- **Test** (P3) : mire de texte coloré (syntaxe colorée, fond sombre) en 4:4:4 et en 4:2:0,
  métrique sur les canaux de chroma séparément ; critère au §8.

### 4.3 Les régimes d'échelle

| `s` par axe | Régime | Noyau | Netteté |
|---|---|---|---|
| exactement 1 sur les deux axes | identité | **contournement, §5** : aucun shader d'enhancer | aucune (exigence du plan) |
| 1 < s ≤ 2 | agrandissement, cas nominal | §3.2 | `S(s)` de 0 à 1 |
| s > 2 (720p sur 4K, ×3) | agrandissement fort | même noyau : la fenêtre reste ±1 texel d'entrée, donc ±s pixels de sortie ; au-delà de ~×2,5 l'image est douce comme avec FSR1 et SGSR1, qui ne sont spécifiés que jusqu'à ×2 | 1 |
| 0,5 ≤ s < 1 (1440p sur un téléphone 1080p, fenêtre) | réduction modérée | filtre de surface : 4 taps bilinéaires aux coins d'un carré centré sur `p`, de côté `(1/s)` texel ; pas de vote de contour | éteinte (la réduction aiguise déjà) |
| 0,25 ≤ s < 0,5 (4K dans une petite fenêtre) | réduction forte | 9 taps (3×3) sur un carré de côté `1/s` ; en dessous de 0,25, 16 taps | éteinte |

- Les axes peuvent différer légèrement (arrondis du canvas) ; le régime se décide sur `min(s_x,
  s_y)` et le noyau utilise `s` par axe.
- `LINEAR_LIGHT` (réduction en lumière linéaire) est une option mesurée en P3 : le décodage sRGB
  par tap coûte quelques opérations, le gain en qualité est connu côté hôte (+4,6 dB au banc
  `mw-scaler-bench` sur le N95, pour +30 % de temps) ; décision par classe d'appareil. *[P3, 21/10 :
  mesuré (J12, J14) — la lumière linéaire est le terme décisif (+2,5 à +12 dB), mais calculée elle
  coûte : la grille 2×2 à `LINEAR_LIGHT 1` vaut 1,7 à 2,2 × le plancher sur le 780M, le sRGB exact
  calculé (`2`) ×2,9 de plus. Et elle se fait gratuitement par le matériel : vues `-srgb` sur la
  texture de la passe 0 et sur le canvas, l'échantillonneur décode chaque texel avant de filtrer, la
  cible encode à l'écriture (`LINEAR_LIGHT 0`). Entre ×0,5 et 1:1 **un seul échantillon bilinéaire à
  travers les vues** remplace la grille 2×2 — meilleur en PSNR, FLIP et aliasing, au coût du
  plancher, donc C10 tenu en coût ; sous ×0,5 les grilles 3×3 / 4×4 à travers les vues (aliasing ÷ 5
  contre la lumière calculée). `MWSR_DOWN_TAPS_SRGB`, `mwsrRegime(…, { srgb })` ; la table calculée
  (2×2 / 3×3 / 4×4, `LINEAR_LIGHT 1`) reste pour les feeds sans vue sRGB (`rgba16float`). Rapport
  `upscaler-mwsr-p3.md` §4 et §6, journal J15.]*
- Le pinch-zoom et l'échelle de qualité changent `s` d'une image à l'autre : les pipelines des
  deux modes sont construits une fois à la création du renderer, et le choix se fait au dessin ;
  aucune compilation en cours de flux.
- Au-delà de ×2 : pas de second étage (une passe de plus coûterait plus qu'elle ne rapporte sur
  mobile, et le cas est rare). Noté au §12 comme piste si le labo montre un besoin.

---

## 5. Le passage direct à résolution native

### 5.1 Ce qui se passe aujourd'hui

Quand l'image et le canvas ont la même taille (plein écran avec « Match my screen » ou une boîte
Auto d'ordinateur, écran à `devicePixelRatio` entier), rien n'est contourné : sur WebGL2,
`gl-fsr1` fait EASU à l'échelle 1 (douze taps pour recopier un pixel) puis RCAS ; sur WebGPU, passe
0 puis l'enhancer. Seul `Canvas2D` (enhancer éteint) est déjà une identité : tampon à la taille de
l'image, aucun étirement.

### 5.2 La règle

**Détection par image, dans le renderer** : `inW === cw && inH === ch` (égalité exacte sur les
deux axes, aucune tolérance : une échelle de 0,98 n'est pas « identique » et se filtre). Alors :

- **aucune passe d'enhancer, aucune netteté, aucune texture intermédiaire** ; l'overlay affiche
  « Off (1:1) » pour que Bruno et les utilisateurs voient que l'enhancer dort ;
- le gouverneur ne reçoit **pas** d'observation pendant le contournement (sinon il mesurerait le
  coût d'un blit et « remonterait » un enhancer qui ne tourne pas) ;
- dès que `s` change (fenêtre, zoom, baisse de résolution sous congestion), l'enhancer reprend à
  l'image suivante, sans recréation de quoi que ce soit.

### 5.3 Le chemin le moins cher pour poser l'image, par renderer

| Renderer | Chemin au 1:1 | Copies GPU | À mesurer (porte P1) |
|---|---|---|---|
| WebGPU, SDR | la passe `off` existante : un triangle plein écran qui lit la texture externe et écrit le canvas. L'option `copyExternalImageToTexture({ source: frame }, { texture: canvasTex })` a été examinée et **écartée avant mesure** : dans le code de Chrome elle construit la même texture externe puis lance son propre pipeline de rendu plein écran (Dawn `CopyTextureForBrowser`), donc jamais moins cher que notre triangle ; Firefox ne l'accepte pas depuis une `VideoFrame`, Safari en 32 bits par pixel seulement (§2.7) | une écriture du canvas | contre Canvas2D au clic → photon plein écran |
| WebGPU, HDR `linear`/`tonemap` | la passe 0 (YUV → lumière ou ACES) écrit directement le canvas : c'est déjà ce que fait `off` | une | rien de plus : la passe 0 est la présentation |
| WebGPU, `tonemap-ext` | idem, depuis la texture externe | une | idem |
| WebGL2 | téléversement de la `VideoFrame` (`texImage2D`) puis un triangle trivial, comme son `off` d'aujourd'hui | le téléversement + une écriture | contre Canvas2D au clic → photon plein écran |
| Canvas2D | `drawImage(VideoFrame)` à taille égale : déjà l'identité | une (celle du navigateur), zéro copie sur les plateformes où Chrome dessine la trame comme texture | référence |

**Ce que l'architecture ne permet pas** : passer de WebGL2/WebGPU à Canvas2D quand `s` devient 1
en cours de flux (un canvas ne change pas de contexte), et le contraire quand l'échelle de qualité
baisse la résolution et que l'enhancer doit reprendre. Deux issues, à trancher par Bruno sur les
mesures de P1 :

1. **Accepter le blit trivial** comme « passage direct » du renderer à shaders :
   une écriture de surface, c'est le minimum possible sur un canvas, et le surcoût résiduel par
   rapport à Canvas2D est celui de la présentation WebGPU/WebGL mesurée en septembre, pas celui
   d'un shader. C'est la recommandation du plan si l'écart au clic → photon plein écran est sous
   1 ms de médiane.
2. **Deux canvas superposés** (un `2d`, un `webgpu`/`webgl2`), l'un visible à la fois, bascule au
   changement de régime : vrai Canvas2D au 1:1 **et** enhancer dès que l'échelle bouge. Coût : un
   second contexte vivant, une image de transition, et toute la plomberie de StreamView (sonde,
   curseur hôte, overlay, zoom) à faire suivre. À n'ouvrir que si P1 mesure un écart réel ; hors
   v0.4.1 sinon.

Le « zéro copie » absolu reste l'élément `<video>` (le compositeur présente la trame décodée,
parfois en plan matériel) : 35-45 ms plus lent au clic (wiki 15.5), écarté par la règle de
latence. Le plan ne le remet pas en jeu.

### 5.4 Au lancement

Quand l'enhancer est éteint, rien ne change : Canvas2D en SDR. Quand il est coché et que la
géométrie de lancement est 1:1, le renderer à shaders est tout de même créé (il devra agrandir si
le flux descend d'un cran) et contourne par image. Le plan ne propose pas de choisir Canvas2D au
lancement sur la foi de la géométrie, précisément à cause de l'échelle de qualité.

---

## 6. La méthode d'optimisation

### 6.1 Le labo

Une page (`scripts/bench/scaler-lab/` ou un dossier debug du frontend, hors bundle de
production) qui :

1. charge les images d'entrée (PNG de référence réduits hors ligne, et la boucle WebCodecs pour les
   variantes compressées) dans une `texture_2d`, et aussi comme `VideoFrame` pour le feed externe ;
2. compile la liste des variantes (clé `mwsr` × `override`s, plus les témoins SGSR1 dépôt et
   source, NIS, FSR1, bilinéaire, Smooth2D) et signale toute erreur de compilation avec le texte du
   compilateur ;
3. rend chaque variante N fois à chaque échelle, avec `timestamp-query` autour de chaque passe
   (code d'`UltraPlayer.js`), ou à défaut un lot de K soumissions amorties ; compte les quads sur le
   chemin contour par un compteur atomique dans un tampon (variante d'instrumentation, jamais
   mesurée pour le temps) ;
4. relit la sortie en PNG (`copyTextureToBuffer`) pour la chaîne Python (FLIP, SSIM, PSNR,
   SSIMULACRA 2, dépassements, scintillement), et affiche côte à côte avec un curseur et un
   agrandissement ×4 pour l'œil ;
5. écrit un JSON par cellule (variante, échelle, image, GPU, navigateur, fonctionnalités exposées,
   médiane, p99, dispersion), que `report.py` du banc sait agréger.

La page tourne sur n'importe quel navigateur du parc (Safari, Chrome ARM, iPhone compris) :
c'est elle qui donne les chiffres mobiles, pas un outil natif.

Le point 4, pour l'œil, est une page à part depuis le 23/10 (journal J30) : `compare.html`, les
renderers du produit sur une image 1440p (réductions 1440 → 1080 et 720, agrandissements 720 et
1080 → 1440 depuis sa réduction bicubique), leurs sorties en bandes sous un même zoom, chacune avec son
SSIM, son PSNR et son temps par image ; `scaler_lab.py --serve` l'imprime (`/compare`) pour un
téléphone comme pour un PC.

### 6.2 Les estimateurs hors ligne

- **Mali** : `malioc` (Arm Mobile Studio) sur le GLSL ES du jumeau et sur le SPIR-V émis par Tint
  ou naga depuis le WGSL ; cycles par unité, unité limitante, registres, pourcentage d'opérations
  16 bits. C'est la seule fenêtre sur Mali sans téléphone Mali ; estimation, à dire comme telle.
- **Adreno** : compilateur hors ligne Adreno (Qualcomm Package Manager) sur SPIR-V ou GLSL :
  comptes d'instructions et de lectures de texture, à mettre en regard des mesures sur l'Adreno
  618 du banc ARM.
- **Désassemblage** : Tint (`--format hlsl|msl|spirv`) et naga pour lire ce que le navigateur
  compile vraiment (les boucles déroulées, les branches, les conversions implicites), et PIX sur
  DualRTX pour le D3D12 final.

### 6.3 La boucle d'optimisation, et la place de l'assistance par modèle

La méthode est celle d'un superoptimiseur à la main, pas d'un outil : **énumérer des variantes,
mesurer, garder, consigner**. Chaque variante est une combinaison de choix discrets (polynôme de
fenêtre, luma, forme du vote, nombre de taps, borne, f16 ou non, ordre des opérations,
approximations de `inverseSqrt` et de la division), générée depuis la table de constantes du
§3.6 ; le journal (`journal.md` à côté du labo) note pour chacune : hypothèse, mesure, verdict.
Règles :

- rien n'entre dans le shader sans un chiffre du labo sur au moins deux GPU de classes
  différentes (un iGPU d'ordinateur, l'Adreno ou l'Apple) ;
- une variante qui gagne du temps mais perd en qualité (critères C3-C4) est consignée et refusée ;
- trois tours au plus par question (le critère d'arrêt du §8.3 s'applique au chapitre entier).

**L'assistance par modèle de langage** (ce que ce chantier fait de toute façon, puisqu'une
session Claude écrit le shader) prend ici la forme de la boucle « proposer, mesurer, garder » avec
le harnais comme juge : le modèle propose des réécritures candidates (tap de moins, polynôme plus
court, empaquetage f16, réordonnancement), le labo les mesure, le journal garde la trace. C'est la
forme réaliste de la « génération de shaders assistée par IA » : pas de génération à l'aveugle,
un harnais qui tranche. Les travaux publiés sur ce schéma (§2.8) disent tous la même chose : les
réécritures qu'un compilateur sait faire ne rapportent rien (0,0 ± 0,2 % chez shaderopt), les
gains sont algorithmiques (moins de fetches, f16 sur mobile, une passe au lieu de deux), et le
générateur triche dès que le vérificateur est à sa portée. D'où trois règles de harnais : le
temps vient des horodatages GPU, jamais d'un chronomètre que le code mesuré peut toucher ; les
images sont comparées hors de la page à une référence f32 indépendante ; la session qui propose
les variantes n'édite pas les scripts de mesure (ils sont commités avant P2.2 et ne bougent plus
pendant la boucle).

**SweetOpt** (§2.8) accepte du HLSL SM5, du GLSL fragment et ses propres expressions, pas du WGSL,
avec des modèles de coût d'ordinateur seulement : il sert sur le **chemin contour** (une fonction
de ~80 opérations, à découper en régions de 24 opérations), une fois la structure fixée ; le
résultat est reporté à la main dans le WGSL et le GLSL et revalidé par le test de conformité et par
la mesure (son coût est statique et ne connaît ni Adreno ni Apple). À utiliser en fin de P2.2,
jamais avant que les mesures aient fixé la structure : optimiser des instructions d'un noyau qu'on
va changer est du temps perdu. Les constantes du noyau trouvées par la boucle sont les nôtres
(§10) : un outil qui réécrit une expression n'en change pas l'auteur.

### 6.4 Ce qu'on mesure d'abord (P0, avant toute ligne de noyau)

1. le coût de **N échantillons bilinéaires sur `texture_external`** contre **blit + N sur RGBA**,
   pour N = 1, 5, 9, sur chaque GPU du parc : c'est la décision n° 2 du §0 ;
2. le coût des **dérivées** (`fwidth` d'une luma) contre un deuxième échantillon : la base du vote ;
3. la **dispersion** répétition à répétition par GPU, et la quantification des horodatages
   observée ;
4. les **lignes de base** complètes des cinq techniques existantes, chaîne et noyau seul.

---

## 7. Le protocole de comparaison équitable

### 7.1 Les règles d'équité

1. **Deux comptes par technique, toujours** : la **chaîne telle qu'elle tourne** chez
   l'utilisateur (passe 0 comprise pour les shaders WebGPU actuels, deux passes pour FSR1 WebGL2,
   `importExternalTexture` ou `texImage2D` compris) et le **noyau seul** sur une texture RGBA déjà
   matérialisée, même taille, même format. La première dit ce que le joueur paie ; la seconde dit
   ce que vaut l'algorithme. Un gain qui n'apparaît que dans la première est un gain d'architecture
   (vrai, mais à nommer ainsi) ; un gain qui n'apparaît que dans la seconde ne se verra pas.
2. **Mêmes images, même ordre.** Trois jeux d'entrée, les mêmes pour toutes les techniques :
   - **synthétique sans compression** : la page de laboratoire rend elle-même le contenu à la
     résolution de sortie (vérité terrain) et à la résolution du flux (entrée), avec les pages
     de banc du dépôt (`scripts/bench/content/still.html` texte fixe, `scroll.html` défilement,
     `cod.html` clip) et deux mires ajoutées (texte coloré pour le 4:4:4, zone-plate et traits fins
     pour la réduction) ; la réduction de la vérité vers l'entrée se fait hors ligne (Lanczos-3,
     Python), une fois, et les fichiers sont versionnés hors dépôt comme les clips du banc ;
   - **synthétique compressé** : les mêmes images passées dans une boucle WebCodecs
     encodeur → décodeur dans la page (HEVC, puis H.264 et AV1, 20 Mbit/s à 1080p60 et 40 à 1440p),
     pour que la `VideoFrame` qui nourrit la texture externe ait les artefacts d'un vrai flux ;
   - **flux réel** depuis la `--dev` de DualRTX pour les mesures de bout en bout (clic → photon,
     lignes `[perf]`, images présentées / décodées).
3. **Mêmes sorties** : même taille de canvas (pleine résolution de l'écran du client, et une
   fenêtre fixe pour la réduction), même format, même configuration (`opaque`, et `rgba16float` +
   `extended` pour les cellules HDR), même `devicePixelRatio`.
4. **Chauffe exclue, répétitions, dispersion** : 2 s de chauffe (compilation des shaders, montée
   des horloges), puis au moins 600 images par cellule, trois répétitions entrelacées A/B/A/B/A/B,
   médiane et p99 ; une différence n'est annoncée que si elle dépasse **deux fois** l'écart
   répétition à répétition, et le rapport donne cet écart.
5. **Conditions tenues et consignées** : GPU au mode par défaut (jamais « performances max »,
   règle du banc), HAGS tel quel, luminosité fixée et verrouillage de rotation sur les mobiles,
   mode avion sauf pour les cellules de flux réel, batterie entre 40 et 80 % et chargeur
   débranché pendant les cellules d'énergie, refroidissement entre deux cellules sur téléphone
   (température de surface stable), aucune autre application, encodeur et décodeur jamais sur le
   même GPU (règle DualRTX), onglet visible (la sonde dort sinon).
6. **Les shaders de référence sont ceux du dépôt**, tels qu'ils tournent (le port SGSR1 avec son
   seuil 6/255 et sa netteté 1,5, FSR1 avec RCAS 0,75 stop, NIS à 0,5), **et** la source Qualcomm
   d'origine (8/255, 2,0, clamp ±23/255) ajoutée au labo comme cellule témoin, pour qu'on ne
   gagne pas contre un port affaibli.

### 7.2 Ce qu'on mesure et avec quoi

| Grandeur | Instrument | Où | Remarque |
|---|---|---|---|
| Temps GPU par image, par passe | `timestamp-query` (code de `UltraPlayer.js`) ; à défaut, temps mur de `onSubmittedWorkDone` amorti sur un lot de K soumissions identiques (K choisi pour que le coût fixe par soumission tombe sous 2 %) | labo, tous navigateurs | Chrome quantifie les horodatages à **100 µs** sauf avec `chrome://flags/#enable-webgpu-developer-features` : drapeau obligatoire sur le Chrome de labo, ou empilement de N passes identiques ; Firefox n'expose pas la requête ; l'amortissement est dit comme tel dans le rapport ; une mesure témoin par plateforme avec PIX (Windows) ou Perfetto (Android, si téléphone) |
| Cycles estimés | compilateur hors ligne Mali (`malioc`) sur le GLSL et le SPIR-V émis par Tint/naga ; équivalent Adreno si disponible (§6) | hors ligne | seule mesure Mali possible sans téléphone Mali ; estimation, pas mesure |
| Clic → photon | sonde clic → drapeau du dépôt (`mwLatency.run()`, Chrome dédié par `cdp.py`, plein écran) | DualRTX, N95, UM790Pro, `mw-arm` | protocole du chapitre 15 et de `docs/bench-campaign.md` §5 ; dix clics après un clic de chauffe, trois séries |
| Âge du contenu présenté | sonde d'âge du contenu du dépôt (`ContentAgeProbe`, `scripts/bench/content-age/`) | tout client qui affiche la page, téléphones compris | mesure dans la page, pas de drapeau hôte : c'est l'instrument des mobiles |
| Débit présenté | `[perf]` : images présentées / décodées, pertes par cause, file de décodage | tous | un enhancer qui tient 60 i/s mais en présente 40 est un échec |
| Puissance GPU | `powermetrics --samplers gpu_power` (macOS, Mac M1) ; HWiNFO en journalisation (puissance GPU et paquet : DualRTX, N95, UM790Pro) ; `nvidia-smi` pour la RTX | ordinateurs | 5 min par cellule, moyenne et p95 |
| Énergie sur mobile | décharge de batterie en pourcentage sur 30 min de clip fixe, luminosité fixe, trois cellules entrelacées A/B ; sur Android (si prêt) `dumpsys batterystats` et rails Perfetto quand la puce les expose ; occupation GPU Adreno par `/sys/class/kgsl/kgsl-3d0/gpubusy` | iPhone, iPad, téléphone Android de prêt | un point de pourcentage est le plancher de bruit : il faut des cellules longues et répétées |
| Qualité | relecture des pixels de sortie (`copyTextureToBuffer`) en PNG, puis Python hors ligne : PSNR, SSIM, **ꟻLIP** (NVIDIA, BSD-3, conçu pour comparer des rendus), SSIMULACRA2 sur images, VMAF sur courtes séquences à titre indicatif (entraîné pour la compression, pas pour l'upscale : à ne pas sur-interpréter) ; métriques sans référence : taux de dépassement (pixels hors du min/max 2×2 local de plus de 2/255 = sonnerie), scintillement image à image sur contenu fixe, et la métrique de défilement du banc hôte (décalage d'1 px) | labo | la vérité terrain est le rendu natif de la page, jamais un flux décodé |
| Conformité WGSL / GLSL | même entrée, deux textes, différence maximale par canal | labo | ≤ 2/255 en SDR |

### 7.3 La matrice

Appareils (et navigateurs) : DualRTX sur 780M, Arc A380 et RTX 5060 Ti (Chrome ; Firefox si son
WebGPU y est actif) ; N95 (Chrome) ; UM790Pro (Chrome, Windows et Ubuntu) ; Mac M1 (Chrome et
Safari) ; `mw-arm` (Chrome Windows ARM) ; AMD 610M ; iPhone et iPad (Safari ; Chrome iOS est
WebKit aussi) ; téléphone Android de prêt si obtenu.

Cellules : {mwsr performance, mwsr qualité, SGSR1 dépôt, SGSR1 source, NIS, FSR1, bilinéaire
(`off`), Smooth2D} × {WebGPU, WebGL2 quand la saveur existe} × échelles {×1 (contournement),
×1,33, ×1,5, ×2, ×3 ; ×0,75, ×0,5} × contenus {texte, interface, jeu, mires} × {brut, compressé}
× {SDR, HDR `linear` (AV1, DualRTX et Mac), 4:4:4 (hôte NVENC)}. La matrice complète ne se joue
que sur DualRTX et un mobile ; les autres machines jouent l'étoile (chaque facteur une fois autour
du cas nominal ×1,5 texte compressé SDR), comme la campagne de banc le fait déjà.

### 7.4 Ce que le rapport doit dire

Par appareil : temps GPU chaîne et noyau (médiane, p99, dispersion), part des quads sur le chemin
contour (compteur dans le labo), qualité (FLIP moyen et 99e centile, SSIM, dépassements), clic →
photon ou âge du contenu, puissance ou décharge ; un tableau « démontré / attendu » mis à jour
par rapport à ce plan ; et, pour chaque objectif du §0, « atteint / non atteint / non mesurable
ici » avec la raison.

---

## 8. Critères d'acceptation, non-régression, risques, dépendances

### 8.1 Critères chiffrés (hypothèses à vérifier, pas des résultats)

Les seuils sont ceux qui, atteints, justifient de changer un défaut ; en dessous, `mwsr` reste un
cran de l'échelle et une option du menu debug, ce qui est déjà utile.

| # | Critère | Seuil | Où | Objectif du §0 |
|---|---|---|---|---|
| C1 | Temps GPU mobile contre SGSR1 (même saveur, chaîne telle que livrée) | `mwsr` ≤ 0,8 × SGSR1 sur jeu et interface ; ≤ 1,0 × sur texte dense ; noyau seul ≤ 0,9 × | `mw-arm` (Adreno 618), iPhone, iPad | plus rapide que SGSR1 |
| C2 | Temps GPU ordinateur contre FSR1 | chaîne WebGPU ≤ 0,6 × FSR1 ; chaîne WebGL2 ≤ 0,8 × FSR1 | 780M, Arc, RTX, N95, M1, 610M | plus rapide que FSR1 |
| C3 | Qualité contre FSR1 à ×1,5 et ×2 | FLIP moyen ≤ FSR1 + 0,01 et SSIM ≥ FSR1 − 0,005 sur texte, interface, jeu, brut et compressé ; dépassements ≤ FSR1 | labo | qualité comparable |
| C4 | Qualité contre SGSR1 | FLIP ≤ SGSR1 sur tout contenu (au moins égal) | labo | — |
| C5 | Latence | clic → photon plein écran DualRTX : médiane ≤ Auto actuel + 1 ms, p90 ≤ + 2 ms ; images présentées / décodées ≥ 98 % partout | DualRTX, N95, UM790Pro, `mw-arm` | latence |
| C6 | Énergie | Mac M1 : puissance GPU moyenne `mwsr` ≤ SGSR1 et ≤ FSR1, écart > 2 × dispersion ; iPhone : décharge sur 30 min ≤ SGSR1 (pas pire, au point près) ; `mw-arm` : occupation GPU inférieure | M1, iPhone, `mw-arm` | plus économe |
| C7 | Contournement 1:1 | temps GPU du renderer = celui de `off` ± 5 % ; aucune image perdue au passage 1:1 ↔ échelle ; overlay « Off (1:1) » | tous | passage direct |
| C8 | HDR `linear` | temps ajouté par le shader ≤ FSR1 `linear` ; maximum conservé ; aucun pixel au-delà du maximum local d'entrée ; ΔE borné (seuil fixé en P0 sur FSR1 lui-même) ; regard de Bruno sur le M27Q en HDR | DualRTX, Mac | HDR |
| C9 | 4:4:4 | FLIP sur les canaux de chroma de la mire de texte coloré : meilleur que SGSR1, ≥ FSR1 | DualRTX (NVENC) | 4:4:4 |
| C10 | Réduction ×0,75 et ×0,5 | énergie d'aliasing (zone-plate, traits fins, métrique de défilement) ≤ Smooth2D ; PSNR à 1 dB du Lanczos-2 de l'hôte ; coût ≤ bilinéaire + 30 % | labo, téléphone 1080p | downscale |
| C11 | Conformité des jumeaux | écart maximal WGSL / GLSL ≤ 2/255 par canal *[P4 : reformulé — ≤ 2/255 sur 99,9 % des pixels et ≤ 4 au plus là où aucune dérivée n'intervient (réduction : 1/255 mesuré) ; noyau : même qualité face à la vérité terrain (FLIP à 0,002, PSNR à 0,1 dB ; mesuré 0,000 et 0,025) ; la chaîne WebGPU identique au noyau (0/255). Rapport P4 §3]* | labo | — |
| C12 | Robustesse | aucun NaN ni noir sur image plate (divisions protégées), reprise après `device.lost`, Safari et Firefox sans erreur de compilation, empreintes dorées stables sur WebGPU logiciel | labo, CI | — |

Ce qui est **non mesurable dans ce parc** et sera dit tel quel dans le rapport : Adreno 7xx et
8xx, Mali de téléphone, Chrome Android (sauf prêt d'un téléphone). Le verdict « plus rapide que
SGSR1 sur mobile » sera donc « démontré sur Adreno 618 et sur Apple, extrapolé ailleurs », ou
rien.

### 8.2 Non-régression

- **Tests existants** : la suite `vitest` (748 tests au dernier décompte) passe ; `createRenderer.test.js` et
  `EnhancerGovernor.test.js` sont étendus (routage de `mwsr` et `gl-mwsr`, place dans l'échelle,
  mémoire du cran, mappage HDR `gl-mwsr → mwsr`, `NO_WEBGPU_ALGOS`).
- **Logique pure testée sans GPU** : le choix des `override` et du `TIER` par appareil, le calcul
  des uniformes (échelle, phase, régime), la détection 1:1, extraits dans des fonctions pures.
- **Validation statique des shaders en CI** : un script Node extrait les chaînes WGSL et GLSL des
  modules et les passe à un validateur (naga ou Tint en ligne de commande, ou, plus simple à
  installer, un Chrome sans tête avec WebGPU logiciel) ; aujourd'hui une faute de WGSL n'est vue
  qu'au premier flux.
- **Tests dorés** : un Chrome sans tête (Playwright) avec l'adaptateur WebGPU logiciel
  (SwiftShader, drapeaux `--enable-unsafe-swiftshader` et adaptateur logiciel) rend chaque variante
  sur trois images fixes à deux échelles et compare une empreinte des pixels à une référence
  versionnée ; la même page vérifie la conformité WGSL / GLSL. Job CI séparé et non bloquant au
  début (temps et flakiness à observer), bloquant quand il est stable.
- **Banc d'acceptation** : la campagne `/bench` existante tourne avec `mwsr` dans sa matrice
  (enhancer coché) sur les six machines ; aucun rouge nouveau.
- **Les trois ports existants ne bougent pas** : SGSR1, NIS, FSR1 restent tels quels (références de
  banc, crans de l'échelle, options debug) ; leurs tests dorés les protègent aussi.

### 8.3 Risques

| Risque | Effet | Parade, et la phase qui le lève |
|---|---|---|
| L'échantillonnage direct de la texture externe coûte plus que prévu (conversion YUV à chaque tap, deux plans lus par échantillon) et mange le gain de la passe 0 | la décision d'architecture n° 2 tombe | P0 mesure « N bilinéaires sur externe » contre « blit + N bilinéaires sur RGBA » avant d'écrire une ligne de noyau ; repli : passe 0 gardée, gain limité au noyau |
| Le vote par dérivées rate des contours fins (luma bilinéaire trop lisse) ou se déclenche sur du bruit de compression | qualité sur le texte, ou chemin contour partout | P2 : seuil `τ` et cohérence `a` réglés sur le jeu compressé ; variante témoin avec le vote à 3 taps ; critère C3 |
| La borne faible (min/max des 5 taps) laisse sonner le texte à ×2 | halos | `DERING_EXACT` (4 chargements sur les contours) ; critère C3 sur les dépassements |
| WebGPU reste plus lent que WebGL2 en SDR sur le thread principal (présentation) quel que soit le shader | le gain WGSL n'atteint pas la production SDR | le jumeau GLSL est dans le périmètre dès P4 ; porte P4 remesure la question ouverte du wiki 15.3 |
| f16 : absent sur le D3D12 de certaines machines, probablement absent de l'Adreno 618 du banc dans Chrome (~29 % des Adreno 6xx l'exposent), non exposé par Firefox, piégeux sur Mali (précision des coordonnées), nul sur NVIDIA récent et **néfaste sur Pascal** | variante inutile, fausse ou plus lente | f16 est un `override` décidé par mesure et par `adapter.info`, jamais imposé ; coordonnées toujours en f32 ; le jumeau WebGL2 a son `mediump` sur le même Adreno |
| Le coût doublé des échantillons sur la texture externe (deux plans par échantillon) rend la variante « passe 0 gardée » plus rapide sur le texte dense | la décision n° 2 devient conditionnelle | les deux feeds vivent dans la même source ; P0 mesure, et le choix peut dépendre de la classe de GPU, voire rester les deux dans le menu debug |
| Sous-groupes ou `textureLoad` externe indisponibles sur Safari | couches WebGPU réduites sur iOS | ce sont des couches optionnelles ; la base tourne sans |
| Aucun Adreno récent ni Mali au banc | objectif mobile démontré partiellement | dit dans le rapport ; `malioc` pour une estimation Mali ; prêt d'un téléphone demandé tôt |
| La qualité « comparable à FSR1 » n'est pas atteinte sur le texte | `mwsr` ne remplace pas FSR1 sur ordinateur | critère d'arrêt explicite en P2 : après trois itérations du labo, si C3 n'est pas tenu à ×2, l'objectif ordinateur devient « plus rapide à qualité SGSR1 » et FSR1 reste le cran « qualité » |
| Le gouverneur se trompe avec un cran moins cher que `sgsr` (ordre de l'échelle, mémoire par appareil) | descente ou remontée absurdes | tests du gouverneur ; le cran `mwsr` entre dans l'échelle avec une migration de la mémoire `mw_enhancer_rung` |
| Dérive de calendrier : la v0.4.0 glisse, ou un chantier prioritaire (PyroWave) reprend les bancs | P0 et P5 attendent les machines | le plan n'occupe aucune machine avant P0 ; P2 et P3 sont du labo sur DualRTX seul |
| Brevets | aucun grant explicite dans MIT ni BSD-3 ; les techniques (interpolation dirigée, netteté adaptative) sont de l'art ancien et public | code original, constantes propres, pas de nom de marque dans l'interface ; risque identique pour les trois ports déjà livrés |

### 8.4 Dépendances

- La release v0.4.0 publiée (le plan n'en touche aucun fichier) et `main` propre.
- Les machines du §1.8 à leurs créneaux : DualRTX pour P0-P3, le Mac de Léo et l'iPhone pour P0 et
  P5, `mw-arm` pour P0 et P5, N95 et UM790Pro pour P5 ; un téléphone Android de prêt (souhaité).
- Les clips et pages de banc hors dépôt (cache déjà prévu par la campagne).
- Les versions de navigateur du parc relevées en P0 (Chrome, Safari, Firefox), avec les
  fonctionnalités WebGPU exposées par chacun (`adapter.features`), consignées dans le rapport.
- Python avec les paquets de métriques (FLIP, scikit-image, numpy) sur DualRTX ; `malioc` (Arm
  Mobile Studio, gratuit) si on veut l'estimation Mali.
- Rien côté hôte : aucun changement du natif, aucun protocole nouveau.

---

## 9. Les phases, leurs portes, le modèle et l'effort

Convention du projet (règle du 25/09) : Opus 5.5 pour chaque tâche, seul l'effort varie. La
colonne « modèle » suit cette règle ; là où une autre option serait plus efficace, elle est dite
entre parenthèses et c'est Bruno qui tranche au « go » de la phase. Les durées sont des ordres de
grandeur en sessions de travail, bancs compris ; une porte ferme chaque phase et ouvre la suivante
par un « go » de Bruno.

| Phase | Contenu | Livrables | Porte (ce qu'il faut pour passer) | Modèle · effort | Taille |
|---|---|---|---|---|---|
| **P0 — Labo et lignes de base** | page de laboratoire (`scripts/bench/scaler-lab/` ou dossier debug du frontend : N variantes × images × échelles, horodatages GPU repris d'`UltraPlayer.js`, amortissement de secours, relecture PNG, compteur de quads contour) ; chaîne de métriques Python ; relevé des fonctionnalités WebGPU de chaque navigateur du parc ; lignes de base SGSR1 (dépôt et source), NIS, FSR1, bilinéaire, chaîne et noyau seul, sur DualRTX (3 GPU), `mw-arm`, Mac M1, iPhone ; **l'essai décisif** : N échantillons bilinéaires sur texture externe contre blit + N sur RGBA | page, scripts, rapport de base (tableaux du §7.4 pour les techniques existantes), matrice des fonctionnalités | dispersion répétition à répétition < 5 % sur le temps GPU ; coût de l'échantillonnage externe connu ; décision passe 0 oui/non | Opus 5.5 · `high` (banc et harnais) | 3-4 sessions |
| **P1 — Contournement 1:1** | détection par image dans `WebGpuRenderer` et `WebGlRenderer` ; présentation par le blit `off` existant (l'option `copyExternalImageToTexture` est écartée par lecture du code de Chrome, §5.3) ; gouverneur mis en pause pendant le contournement ; overlay « Off (1:1) » ; tests | code, mesure blit WebGPU / blit WebGL2 / Canvas2D au clic → photon plein écran | C7 tenu ; écart à Canvas2D connu ; décision « blit » ou « deux canvas » prise par Bruno | Opus 5.5 · `medium` (code cadré) puis `high` (la mesure) | 1-2 sessions |
| **P2 — Le noyau** | P2.1 écriture du noyau de base WGSL (feed `texture_2d` d'abord, dans le labo), polynôme, seuil, luma, borne, netteté fusionnée ; P2.2 boucle d'optimisation (§6) : variantes, mesures, `malioc`, journal des essais ; P2.3 couches `HALF`, `QUAD_UNIFORM`, `DERING_EXACT`, feed externe ; P2.4 expérience compute par tuiles (optionnelle, porte propre) | shader, journal, tableau démontré / attendu | C1 et C2 provisoires tenus sur 780M, Arc, `mw-arm`, M1 ; C3 et C4 tenus au labo ; sinon critère d'arrêt du §8.3 appliqué et objectif ordinateur rebaisé | Opus 5.5 · `max` pour P2.1 (cœur de chantier, algorithme délicat ; Fable 5.1 `max` serait l'alternative la plus efficace sur ce seul chapitre si Bruno l'accepte) ; `xhigh` pour P2.2 (mesures à interpréter) ; `high` pour P2.3 et P2.4 | 4-6 sessions |
| **P3 — HDR, 4:4:4, réduction** | `HDR_LINEAR` (compression réversible), `CHROMA444`, mode réduction (4 / 9 / 16 taps, `LINEAR_LIGHT` mesuré), mires, tests dorés | shader complet, résultats C8-C10 au labo | C8, C9, C10 tenus au labo ; regard de Bruno en HDR | Opus 5.5 · `high` | 2-3 sessions |
| **P4 — Intégration et jumeau WebGL2** | clé `mwsr` dans `WebGpuRenderer` (pipelines par `override`, deux modes, bind groups en cache), `gl-mwsr` dans `WebGlRenderer`, test de conformité, `createRenderer` / `NO_WEBGPU_ALGOS` / `ENHANCER_LADDER` / `pickAutoEnhancer` derrière un drapeau (pas encore le défaut), menu debug, i18n (en, fr, zh), overlay, worker, validation statique en CI, tests `vitest`, docs de code ; **porte WebGPU contre WebGL2** remesurée avec le nouveau shader par classe d'appareil | code intégré, CI verte, rapport WebGPU / WebGL2 | C11 et C12 tenus ; aucune régression `vitest` ni `/bench` ; décision par classe d'appareil : quelle saveur en SDR | Opus 5.5 · `medium` pour l'intégration (travail spécifié par ce plan), `high` pour la porte ; les chaînes i18n, la doc et le squelette des tests sont du `low` (Sonnet 5.5 ferait l'affaire si Bruno accepte une exception) | 3-4 sessions |
| **P5 — Campagne de validation** | matrice du §7.3 sur le parc ; énergie (M1, iPhone, `mw-arm`) ; clic → photon ; `/bench` avec `mwsr` ; A/B subjectif de Bruno (texte, bureau, jeu, LAN et Internet) | rapport de campagne, verdict objectif par objectif | C1-C10 jugés ; liste des non mesurables | Opus 5.5 · `xhigh` (bancs partagés, mesures à interpréter) ; Bruno aux manettes pour le Mac, l'iPhone et le ressenti | 3-4 sessions, étalées sur les créneaux |
| **P6 — Défauts, documentation, release** | bascule de `auto` par classe d'appareil selon P4 et P5 (ou non), migration de la mémoire du gouverneur, `README.next.md`, chapitre du wiki (addendum au 15 ou chapitre 16), notes de release (format « deux thèmes »), mémoires de session, paragraphe « Concrètement, pour l'utilisateur » | commits, docs, notes | relecture de Bruno ; `/bench` vert ; push | Opus 5.5 · `medium` | 1-2 sessions |

Total indicatif : 17-25 sessions, dont une bonne part de banc. Les phases P2 et P3 n'occupent
que DualRTX (labo local) ; P0, P4 (porte) et P5 ont besoin des créneaux Mac, iPhone et `mw-arm`.

**Ordre et parallélisme** : P1 est indépendant de P2-P3 et peut se livrer tôt (il a sa valeur
seul). P0 précède tout (sans lignes de base, pas de verdict). P2.4 (compute) ne bloque rien.
Chaque porte se termine par un `/compact` et un rapport à la session d'orchestration avec
l'effort conseillé de la phase suivante, comme pour les autres plans.

**Décisions de Bruno du 22/10/2026**, après la porte P4 et le clic-photon des téléphones (rapport P4
§5, journal J28) ; relayées par 59 vers 13:05, le présentateur des Mali choisi ensuite dans la session.
Elles fixent ce que P6 appliquera sur les résultats de P5 ; rien ne devient défaut avant.
- **L'`auto` par classe d'appareil** : PC et Mac → WebGL2 (`gl-mwsr`) ; iPhone (iOS 26) → WebGPU
  (`mwsr`) quand WebGPU est là, `gl-mwsr` sinon ; Snapdragon et Adreno → `gl-mwsr` à la place de
  SGSR1 ; Mali d'entrée de gamme (le realme Note 60, Mali-G57 MP1) → aucun upscaler automatique.
- **Le présentateur simple de ces Mali** : WebGPU dès qu'il existe, amélioration activée ou non
  (Canvas2D puis WebGL2 sinon) ; une exception, sur cette seule classe, au Canvas2D par défaut du
  03/09. Au photon sur le realme : égal à 960×540, 53-55 i/s au lieu de 30 en 1080p réduit — ce que
  la résolution Auto envoie à un téléphone, l'écran de l'hôte jusqu'à 1440 lignes —, même réactivité
  à une image près. La batterie se mesure en P5.
- **Le troisième repli de l'import WebGPU**, avant la v0.4.1 comme le deuxième (rapport P4 §5) :
  après `importExternalTexture` et l'`ImageBitmap`, `VideoFrame.copyTo` vers RGBA puis
  `writeTexture` ; si tout échoue N fois de suite, le renderer bascule sur `gl-<algo>`, puis
  Canvas2D, au lieu de rester noir.
- **Le job CI « Upscaler lab »** sort en 0 quand ses seuls problèmes sont « non applicable »
  (annotations `::warning::`), en 1 seulement pour une vraie différence ou un dump manquant ; ses
  déclencheurs ne changent pas.
- **Reste ouvert pour P6** : la frontière de la classe « Mali d'entrée de gamme », et les GPU
  mobiles qu'aucun banc n'a vus (Mali haut de gamme, Xclipse, PowerVR), en SGSR1 dans l'Auto
  aujourd'hui.

---

**Décisions de Bruno du 24/10/2026** (après le comparateur sur le parc, J36-J38) :

- **Le comparateur (`compare.html`) est à garder**, jamais à supprimer : il servira encore.
- **Zone du regard : coupée par défaut**, l'option conservée (`mw_mwsr_gaze`) pour l'activer plus tard s'il le
  souhaite.
- **Ordinateur : CR5** (chemin plat Catmull-Rom à cinq échantillons, `FLAT_TAPS 5`, `FLAT_C 0,5`), à condition
  que le 780M le montre compétitif face à Q3 : il l'est (J36-J37 : UM790Pro Q3 5,06 ms contre CR5 4,15 ; DualRTX
  780M 6,13 contre 5,85 ; RTX 0,98 contre 0,96 ; Mac 3,28 contre 3,70, dans le bruit des passes).
- **Téléphone et tablette : Q3** (quadratique à trois échantillons, `FLAT_TAPS 3`), s'il reste le plus rapide sur
  iPhone et Android : il l'est (iPhone 4,41 contre 4,82 ms, −9 % ; realme Note 60 51,6 contre 59,2, −13 %), sauf
  comportement différent d'Android ; les deux TV (Mi TV Mali-G31, Freebox POP) peuvent servir à mesurer Android
  (optionnel : « tu peux aussi »).
- **Depuis le 02/11/2026 (J56, décision de Bruno) : le chemin plat à une lecture (`FLAT_TAPS 2`) partout**, téléphones et tablettes
  compris. CR5 et Q3 restent au labo jusqu'à la lecture sur téléphone et 780M.
- **Netteté : 1,85 pour tout** (message de Bruno de 11:50, fait à midi, journal J38) ; il veut garder la
  possibilité d'une netteté dynamique selon le facteur d'agrandissement et de réduction : étude proposée au §12.
- **Borne : les texels seuls**, confirmée (« le résultat est bien meilleur » ; le défaut depuis J37).
- Le ×2 : étude faite le 24/10 après-midi (journal J39). Le levier est la netteté elle-même, pas le noyau : à ×2
  le poids en travers vaut 1,85 (force 1) là où son œil l'a réglé à ×1,33 avec 1,85 × 2/3 = 1,23, et ce surcroît
  épingle les pixels aux texels (le « baveux » : des paquets, pas du flou). **Proposition : la loi d'échelle
  `mwsrSharpness`** (1,85 jusqu'à ×1,33, 1,23 dès ×1,5 : le même mélange à toutes les échelles), écrite et testée ;
  **câblée au oui de Bruno du 24/10 (journal J40)** ; à son œil au comparateur (720p → 1440p, bouton « netteté de la loi d'échelle » contre 1,85 et
  FSR1) ; les deux autres voies de J37 restent (FSR1 au-delà de ×1,6 par le gouverneur, accepter).

Le défaut du produit était encore `FLAT_TAPS 1` (l'échantillon bilinéaire lui-même) jusqu'au 24/10 à midi : le
chemin plat par classe d'appareil est fait le jour même (P6, §12 ; journal J38 : dorés et conformité refaits sur
SwiftShader, RTX et 780M vérifiés au comparateur), `mwsrClassOverrides` posé par les deux renderers.
## 10. Sources, licences et règles de réutilisation

### 10.1 Licences des composants étudiés et ce qu'elles imposent

| Composant | Licence | Ce que le projet peut en faire | Obligations si du code en est repris |
|---|---|---|---|
| FSR1 (`ffx_fsr1.h`, `ffx_a.h`), AMD | MIT, © 2021 AMD (+ © 2014 Michal Drobot pour les approximations) | lire, porter, dériver ; déjà porté dans le dépôt | garder les deux avis dans toute copie « substantielle » ; AMD demande de ne pas exposer « RCAS » dans l'interface et de nommer la technique « AMD FSR 1.0 » |
| Port WGSL firdawolf de FSR1 | MIT | déjà dans le dépôt | avis conservé (il l'est) |
| Gist d'Atyuwen (`opt_fsr_easu.fxh`, `opt_fsr.fxh`) | « Distributed under the MIT License. Copyright (c) 2021 atyuwen. » (texte complet absent ; dépend de l'en-tête AMD) | lire ; ne pas recopier (le plan n'en a pas besoin) | si un extrait entrait : texte MIT complet + ce copyright + l'avis AMD |
| Billet d'Atyuwen | CC BY-NC 4.0 (prose et figures) | citer, pas reproduire | attribution ; pas d'usage commercial du texte |
| SGSR1, Qualcomm | BSD-3-Clause, © 2023/2025 Qualcomm Innovation Center | déjà porté ; compatible GPL-3.0-or-later (GPLv3 §7 b et d couvrent ses clauses) | avis + conditions + disclaimer dans le fichier dérivé et dans les notices binaires ; **ne pas** utiliser « Qualcomm » ni « Snapdragon™ » pour promouvoir le produit ; « SGSR1 » dans un menu est descriptif et acceptable |
| NIS, NVIDIA | MIT, © 2022 NVIDIA | déjà porté | avis conservé |
| CAS, AMD | MIT | idées (netteté bornée par le contraste local) | — |
| RAVU, nnedi3, SSimSuperRes, KrigBilateral, SSimDownscaler (mpv) | LGPL-3 (nnedi3 amont GPL) | lire ; ne pas intégrer | — |
| Anime4K | MIT | lire | — |
| adaptive-sharpen | BSD | lire | — |
| ꟻLIP (NVIDIA) | BSD-3 | outil de mesure hors ligne | — |
| SSIMULACRA 2 | BSD-3 + fichier PATENTS | outil de mesure hors ligne | — |
| VMAF | BSD-2 + concession de brevets | outil de mesure hors ligne | — |
| SweetOpt | GPL-3 (l'outil) | outil hors ligne ; sa sortie n'est pas couverte | — |
| `malioc`, Radeon GPU Analyzer, compilateur Adreno | gratuits (Arm Mobile Studio ; RGA MIT ; Qualcomm) | estimateurs hors ligne | — |

Aucune de ces licences ne contient de concession de brevet expresse (sauf VMAF et le fichier
PATENTS de SSIMULACRA 2) ; MIT et BSD-3 ne le font pas plus l'une que l'autre. Le risque est
identique pour les trois ports déjà livrés et pour un shader original ; les techniques en jeu
(interpolation dirigée par le gradient, fenêtre polynomiale, netteté bornée par le contraste
local) sont publiées depuis vingt ans.

### 10.2 La règle de salle blanche pour `mwsr`

- **Lu** : tout ce qui précède, ainsi que la documentation des éditeurs.
- **Repris comme idée, en le citant** (en tête du fichier et dans la doc) : taps bilinéaires au
  lieu de gathers (Atyuwen, Jimenez, MJP), sortie anticipée sur un vote de contour (SGSR1, NEDI),
  borne min/max locale contre la sonnerie (EASU, SGSR1), netteté résolue par la marge locale (CAS,
  RCAS), compression de dynamique réversible autour du noyau (SRTM d'AMD), vote par dérivées
  d'écran (le nôtre), fenêtre polynomiale en `d²` sans transcendante (principe commun à EASU et
  SGSR1, polynôme différent).
- **Jamais recopié** : une ligne de code, un polynôme, une constante (ni `25/16`, ni `2/5`, ni
  `(x−1)(x−4)³`, ni `0,55`, ni `2,181818`, ni `10,14185`, ni `0,1875`), une structure de fonction.
  La table de constantes du §3.6 est produite par le labo (P2), et le journal en garde l'origine.
- **Dans l'interface** : « Netteté MoonlightWeb », ou le nom que Bruno choisira ; jamais une marque
  tierce accolée au nôtre.
- **Le fichier** porte l'en-tête GPL-3.0-or-later du projet et le copyright de Bruno, comme le reste
  du dépôt ; les trois ports gardent les leurs.

### 10.3 Sources consultées (sélection ; les rapports de recherche complets sont dans le scratchpad de la session, à archiver avec le plan)

FSR1 : `github.com/GPUOpen-Effects/FidelityFX-FSR` (readme, license, `ffx-fsr/ffx_fsr1.h`,
`ffx_a.h`, `docs/FidelityFX-FSR-Overview-Integration.pdf`), `gpuopen.com/fidelityfx-superresolution/`,
Unity/AMD SIGGRAPH 2021 (`advances.realtimerendering.com/s2021/Unity AMD FSR - SIGGRAPH 2021.pdf`),
ports : Godot `fsr_upscale.glsl`, Unity `FSRCommon.hlsl`, mpv `agyild/FSR.glsl`, RetroArch
`edge-smoothing/fsr`, three.js `FSR1Node.js`, Looking Glass, Unreal « Mobile FSR ».
Atyuwen : `atyuwen.github.io/posts/optimizing-fsr/`, gist `78d6e810e6d0f7fd4aa6207d416f2eeb`
(révisions et commentaires), fork `atyuwen/FidelityFX-FSR`.
SGSR : `github.com/SnapdragonStudios/snapdragon-gsr` (README, LICENSE, `sgsr/v1/README.md`, les
quatre shaders v1, `sgsr/v2/README.md`, issues 1-8), plugins Unreal et Unity de Qualcomm, billets
Qualcomm 2023 et 2024, framework Vulkan Adreno.
NIS et CAS : `github.com/NVIDIAGameWorks/NVIDIAImageScaling` (`NIS_Scaler.h`, `NIS_Config.h`,
guide PDF), `github.com/GPUOpen-Effects/FidelityFX-CAS` (`ffx_cas.h`), `gpuopen.com/fidelityfx-cas/`.
Noyaux et réduction : GPU Gems 2 ch. 20, gist MJP Catmull-Rom, mpv `DOCS/man/options.rst` et
version 0.37, `libplacebo.org/options`, Robidoux (`usage.imagemagick.org/filter/nicolas/`), articles
NEDI (Li et Orchard 2001), iNEDI (Asuni et Giachetti 2008), DCCI (G'MIC), `bjin/mpv-prescalers`,
gists igv, `bloc97/Anime4K`, `Anime4K-WebGPU`, arXiv 1609.05158, 1608.00367, 2108.10335,
1606.01299, 2105.01237, SR+Codec (BMVC 2024, arXiv 2305.04844), ONNX Runtime Web (WebGPU EP).
Métriques : `github.com/Netflix/vmaf` (FAQ, README), `github.com/NVlabs/flip`,
`github.com/cloudinary/ssimulacra2`, `richzhang/PerceptualSimilarity`, `dingkeyan93/DISTS`.
Outillage : `developer.chrome.com/blog/new-in-webgpu-121` (horodatages), `web3dsurvey.com`,
`toji.dev/webgpu-profiling/` (PIX, Xcode), `GameTechDev/PresentMon`, page de manuel `powermetrics`,
`learn.arm.com` et `developer.arm.com` (malioc), `perfetto.dev` (batterie, GPU),
`developer.android.com/agi` et l'étude Kuro Games, `GPUOpen-Tools/radeon_gpu_analyzer`.
Plateformes WebGPU (sources lues sur `main` le 10/10/2026) : spécifications `gpuweb/spec/index.bs`,
`wgsl/index.bs`, `proposals/compatibility-mode.md` ; Dawn `core.def`, `wgsl.def`,
`multiplanar_external_texture.cc`, `ExternalTexture.cpp`, `CopyTextureForBrowserHelper.cpp`,
`PhysicalDevice{Vk,D3D12,MTL}` ; Chromium `external_texture_helper.cc`, `gpu_queue.cc`,
`gpu_canvas_context.cc`, `webgpu_swap_buffer_provider.cc`, `gpu_adapter.cc`,
`webgpu_decoder_impl.cc`, `webgpu_blocklist_impl.cc`, `paint_canvas_video_renderer.cc`,
`color_space.cc`, `webcodecs/video_frame.cc`, `supported_profile_helpers.cc`,
`dc_layer_overlay.cc`, `ca_layer_overlay.cc` ; WebKit `HardwareCapabilities.mm`,
`ExternalTexture.mm`, `BindGroup.mm`, `GPUCanvasContextCocoa.mm`, `WGSL.cpp`,
`TypeDeclarations.rb` ; Firefox `dom/webgpu/*.cpp`, `StaticPrefList.yaml` ; `mdn/browser-compat-data` ;
billets `developer.chrome.com/blog/new-in-webgpu-{116,121,129,133,134,137,138,142,144,146,149-150,155-156}` ;
`chromestatus.com` ; `webkit.org/blog/17333/` ; bugs WebKit 272702 et 254668 ; gpuweb issues 4384,
4919, 5236 ; Khronos WebGL PR 3668 ; `w3c/window-management` PR 150 ; `ColorWeb-CG` HDR canvas.
Optimisation : `ceejay.dk/sopt/` et `github.com/CeeJayDK/sopt` (versions 0.6.0, 0.7.0, TexBench),
`StanfordPL/stoke`, `google/souper`, `mangpo/greenthumb`, Mirage, `rudybear/shaderopt`
(`lab/reports/FINAL.md`), `KhronosGroup/SPIRV-Tools`, `laurentlb/shader-minifier`, Mesa NIR ;
KernelBench (arXiv 2502.10517), NVIDIA (DeepSeek-R1, 02/2025), AlphaEvolve (05/2025), Sakana
(TechCrunch, arXiv 2509.14279), Kevin-32B (Cognition), METR (06/2025), Sitthi-Amorn 2011, Wang
2014, He 2016 ; guide Arm 101897 (2025), note Arm ECM-0442477, Imagination « Demystifying
Precision », WWDC16 « Advanced Metal Shader Optimization », GPUOpen RDNA Performance Guide, livres
blancs NVIDIA Pascal/Ampère/Ada, Rákos 2010, Bjørge SIGGRAPH 2015, Qualcomm « Adreno GPU on Mobile:
Best Practices », Vulkanised 2025 (Arm).

### 10.4 Ce que les recherches n'ont pas pu établir (reporté au banc)

Toute mesure de FSR1 sur Adreno ou Mali ; toute mesure indépendante de SGSR1 ; toute mesure
d'énergie d'un upscaler mobile ; les coûts par image de n'importe quel upscaler spatial sur
Radeon 780M, Arc 140V, Apple M1-M4, Mali-G7xx, Adreno 8xx ; le coût de `textureGather` sur Adreno
et Mali ; le producteur NV12 « lisible par WebGPU » de chaque OS (le prédicat de Chrome est lu, pas
les décodeurs) ; l'état réel de `toneMapping extended` et de `subgroups` dans un Safari 26 livré ;
la couverture pilote par pilote de `shader-f16` et `timestamp-query` sur Android ; le format 4:4:4
livré par VideoToolbox et MediaCodec à WebCodecs ; le comportement de Chrome quand le GPU du
décodeur n'est pas celui de l'adaptateur WebGPU.

---

## 11. Concrètement, pour l'utilisateur

Sur un **téléphone ou une tablette**, l'enhancer coché, l'image d'un flux agrandi (un hôte 1080p
sur un écran plus fin, ou le zoom à deux doigts) garde ses contours nets comme aujourd'hui avec
SGSR1, mais le GPU du téléphone travaille moins par image : moins de chauffe sur une longue
partie, un peu plus d'autonomie, et surtout moins de risque que l'échelle de qualité éteigne
l'enhancer parce qu'il ne tenait plus dans une image. Quand le téléphone **réduit** l'image (un
hôte 1440p sur un écran 1080p, le cas le plus courant en Auto), les lignes fines et le texte des
menus cessent de scintiller ou de moirer : c'est nouveau, aucun des enhancers actuels ne filtrait
la réduction. Au zoom à deux doigts, le passage d'un régime à l'autre ne se voit pas.

Sur un **ordinateur** en fenêtre ou avec un flux plus petit que l'écran, l'image agrandie est
aussi nette que FSR1 sur le jeu et l'interface, un peu moins peut-être sur du texte très dense (le
plan le mesure et le dira), pour nettement moins de travail GPU : sur un portable ou un iGPU qui
décode déjà en 1440p, c'est la différence entre un enhancer qui tient 60 images par seconde et un
qui en perd. En **plein écran à la résolution de l'hôte**, rien ne tourne plus du tout : l'image
est posée telle quelle, sans netteté ajoutée, et l'overlay le dit (« Off (1:1) ») ; si le réseau
force le flux à descendre d'un cran, l'agrandissement reprend à l'image suivante sans coupure.

En **HDR** (AV1, écran HDR), l'enhancer tourne en lumière de scène sans écrêter les reflets ni
griser les blancs, comme FSR1 aujourd'hui, pour moins cher ; le coût du décodage logiciel AV1,
lui, ne change pas, et le HDR en HEVC reste sans enhancer (c'est le navigateur qui le décide). En
**4:4:4** (bureau, code, tableurs), le texte coloré n'a plus de franges de couleur sur ses bords :
la chrominance est reconstruite avec la même finesse que la luminance.

Ce que l'utilisateur ne verra pas : la latence ne bouge pas de façon perceptible dans un sens ou
dans l'autre (le plan exige qu'elle ne régresse pas d'une milliseconde) ; sur un iPhone ou un Mac,
le gain d'énergie se mesure sur une heure de jeu, pas sur une partie. Ce que le chantier ne règle
pas : le surcoût de présentation de WebGPU en fenêtre sur le thread principal (le jumeau WebGL2 le
contourne, comme aujourd'hui), le décodage logiciel du HDR AV1, l'absence d'enhancer sur HDR HEVC
et sur les téléviseurs, et la netteté au-delà de ×2,5, qui reste douce comme avec tous les
upscalers spatiaux.

---

## 12. Reste à faire (liste de tâches) et prompt de lancement

### 12.1 Tâches, par phase (effort Opus 5.5 entre crochets)

**P0 — Labo et lignes de base** (joué la nuit du 19 au 20/10/2026 ; rapport `upscaler-mwsr-p0.md`)
- [x] Page de laboratoire : images dessinées par la page, boucle WebCodecs, cellules, horodatages par lots, relecture PNG, export JSON (`scripts/bench/scaler-lab/`, `4e249b3f`) ; sans compteur de quads ni amortissement (les horodatages ont marché partout)
- [~] Mires : texte coloré (`code`), zone-plate et traits fins (`zone`) faits ; rampe PQ en P3
- [x] Réduction hors ligne de la vérité terrain (Lanczos-3, lumière linéaire) par le pilote (`--quality`)
- [~] Chaîne Python : PSNR, SSIM, dépassements faits (`metrics.py`) ; FLIP attend une roue Python 3.14 ; SSIMULACRA 2, scintillement et défilement en P2 s'ils servent
- [~] Relevé `adapter.features` / `adapter.info` : DualRTX (RTX, 780M) fait ; téléphones et Mac aux créneaux de Bruno ; `mw-arm` sans WebGPU
- [x] Essai décisif externe contre blit + RGBA (N = 1, 5, 9, chargements, dérivées) : RTX et 780M
- [~] Lignes de base : DualRTX fait (l'Arc A380 n'est plus énumérée) ; iPhone 13 Pro fait (20/10) ; **Mac M1 Pro fait (21/10, rapport P0 §4 quater, avec le noyau `mwsr` : C1 provisoirement tenue sur Apple, passe 0 à garder sur ce GPU)** ; **realme Note 60 fait (21/10, rapport P0 §4 quinquies, journal J16 : C1 tenue sur Mali-G57 — noyau 0,74 × SGSR1 sur le texte, 0,38 × sur le jeu ; `half` −10 %, `dering 0` −27 % ; passe 0 + `mwsr` 6,75 ms à la taille de l'écran ; réduction par vues sRGB au coût du plancher)** ; `mw-arm` (WebGL2) à jouer ; Smooth2D (Canvas2D) non mesuré, hors WebGPU
- [x] Rapport P0 et porte : tenue pour l'ordinateur (dispersion ≤ 2,6 %, pas de passe 0 en SDR)

**P1 — Contournement 1:1** (code le 20/10/2026, commit `36edf6ec`)
- [x] Détection par image (`scaleRegime.js`, la règle de taille des deux renderers, partagée), copie directe (la passe 0 HDR existe dans ses deux cibles dès la création), gouverneur en pause (`passthrough` dans l'observation : les minuteries repartent de zéro), overlay « Off (1:1) », dans `WebGpuRenderer` et `WebGlRenderer`
- [x] Tests `vitest` : `scaleRegime.test.js` (ajustement, plafond, 1:1 exact contre presque-1:1), `EnhancerGovernor.test.js` (ni descente ni remontée sur une fenêtre de copies, horloge remise à la reprise)
- [x] Labo (`scaler_lab.py --p1`, le 20/10 sur SwiftShader puis RTX 5060 Ti, rapport `upscaler-mwsr-p1.md`) : chaîne du produit à 1:1 = 0,030-0,032 ms pour SGSR1, NIS et FSR1 comme pour `off` (avant P1 : 0,11 / 0,38 / 0,29), en lot comme à 60 i/s ; passage 1:1 ↔ ×2 à chaque image : 32 copies et 32 passages du noyau sur 64 draws, aucune image perdue, noyau à ×2 inchangé. **C7 tenue au labo** ; l'overlay reste à voir sur la `--dev`
- [x] Mesure plein écran DualRTX, clic → photon (21/10, rapport P1 §6) : à 1:1 2560×1440 à 60 Hz, décodage sur l'AMD, médianes Canvas2D 26,9 ms, copie 1:1 WebGPU 27,6 ms, copie 1:1 WebGL2 30,6 ms, égales à la dispersion série à série près (± 3 ms) ; l'overlay « Off (1:1) » vu sur la `--dev`
- [ ] Décision de Bruno : blit, ou deux canvas (hors v0.4.1 sinon) — recommandation : **blit** (aucun écart mesurable)

**P2 — Le noyau**
- [x] P2.1 (21/10/2026, Fable 5.1 `max`, commits `fa42a66c` et `8e9a5903`, rapport `upscaler-mwsr-p2.md` §1, journal J1-J4) : noyau de base WGSL sur `texture_2d` dans le labo — un échantillon et le vote par dérivées ; sur les contours deux échantillons le long à un demi-texel et deux en travers, cohérence marche/crête, borne des cinq puis des quatre texels (`DERING 1`) ; première table `W_ALONG 0,15`, `R_ALONG 0,5`, `W_ACROSS 0,35`, `W_MIN −0,3`, `τ 0,05` ; qualité : FSR1 à 0,15-0,45 dB près à ×2, au-dessus à ×1,33, 0 % de dépassement partout
- [x] P2.2 (21/10/2026, rapport `upscaler-mwsr-p2.md` §2, journal J5-J8) : FLIP branché (venv 3.12), part du chemin contour par image ; coins de la grille tournée, seuil du vote, rayon et force essayés et rejetés en FLIP ; table `W_ACROSS 0,3` (le reste inchangé), texte nettoyé des variantes rejetées ; **C3 tenu en FLIP et en dépassements sur neuf cas**, ×3 compris ; `malioc` et SweetOpt reportés au jumeau GLSL (P4)
- [x] P2.3 (21/10/2026, rapport `upscaler-mwsr-p2.md` §3, journal J9-J10) : feed externe contre passe 0 sur M1, 780M et RTX → **passe 0 partout** (décision n° 2 renversée par la mesure) ; `half` (f16) −9-10 % sur le 780M, rien sur NVIDIA, mêmes pixels ; borne par échantillons sans gain (retirée) ; sortie anticipée gagnante partout (`EARLY_OUT` retiré) ; `QUAD_UNIFORM` sans objet ; restent `half` et `DERING 0` à mesurer sur iPhone (realme fait le 21/10, J16 : −10 % et −27 %)
- [ ] P2.4 expérience compute par tuiles (optionnelle) — proposée à l'abandon : le noyau est au plancher d'une passe sur ordinateur, et le canvas storage recopie sur mobile (§3.4) [`high`]
- [ ] Rapport P2 et porte (C1-C4 provisoires ; critère d'arrêt) [`xhigh`]

**P3 — HDR, 4:4:4, réduction** (21/10/2026, rapport `upscaler-mwsr-p3.md`, journal J11-J14)
- [x] HDR : le feed `linear` est encodé sRGB étendu (pas de lumière de scène) → `HDR_LINEAR` sans objet, noyau inchangé ; mires HDR, ΔE ITP, feed `rgba16float` au labo ; **C8 qualité tenue** (ΔE ≤ FSR1 sur texte et jeu, +2 % sur la rampe ; max conservé ; 0 % au-delà du max local contre 5 % FSR1)
- [x] `CHROMA444` (vote, cohérence et poids par canal) et ses mires `code` / `codeiso` en 4:4:4 et 4:2:0 simulé : rien sur la syntaxe ordinaire (la luma voit tout), FLIP 0,014 → 0,011 et ΔE00 −10 % sur les bords isoluminants ; **C9 : > SGSR1 partout, = FSR1 sur la syntaxe, −3 % sur l'isoluminant** ; `LUMA_SHARP` essayé et retiré
- [x] Mode `down` (grilles 2×2 / 3×3 / 4×4 par `mwsrRegime`, étendue 1 / 1,25, `LINEAR_LIGHT` 1 par défaut, 2 en option) : lumière linéaire +2,5 à +12 dB, aliasing ≤ Smooth2D, 28-35 dB contre le Lanczos-2 de l'hôte ; **C10 qualité tenue**
- [x] Tests dorés : `--golden` / `--golden-check` (SwiftShader, 9 cas × 4 cellules, `golden.json`)
- [x] Temps GPU RTX et 780M (J14) : **C8 temps tenu** (`mwsr` 0,16-0,23 × FSR1 sur le feed f16), `c444=1` +7-8 %, `half` −9 % sur le 780M et les mêmes pixels en HDR ; la grille 2×2 en lumière calculée manque « ≤ bilinéaire + 30 % » (×1,7-2,2 sur le 780M, ×1,3-1,4 sur le RTX)
- [x] Vues sRGB (J15, SwiftShader) : un échantillon bilinéaire à travers les vues entre ×0,5 et 1:1 → **C10 tenu en coût (+0 %) et en qualité** ; 3×3 à travers les vues sous ×0,5 (aliasing ÷ 5, FLIP ÷ 11) ; `MWSR_DOWN_TAPS_SRGB`, `mwsrRegime(…, { srgb })` → `linearLight` ; dorés régénérés
- [x] Coût des cellules sRGB confirmé au RTX et au 780M (14:33) : les vues ne coûtent rien, l'échantillon unique = le plancher, la 3×3 à travers les vues = la 3×3 calculée ; et sur le realme (J16) l'échantillon unique = le plancher
- [ ] Porte P3 : lecture du rapport `upscaler-mwsr-p3.md` par Bruno (C8-C10 tenus au labo ; regard HDR sur le M27Q en P5) [`high`]

**P4 — Intégration et jumeau WebGL2** (rapport `upscaler-mwsr-p4.md`, 21/10/2026, Fable `max`)
- [x] `mwsr` dans `WebGpuRenderer` (`e965e9f9`, journal J17) : un pipeline par régime (`mwsrRegimes()`), tous à la création ; vues `-srgb` sur le canvas SDR et la texture de la passe 0, liées en mode `down`, repli lumière logicielle si refus ou HDR ; `CHROMA444` depuis `yuv444` (StreamView, worker) ; `half` hors NVIDIA ; `chain-mwsr` du labo = empreinte du noyau et de `mwsr:srgb=1` sur les neuf cas dorés
- [x] `gl-mwsr` dans `WebGlRenderer` (`031805ab`, J18) : `mwsrGlsl()` depuis les mêmes tables ; conformité : `dFdy` = −`dpdy`, ligne de référence des dérivées propre à l'implémentation → C11 reformulé (réduction 1/255 ; noyau : même qualité, FLIP 0,071/0,019/0,029 des deux côtés) ; `scaler_lab.py --conformity` en deux Chromes (`--swiftshader`, `--webgl-swiftshader`), `--page-timeout`
- [x] `createRenderer` (via `WEBGL_ALGOS`), `ENHANCER_LADDER` (`mwsr` entre `sgsr` et `off`, mémoire sans migration), `pickAutoEnhancer` et le HDR d'`auto` derrière `localStorage.mw_mwsr = '1'` (`83aedfad`)
- [x] Menu debug (deux saveurs), i18n (en, fr, zh), overlay (`MWSR`, `WebGL MWSR`), worker (`yuv444`) (`83aedfad`)
- [x] CI : job `scaler-lab` non bloquant (conformité sur SwiftShader, les deux côtés ; chaque texte compile) (`48bb8213`) ; pas de validation statique possible sous Node (notée au rapport §4)
- [x] Porte WebGPU contre WebGL2 avec le nouveau shader sur ordinateur (21/10 au soir, journal J20, rapport §5) : jeu `--p4` (`submit`, `done`, `gpu` par draw ; barrière sondée côté WebGL2, `EXT_disjoint_timer_query_webgl2`, niveau H.264 par taille), RTX et 780M au go de 59 → **WebGL2 reste la saveur SDR** (RTX 1,9-2,3 × à 60 i/s, 780M 1,1-1,2 × ; exception la réduction 4K → 1440p sur le 780M, WebGPU 0,82 ×) ; le drapeau `mw_mwsr` vérifié sur la `--dev` (overlay des deux saveurs, témoin FSR1, repli Canvas2D sans WebGPU, « Off (1:1) »)
- [x] La porte sur les appareils de Bruno (22/10 au matin, journaux J23-J26, rapport §5) : **iPhone 13 Pro → WebGPU** (`done` 0,25-0,53 × WebGL2 partout : l'envoi `texImage2D` de WebKit coûte 3-24 ms de fil principal) ; **Mac M1 Pro (Chrome) → WebGL2 reste** (égalité à 60 i/s, WebGPU 0,5-0,6 × saturé) ; **`mw-arm` (Adreno 618) → WebGL2 par nécessité, `gl-mwsr` 0,63-0,77 × SGSR1**, C1 tenue (8 cas, mw-arm hors réseau ensuite) ; **realme Note 60 → égalité, WebGL2 reste** (0,78-0,98 × ; à 1440p de sortie aucune saveur ne tient 60 i/s) ; proposition de décision par classe au rapport §5, à trancher par Bruno (plan §9, P6) ; cas à la taille de l'écran du realme faits ensuite (WebGPU 0,76-0,90 ×, réduction 0,50 ×, la règle au seuil sur le texte ×2 : Mali défendable en WebGPU, au choix de Bruno)
- [x] Clic-photon sur les téléphones avant P6 (22/10, go de Bruno, journal J28, rapport §5) : la sonde `mwLatency` est biaisée sur téléphone (relecture synchrone en WebGL2) → flash dans la page au clic, écran enregistré, flash → drapeau ; **iPhone 13 Pro → WebGPU confirmé** (`mwsr` − `gl-mwsr` : −6 ms en 720p, +4 en 1080p, intervalles contenant zéro ; en 1080p WebGPU garde le flux à 60 i/s verrouillé, WebGL le fait tomber à 56-58) ; **realme → l'échelle coupe la passe en 2 s dans les deux saveurs à ×1,33**, reste le présentateur simple (égal à 960×540, WebGPU 53-55 i/s contre 30 à 1080p) ; passe forcée au labo : `mwsr` une image après `gl-mwsr` ; piège de la mémoire de l'échelle (`mw_enhancer_rung`) noté ; la mesure du rafraîchissement sous charge signalée à 59 pour la cadence ; décision par classe de Bruno le 22/10 (§9)
- [x] Repli du renderer WebGPU quand `importExternalTexture` refuse l'image (décision de Bruno du 21/10 au soir, journal J21) : copie par `ImageBitmap` dans la texture d'entrée, import retenté tous les 300 draws, console limitée ; test à device simulé, dorés et conformité exacts sur SwiftShader avec `importfail=1` ; coût mesuré le 22/10 sur le 780M : `done` ×1,2-1,3 à ×2 (≈ +1 ms par image), ×1,8 en entrée 4K, fil principal égal (journal J21)
- [x] Troisième repli de l'import WebGPU (décision de Bruno du 22/10, §9 ; journal J29) : après l'import et le bitmap, la lecture (`VideoFrame.copyTo` en RGBA, `writeTexture`, la passe 0 depuis cette texture) ; 30 images perdues de suite, un device perdu avant sa première image ou qui ne revient pas, et le renderer abandonne : la page passe sur `gl-<algo>`, Canvas2D si WebGL2 refuse ; tests à device simulé ; SwiftShader sous Windows et sous Linux : dorés exacts par chaque voie. Cause réelle du noir de la CI : sous Linux en logiciel, le canvas WebGPU ne peut pas présenter, et Chrome détruit le device et l'instance WebGPU de la page
- [x] Job CI « Upscaler lab » (J29) : sortie 0 quand les seuls problèmes sont des chaînes non applicables (`::warning::`), 1 pour une vraie différence ou une image manquante (`::error::`) ; les chaînes dessinent dans une texture du labo (`nopresent=1`), le côté WebGPU passe sous Linux (rejoué dans le WSL de DualRTX) ; déclencheurs inchangés
- [x] À Bruno : la réduction de la zone plate du jumeau sous Linux (J29 ; `chain-gl-mwsr` contre `mwsr`, 45/255 sur 1 % des pixels, l'échantillonnage WebGPU de SwiftShader propre à Linux, le bilinéaire nu `tex-s1` compris) : la garder comme différence connue, et le job sort en 1 à son pas WebGL2, ou juger cette paire contre l'écart des bilinéaires nus des deux API (méthode de C11) [`medium`] — option 2 le 23/10 (J32)
- [x] Le rouge restant du job au pas WebGL2 (réduction de `code`, 0,173 % au-delà de 2/255 contre 0,1 %, depuis le 23/10) : corrigé le 26/10 au oui de Bruno (J46). Chaque Chrome dessinait sa mire ; les deux côtés lisent maintenant la même image d'entrée, dessinée une fois par le côté WebGPU. Sous Windows, les trois réductions deviennent exactes (0/255) ; le runner le confirmera au prochain push

**P5 — Campagne**
- [ ] Matrice du §7.3 (DualRTX complet ; étoile ailleurs) [`xhigh`]
- [ ] Énergie : `powermetrics` sur M1, décharge iPhone, occupation GPU `mw-arm` ; décharge du realme, présentateur WebGPU contre WebGL2 (décision du 22/10, §9) [`xhigh`]
- [ ] `/bench` avec `mwsr` ; clic → photon plein écran [`high`]
- [ ] Au prochain `/bench` complet, sur Qualcomm (`mw-arm`) : vérifier que la barrière que le renderer pose après chaque dessin (`_setFence`, lue au dessin suivant par `takeGpuBehind`) ne fait pas compter au gouverneur de faux « GPU en retard » (descente d'échelle injustifiée). Journal J38 du 24/10 : sur un OffscreenCanvas, cette barrière retarde de 120 à 190 ms l'achèvement vu par une barrière suivante sur les pilotes D3D11 de NVIDIA et de Qualcomm (26 ms sans elle sur l'Adreno 618) ; sur un canvas d'élément la RTX signale en 1 ms (J37, banc 3), Qualcomm en flux réel reste à voir. Décision de Bruno du 24/10 (par 59) ; rien dans le produit avant [`high`]
- [ ] A/B subjectif de Bruno (texte, bureau, jeu ; LAN et Internet ; HDR sur le M27Q)
- [ ] Rapport : objectif par objectif, démontré / attendu, non mesurables [`xhigh`]

**P6 — Défauts, documentation, release**
- [x] **Bascule de `auto` par classe d'appareil, faite le 24/10 au « MWSR défaut en v0.4.1 » de Bruno (avant P5, à sa demande ; journal J41)** : `pickAutoEnhancer({ hdr })` rend la saveur — `gl-mwsr` sur PC, Mac, Snapdragon et les GPU mobiles jamais mesurés (SGSR1 avant), `mwsr` sur iPhone et iPad quand WebGPU est là et en HDR partout, `off` sur un Mali d'entrée de gamme (`isEntryMaliGpu` : G31, G51, G52, G57, G310 et la série T, lus dans la chaîne du renderer WebGL) ; présentateur simple WebGPU sur cette classe (`plainPresenterIsWebGpu`, hors TV et appareils d'un gigaoctet) ; le drapeau `mw_mwsr` retiré ; mémoire du gouverneur sans migration (lue sous le même plafond seulement) ; `README.next.md`, wiki (ch. 1, 3, 4, 7) et brouillon des notes v0.4.1 ; frontière de la classe Mali confirmée par Bruno le 24/10 vers 23:50 (« On fait comme ça »)
- [ ] Chapitre du wiki sur MWSR (addendum au 15 ou chapitre 16) [`medium`]
- [x] **Fait le 24/10 à midi (journal J38)** : chemin plat par classe d'appareil (décisions du 24/10, §9) : CR5 (`FLAT_TAPS 5`, `FLAT_C 0,5`) sur ordinateur, Q3 (`FLAT_TAPS 3`) sur téléphone et tablette, par les constantes de pipeline des deux jumeaux à la création ; dorés et table de conformité refaits ; SwiftShader puis le RTX et le 780M au go de 59 ; TV Android mesurée le 24/10 au soir (Mi TV, Mali-G31 : Q3 176,6 ms contre CR5 208,1, −15 %, comme le realme ; journal J41) [`high`] ; dorés et conformité refaits sur SwiftShader, RTX et 780M vérifiés au comparateur (0,950 / 25,4 dB partout)
- [x] **Étude « netteté dynamique selon l'échelle », faite le 24/10 (journal J39)** : la loi ×2 en paramètres du noyau (`S_X2`, `X2_BASE`, `X2_FLAT`, `X2_R_ACROSS`, neutres par défaut, dorés intacts ; plancher de cohérence et plafond `W_MIN` par l'échelle mesurés morts, retirés) ; le levier à ×2 est l'uniforme : netteté 1,25 → étalon centre 28,8 → 31,0 dB, FLIP 0,067 → 0,056 (FSR1 32,3 / 0,045), `game` 34,9 → 35,5, texte au FLIP de FSR1 ; `mwsrSharpness(s)` écrite et testée, pas câblée ; comparateur : bouton de la loi et les trois constantes en réglages (`x2b`, `x2r`, `x2f`)
- [x] **Loi d'échelle de la netteté (J39), câblée le 24/10 au oui de Bruno (journal J40)** : `mwsrUniforms()` prend `mwsrSharpness(min(outW/inW, outH/inH))` quand l'appelant ne nomme pas de netteté (1,85 jusqu'à ×1,33, 1,542 à ×1,4, 1,233 dès ×1,5 ; réduction inchangée), le comparateur part de la loi (« (loi) » au réglage, `sharp=` du lien prime) ; dorés refaits, conformité des deux côtés ; aucune UI touchée
- [x] **La netteté à ×2 passe à 1,5, au choix de Bruno du 29/10 (plan B §8)** : `MWSR_SHARP_X2` ; la loi d'avant jusqu'à ×1,75 (1,85 jusqu'à ×1,33, 1,23 dès ×1,5), puis une rampe jusqu'à 1,5 à ×2, tenue au-delà ; vérifiée à ×1,5 et ×1,75 (rien n'y gagnait) ; texte ×2 86,1 → 86,5, jeu 66,2 ; dorés des trois cas ×2 refaits, conformité des deux jumeaux ; `cpp/mwsr.h`, README et comparateur suivent
- [x] **Le nom de l'iGPU AMD corrigé, oui de Bruno du 29/10** : « Radeon iGPU (Ryzen 9 9900X) » dans le README de `mwsr/`, ses figures de vitesse et /mwsr/ ; « AMD iGPU » dans les commentaires du code ; notes de correction ici (§1.8), dans P0 et dans le journal du labo. Les chiffres ne changent pas
- [x] **Le vrai Radeon 780M, oui de Bruno du 29/10** : les temps du README sur l'UM790Pro, le 30/10 sous Ubuntu, puis le 02/11 sous Windows 11 sur le texte final (journal J56)
- [ ] En réduction, un affûtage après la grille seulement si Bruno le demande [`low`]
- [x] **Le HDR dans le chemin du produit, testé les 25 et 26/10 à la demande de Bruno (journal J45).** `hdr_check.py` fait dessiner au vrai `WebGpuRenderer` des images PQ BT.2020 en 10 bits, agrandies et réduites, sur les chemins `linear` et `tonemap`. Le mode « off » en HDR mettait à l'échelle au plus proche voisin : corrigé en 1d6f92f5
- [x] La réduction HDR assombrit le contraste fin (rayures de 1000 et 0,5 nit : −8 % à ×0,5, −28 % à ×0,667). Proposé : sur un feed flottant, la passe 0 écrit de la lumière linéaire quand on réduit (exact, sans coût en plus) ; à la décision de Bruno [`high`] — oui de Bruno le 26/10, fait (journal J47) : `LINEAR_LIGHT 3`, les rayures gardent 0,987 à 1,004 de leur lumière ; RTX : −7 à −11 % de temps GPU sur les réductions HDR, SDR inchangé (dorés au bit près, temps à 1 % près)
- [x] L'agrandissement HDR a régressé depuis J11 (ΔE ITP du texte 27 % au-dessus du bilinéaire avec les défauts d'aujourd'hui, 2 % en dessous le 21/10) : étude réglage par réglage au labo, puis l'œil de Bruno sur un écran HDR [`high`] — étude faite le 26/10 (J48, `hdr_check.py --ablate`) : c'est `DERING 2` (texte 20,86 → 16,24 sans lui, jeu −5 %, le pic de 4:2:0 1,15 → 1,02) ; il garde en revanche les traits d'un pixel (rampe 1080p : 14,47 contre 19,93)
- [x] À Bruno : `DERING 1` sur le seul chemin HDR `linear` (le SDR et `tonemap` gardent `DERING 2`), ou garder les défauts ; à l'œil sur un écran HDR (J48) [`medium`] — verdict du 26/10 au soir, après l'A/B HDR : `DERING 2` partout (« Je confirme, je veux Dering 2 partout ») ; aucun changement de code (journal J50)
- [x] La traîne p99 de la réduction HDR à ×0,667 (J47), et la vitesse HDR + SDR refaite sur la RTX qui ne décode pas [`high`] — V10 du 28/10 (journal J51) : gains de J47 confirmés (−11,6, −7,6 et −11,5 % ; témoin +0,1 %), SDR à 1 % près ; la traîne est l'état d'énergie de la carte (P5, mémoire à 810 MHz : 2,5 à 3,5 ms pour les deux variantes), pas le shader ; rien à changer
- [x] MWSR pour les autres programmes, à la demande de Bruno le 28/10 (MIT, dossier `mwsr/`, nom MWSR, shaders + module + C++) [`high`] — cb766889 puis journal J52 : WGSL, GLSL, jumeau HLSL (FXC et DXC), `cpp/mwsr.h`, exemple D3D11, README anglais ; HLSL sur WARP contre WGSL sur SwiftShader à 1/255 hors bascules du vote ; `X2_FLAT` (labo, neutre par défaut) instable d'un GPU à l'autre, correctif à la décision de Bruno
- [x] **A29, la loi ×2 retirée avant le tag v0.4.1**, à la demande de Bruno le 28/10 (« supprimer X2 pour la v0.4.1 ») — journal J54 : `S_X2`, `X2_BASE`, `X2_FLAT`, `X2_R_ACROSS` (J39, neutres par défaut) sortis des trois jumeaux, des tests, du labo, du comparateur et du README de `mwsr/` ; dorés au bit près, conformité WebGL2 et HLSL inchangées

**v0.4.2 — le réglage livré, demandé par Bruno le 01/11/2026**
- [x] Le balayage LPIPS puis VMAF NEG et la loi : `sharp` et `racross` suivent l'échelle, deux tables (avec les zones et sans), `walong` 0 — journal J55. Zones lues dès ×1,15. Comparateur public et README de `mwsr/` en LPIPS et VMAF NEG, le SSIM retiré
- [ ] Le coup d'œil de Bruno sur le comparateur en ligne, le texte à ×1,333 compris (VMAF NEG 57,5 avec zones et 58,9 sans, contre 62,65 au livré) [`low`]
- [x] Décidé par Bruno le 02/11 : les termes d'écran de la loi restent à zéro ; les écrans 1080 et 2160 avec zones restent à 89-93 % du gain LPIPS (règle : 95 %) ; le texte à ×1,333 garde la loi et se juge à l'œil
- [ ] L'activation par défaut des zones, au oui de Bruno, après son coup d'œil et le coût côté hôte. Ce coût est mesuré (`upscaler-mwsr-b.md` §12 f.3 et f.4) : 64 µs dans un vrai stream sur la RTX en 1440p, budget de 1 ms tenu en direct, puis la légère, puis la loi. Un vrai stream hors de ce cas n'est pas mesuré (§12 g) [`medium`]
- [x] L'optimisation du shader livré, journal J56 : sans lectures le long des bords ni cohérence, un poids par image, zéro perte mesurée ; puis, à la décision de Bruno du 02/11, le chemin plat à une lecture (`FLAT_TAPS 2`) partout, seuil 0,0025. WebGPU contre la v0.4.1 : RTX −48 %, iGPU −57 %
- [x] Les temps définitifs du README sur la RTX et l'iGPU, avec FSR1, NIS et SGSR1 dans la même page, les scores et les figures (J56). La colonne de la Radeon 780M le 02/11, sous Windows 11 (J56)
- [x] La lecture sur la 780M, le 02/11 : −20 % contre la v0.4.1, le plus rapide des quatre sur les six images, la f16 sans gain (J56)
- [ ] La lecture sur téléphone (Bruno, sur le comparateur en ligne, après le signal de 59) et sur la Mi TV (WebGL2, `mediump` contre `highp`, créneau de 59) [`medium`]
- [ ] Après cette lecture : retirer les chemins à 3, 5 et 9 échantillons et `fc` (décidé par Bruno le 02/11), et décider de la f16 [`medium`]

**Hors v0.4.1 — étude optionnelle (§14, proposée le 21/10/2026, à la décision de Bruno)**
- [x] **P7.0, faite le 25/10 (journal J42, §14.4) : la porte reste fermée.** Mode séquence du labo (`seq-lab.js`, `p7_flows.py`), WebCodecs logiciel à deux débits, sept variantes d'historique contre `mwsr` seul, vingt séquences sur SwiftShader. Aucune variante ne passe : sans mouvement connu, de −0,01 à −0,13 dB et pas de stabilité gagnée ; même avec le mouvement exact, +0,11 dB en moyenne et des traînées
- [ ] ~~P7.1 (si go) noyau `mwsr` avec histoire adaptative au mouvement, cran `mwsr-t` des deux renderers, campagne allégée, A/B de Bruno à faible débit~~ : sans objet, la porte de P7.0 est restée fermée
- [x] **P8.0, faite le 25/10 (journal J43, §14.6) : la porte reste fermée.** Déblocage côté client avant `mwsr`, un seuillage DCT sur des grilles 8×8 décalées : +0,33 dB sur le flux seul (x264), mais après `mwsr`, sur le clip CoD en 1440p, +0,02 à +0,09 dB en 720p à 4 Mbit/s (la porte demandait +0,1) ; pas de passe dans le produit
- [ ] L'aide de l'hôte (drapeau de scène, vecteurs) : à la décision de Bruno ; le déblocage guidé par le découpage et le QP des blocs est écarté par P8.0 [`high`]
- [x] **Les versions 2 et 3 dans le menu de débogage, faites le 25/10 à la demande de Bruno (§14.7, journal J44).** Le noyau précédé du déblocage de P8.0 : seuil fixe 8 borné à 2 (version 2), ou force réglée sur le QP de chaque image, lu dans l'en-tête de sa tranche (version 3). WebGPU seul, builds de débogage seuls, jamais en auto. Vérifiées sur SwiftShader. Verdict de Bruno le 25/10 au soir : la version 1 reste celle de l'auto ; les versions 2 et 3 restent dans le menu de débogage

**Dépendances à demander tôt** : un téléphone Android de prêt (Snapdragon 8 Gen 1 ou plus, et
un Mali si possible) ; les créneaux Mac et iPhone pour P0 et P5 ; `malioc` installé sur DualRTX.

### 12.2 Prompt de lancement (à coller par Bruno dans une nouvelle session, après la v0.4.0)

> Plan v0.4.1 « shader d'upscaling MoonlightWeb » : `docs/design/upscaler-mwsr.md` (copie du plan
> remis le 10/10/2026). Dépôt `E:\Code\moonlight-web`, branche `main`, index privé (sessions
> parallèles), commits en anglais, formatage avant commit, push par Bruno.
>
> Décisions déjà prises (§0 du plan) : un algorithme en deux jumeaux (WGSL de référence, GLSL ES
> 3.00 à coût égal, accélérations WebGPU par `override`), pas de passe 0 en SDR si P0 le confirme,
> salle blanche (aucune ligne ni constante des ports FSR1/SGSR1/NIS/Atyuwen). Les trois ports
> existants ne bougent pas. Aucun changement côté hôte.
>
> Faits à ne pas redécouvrir : §1 (architecture : passe 0, trois feeds HDR, gouverneur, Auto 1440
> lignes sur mobile), §2.7 (coût de la texture externe : deux fetches par échantillon ; horodatages
> quantifiés à 100 µs sans le drapeau développeur ; `copyExternalImageToTexture` écarté ; f16
> probablement absent sur l'Adreno 618), §2.8 (les réécritures de compilateur ne rapportent rien ;
> le vérificateur hors de portée du générateur).
>
> Commencer par **P0** (effort `high`) : page de laboratoire, relevé des fonctionnalités, essai
> décisif « externe contre RGBA », lignes de base sur DualRTX (780M, Arc, RTX), puis `mw-arm`,
> Mac M1 et iPhone sur mes créneaux. Règles de banc : jamais « performances max », HAGS tel quel,
> encodeur et décodeur sur des GPU différents, onglet visible, Chrome de banc dédié par `cdp.py`,
> drapeau `enable-webgpu-developer-features` sur ce Chrome seulement. Pas de banc sur une machine
> sans mon accord ; le Mac est à Léo.
>
> À la porte P0 : rapport (dispersion, décision passe 0, lignes de base), paragraphe
> « Concrètement, pour l'utilisateur », proposition de `/compact`, et effort conseillé pour P1 et
> P2.1 (P2.1 = `max`). P1 peut être livré avant P2. Rien ne devient défaut avant P5.

---

## 13. Décisions du 19/10/2026 et mise à jour du parc (au lancement de P0)

Bruno a répondu le 19/10/2026 aux cinq questions posées au lancement ; le plan ci-dessus reste le
texte de référence, ce chapitre dit ce qui change.

| # | Question (§0, §9) | Décision | Conséquence dans le plan |
|---|---|---|---|
| D1 | Un algorithme, deux jumeaux | **Oui** | §3.6 tel quel |
| D2 | Pas de passe 0 en SDR | **Oui, si P0 le confirme** | §6.4 tel quel : l'essai « externe contre RGBA » reste la première mesure |
| D3 | Salle blanche | **Oui**, avec la consigne : « pense à utiliser les fonctions modernes des shaders ; on fera un fallback vers les shaders existants si ça ne le supporte pas » | la base de `mwsr` emploie les fonctions récentes du langage (§3.4 : dérivées, `override`, f16, sous-groupes, lecture directe de la texture externe) ; là où un navigateur n'offre pas ce qu'il faut, le client **garde les ports actuels** (FSR1, SGSR1, NIS, bilinéaire) comme repli, en plus de leur rôle de références de banc. Le jumeau GLSL (P4) reste prévu, mais il n'est plus la seule issue d'une plateforme sans WebGPU moderne : si P4 montre qu'il n'apporte rien par rapport à ce repli sur une classe d'appareil, Bruno pourra le réduire |
| D4 | Modèle de P2.1 | **Fable 5.1 `max`** (« j'ai pris un Max x20 »), exception à la règle « Opus 5.5 partout » pour ce seul chapitre | consigne attachée : dès que ce niveau n'est plus nécessaire, **faire une pause et demander un `/compact` et un changement d'effort et/ou de modèle** ; le reste des phases garde la colonne du §9 |
| D5 | Appareils mobiles | Bruno dispose d'un **iPhone 13 Pro** et d'un **realme Note 60** | voir ci-dessous |

**Le parc, corrigé.**

- **`mw-arm` n'a pas de WebGPU** : sur son Adreno 618, `navigator.gpu.requestAdapter()` rend `null`
  (relevé du banc du 06/09/2026, mémoire du dépôt). Le §1.8 et le critère C1 le comptaient comme
  banc WebGPU mobile : il ne sert qu'aux **lignes de base WebGL2** (`gl-sgsr`, `gl-fsr1`) et au
  jumeau GLSL (P4). Le mobile WebGPU se mesure sur les deux appareils de Bruno.
- **iPhone 13 Pro** : A15, écran 2532×1170 à 120 Hz, Safari 26 (WebGPU, `shader-f16`, texture externe
  sans copie ; `timestamp-query` si le compteur est exposé). C'est presque la configuration de l'article
  d'Atyuwen (iPhone 12, A14, même sortie 2532×1170) : ses chiffres (EASU 5,4 → 1,8 ms) deviennent
  **vérifiables au banc**, dans un navigateur au lieu d'un moteur natif.
- **realme Note 60** : Unisoc T612 (12 nm, 2 × A75 + 6 × A55 à 1,8 GHz), **Mali-G57 MP1**, écran IPS
  720×1600 à 90 Hz, Android 14. Premier **Mali** et premier **Android** du parc, et un GPU d'entrée de
  gamme : le cas le plus dur pour « plus rapide que SGSR1 sur mobile », et le plus parlant pour
  l'énergie. Chrome Android y tourne sur Vulkan si le pilote le permet (Chrome 121+), sinon en
  **mode compatibilité** OpenGL ES 3.1 (Chrome 146+) : P0 relève `adapter.info`, `adapter.features`
  (`shader-f16` est attendu sur Mali-G57, `timestamp-query` incertain) et le mode. Avec le câble USB
  et le débogage activé, `adb shell dumpsys batterystats` et `/sys/class/power_supply/battery/
  current_now` donnent une **mesure d'énergie** que le plan disait impossible (§0 : « non démontrable
  sans banc ») : elle devient possible, bruitée, en différences A/B sur des cellules longues (§7.2).
  Sur cet écran de 720 lignes le cas d'usage est la **réduction** (flux Auto jusqu'à 1440 lignes,
  §1.6) et l'agrandissement du zoom à deux doigts : les deux régimes y sont mesurés.
- **Arc A380** : absente de la liste des adaptateurs de DualRTX le 19/10 au soir (seuls le RTX 5060 Ti,
  le Radeon 780M et les écrans virtuels y figurent) ; Bruno a confirmé le 20/10 qu'il a dû la
  **débrancher**, sa compatibilité sera vérifiée plus tard. Le §1.8 la comptait comme troisième GPU
  d'ordinateur ; sans elle, les classes d'ordinateur restent NVIDIA (RTX), AMD (iGPU du 9900X et 780M de l'UM790Pro),
  Intel (N95) et Apple (M1) ; la classe Intel Arc attend son retour.

**Ce que cela change aux critères.** C1 et C6 se lisent désormais « iPhone 13 Pro, realme Note 60,
et `mw-arm` en WebGL2 seulement » ; la mention « démontré sur Adreno 618 et Apple, extrapolé ailleurs »
(§8.1) devient « démontré sur Apple A15 et Mali-G57, extrapolé aux Adreno 7xx/8xx et aux Mali G7xx ».
Les créneaux des deux téléphones sont ceux de Bruno (P0 pour le relevé des fonctionnalités et les
lignes de base, P5 pour la campagne) ; la page de laboratoire (§6.1) tourne dans leur navigateur
depuis le serveur du labo sur le LAN.

**Départ.** P0 a commencé la nuit du 19 au 20/10/2026 : ce plan copié dans `docs/design/`, la page de
laboratoire `scripts/bench/scaler-lab/`, les lignes de base sur DualRTX (SwiftShader d'abord, puis le
780M et le RTX), le rapport P0 dans `docs/design/upscaler-mwsr-p0.md`.

---

## 14. Question du 21/10/2026 : une mémoire des images précédentes ?

Question de Bruno, posée à la fin de P2.3 : « est-ce possible de fonctionner avec une empreinte
mémoire des anciennes images tant que c'est dans la continuité de la même scène ? Ça pourrait gagner
en netteté ? À ne pas faire tout de suite, mais à prévoir comme plan si c'est réaliste. »

**Réponse courte.** Possible, sans image de retard, pour une texture de plus et un coût de l'ordre
d'une passe 0 ; mais ce qu'une telle mémoire peut rendre dans notre situation est de la **stabilité**
(moins de fourmillement des contours en mouvement lent, moins de respiration de la compression), pas
de la **résolution**. La netteté par accumulation demande des échantillons décalés au sous-pixel et
recalés exactement, ce que le flux ne porte pas. Réaliste comme **étude courte, hors v0.4.1**, avec
une porte chiffrée avant toute ligne dans le renderer (§14.3).

### 14.1 D'où viendrait la netteté, et pourquoi elle n'y est presque pas

Trois régimes :

- **Contenu fixe** (menus, texte, cartes, HUD) : l'encodeur l'envoie en blocs « skip », les images
  décodées sont identiques au pixel près : il n'y a rien à accumuler. Après une coupure, le contrôle
  de débit raffine les blocs fixes pendant quelques images : l'image courante y est meilleure que
  l'histoire, qui devient un passif.
- **Mouvement lent** (panoramique, défilement) : le seul régime où des phases sous-pixel différentes
  de la même scène existent dans des images successives — pour les contours que le rastériseur a
  échantillonnés autrement (arêtes, traits fins), pas pour les textures mipmappées, déjà limitées à
  la fréquence du rendu, ni au-delà de ce que l'hôte a rendu : le flux est rendu à sa résolution, le
  détail au-dessus de sa Nyquist n'existe nulle part. Les récolter demande de recaler l'histoire au
  dixième de pixel. DLSS, FSR2 et SGSR2 le font avec les vecteurs exacts du moteur et une gigue
  délibérée qui couvre toutes les phases ; nous n'avons ni l'un ni l'autre : WebCodecs n'expose pas
  les vecteurs du codec, et l'hôte capture un bureau, il n'en a pas davantage. Un flot estimé côté
  client (recherche de blocs par tuile, raffinement sous-pixel) recale un panoramique, pas les objets,
  le HUD, les particules ni les transparences : là, le rejet de l'histoire ramène à l'image seule. Et
  le recalage approché, plus le rééchantillonnage bilinéaire de l'histoire à chaque image, adoucit au
  lieu d'affûter — c'est pourquoi tout TAA finit par une passe de netteté.
- **Mouvement rapide, coupures, effets** : l'histoire est rejetée ; aucun gain, et le fantôme si on se
  trompe.

Ce qui existe sans vecteurs est un réseau de neurones sur tensor cores (RTX Video Super Resolution :
plusieurs millisecondes sur une RTX de bureau), hors de portée d'un shader d'une milliseconde sur un
iGPU, et hors du chemin WebCodecs → canvas du client. Les deux voies vers une vraie super-résolution
temporelle passeraient par l'hôte — exporter les vecteurs de l'encodeur matériel (NVENC le permet en
mode estimation seule ; des vecteurs par bloc, choisis pour le débit, pas pour la vérité), ou gigoter
la réduction côté hôte (incompatible avec la compression : le contenu fixe cesse d'être « skip ») —
et ne relèvent pas de ce plan.

### 14.2 Ce qu'une mémoire donnerait vraiment

Un filtre récursif adaptatif au mouvement, **avant** l'agrandissement : `H' = mix(C, H, α)` là où
`|C − H|` est sous un seuil et où `H` tient dans la borne 2×2 de l'image courante (celle que
`DERING 1` calcule déjà), `H' = C` ailleurs ; réinitialisation à la coupure (différence globale).
Ce qu'on en attend, à mesurer :

- moins de fourmillement et de rampement des contours affûtés par `mwsr` pendant les mouvements lents ;
- moins de respiration des blocs et de bruit de moustique autour des contours lents, surtout aux
  faibles débits (Internet, 10-20 Mbit/s) ;
- par ricochet, une netteté (`sharp`, `W_ACROSS`) qu'on peut monter d'un cran sur une entrée plus
  propre sans amplifier le bruit.

Ce qu'on n'en attend pas : un gain de PSNR ou de FLIP sur le fixe (la sortie y est l'entrée), un gain
de résolution en mouvement.

**Coût.** Pas d'image de retard : le filtre est récursif, la sortie de l'image t ne dépend que de t et
de la mémoire ; la règle « latence d'abord » tient. GPU : une lecture et une écriture de texture à la
résolution d'entrée, de l'ordre de la passe 0 (+0,13 ms sur le 780M en P2.3), à fusionner avec elle ;
une estimation de mouvement par tuile ajouterait une petite passe compute. Mémoire : une texture
RGBA8 à la résolution d'entrée (8 Mo à 1080p, 33 Mo en 4K) ; accumuler à la résolution de sortie,
seule voie vers un gain de résolution, coûterait 66 Mo en 16F à 4K, hors budget sur téléphone.
Renderers WebGPU et WebGL2 seulement (une texture persistante) ; Canvas2D exclu ; le contournement
1:1 reste une copie (le filtre y serait un cran à part, plus un « off »).

**Risques.** Les traînées sur ce qui bouge, le défaut que les joueurs reprochent à tous les
traitements de téléviseur ; le HUD fixe sur fond mobile et les transparences ; la coupure non
détectée ; un cran de plus à l'échelle du gouverneur ; le HDR à filtrer en lumière linéaire.

### 14.3 L'étude proposée (P7, hors v0.4.1, après P4)

Porte avant toute ligne dans le renderer : **une session de labo**, SwiftShader puis le RTX.

1. Mode séquence du labo : la page dessine un panoramique sous-pixel de ses images de référence
   (0,3 à 2 px par image, puis un arrêt, puis une coupure), réduit chaque image à la résolution du
   flux et passe la séquence par la boucle WebCodecs existante (`makeFrames` : 60 i/s, 20 Mbit/s,
   `realtime`, encodeur matériel) à deux débits ; la vérité terrain de chaque image est connue.
2. Mesures par image : FLIP et PSNR contre la référence, `mwsr` seul contre `mwsr` avec histoire (α et
   seuil balayés) ; scintillement (différence de sortie image à image là où la référence est fixe) ;
   traînée (erreur sur les cinq images après la coupure et sur le bord d'un objet mobile).
3. **Go** si l'histoire gagne, sur les séquences lentes, FLIP − 0,005 ou + 0,3 dB *et* divise le
   scintillement par deux, sans traînée mesurable au-delà de `mwsr` seul sur les rapides (seuils à
   fixer par Bruno) ; sinon fermé en une session, et le §3.5 reste tel quel.

Si go : P7.1, le noyau avec histoire (table, journal, comme P2.2), puis le cran `mwsr-t` dans les deux
renderers (comme P4), une campagne allégée (780M, M1, iPhone) et l'A/B de Bruno en Internet à faible
débit : trois à quatre sessions, effort `xhigh`. Attendu honnête : un gain de stabilité mesurable sur
les panoramiques lents et les faibles débits, un gain de résolution nul ; si l'étude dit autrement,
tant mieux, c'est pour cela qu'on mesure.

### 14.4 Le verdict de P7.0 (25/10/2026) : non

L'étude est faite (journal J42). **Le dispositif :**
- des séquences tirées d'une image fixe de 3840×2160, texte du labo ou capture de jeu : panoramique de 0,3 à 6 px du
  flux par image, arrêt, coupure ; ou un bloc qui traverse l'image ;
- une vérité exacte, tirée de la même fenêtre ;
- WebCodecs en logiciel à 10 et 20 Mbit/s, avec un échauffement qui rejoue le premier mouvement à l'envers, pour que
  le débit soit tenu ;
- ×2 et ×1,33 vers 1440p ;
- sept variantes d'historique contre `mwsr` seul : trois sans mouvement connu (tirage borné, poids doux, bornage
  3×3), trois avec le mouvement exact (des oracles), dont l'accumulation de la sortie en 1440p.

**Aucune variante ne franchit la porte du §14.3 :**
- **Sans mouvement connu, rien ne se gagne.** Les historiques bon marché, ceux qui marcheraient avec tout hôte, font
  de −0,01 à −0,13 dB, avec une instabilité des contours inchangée (×0,99-1,00). À l'arrêt, ils font bouger la
  sortie de 5 à 60 % de plus que `mwsr` seul, car ils se recalent encore après le panoramique. Le flux décodé, lui,
  ne bouge plus : l'encodeur répète ses blocs, comme le prévoyait le §14.1. Le bornage 3×3 sans vecteurs perd de 1,5
  à 6 dB.
- **Avec le mouvement exact, le gain reste petit.** L'accumulation en 1440p gagne +0,11 dB et −0,002 de FLIP en
  moyenne, jusqu'à +0,58 dB sur le texte en ×1,33. Ce gain est plat le long du panoramique : c'est un débruitage,
  pas de la résolution. L'instabilité ne baisse que de 8 % (×0,92), et il reste des traînées : jusqu'à −6,4 dB.
- **Personne n'échappe aux traînées.** Toutes les variantes perdent à la coupure et derrière un objet qui bouge
  autrement que la caméra : jusqu'à −8 dB sur cinq images pour le bornage 3×3.
- **Pas de débit à gagner.** À 10 Mbit/s, aucune variante ne rattrape `mwsr` seul à 20 : −0,41 dB pour la meilleure,
  contre −0,61 pour `mwsr` seul.

P7 se ferme donc à P7.0. `mwsr` et le §3.5 restent tels quels, et il n'y aura pas de cran `mwsr-t`. La page de
comparaison faite pour Bruno montre recadrages, cartes d'erreur, courbes par image et coûts.

### 14.5 Après P7 : l'aide de l'hôte et le déblocage (idées de Bruno du 25/10/2026)

Bruno propose une troisième version, qui dépendrait de l'hôte natif. Celui-ci enverrait :
- un drapeau de nouvelle scène ;
- des vecteurs de mouvement, à faible débit (facile en LAN) ;
- ou une image légère, monochrome, de contours ;
- puis la taille des blocs, pour un déblocage côté client.

**Ce que P7.0 en dit déjà.**
- **Le plafond des vecteurs.** Les oracles sont exactement ce que des vecteurs parfaits permettraient : +0,2 à
  +0,6 dB sur un panoramique, sans stabilité gagnée.
- **Ce qu'il faudrait encore.** Le drapeau de scène supprime la traînée de la coupure, pour quelques bits par image.
  Seuls des vecteurs par bloc supprimeraient celle des objets qui bougent seuls.
- **Le coût.** Une passe d'accumulation en 1440p et 59 Mo de mémoire.

**Les débits en jeu.**
- Un vecteur par bloc de 16×16 en 720p représente environ 7 Mbit/s brut, sans doute de 0,3 à 0,7 Mbit/s compressé.
  Un seul mouvement de caméra tient en quelques octets.
- Sur le bureau, les rectangles de déplacement de DXGI (`GetFrameMoveRects`) donnent un mouvement exact et gratuit.
- Une image de contours en pleine résolution coûte presque ce que coûterait de monter le débit du flux principal.
  L'encodeur dépense mieux ces bits. La forme normalisée de cette idée est LCEVC (MPEG-5 partie 2, brevets V-Nova) :
  une couche de base, plus une couche d'amélioration légère appliquée par un shader.

**Le déblocage, mesurable avant tout.** Les blocs visibles dans les scènes rapides (le clip CoD des bancs, lui-même
une capture YouTube en VP9 à 12,3 Mbit/s, réencodée) ne sont touchés ni par `mwsr` ni par un historique. Une passe
de déblocage côté client connaîtrait la grille du flux. Elle lisserait les petites marches posées sur cette grille,
dans les zones plates, sans toucher aux vraies arêtes. Ses limites viennent de la taille variable des blocs en HEVC
et en AV1, et des détails réels posés sur la grille. Le découpage et le QP de chaque bloc lèveraient ces deux
limites : la force du lissage suivrait la compression. Le mode séquence de ce labo la mesure telle quelle : flux à
10 Mbit/s avec de vrais blocs, vérité connue.

**Deux routes pour obtenir le découpage, le QP et les vecteurs :**
- **L'hôte natif les envoie.** C'est léger pour le client, mais limité au natif. NVENC donne au moins le QP moyen de
  chaque image ; ses statistiques par bloc restent à vérifier.
- **Le client les lit lui-même dans le flux.** Ces informations sont déjà dans le flux H.264, HEVC ou AV1 que reçoit
  le client, mais WebCodecs ne les expose pas. Un analyseur d'entropie en WASM, qui ne reconstruit pas l'image, les
  lirait pour tout hôte, sans aucun débit en plus. En contrepartie, il coûte du CPU au client et c'est un gros
  chantier.

Ces choix reviennent à Bruno. Le premier pas mesurable est le déblocage au labo.

### 14.6 Le verdict de P8.0 (25/10/2026) : non

L'étude est faite (journal J43, outils dans `scripts/bench/scaler-lab/p8/`). Elle mesure un déblocage côté client,
entre le décodeur et `mwsr`, en deux temps.

**Hors ligne, sur le flux seul, sur le processeur.**
- **Le dispositif.** Trois fenêtres du clip CoD des bancs (une explosion, un vol rapide au-dessus d'une ville, une
  scène calme), plus le texte du labo et la capture de 0 A.D. en panoramique. Encodage en 720p et 1080p à 60 i/s
  par x264, x265 et libaom en temps réel, avec un tampon d'une image comme l'hôte natif. La vérité est l'entrée de
  l'encodeur.
- **Le constat.** x264 laisse une grille nette ; x265 et libaom presque aucune dans les aplats. Les images fixes n'en
  ont pas : ce qu'elles perdent, c'est du détail.
- **Le meilleur filtre.** C'est un seuillage DCT sur des grilles 8×8 décalées, appliqué à la luminance. À seuil
  fixe 3, il gagne +0,33 dB sur le clip en H.264 (+0,33 en HEVC, +0,26 en AV1) et efface 42 % des marches de la
  grille. Il ne touche pas au texte (+0,03 dB) et perd −0,04 dB sur le jeu.
- **Les autres.** Le filtre des bords à la H.264 gagne peu (+0,09 dB). Les filtres de ffmpeg (`deblock`, `spp`,
  `pp7`, `fspp`, même nourris du QP réel de chaque bloc) perdent tous.
- **L'aide de l'hôte.** Un seuil réglé sur le QP de chaque image donne +0,35 dB et efface 60 % des marches. Le
  meilleur seuil de chaque image, le plafond, donne +0,39 dB.

**Au labo, dans la chaîne du produit.**
- **Le dispositif.** Le mode séquence de P7 a reçu les passes WGSL du filtre (`dbd`, et `dbe` pour le filtre des
  bords). Il lit aussi le clip lui-même (`vid=`) : chaque image du clip est la vérité en 1440p. L'encodeur est celui
  de Chrome en logiciel, puis `mwsr` agrandit en 1440p.
- **L'essai sur les images fixes** confirme qu'elles n'ont pas de grille : moins de 0,01 dB pour chaque réglage. La
  porte a donc été amendée avant la campagne : le critère des blocs se juge sur deux fenêtres du clip, la ville et
  l'explosion, avec les mêmes seuils.
- **La campagne** (`p8v`, quatre pages, une heure sur SwiftShader). À bas débit, les gains vont de +0,02 dB (seuil
  3) à +0,09 dB (seuil 12 borné à 4) en 720p à 4 Mbit/s, et de +0,02 à +0,13 dB en 1080p à 6 Mbit/s. FLIP ne bouge
  presque pas (au mieux −0,001). L'image devient un peu plus stable (×0,98 à ×0,99).
- **Le classement s'inverse.** Sur le flux seul, le réglage léger gagne et les forts perdent, jusqu'à −1,2 dB sur le
  jeu. Après `mwsr`, c'est le plus fort qui gagne le plus, sans doute parce qu'il efface aussi le bruit que
  l'affinage ferait ressortir. Mais il abîme le détail fin à haut débit : −0,14 dB sur l'explosion à 10 Mbit/s.
- **Le plafond d'une force adaptée à chaque image**, le meilleur réglage image par image, ne donne que +0,10 dB en
  720p à 4 Mbit/s.
- **À l'œil**, la différence est à peine visible. Ce que le débit fait perdre après `mwsr`, c'est surtout du
  détail, et dans le ciel les grandes marches entre blocs restent visibles même avec le réglage le plus fort.

**La porte**, fixée avant la campagne : +0,1 dB ou −0,002 de FLIP à bas débit, en 720p et en 1080p chacun ; rien sous
−0,05 dB à haut débit ; pas d'instabilité en plus. Le seuil 12 borné à 4 passe en 1080p (+0,13 dB) mais pas en 720p
(+0,09) ; les autres ne passent nulle part. Les images fixes, qui devaient vérifier qu'on n'abîme rien, n'ont pas
été lancées, faute de candidat.

P8 se ferme donc à P8.0. Il n'y aura pas de passe de déblocage dans le produit, et `mwsr` reste tel quel. L'aide de
l'hôte pour le déblocage (découpage et QP des blocs) ne vaut pas son coût : même une force parfaite ne donne que
+0,10 dB après `mwsr`. Le filtre coûterait environ 500 multiplications par pixel du flux sur 16 grilles, la moitié
en damier ; il n'a pas été chronométré sur GPU. La page de comparaison faite pour Bruno montre recadrages, cartes
d'erreur, courbes par image et tableaux.

### 14.7 Les versions 2 et 3, dans le menu de débogage (25/10/2026)

Bruno garde `mwsr` tel quel pour la production : c'est la version 1. Il a demandé les deux autres dans la liste
déroulante, pour les juger lui-même. Elles n'existent que dans les builds de débogage, sur WebGPU ; « auto » et les
builds de production ne les prennent jamais.

- **MoonlightWeb 2** : le filtre de P8.0 avant le noyau, à force fixe : seuil 8, changement de luma borné à 2. Des
  réglages mesurés au labo, c'est le seul qui ne perd dans aucun des huit cas (+0,03 à +0,16 dB). Le seuil 12 borné
  à 4 gagne un peu plus en moyenne (+0,10 contre +0,08), mais perd 0,14 dB sur l'explosion en 720p à 10 Mbit/s.
- **MoonlightWeb 3** : la même passe, avec une force réglée sur le QP de chaque image. Le seuil vaut 0,4 fois le pas
  de quantification 2^((QP − 4)/6), plafonné à 12. Le changement de luma est borné au tiers du seuil, 2 au moins.
  - QP 34 et plus : seuil 12 borné à 4 ;
  - QP 28 : environ seuil 6 borné à 2 ;
  - QP 22 : seuil 3 ;
  - sous QP 20 : aucune passe.
  Le facteur vient du balayage hors ligne, où 0,25 à 0,3 était le mieux pour le flux seul. Il est relevé parce
  qu'au labo, après `mwsr`, les réglages plus forts gagnaient. Ce choix n'est pas mesuré tel quel : les QP des
  encodages du labo n'ont été qu'estimés, d'après leur PSNR.
- **Le QP vient de l'hôte sans rien lui demander.** Son encodeur l'écrit dans l'en-tête de chaque tranche :
  26 + `init_qp_minus26` du PPS + `slice_qp_delta`. Le client le lit avant le décodage (`SliceQp.js`, H.264 et
  HEVC). Cela marche donc avec Sunshine, Apollo, Wolf ou l'hôte natif, sans message en plus. En AV1, ou quand un
  en-tête ne se lit pas, la version 3 prend la force de la version 2.
- **Le coût** : un dispatch de calcul pour les 16 grilles, avec des sommes en virgule fixe par atomiques, puis une
  passe de rendu, à la taille du flux. Quand ni l'une ni l'autre n'est choisie, rien n'est construit et aucun
  en-tête n'est lu (`wantsFrameQp`). Le coût n'est pas chronométré sur GPU : une image 720p demande environ 1 s sur
  SwiftShader.
- **Vérifié** (journal J44) :
  - le lecteur de QP contre l'analyseur d'en-têtes de ffmpeg, sur six flux x264 et x265 (P seules, B, prédiction
    pondérée, 4:4:4, deux tranches) ;
  - les passes du produit contre numpy, sur SwiftShader : au plus un niveau d'écart, 0,001 en moyenne ;
  - le vrai `WebGpuRenderer` dans les trois modes : sans erreur, et l'image de QP 18 laissée telle quelle
    (`deblock_check.py`).

**Le verdict de Bruno (25/10/2026, au soir, sur l'instance de développement) :** il garde la version 1 pour l'auto.
« J'adore le résultat, plus rapide et plus propre que FSR1, c'est une réussite. » Les versions 2 et 3 restent dans
le menu de débogage, où elles ne coûtent rien tant qu'on ne les choisit pas. Avant ce verdict, Bruno a demandé si
un historique, un drapeau de scène, la taille des blocs ou une image annexe de contours auraient aidé, et si tout
pouvait se fondre dans le noyau. Aucune de ces combinaisons n'a été mesurée telle quelle ; leurs plafonds le sont
par P7.0 (§14.4) et P8.0 (§14.6). Les options du noyau sont déjà des constantes `override`, retirées du code
compilé quand elles sont coupées. Fondre le déblocage dans la passe du noyau referait le calcul des 16 grilles
pour chaque pixel de sortie : seule une passe de calcul par tuile y gagnerait, une réécriture qui n'aurait de sens
que si la version 2 ou 3 se voyait mieux.

## 15. Deux idées de Bruno du 23/10/2026 : les aplats, et les zones du regard

Contexte : le comparateur côte à côte (§6.1, point 4) sur l'iPhone, 1080p → 1440p : `mwsr` 3,7 ms par
image contre FSR1 8,1, SGSR1 5,1, NIS 68 ; « bien plus rapide », mais « moins sharp que FSR1 et
SGSR1 ». Le réglage de netteté est au journal J31 (netteté 1,5 proposée, curseurs dans le comparateur).
Bruno propose deux pistes, « en tant que paramètres ».

### 15.1 Les grandes surfaces monochromes : le mécanisme existe, c'est le vote

« Sur les grandes surfaces monochromes, pas besoin du même niveau, la différence ne se verra pas. »
C'est exactement le chemin plat du noyau (§3.2) : un quad dont la pente de luma par texel d'entrée est
sous `τ` (0,05) rend son échantillon bilinéaire et s'arrête, une instruction texture et une dizaine
d'opérations ; seuls les quads de contour paient les cinq échantillons. Parts mesurées de pixels sur
le chemin contour (`mwsr:show=1`, J31) : `game` 1,6 à 2 %, l'étalon 28 à 29 %, `text` 26 à 35 % à
`τ 0,05` ; à `τ 0,03` : `game` 2,7 à 8,2 %, l'étalon 37 à 40 %. Sur une image de jeu, 98 % des pixels
prennent donc déjà le chemin le moins cher qui existe (la passe et un échantillon, le plancher du
§3.1) ; un aplat ne coûte rien de plus que le bilinéaire. Le paramètre, c'est `τ`, désormais un curseur
du comparateur ; entre 0,02 et 0,05 il ne change rien aux chiffres (J2, J31), 0,05 reste.

### 15.2 Les zones du regard : un bonus au centre plutôt qu'une économie au bord

L'image de Bruno : un anneau vert au centre (environ 40 % de la largeur, presque toute la hauteur), un
anneau orange autour (environ 70 % de la largeur), le reste « doit rester lisible », sans transition
qui se sente. Deux lectures, et les chiffres tranchent :

- **Économiser au bord.** Comme 98 % des pixels d'une image de jeu sont déjà sur le chemin plat, il n'y
  a rien à économiser au bord : au mieux 2 % de la passe. Sur du texte dense (26 à 40 % de pixels au
  noyau) l'économie serait de quelques pour cent de la passe, et la périphérie, c'est là que vivent
  les ATH : le compteur de munitions, la vie, la mini-carte de l'image de Bruno. Les y rendre moins
  nets se verrait au moment où on les lit.
- **Dépenser plus au centre.** C'est la lecture utile : à coût égal, le centre peut recevoir ce que la
  table refuse à l'image entière, par exemple `τ 0,02` et une netteté plus haute dans l'anneau vert
  (les chiffres de J31 disent que la netteté 1,5 à 1,75 gagne sur le texte et le jeu et coûte sur les
  scènes douces : au centre, où l'œil est, le gain ; au bord, la table). Le temps ne bouge presque pas
  (sur `game`, `τ 0,02` au centre ≈ +3 % de pixels au noyau).

**Les paramètres, si on l'essaie** : une pondération `z(x, y)` de la position du pixel de sortie, 1
dans l'ellipse verte, descendant par `smoothstep` sur la largeur de l'anneau orange jusqu'à 0 dehors ;
`τ' = mix(τ_bord, τ_centre, z)` et `netteté' = mix(n_bord, n_centre, z)` : les deux continues, donc
aucune frontière visible, et six uniformes (centre et rayons de l'ellipse, largeur de l'anneau,
`τ_centre`, `n_centre`), la table du §3.2 restant les valeurs de bord. Coût sur le chemin plat : un
produit scalaire et un `smoothstep`, à mesurer (le plancher est sensible à tout). **Chemin** : d'abord
les curseurs dans le comparateur (une superposition des anneaux, le tiers central déjà mesuré à part par
`metrics.py`), SwiftShader puis le RTX, l'œil de Bruno sur l'iPhone ; une session, effort `medium`,
après son verdict sur la netteté 1,5 (J31). Rien dans le produit avant.

**Fait le 23/10/2026 (journal J34), sur le go de Bruno.** Les paramètres, dans les deux jumeaux :
la constante de pipeline `GAZE` (0 par défaut : le texte d'avant, au pixel près, les empreintes
dorées tiennent) et la table `MWSR_ZONE` (centre 0,5 / 0,5, demi-axes 0,2 / 0,45 de l'image,
anneau 0,75 des demi-axes, τ 0,02 et netteté 2,5 au centre), portée par le bloc d'uniformes (8 →
16 flottants) avec le poids `gaze` ; `z(x, y) = (1 − smoothstep(1, 1 + anneau, d)) · gaze`, d la
distance elliptique normalisée ; `τ' = mix(τ, τ_centre, z)` et `netteté' = mix(n, n_centre, z)`,
continues ; `FLAT_ZONE` met le chemin plat cubique ou quadratique (§3.2, variante) dans la zone
seule, fondu par z ; `SHOW 2` peint z. Coût quand `GAZE` vaut 1 : un `length`, un `smoothstep`,
deux `mix` par pixel ; rien sinon. **Le poids suit le mouvement de l'image**, règle de Bruno du
même jour (« l'appliquer aux images qui bougent, probablement un jeu ; désactiver en bureau pour
être homogène partout ») : `stream/GazeZone.js` lit la taille des images delta que l'hôte encode,
en bits par pixel et par seconde (0,25 → 0, 1,0 → 1 ; un jeu 1080p à 5 Mbit/s fait 2,4, un bureau
immobile 0,05, un curseur qui bouge 0,7), images clés exclues, montée en 1 s et descente en 3 s,
par pas de 1/64 ; les renderers le reçoivent par `noteEncodedFrame()` aux deux chemins de décodage
(worker et fil principal) et le lisent à chaque dessin ; `setMwsrZone()` et `setMwsrGaze()` servent
le laboratoire, qui tient le poids à 1. **Dans le produit, rien d'allumé** : la clé de labo
`mw_mwsr_gaze` (`BrowserDetect.mwsrGazeWanted` : téléphones et tablettes, jamais un client de
bureau — lecture de la consigne « en desktop », à confirmer par Bruno), coupée par défaut jusqu'à
son verdict sur l'iPhone (comparateur : section « Zone du regard », les deux anneaux dessinés sur
les bandes, `gaze=1` dans le lien). Les mesures sont au journal J34.

## 16. Le noyau J (03/11/2026)

Bruno adopte J, le noyau des recherches de la nuit du 02/11 : le §3.2 décrit le noyau d'avant, l'entête de `MwsrShader.js` celui d'aujourd'hui, et le journal J57 les mesures. Trois choix tiennent la suite :

- **La loi suit l'œil.** Les polynômes de `kc`, `kg`, `kr`, `kl` viennent du labo (LPIPS, puis VMAF NEG) ; `kg` et `kl` sont multipliés par une force, 1 à ×1,33, 1,35 à ×2, droite en ln(ratio) entre les deux, tenue au-delà. Bruno l'a voulue plus nette que le labo.
- **HDR sans marge.** `kl` 0 sur une entrée qui passe 1,0, sinon la borne fabrique des hautes lumières.
- **Un seul noyau.** Sur tous les appareils, sans chemin Y seul (V1, V2) : `mwsr-yuv.js` casse donc bruyamment sur ces deux variantes, leurs résultats restent au journal.
- **La réduction devient nette (journal J58).** Bruno retient la Mixte à l'œil, 11 images sur 11 : de ×0,5 à 1:1, là où le vote voit du détail, une croix de 4 lectures donne le détail et la borne, plus doux sur un bord franc (`MWSR_DOWN_LAW`). La référence Lanczos-3 du labo favorise les filtres proches d'elle : le LPIPS baisse aux jeux à ×0,67 et au texte à ×0,5, et c'est l'œil qui a tranché. Rien sous ×0,5, rien en HDR.
