# Upscaler `mwsr` — B : entrées YUV (NV12, P010), plans Y et UV, affûtage

Demandes de Bruno du 29/10/2026, relayées par la session de gestion des plans, et son go du même jour
(« continue avec les fix du comparateur et enchaîne sur B »). Règles : un shader neuf passe d'abord sur
SwiftShader et WARP ; **aucun banc sur un GPU réel avant le feu vert de la gestion des plans** ; jamais
l'encodeur et le décodeur sur le même GPU ; rien d'activé dans le produit sans le oui de Bruno.
Plan d'ensemble : `upscaler-mwsr.md`. Labo : `scripts/bench/scaler-lab/`.

## 1. Les demandes

1. **Entrées NV12 et 10 bits (P010)**, sur le modèle de `vt_metal.mm` (moonlight-qt de Bruno) : le plan
   Y en R8 ou R16, le plan UV en RG8 ou RG16, Y agrandi par MetalFX, UV par un bilinéaire (MPS), puis
   la conversion YUV → RGB au dessin final. Mots de Bruno : « C'est peut-être encore plus rapide si on
   upscale sur UV en bilinear et Y en MWSR. »
2. **Le shader travaille au choix sur Y et UV, sur Y seul ou sur UV seul.** « Le but n'est pas de rajouter
   des couches au shader actuel, mais que son pipeline soit optimisé pour supporter ces formats
   directement sans convertir inutilement. »
3. **Un banc dans MoonlightWeb** (pas sur le comparateur), avec la technique du navigateur, en 4:2:0 et
   en 4:4:4, sur les variantes du §3.
4. **Une sortie en RGB (sRGB) ou en YUV** selon un paramètre, RGB par défaut ; expliquer le sRGB (§4).
5. **L'affûtage** : « MWSR semble travailler un peu trop sur le sharpening, ce qui semble lui faire perdre
   quelques points en fidélité. » Des variantes à ×1,33 et ×2, en RGB, pour la meilleure fidélité (§5).

## 2. Ce que le navigateur donne (§2.7 du plan, et le renderer)

- **Chrome ne donne jamais les plans.** Une NV12 matérielle SDR est importée sans copie, mais seulement
  comme `texture_external` : chaque lecture va chercher Y et UV et applique la matrice. On ne peut lire
  ni le plan Y seul ni le plan UV seul.
- **Le 10 bits n'arrive pas en 10 bits.** P010, I420 logiciel, 4:2:2 et 4:4:4 passent par une copie RGBA
  8 bits que Chrome fait avant notre shader. Safari lie deux textures Metal sans copie, mais l'API WebGPU
  les cache de la même façon.
- **Les plans bruts ne s'obtiennent que par `VideoFrame.copyTo`**, une relecture vers la mémoire du
  processeur. Le chemin HDR du renderer le fait déjà, pour les décodeurs logiciels 10 bits, puis sa
  passe YUV recompose le RGB.
- **Donc, dans le navigateur, la passe 0 d'aujourd'hui est déjà la conversion minimale.** Elle lit la
  texture externe une fois par texel et écrit du RGBA 8 bits. Sans elle, MWSR lirait la texture externe
  à chacune de ses 5 à 10 prises : deux plans et une matrice par prise. P0 et P2 l'ont mesuré perdant
  sur tous les GPU.
- **Ce qui reste à gagner dans le navigateur :**
  - une passe 0 qui écrit Y (R8) et UV (RG8 en demi-résolution) au lieu du RGBA : 1,5 octet par pixel
    au lieu de 4 ;
  - un MWSR qui calcule sur Y seul : un canal au lieu de trois dans le chemin des bords ;
  - en HDR, la passe YUV de la relecture fondue dans MWSR, ce qui évite une texture `rgba16float`
    pleine résolution (8 octets par pixel, écrits puis relus).
- **Le gain complet, les plans lus tels quels et aucune conversion, n'existe que dans un renderer
  natif.** Par exemple `vt_metal.mm` ou un renderer D3D11 sur NV12 et P010 (vues R8/R8G8 et
  R16/R16G16). C'est le dossier `mwsr/` (HLSL, GLSL, C++) qui le porte. Son exemple D3D11 se teste
  sur WARP.

## 3. Les variantes

Un seul module (`MwsrShader.js`), d'où sortent WGSL, GLSL et HLSL comme aujourd'hui. Trois paramètres
de construction, pas une couche de plus :
- `INPUT` : `rgb` (aujourd'hui), `yuv` (deux textures, Y et UV : NV12, P010, NV24) ;
- `PLANES` : `yuv` (MWSR sur Y et sur UV), `y` (MWSR sur Y, UV au bilinéaire), `uv` (MWSR sur UV, Y au
  bilinéaire) ;
- `OUTPUT` : `rgb` (défaut), `yuv`.

| # | Variante | Demande de Bruno | Passes |
|---|---|---|---|
| V0 | Conversion RGB par le navigateur (passe 0 → RGBA), puis MWSR RGB : **la référence**, le produit d'aujourd'hui | « conversion RGB matérielle, puis MWSR » | 2 |
| V1 | Y en MWSR et UV au bilinéaire du sampler, chacun dans sa texture de sortie, puis composés en RGB : le modèle de `vt_metal.mm` | « UV bilinéaire matériel et Y en MWSR en parallèle » | 3 |
| V2 | Y en MWSR et UV au bilinéaire dans le même shader, sortie RGB | « UV bilinéaire et Y en MWSR dans le shader » | 1 (+ passe 0) |
| V3 | Y et UV en MWSR, les poids tirés de Y et appliqués à UV, sortie RGB | « UV et Y en MWSR » | 1 (+ passe 0) |
| V4 | Y et UV convertis en RGB dans MWSR, à chaque prise | « Y et UV convertis en RGB dans MWSR » | 1 (+ passe 0) |
| V5 | MWSR lit la texture externe, sans passe 0 (perdant en P0 et P2 : le témoin) | — | 1 |
| V6 | La chroma 4:2:0 refaite d'après les bords de Y (« chroma from luma »), même à ×1 | autre idée | 1 (+ passe 0) |
| V7 | HDR : la passe YUV de la relecture fondue dans MWSR (plans 10 bits lus par MWSR) | autre idée | 1 |

- En RGB, MWSR applique à R, G et B des poids tirés de la luma. Le même mélange appliqué à Y, U et V
  donne la même image, à la borne des texels près. Mais cette borne compte (§8) : bornés en YUV, les
  canaux laissent une frange de couleur dépasser en RGB.
- V2 laisse la couleur au bilinéaire ; c'est ce que fait `vt_metal.mm`, et la fidélité dira ce qu'on
  y perd.
- La passe 0 de V1 à V4 et de V6 : sur le navigateur, une passe « Y + UV » depuis la texture externe.
  Dans un renderer natif, rien : les plans du décodeur.
- 4:4:4 : UV en pleine résolution (NV24). V6 n'y sert à rien.
- 10 bits : plans en `r16float`/`rg16float` (filtrables partout), ou `r16unorm` là où l'adaptateur a
  la fonctionnalité.
- Le placement de la chroma : celui de Chrome (centré) partout, pour comparer à l'identique.

## 4. Le sRGB, en clair

- L'écran et le canvas attendent du RGB **encodé** : la courbe sRGB donne plus de valeurs aux sombres,
  là où l'œil distingue le mieux. Une vidéo décodée, une fois la matrice YUV → RGB appliquée, donne
  déjà du RGB encodé par une courbe presque identique (BT.709). **La sortie RGB n'a donc besoin
  d'aucune conversion sRGB** : la matrice suffit.
- Il faut repasser en **lumière linéaire** (décoder le sRGB, calculer, réencoder) seulement pour
  **moyenner** des pixels : une réduction. Une moyenne de valeurs encodées assombrit les détails
  clairs, par exemple un texte blanc fin sur noir. MWSR le fait déjà, gratuitement, par les vues
  `-srgb` des textures (P3).
- Pour **agrandir**, travailler sur les valeurs encodées est la règle : FSR1 en mode perceptuel,
  MetalFX « Perceptual » dans `vt_metal.mm`. C'est aussi moins cher.
- **Verdict** : sortie RGB par défaut, sans étape sRGB de plus. La sortie YUV ne sert qu'à un renderer
  natif qui convertit lui-même au dessin final (le `ps_draw_biplanar` de `vt_metal.mm`), jamais au
  canvas du navigateur.

## 5. L'affûtage (×1,33 et ×2, RGB)

- Aujourd'hui, la netteté vaut 1,85 à ×1,33 (l'œil de Bruno sur l'iPhone, J37), et la loi garde ce
  poids jusqu'à ×2 (J40). Le chemin plat ajoute un Catmull-Rom (`FLAT_C` 0,5), qui affûte aussi un peu.
- Les chiffres du README ne disent pas « trop » d'avance :
  - à ×2, NIS et SGSR1, plus affûtés, battent MWSR sur le texte (87,6 contre 86,1) ;
  - à ×1,33, FSR1 le bat sur le jeu (87,5 contre 85,9) ;
  - mais MWSR est premier sur le texte à ×1,33 (96,0).
  - Le balayage teste donc dans les deux sens.
- **Le balayage**, sur le texte et sur 0 A.D. du comparateur, à 1080p → 1440p et à 720p → 1440p :
  - la netteté de 0 à 2,5 ;
  - `W_ALONG` (le lissage le long du bord) 0, 0,15 et 0,3 ;
  - `FLAT_C` 0,33, 0,5 et 0,75.
  - Mesures : SSIM de la luma et des couleurs, FLIP, dépassement.
- **SwiftShader suffit** : la fidélité ne dépend pas du GPU, à 1/255 près. Ce banc peut donc tourner
  avant le feu vert.
- Résultat : une courbe par image et par échelle, et une recommandation. L'œil de Bruno tranche,
  puisque c'est lui qui a fixé 1,85.

## 6. Le banc

- **Où** : `scripts/bench/scaler-lab/`, la page qui fabrique de vraies NV12 de décodeur par WebCodecs.
  On y ajoute des cellules `mwsr-yuv:…`, avec les mêmes options que `mwsr:…`.
- **Les plans** :
  - pour le noyau seul, Y et UV fabriqués depuis l'image de test, en BT.709 à plage limitée, 8 et 10
    bits, 4:2:0 et 4:4:4 ;
  - pour la chaîne, la passe « Y + UV » depuis la texture externe, et `copyTo` pour le 10 bits.
- **L'encodeur** : le clip est encodé d'avance, par x264 en logiciel, et seulement décodé dans la page.
  Encodeur et décodeur ne sont donc jamais sur le même GPU.
- **Étape 1, maintenant (SwiftShader, puis WARP pour le HLSL)** :
  - chaque variante compile et dessine ;
  - V3 rend V0 à 1/255 près ;
  - la fidélité de chaque variante, sur les images du comparateur et de l'étalon ;
  - l'exemple D3D11 lit une NV12 sur WARP.
- **Étape 2, au feu vert** :
  - le temps GPU de chaque variante : horodatages par lots, comme en P0, chaîne et noyau seul ;
  - à 1080p → 1440p, 720p → 1440p et 1440p → 1440p (V6) ;
  - sur le RTX 5060 Ti, l'iGPU du Ryzen 9 9900X, puis les téléphones aux créneaux de Bruno.
- **Le verdict** : une variante ne passe que si elle garde la fidélité de V0 (ou la dépasse) et coûte
  moins. L'intégrer au produit attend le oui de Bruno.

## 7. L'ordre

1. Le balayage de l'affûtage sur SwiftShader, et ses courbes à Bruno.
2. Les variantes dans le labo d'abord (`scripts/bench/scaler-lab/mwsr-yuv.js`), dérivées du texte du
   produit, et leur validation sur SwiftShader. Elles n'entrent dans `MwsrShader.js` (`INPUT`,
   `PLANES`, `OUTPUT`) puis dans le HLSL, avec l'exemple D3D11 sur WARP, que si les temps GPU leur
   donnent raison.
3. Au feu vert : les temps GPU.
4. Le verdict à Bruno, puis l'intégration s'il dit oui.

## 8. Étape 1 sur SwiftShader (29/10/2026)

**L'affûtage.** Échelle du labo (`scaler_lab.py --swiftshader --quality`), sur le texte et sur 0 A.D.
du comparateur et sur l'étalon de Bruno (`C:\Test\00_Background.png`). Fidélité = SSIM de la luma ×100,
FLIP plus bas = plus proche.

| Image | Échelle | Netteté | SSIM luma | FLIP | PSNR |
|---|---|---|---|---|---|
| Texte | ×1,33 | 1,85 (aujourd'hui) | 95,96 | 0,049 | 22,23 |
| Texte | ×1,33 | 1,25 | 95,35 | 0,045 | 22,10 |
| 0 A.D. | ×1,33 | 1,85 | 85,92 | 0,048 | 27,73 |
| 0 A.D. | ×1,33 | 1,25 | 85,97 | 0,037 | 28,21 |
| Étalon | ×1,33 | 1,85 | 93,64 | 0,042 | 24,21 |
| Étalon | ×1,33 | 1,25 | 93,91 | 0,034 | 24,35 |
| Texte | ×2 | 1,23 (la loi) | 86,12 | 0,068 | 17,81 |
| Texte | ×2 | 0,83 (la loi depuis 1,25) | 85,16 | 0,066 | 17,64 |
| 0 A.D. | ×2 | 1,23 | 66,24 | 0,065 | 24,52 |
| 0 A.D. | ×2 | 0,83 | 65,64 | 0,059 | 24,66 |
| Étalon | ×2 | 1,23 | 86,10 | 0,056 | 20,57 |
| Étalon | ×2 | 0,83 | 86,29 | 0,048 | 20,74 |

- Bruno a raison pour les images naturelles. Sur l'étalon et sur 0 A.D., moins d'affûtage rapproche
  du natif sur presque toutes les mesures : le FLIP baisse de 9 à 23 %, la PSNR monte.
- Sur le texte, c'est l'inverse : la SSIM de la luma veut plus d'affûtage (son maximum est vers 2,2 à
  ×1,33), alors que le FLIP en veut moins.
- Le balayage complet, de 0 à 3, situe les optimums entre 1,0 et 1,5 à ×1,33, et entre 0,5 et 1,0
  à ×2 sur les images naturelles.
- La netteté 1,25, avec la loi d'aujourd'hui qui donne 0,83 à ×2, tombe dans ces deux zones à la
  fois.
- Le lissage le long du bord (`W_ALONG` 0,22 ou 0,3) et le cubique des zones plates (`FLAT_C` 0,33 ou
  0,75) ne changent presque rien. On les garde.

**La courbe, après la question de Bruno** (même banc, la netteté de 0 à 3 par pas de 0,25, plus le
code coloré du labo ; les courbes et les planches restent hors dépôt). Bruno juge d'abord sur le
texte, parce qu'on juge la netteté en lisant. Il trouve aussi 0,83 un peu flou sur le jeu à ×2, malgré
la fidélité. La « netteté mesurée » est la moyenne du gradient de la luma, en % de celle du natif.

| Image | Meilleure SSIM luma à ×1,33 | à ×2 |
|---|---|---|
| Texte | 2,25 (96,13) | 2,5 (86,78) |
| Code coloré | 0,5 (98,23) | 0,25 (95,34) |
| 0 A.D. | 1,5 (86,12) | 1,25 (66,25) |
| Étalon | 1,25 (93,91) | 0,83 (86,29) |

- Le FLIP veut toujours moins d'affûtage, entre 0,5 et 1,0. C'est lui qui tirait vers 1,25, et l'œil
  trouve ce réglage trop doux. La 1re proposition (1,25, soit 0,83 à ×2) est retirée.
- **À ×1,33, on garde 1,85.** Il reste à 0,3 point au plus du meilleur de chaque image (texte −0,17,
  0 A.D. −0,20, étalon −0,27, code −0,20). Le meilleur au pire cas serait 1,75 (−0,24 au plus) : un
  écart invisible.
- **À ×2, la loi donne 1,23.** 0 A.D. y est à son meilleur, mais le texte perd 0,66 point.
  - Compromis, 1,5 : texte +0,35, 0 A.D. −0,08, étalon −0,41, code −0,23. Les contours du jeu
    deviennent aussi nets que ceux de FSR1 (netteté mesurée 48,6 % contre 48,8 %).
  - Texte d'abord, 2,0 : texte +0,60, mais 0 A.D. −0,84, étalon −1,18, code −0,57.
- La loi du compromis : la netteté descend de 1,85 à ×1,33 jusqu'à 1,5 au lieu de 1,23, puis y
  reste. C'est un plancher dans `mwsrSharpness`. Il faut le vérifier à ×1,5 avant d'y toucher, et
  rien n'entre au produit sans le oui de Bruno.

**Le choix de Bruno (29/10, « netteté à ×2 : => A »)** : 1,85 gardé à ×1,33, 1,5 à ×2. La forme de
la loi vient de la vérification à ×1,5 et à ×1,75, sur les mêmes images :

| Échelle | Sortie | Texte | Code coloré | 0 A.D. | Étalon |
|---|---|---|---|---|---|
| ×1,5 | 1080p | 1,23 (92,30) | ≤ 1,0 (95,04) | 1,0 (83,48) | ≤ 1,0 (92,31) |
| ×1,5 | 1350p | ≤ 1,0 (96,15) | ≤ 1,0 (97,34) | ≤ 1,0 (87,86) | ≤ 1,0 (94,06) |
| ×1,75 | 1260p | 1,23 (91,67) | ≤ 1,0 (95,10) | ≤ 1,0 (80,30) | ≤ 1,0 (91,06) |

(meilleure netteté parmi 1,0, 1,23, 1,375, 1,5, 1,75 et 2,0, et sa SSIM de la luma)

- Un plancher à 1,5 dès ×1,41 aurait coûté de 0,3 à 0,4 point au jeu à ×1,5, sans rien rendre au
  texte. Une montée depuis ×1,5 coûterait encore jusqu'à 0,26 point à ×1,75.
- Le gain du texte est propre à ×2 (720p → 1440p, 1080p → 2160p). D'où `MWSR_SHARP_X2` : la loi
  d'avant jusqu'à ×1,75 (1,23 dès ×1,5), puis une rampe jusqu'à 1,5 à ×2, tenue au-delà.
- Rien ne change à ×1,33, ×1,5 ni ×1,75. Seules les empreintes dorées des trois cas ×2 changent, et la
  conformité des deux jumeaux tient sur SwiftShader.
- Le README et le comparateur passent le texte de MWSR à ×2 de 86,1 à 86,5. Le jeu reste à 66,2, et
  son FLIP passe de 0,065 à 0,071.

**Correction du 29/10 au soir.** Ce tableau a un biais : à ×1,5 et ×1,75, les sorties sont sous 1440p
(1080p, 1350p, 1260p). Les images du texte et de 0 A.D. n'existent qu'en 1440p, et la référence était
donc une réduction, plus douce qu'une vraie image native, qui demande moins de netteté. Refait avec les
contenus que le labo dessine à la taille de sortie (§11, `b-native/`, hors dépôt) :
- le texte veut 2,5 à ×1,33, 1,25-1,5 à ×1,5, 1,75 à ×1,75 (+0,22 sur la loi) et 2,5 à ×2 ;
- le code coloré veut 0 à 0,5 partout (la loi lui coûte 0,25 à ×1,5 et 0,74 à ×1,75) ;
- le jeu dessiné ne bouge presque pas (±0,05).

La loi commitée reste un compromis défendable : monter à ×1,75 rendrait 0,22 au texte et prendrait
0,5 au code. Mais « toutes les images veulent ≤ 1,23 à ×1,5 et ×1,75 » était faux pour le texte à
×1,75. C'est justement le conflit que la netteté adaptative résout.

**Les variantes YUV** (texte, 1080p → 1440p, plans BT.709 à plage limitée) :

| Cellule | SSIM | SSIM luma | FLIP |
|---|---|---|---|
| `mwsr` (RGB complet, 4:4:4) | 95,74 | 95,96 | 0,049 |
| V0 4:4:4 (RGB refait depuis les plans) | 95,74 | 95,96 | 0,049 |
| V2 4:4:4 (UV au bilinéaire) | 95,18 | 95,92 | 0,051 |
| V3 4:4:4 (Y, Cb, Cr en MWSR) | 95,54 | 95,93 | 0,051 |
| V0 4:2:0 | 94,24 | 95,89 | 0,056 |
| V1 = V2 4:2:0 | 94,14 | 95,89 | 0,059 |
| V3 4:2:0 | 94,27 | 95,89 | 0,061 |
| V4 4:2:0 | 94,40 | 95,92 | 0,056 |

- Toutes les cellules compilent et dessinent sur SwiftShader, en 8 et en 10 bits. Les cellules
  `split+yuv` tournent aussi sur de vraies images NV12 de décodeur, et rendent `blit+mwsr` à 1 à 2
  niveaux près en moyenne.
- En 4:4:4, l'aller-retour par les plans ne change rien : V0 rend `mwsr` aux mesures près.
- **V3 n'est pas V0.** Borné en YUV, le texte rendu en sous-pixels garde des franges de couleur plus
  vives, qui dépassent en RGB : 2 % de dépassement contre 0 %.
- **V2 (et V1) perd un peu de couleur** sur le texte : 0,1 point de SSIM en 4:2:0, 0,6 en 4:4:4. La
  luma ne bouge pas.
- **V4 est la plus fidèle** en 4:2:0, parce qu'elle lit la chroma au bilinéaire à chaque prise, au
  lieu d'un bilinéaire de bilinéaire.
- Les temps de SwiftShader sont ceux du processeur : ils ne disent rien d'un GPU. Le classement en
  vitesse attend le feu vert.

## 9. Étape 2 : les temps GPU (29/10/2026, au feu vert de 59)

**Conditions.**
- **Les GPU** : la RTX 5060 Ti du bus 01 (pas celle de l'écran principal) et le GPU intégré du Ryzen 9
  9900X : RDNA2, 2 unités de calcul, id 0x13C0. C'est lui que les documents précédents appellent
  « 780M ». Le README de `mwsr/` et /mwsr/ le nomment juste depuis le 29/10 (oui de Bruno).
- **Le Chrome du labo** : épinglé par LUID et sondé avant la mesure. `nvidia-smi` montre que la RTX
  mesurée seule a travaillé.
- **Le clip** : encodé en logiciel (`enc=sw`), puis décodé en NV12 sur le GPU mesuré. L'encodeur et le
  décodeur ne sont donc jamais sur le même GPU.
- **Les mesures** : en f32, au p50 des horodatages, sur le jeu et le texte du labo, à 1080p → 1440p et
  à 720p → 1440p.
- **Deux temps par variante, comparés à V0** :
  - la chaîne : de l'image du décodeur à la sortie, avec la passe 0 ;
  - le noyau seul : sur des plans faits une fois.

| Variante | RTX, chaîne | RTX, noyau | iGPU, chaîne | iGPU, noyau |
|---|---|---|---|---|
| V0 (référence) | 0,079 à 0,112 ms | 0,071 à 0,089 ms | 2,41 à 3,09 ms | 2,30 à 2,84 ms |
| V1 (modèle `vt_metal.mm`) | +14 à +23 % | +13 à +23 % | −1 à −8 % | −7 à −11 % |
| **V2 (Y en MWSR, UV au bilinéaire)** | **0 à −6 %** | **−3 à −10 %** | **−11 à −16 %** | **−19 à −20 %** |
| V3 (Y, Cb, Cr en MWSR) | +46 à +58 % | +55 à +63 % | +32 à +73 % | +31 à +73 % |
| V4 (RGB à chaque prise) | +61 à +80 % | +66 à +85 % | +58 à +106 % | +58 à +107 % |
| V5 (texture externe directe) | +37 à +69 % | — | +35 à +52 % | — |

- **V2 est la seule qui gagne.** C'est l'idée de Bruno : Y en MWSR, UV au bilinéaire, dans un seul
  shader. Elle gagne de 11 à 16 % sur le GPU intégré, de 0 à 6 % sur la RTX.
  - Son prix : un peu de couleur, 0,1 point de SSIM RGB en 4:2:0 et 0,6 en 4:4:4 (§8). La luma ne
    bouge pas.
  - Le gain est moindre sur la chaîne que sur le noyau seul : dans le navigateur, la passe 0 en deux
    plans coûte un peu plus que la copie RGBA (iGPU 0,41 contre 0,25 ms à 1080p).
- **10 bits et 4:4:4 coûtent autant que 4:2:0 en 8 bits**, à 2 % près, pour chaque variante.
- **V1 perd sur la RTX et gagne un peu sur l'iGPU.** V3 et V4 sont écartées.
- **V5 confirme P0 et P2** : lire la texture externe à chaque prise coûte plus que la passe 0.
- **V6** (la chroma d'après Y) **et V7** (le fondu HDR) ne sont pas codées : pas de temps.
- **Reste à mesurer**, si Bruno veut V2 dans le produit :
  - ~~le f16, que le produit prend sur les GPU intégrés AMD~~ : fait le 29/10, ci-dessous ;
  - un téléphone : le realme Note 60, le 29/10 au soir (décision de Bruno) ;
  - le natif (D3D11 sur WARP, puis un GPU), où V2 lit les plans du décodeur sans aucune passe 0.

**Le f16 sur l'iGPU (29/10, 18:10-18:12, créneau de 59, l'iGPU seul).** SwiftShader n'a pas
`shader-f16` et Chrome n'expose pas WARP : la V2 en f16 a compilé pour la première fois là.
- Elle rend comme la f32 : 0,0002 de SSIM au plus (×2, texte et 0 A.D. du comparateur).
- Temps en f16, au p50, sur le jeu et le texte du labo (clip encodé en logiciel, décodé sur l'iGPU) :

| Cas | Chaîne du produit (passe 0 + MWSR) | Chaîne V2 | Écart | Noyau V0 | Noyau V2 | Écart |
|---|---|---|---|---|---|---|
| ×2, jeu | 2,40 ms | 2,02 ms | −16 % | 2,30 ms | 1,85 ms | −20 % |
| ×2, texte | 2,81 ms | 2,44 ms | −13 % | 2,70 ms | 2,25 ms | −17 % |
| ×1,33, jeu | 2,67 ms | 2,39 ms | −10 % | 2,66 ms | 2,20 ms | −17 % |
| ×1,33, texte | 3,04 ms | 2,75 ms | −10 % | 2,79 ms | 2,34 ms | −16 % |

- **Le f16 ne gagne presque plus rien sur cet iGPU** : V0 −0 à −2 % contre la f32. V2 est même un
  peu plus lente en f16 (+0,3 à +1,9 %) : elle ne calcule qu'une valeur, la luma, alors que le f16 gagne
  surtout en traitant trois couleurs à la fois. En P2.3, le f16 valait −9 à −10 % ; le noyau a changé
  depuis. Un Mali calcule souvent deux fois plus vite en f16 : le realme peut dire l'inverse.
- Le labo lit le clip en VP9 logiciel (`codec=vp9`) si un téléphone n'a pas de H.264 logiciel.
- **Le vrai Radeon 780M, plus tard** (oui de Bruno, 29/10) : les temps du README, et ceux de V2, sur
  l'UM790Pro. Ce sera quand elle sera libre ; elle est sous Ubuntu pour les tests du clavier.

## 10. Netteté adaptative : l'étude (29/10/2026, demande de Bruno)

Bruno : une netteté choisie d'après l'image (globale ou par zones), à bas coût, lissée sur 200 à
400 ms. Étude seule, depuis les 104 images du balayage du §8 (`oracle.py` et `predict*.py`, hors
dépôt).

**a. Ce qu'on peut gagner, et si une mesure simple le prédit.**
- **L'oracle global** : la meilleure netteté unique pour chaque image, comparée à la loi.
  - À ×1,33 : +0,18 à +0,28 point de SSIM de la luma.
  - À ×2 : +0,09 (0 A.D.) à +0,98 (code coloré).
- **L'oracle par zones** (tuiles de 160 px) n'ajoute presque rien sur une image homogène : +0,01 à
  +0,27. Sur une image mixte (moitié texte, moitié jeu), deux zones gagnent de 0,2 à 0,5 point sur une
  valeur unique.
- **Une mesure simple ne prédit pas le bon réglage.** Mesures testées sur Y : gradient moyen, part de
  bords, part d'aplats, amplitude des bords, fond. Pour chacune, une règle `a + b × mesure` est réglée
  sur trois images, puis jugée sur la quatrième.
  - Aucune ne tient : le texte écarté perd de 0,2 à 4,6 points.
  - Le code coloré, du texte lui aussi, veut l'inverse du texte noir sur blanc (0,25 à 0,5 au lieu de
    2,25 à 2,5).
- **La fidélité n'est pas l'œil.** L'optimum de la SSIM est plus doux que ce que Bruno juge net (0,83
  « un poil trop flou »). Un réglage automatique sur la fidélité adoucirait le jeu.

**b. Global ou par tuiles.**
- Par tuiles : une grille de 16 × 9 valeurs dans une petite texture. La passe `up` la lit au
  bilinéaire, ce qui interpole entre tuiles : pas de couture.
- Le coût est le même qu'en global. Seul le risque de mauvaise prédiction change.

**c. Le lissage dans le temps.**
- Un filtre du premier ordre, comme un condensateur : `s ← s + α (cible − s)`, avec
  `α = 1 − exp(−Δt / τ)` et τ = 0,3 s.
- Δt vient de l'horodatage des images : le lissage est le même à 30, 60 ou 120 i/s.
- Changement de scène : si la luma moyenne ou plus de la moitié des tuiles sautent d'un coup, α = 1.

**d. Le coût.**
- Une passe qui lit Y au quart de la résolution vers la grille 16 × 9, et une passe minuscule pour le
  lissage (deux textures en alternance). Tout reste sur le GPU, sans relecture par le processeur.
  - En WebGPU, la passe `up` de l'image précédente pourrait aussi tenir les statistiques, par des
    atomiques : une image de retard, sans importance avec 300 ms de lissage.
- Estimation : quelques % du temps de MWSR, de 2 à 5 %. La séparation UV, à demi-résolution, coûte
  déjà 0,005 ms sur la RTX et 0,14 ms sur l'iGPU à 1080p ; la statistique lit quatre fois moins.

**e. La validation, si on la fait.**
- Une cellule du labo qui lit une carte de netteté par tuiles. SSIM contre la loi fixe sur le texte,
  le jeu, l'étalon et des images mixtes.
- Puis les séquences de panoramique de P7, à 30, 60 et 120 i/s, pour le pompage, et des coupes de scène.
- Enfin l'œil de Bruno.

**Recommandation : pas maintenant.**
- Le gain mesurable est petit : 0,2 à 0,3 point le plus souvent, un point au mieux.
- Aucune mesure bon marché ne le trouve de façon fiable, et la fidélité tire vers le flou que l'œil
  refuse.
- Si Bruno veut l'essayer à l'œil : la version globale lissée, avec deux réglages choisis à l'œil
  (texte ou jeu), pour 2 à 5 % de temps GPU. Elle attend un détecteur de texte plus solide que ceux
  testés ici.

## 11. Netteté adaptative : l'analyse à part et l'auto-test (29/10/2026, idée de Bruno)

Bruno, après le §10 : l'analyse n'a pas à être dans le rendu. Elle tourne à part, à intervalle
régulier, et produit une grille que MWSR lit comme un paramètre.
- C'est l'architecture du §10 b-d (grille 16 × 9, lue au bilinéaire, lissée sur 0,3 s), en moins
  souvent.
- Le navigateur n'a qu'une file GPU, sans priorité basse : « à part » veut dire une image sur N, ou le
  travail réparti sur plusieurs images. Un renderer natif peut avoir une file de calcul à basse
  priorité.
- On moyenne les résultats de l'analyse, pas les images : le lissage du §10 c le fait déjà.

**Ce que ça permet : un auto-test à rebours.** L'analyse peut coûter plus cher, donc mesurer au lieu
de deviner.
- Le client n'a pas l'image native, mais il a l'image reçue L. Il la réduit du même facteur, la
  remonte avec MWSR à plusieurs nettetés, et garde celle qui retrouve le mieux L, par tuile.
- Hypothèse : une image se ressemble d'une échelle à l'autre.
- Banc : les images reçues du balayage du §8 (×2 : 1280 × 720 ; ×1,33 : 1920 × 1080), 13 nettetés, sur
  SwiftShader. L'oracle du §10 juge le choix (`b-self/judge.py`, hors dépôt).
- Images mixtes : la moitié gauche d'une image et la moitié droite d'une autre (24 paires).

| Lecture | Images seules (8 cas) | Images mixtes (24 cas) |
|---|---|---|
| Oracle global, contre la loi | +0,09 à +0,98 | 0 à +0,65 |
| Oracle par tuiles | +0,18 à +0,99 | +0,20 à +0,97 |
| Auto-test global | −0,06 à +0,39 | −0,17 à +0,31 |
| Auto-test par tuiles | −0,07 à +0,56 | +0,03 à +0,49 |
| Auto-test par tuiles, jamais sous la loi | −0,01 à +0,31 | −0,02 à +0,15 |

- **L'auto-test trouve le réglage là où les mesures du §10 échouaient.**
  - À ×2, il choisit exactement l'oracle sur le texte (2,5) et 0 A.D. (1,25) ; l'étalon à 0,25 près.
  - À ×1,33, il choisit exactement l'oracle sur 0 A.D. et l'étalon ; le texte à 0,5 près (1,75 au lieu
    de 2,25).
  - Le code coloré le trompe aux deux échelles (1,5 au lieu de 0,25 à 0,5), sans perte contre la loi.
  - Les mesures simples du §10 faisaient perdre jusqu'à 4,6 points au texte ; l'auto-test, 0,07 au
    pire.
- **Une calibration apprise sur les autres images échoue** (texte −2,5 à −4,9). Le choix brut de
  l'auto-test est le bon.
- **Il vise la fidélité, plus douce que l'œil.** Libre, il descend le jeu à 1,25-1,5 à ×1,33, sous les
  1,85 que Bruno a validés.
- **D'où la variante « jamais sous la loi »** : l'auto-test ne peut que monter la netteté.
  - À ×1,33, elle ne change rien : la loi est déjà au-dessus de ce qu'il demande.
  - À ×2, elle détecte le texte : 2,5 sur le texte (+0,31) ; +0,14 à +0,15 sur les images mixtes avec
    du texte ; rien ailleurs (−0,02 au pire).
  - Où elle sert, refait sur des références vraiment natives (le texte, le code et le jeu que le labo
    dessine à la taille de sortie ; `b-native/`, hors dépôt) :

| Échelle | Loi | Texte : oracle / auto-test | Gain possible | Gain de l'auto-test (jamais sous la loi) |
|---|---|---|---|---|
| ×1,33 | 1,85 | 2,5 / 1,75 | +0,16 | 0 : l'auto-test s'arrête sous la loi |
| ×1,5 | 1,23 | 1,5 / 1,5 | +0,02 | +0,02 |
| ×1,75 | 1,23 | 1,75 / 2,0 | +0,22 | +0,19 |
| ×2 | 1,5 | 2,5 / 2,5 | +0,31 | +0,31 |

  - Le code coloré veut moins que la loi à toutes les échelles, et l'auto-test ne le voit jamais
    (1,25-1,5 au lieu de 0-0,5) : c'est son angle mort. La version « jamais sous la loi » n'y perd rien.
  - Le gain n'existe donc qu'à partir de ×1,75 environ : à ×1,5 il est nul, et à ×1,33 l'auto-test ne
    voit pas ce que le texte voudrait.
- **Le coût, estimé et pas mesuré.** Par analyse à ×2 vers 1440p :
  - une réduction de L ;
  - trois passes MWSR de 640 × 360 vers 1280 × 720 (la loi, +0,5, +1), chacune le quart de la passe
    du rendu ;
  - une statistique par tuile.
  
  Soit environ une passe MWSR de rendu. Une analyse toutes les 15 images coûte donc de l'ordre de 7 %
  du temps de MWSR, réparti sur ces 15 images. Une analyse toutes les 3 images en coûterait environ un
  tiers.
- **Limites.** Quatre images seulement, propres, sans artefacts de codec. Il faut encore :
  - les séquences encodées de P7, à 10 et 20 Mbit/s ;
  - d'autres jeux ;
  - l'œil de Bruno sur le texte à 2,5, par zones.
- **Recommandation :** la variante « jamais sous la loi », par tuiles, à partir de ×1,75, une analyse
  toutes les 15 images environ.
  - Validation d'abord dans le labo : une cellule qui lit une grille, puis P7 et les coupes de scène.
  - Rien dans le produit sans le oui de Bruno.

## 12. La v0.4.2 : la netteté par zone de l'hôte natif et V2 (30/10/2026, décisions de Bruno)

### a. Ce que le banc libre a établi (29-30/10, hors dépôt)

Bruno a demandé, le 29/10 au soir, de refaire l'étude sans loi imposée : « que le benchmark cherche le
meilleur résultat possible ». Le banc est en Python (`b-free/`, hors dépôt). Son modèle numpy du noyau
rend comme SwiftShader : au plus 0,005 % des pixels s'en écartent de plus d'un niveau.
- **Le corpus.** Sortie en 1440p sur 19 références vraiment natives : texte, code coloré, interface, jeux,
  photos, images mixtes. L'entrée est réduite comme le fait chaque hôte :
  - l'hôte natif en Lanczos-2 linéaire (`ScaleFilter.h`) ;
  - Sunshine, Apollo et le repli de l'hôte natif en bilinéaire gamma ;
  - en plus, une version encodée par x264.
  
  Cinq séquences : défilement, panoramique, Call of Duty, coupes, panneau glissé.
- **La loi est un plafond.** Sur images fixes, l'œil de Bruno n'a jamais choisi plus net que la loi. Il
  a choisi moins net sur les jeux, le code coloré et les images mixtes. La netteté adaptative ne fait
  donc que baisser, et la variante « jamais sous la loi » du §11 est abandonnée.
  - Politique retenue : la baisse proposée, réduite de 0,15 ; aucune baisse si elle fait moins de 0,15.
- **La meilleure approche mesurée : l'hôte natif, une valeur par zone.**
  - Une grille de 32 × 18 zones de 80 px en 1440p.
  - Huit statistiques sur chaque pixel de la capture native, avant la réduction.
  - Un arbre de décision de profondeur 4, appris contre la capture.
  
  Résultats, en SSIM-Y × 100 sur la loi, validés en écartant un contenu, puis une famille :
  - images fixes : +0,066 / +0,063, pire −0,03 ;
  - séquences : 102 % du gain de la meilleure valeur par image en mouvement, 95 % après les coupes ;
    défilement −0,010 en moyenne (pire image −0,024).
- **Les autres approches, mesurées puis écartées :**
  - l'hôte natif avec une valeur par image : +0,051 / +0,052, 84 % en mouvement ;
  - le client, une valeur par image : +0,038 / +0,034, 58 % en mouvement, pour un coût de +2 à +6 % sur
    le realme et de +17 à +28 % sur l'iPhone 13 Pro ;
  - le client par zone : il se dégrade quand la grille s'affine ;
  - les règles par pixel ;
  - le lissage gaussien de la grille, utile seulement si le pompage se voyait.
- **La valeur voyage avec son image.** Une image de retard coûte 4 à 8 points à chaque coupe. Elle va
  donc dans l'en-tête de son image, pas dans un quatrième flux, qui ne serait pas en phase. Pas en SEI
  non plus : aucun encodeur natif n'en écrit, et chaque codec a son propre format.
- **En mouvement, l'œil de Bruno ne voit ni le gain ni le pompage.** Planches animées à l'aveugle, 4 puis
  8 extraits : ses choix ont suivi la place A, 7 fois sur 8.
  - Le pompage mesuré : le pire bloc de 10 px change de 10 niveaux sur 255 entre deux images stables,
    contre 3 avec une valeur par image.
  - Le verdict du banc était « garder la loi ». Bruno a choisi la valeur par zone pour l'hôte natif.

### b. Les décisions de Bruno (30/10/2026)

1. **La netteté par zone en v0.4.2, pour l'hôte natif seulement.**
   - Les autres hôtes gardent la loi : Sunshine, Apollo, Wolf, et un hôte natif d'avant la v0.4.2.
   - C'est le même noyau, avec une entrée en plus, facultative : la grille des zones.
2. **V2 en v0.4.2, sur les GPU faibles seulement.**
   - Ces GPU sont reconnus par la mesure.
   - En 4:2:0 seulement : le 4:4:4 garde le chemin actuel.
   - Le choix est automatique, avec un drapeau de debug coupé par défaut.
   - **Puis, le 30/10 au soir, V2 sort de la v0.4.2 et reste au labo** (choix A de Bruno). Elle perd
     sur la 780M (e.3), et Bruno a vu sa perte de couleur (e.1). Le §d reste la conception, pour plus
     tard.
3. **Avant d'activer V2, quatre vérifications** (e) :
   - l'œil de Bruno sur la couleur ;
   - V2 sur l'iPhone ;
   - le vrai Radeon 780M ;
   - le HDR 10 bits au banc.
4. **Le plan d'abord** : ce paragraphe. Ensuite, la gestion des plans organise les créneaux.

### c. La netteté par zone

**L'hôte natif calcule la grille.**
- **Où :** sur la capture native, dans la passe de réduction ou juste avant elle. Huit statistiques par
  pixel, chaque voisin à 1 px, moyennées par zone de la grille posée sur la capture :
  - la luma et son carré ;
  - le gradient (|dx| + |dy|), et la part des pixels au-dessus de 0,05 et sous 0,01 ;
  - le laplacien ;
  - la saturation (max − min de RGB) ;
  - la part des pixels sombres (luma < 0,15).
- **La décision :** l'arbre du banc. Ses entrées :
  - l'échelle ;
  - les huit statistiques de la zone et leurs moyennes sur l'image ;
  - le filtre de réduction (linéaire ou non).
  
  Sa sortie est une baisse, sur 12 niveaux, donc 4 bits. L'arbre est appris sur tout le corpus, puis ses
  seuils deviennent une table constante dans le code. Un test rejoue cette table sur des cas du banc.
- **Quand :** seulement si l'hôte réduit l'image (capture plus grande que le flux), et en SDR. À 1:1 ou
  en HDR, aucune grille : le client applique la loi. Le banc n'a mesuré ni le HDR, ni une capture à la
  taille du flux.
- **L'échelle :**
  - l'arbre suppose que la capture a la taille d'affichage du client. C'est ce que fait l'écran virtuel
    ajusté au client ;
  - l'hôte prend donc l'échelle capture / flux, et l'écrit avec la grille ;
  - le client ignore la grille si son échelle réelle s'en écarte de plus de 10 % : fenêtre réduite, zoom,
    capture d'un écran physique ;
  - il l'ignore aussi sous ×1,3 (f.2.5) : le banc a mesuré à partir de ×1,33. Plus bas, la force du noyau
    tend vers 0 à 1:1, et la baisse divisée par elle croîtrait sans borne (à ×1,25, le code 11 rendrait une
    netteté de −0,85, contre −0,18 au pire à ×1,33). L'hôte calcule quand même la grille entre ×1 et ×1,3 ;
    l'en empêcher serait une ligne dans `enableMwsrStats`, pas encore faite.
- **Sans retarder l'image :**
  - les statistiques partent avec la conversion et sont relues quand l'image encodée sort ;
  - si elles ne sont pas prêtes, l'image part sans grille, et le client applique la loi.
- **Le transport :**
  - un bloc annexe en tête de l'image, signalé par un bit de l'octet 8 de l'en-tête de morceau (aujourd'hui
    « image clé », 0 ou 1) ;
  - 295 octets par image : 576 valeurs de 4 bits, l'échelle et la taille de la grille. Soit
    environ 140 kbit/s à 60 i/s ;
  - le bloc n'est envoyé que si le client l'a demandé au `/start` (`mwsr_map`). Un frontend ancien resté
    en cache ne le voit donc jamais ;
  - même en-tête de 17 octets sur le canal de données (`FrameSender`) et sur le WebSocket de secours
    (`StreamRelay`), lus tous deux par `_onVideoChunk`. En v0.4.2, seul le canal de données porte la
    grille (f.2.4) ; le WebSocket de secours, son ancien chemin à en-tête de 2 octets compris, n'en a
    pas : le client y applique la loi.
- **Le coût, à mesurer avant d'activer.**
  - Estimé : environ 0,1 ms par image sur une RTX, environ 3 ms sur un GPU intégré. Mesuré le 30/10
    (f.3) : 0,04 à 0,09 ms sur une RTX 5060 Ti et 0,86 à 2,6 ms sur l'iGPU du 9900X, à pleine fréquence ;
    plus sur un bureau au repos (jusqu'à 1,4 et 3,4 ms en 4K). La légère, mesurée le 31/10 (f.4), ne
    coûte que 1,4 fois moins sur l'iGPU, et pas moins sur la RTX en 4K au repos.
  - La règle de Bruno (f.4), celle du Lanczos-2, gardé s'il coûte au plus 1,5 ms (`ResampleCost.h`),
    mesurée dans le stream :
    - la passe complète, si elle tient dans ces 1,5 ms ;
    - sinon, la variante légère : 16 × 9 zones, un pixel sur 16 (un sur 4 dans chaque direction, les
      voisins toujours à 1 px), c'est le « pas 4 » du banc. Elle a été mesurée à +0,064 / +0,062 sur
      images fixes et à 99,6 % en mouvement ;
    - sinon, aucune grille.
- **Les plateformes :**
  - D3D11 (Windows) d'abord ;
  - puis D3D12, EGL et Vulkan (Linux), chacune à son tour ;
  - un hôte qui n'a pas la passe n'envoie rien, et le client applique la loi.

**Le client lit la grille.**
- **La garde :**
  - le renderer garde la grille par horodatage jusqu'au dessin de son image, comme le QP (`noteFrameQp`) ;
  - la grille voyage dans son image : elle n'arrive jamais seule ni en retard ;
  - une image sans grille prend la loi.
- **La lecture :**
  - la grille devient une texture 32 × 18, lue au bilinéaire par la passe `up` ;
  - la netteté vaut `loi(échelle) − baisse / force(échelle)`, avec une baisse positive ou nulle ;
  - la baisse porte sur le poids à travers le bord ; la force est la rampe du noyau, qui vaut 1 à ×2 et
    0,667 à ×1,33.
- **Deux compilations du même source,** avec et sans lecture de la grille :
  - sans lecture, c'est le module d'aujourd'hui, pour tous les autres hôtes : ils ne paient rien ;
  - le coût de la lecture est mesuré : 0 à 2 % sur PC ; sur le realme, environ 0 en f16, mais +8 à +10 %
    en f32.
- **Les renderers et les noyaux :**
  - WebGPU d'abord : c'est là que la grille est validée au labo (`mwsr-map.js`) ;
  - puis le jumeau GLSL (WebGL2) ;
  - V2 aussi, si elle revient (elle sort de la v0.4.2, b.2). Le binding 3 est déjà pris par V2 dans
    `mwsr-yuv.js`.
- **Un drapeau de debug,** coupé par défaut et sans interface, comme `mw_mwsr_gaze` : `mw_mwsr_map` =
  `0` dans le localStorage ignore la grille, pour comparer à la loi sur le même stream. Avant
  l'activation, c'est l'inverse : `1` demande la grille et la lit (f.2.4, f.2.5) ; sans la clé, rien
  n'est demandé.

**Les preuves avant d'activer :**
- la table de l'arbre redonne les décisions du banc ;
- la passe de l'hôte, sur WARP, redonne les statistiques du banc à la précision f16 près ;
- le client, sur SwiftShader : une grille uniforme rend la loi au bit près, et la grille du banc rend
  comme le modèle numpy ;
- le coût réel de la passe, mesurée seule sur une capture, sur la RTX et sur l'iGPU du 9900X (créneau de
  la gestion des plans), puis sur la 780M quand la passe Linux existera ;
- enfin, un vrai stream de l'hôte natif, pour l'œil de Bruno : grille ou loi, par le drapeau.

### d. V2

**Hors de la v0.4.2** depuis le 30/10 au soir (b.2) : ce qui suit n'est pas codé dans le produit.

- **Le noyau.** Celui du labo (`mwsr-yuv.js`, V2) entre dans `MwsrShader.js`, en WebGPU seulement,
  le seul rendu mesuré. WebGL garde le chemin actuel.
  - D'abord, la passe 0 écrit Y et UV depuis la texture externe.
  - Ensuite, le noyau traite Y en MWSR et UV au bilinéaire, avec une sortie RGB.
- **Quand :** sur un flux 4:2:0, jamais sous CHROMA444 (`_yuv444`), et sur un GPU faible.
- **« GPU faible », par la mesure** (règle A de Bruno, 30/10, après la 780M : V2 seulement si elle est
  mesurée au moins 5 % plus rapide, sur ce GPU et à cette taille ; sans horodatages, le chemin actuel) :
  - le renderer demande `timestamp-query` quand l'adaptateur l'a, comme `shader-f16` ;
  - il mesure d'abord la chaîne actuelle (passe 0 et noyau) pendant environ 1 s après le démarrage, à
    la taille courante, et garde le p50 ;
  - sous 0,5 ms par image, il garde le chemin actuel : le GPU n'est pas faible. Mesures du 29 et du
    30/10 :
    - RTX : 0,08 à 0,11 ms ;
    - 780M : 0,29 à 0,47 ms en 1440p, 0,66 ms en 4K ;
    - iGPU du 9900X : 2,4 à 3,1 ms ;
    - iPhone 13 Pro : 0,9 à 1,1 ms ;
    - realme : 18 à 25 ms ;
  - au-dessus, il mesure aussi V2 pendant environ 2 s, en blocs de quelques images alternés avec le
    chemin actuel, pour que la montée en fréquence du GPU ne favorise aucun des deux. V2 s'affiche alors
    par moments ; sa couleur ne s'écarte que de 0,1 point de SSIM ;
  - il prend V2 seulement si son p50 est plus bas d'au moins 5 %. Sinon, le chemin actuel : c'est le
    cas de la 780M en 4K, où V2 perd 15 % ;
  - la mesure est refaite à chaque changement de taille, parce que l'échelle change le coût ;
  - à taille égale, le choix ne change plus pendant la session : pas de bascule en boucle ;
  - sans `timestamp-query` : le chemin actuel. Plus de repli par la marque, qui aurait pris V2 sur la
    780M.
- **L'échelle de l'enhancer ne change pas.** V2 reste la marche `mwsr` : elle la rend seulement moins
  chère.
- **Un drapeau de debug,** coupé par défaut et sans interface : `mw_mwsr_v2` dans le localStorage. `0`
  force le chemin actuel, `1` force V2, par exemple pour la voir sur une RTX. Absent, la règle décide.
- **Le gain et le prix, mesurés :**
  - la chaîne gagne 0 à 6 % sur la RTX, 11 à 16 % sur l'iGPU et 14 à 23 % sur le realme ;
  - la couleur perd 0,1 point de SSIM en 4:2:0, et la luma ne bouge pas.
- **Plus tard :** V2 en option du générateur `mwsr/`, et dans un renderer natif. Là, elle lirait les
  plans du décodeur sans passe 0 ; c'est non mesuré.

### e. Les quatre vérifications de V2, avant de l'activer

La 1 et la 3 sont faites. La 2 et la 4 tombent avec la sortie de V2 (b.2).

1. **L'œil de Bruno sur la couleur.** Planches A/B à l'aveugle, chemin actuel contre V2, en 4:2:0 :
   texte coloré, code, un jeu.
   - Rendu par les cellules SwiftShader du labo (`split+yuv`), sur le processeur seul.
   - Règle fixée d'avance : si Bruno choisit V2 ou « pas de différence » sur la plupart des planches, V2
     tient ; s'il choisit le chemin actuel sur la plupart, V2 attend.
   - **Fait le 30/10 au soir : V2 attend.** 8 extraits de 480 × 270 px, à ×2 et à ×1,33 (code, IDE, un
     jeu, et un panneau de texte posé sur le jeu), découpés là où les deux chemins diffèrent le plus.
     - Bruno a choisi le chemin actuel sur 7 extraits, V2 sur un seul (le panneau à ×1,33), « pas de
       différence » sur aucun.
     - Ses choix ne suivent pas la place : le chemin actuel était 3 fois en A et 4 fois en B. Au hasard,
       7 sur 8 ou mieux arrive 3,5 fois sur 100. Un seul observateur, en une séance.
     - Sur l'image entière, les mesures ne le disaient pas : 0,1 point de SSIM, et un écart CIEDE2000
       presque égal. Les extraits montrent les endroits où V2 s'écarte le plus, et c'est là que Bruno
       voit la différence.
2. **V2 sur l'iPhone.** Dans WebKit, elle ne dessine sans doute pas : les temps sont proches de 0 et
   les horodatages constants.
   - Trouver pourquoi, avec une capture, puis corriger.
   - Mesurer ensuite avec Bruno : Safari, environ 3 min, `phone.sh ios` (hors dépôt).
   - Jusque-là, Safari garde le chemin actuel.
3. **Le vrai Radeon 780M** (UM790Pro sous Ubuntu, créneau de la gestion des plans).
   - D'abord, vérifier que le WebGPU de Chrome tourne sur Vulkan.
   - Ensuite, V0 contre V2, chaîne et noyau, en f32 et en f16, à ×2 et ×1,33, sur le jeu et le texte.
   - Aussi : la lecture de la grille, et les temps du README de `mwsr/` refaits sur ce GPU.
   - Clip encodé dans la page en logiciel et décodé sur la 780M.
   - Environ 30 min, installation comprise (estimé).
   - **Fait le 30/10 (19:13-19:21).** Chrome 154 sans fenêtre, sur Vulkan (RADV), piloté depuis DualRTX
     par des tunnels ssh. Le clip est encodé et décodé en logiciel, comme au §9. La fréquence n'est pas
     fixée : lots de 100 dessins, dispersion sous 1 %, série refaite dans l'ordre inverse, mêmes chiffres
     à 1 % près. p50, en ms :

     | Cas | Chaîne V0 | Chaîne V2 | Écart | Noyau V0 | Noyau V2 |
     |---|---|---|---|---|---|
     | ×2, jeu | 0,294 | 0,349 | +19 % | 0,233 | 0,246 |
     | ×1,33, jeu | 0,390 | 0,482 | +24 % | 0,271 | 0,274 |
     | ×2, texte | 0,368 | 0,384 | +4 % | 0,299 | 0,282 |
     | ×1,33, texte | 0,473 | 0,521 | +10 % | 0,304 | 0,294 |
     | ×2, jeu en 4K | 0,658 | 0,759 | +15 % | 0,526 | 0,552 |

     - **V2 perd sur la 780M.**
       - Son noyau ne gagne rien (−6 à +6 %, contre −20 % sur le GPU intégré à 2 unités de calcul).
       - Sa passe 0 en deux plans coûte 35 à 65 % de plus que la copie RGBA.
       - Supposé : avec 12 unités de calcul, ce GPU n'est pas limité par le calcul.
     - **La lecture de la grille est gratuite** : 0 % (±0,5 %). Le f16 ne gagne rien (0 à −1 %).
     - **Les temps du README**, sur le jeu, à 1080p → 1440p, chacun à sa meilleure précision :
       MWSR 0,267 ms, FSR1 0,707, NIS 0,953, SGSR1 0,216, bilinéaire 0,110. Le f16 n'a pas été
       comparé au pixel au f32 sur ce GPU.
     - **Conséquence pour le §12 d** : en 4K, la chaîne V0 fait 0,66 ms, au-dessus du seuil de 0,5 ms. La
       règle prendrait donc V2, plus lente de 15 %, et le repli par la marque la prendrait partout sur ce
       GPU.
       - Proposé à Bruno : V2 seulement si elle est mesurée plus rapide que V0, d'au moins 5 %, sur ce
         GPU et à cette taille.
       - Sans horodatages : le chemin actuel.
       - **Bruno a choisi cette règle (A)** le 30/10 au soir. Elle s'ajoute au seuil de 0,5 ms, qui
         reconnaît le GPU faible ; le §12 d est réécrit.
     - Les temps du README sont dans la notice de `mwsr/` depuis le 30/10 (oui de Bruno), avec la
       mention que le f16 n'y a pas été comparé au f32.
4. **Le HDR 10 bits au banc** (DualRTX, créneau GPU de la gestion des plans).
   - Un clip P010 encodé en logiciel, décodé sur la RTX puis sur l'iGPU.
   - V0 contre V2 : les temps, et la couleur en PQ (écart et dépassement).

### f. L'ordre, avec l'effort de chaque étape

Le code entre dans le produit coupé : aucun client ne demande la grille, et aucune règle ne choisit V2.
On l'allume au oui de Bruno, après les mesures. Commits sans push ; c'est la gestion des plans qui
pousse.

1. Ce plan. `high`
2. Sans GPU :
   1. les planches couleur de V2 (vérification 1). `medium`
      - **Fait : V2 attend** (e.1). Bruno a ensuite sorti V2 de la v0.4.2 (b.2) : les étapes 6 et 7
        sont sautées, ainsi que son HDR au créneau. La grille (4 et 5) ne dépend pas de V2.
   2. l'arbre exporté en table, et ses tests contre le banc. `high`
   3. la passe de statistiques de l'hôte en D3D11, vérifiée sur WARP. `xhigh`
      - **Fait le 30/10** : `MwsrStats` (`src/convert/windows/`), un compute shader `cs_5_0`, un
        groupe par cellule de la grille 128 × 72, puis une copie relue sans attendre (anneau de 4
        tampons, un ticket par image). Rien ne l'appelle encore : le branchement vient avec 2.4.
      - Sur les 22 captures natives du banc, sur WARP : aucun code de zone ne change (0 sur
        101 376 décisions, aux 4 échelles du banc, réduction linéaire ou bilinéaire). Les valeurs
        des cellules sont à 1 pas de f16 au plus. Seuls 10 pixels sur 81 millions sont comptés
        autrement, à cause de l'ordre des trois produits de la luma dans numpy ; écrits comme le
        shader, aucun.
      - Le test du dépôt (`test_mwsr_stats.cpp`, WARP) compare la passe à une référence CPU qui
        refait ses opérations : écart de 1e-7 au plus.
      - Seulement pour une capture 8 bits : le bureau HDR, même ramené en SDR, n'a pas de grille.
      - Non mesuré : le coût sur GPU (créneau, étape 3). Sur le processeur, la décision des 576
        zones prend 0,3 ms par image sur le 9900X ; ce sont surtout les arrondis f16.
   4. le transport : `mwsr_map` au `/start`, le bit de l'octet 8, le bloc annexe, sa lecture et ses
      tests. `xhigh`
      - **Fait le 30/10 au soir**, coupé : aucun client ne demande la grille sans la clé de labo
        `mw_mwsr_map` = `1` (`mwsrMapWanted`). Après l'activation, la clé s'inversera : `0`
        ignorera la grille, comme prévu plus haut.
      - L'hôte : `SessionConfig::mwsrZones`. La conversion D3D11 lance la passe avant ses
        dessins ; après l'encodage, la session relit les cellules sans attendre, décide les
        576 codes et les joint à l'image (`EncodedFrame::mwsrZones`). Statistiques pas encore
        rendues : l'image part sans grille. Le journal de fin de session dit combien d'images
        l'ont portée.
      - Seulement sur une capture 8 bits SDR, réduite vers un flux de sa forme (à 1 % près), sur
        la chaîne D3D11 : celle de NVENC et d'AMF par défaut. La chaîne D3D12 (Intel) n'a pas de
        passe.
      - Le bloc (`MwsrZoneBlock.h`) : `[longueur:2][version:1][colonnes:1][lignes:1]
        [échelle:2][codes]`, en gros-boutiste, l'échelle en millièmes, deux codes de 4 bits par
        octet. 295 octets pour 32 × 18 zones. La longueur en tête permet de sauter une version
        inconnue.
      - L'octet 8 de l'en-tête de morceau : bit 0 l'image clé, bit 1 le bloc, dans chaque
        morceau de l'image. La découpe est sortie dans `FrameChunks.h`, testable sans WebRTC.
      - Seul le canal de données direct de l'hôte natif porte la grille. Le WebSocket de
        secours, la piste média et le flux commun des invités n'en ont pas : le client y
        applique la loi.
      - Le client (`MwsrZoneBlock.js`) ôte le bloc avant le décodeur et passe la grille à
        `onVideo`. Un bloc dont la longueur est impossible fait perdre l'image, comme un
        morceau manquant. Le renderer la lit depuis l'étape 5.
      - La décision sur le processeur, mesurée sur le 9900X au repos : 0,30 ms par image, dont
        l'essentiel en arrondis f16. Réécrits sur les bits du float, même résultat sur les
        2^32 valeurs possibles : 0,09 ms.
      - Tests : bloc et découpe (serveur), lecture et réassemblage (client), conversion qui
        prend les statistiques et relecture identique à la passe seule (WARP). Non testé : un
        vrai stream, avec le GPU (étape 3).
   5. la grille dans le client WebGPU, les deux compilations, le drapeau, vérifiés sur SwiftShader. `high`
      - **Fait le 30/10 au soir**, coupé comme 2.4 : seul un client qui a la clé `mw_mwsr_map` = `1`
        construit la seconde compilation et lit la grille.
      - Le texte : `mwsrWgsl({ map: true })` ajoute le binding 3, une texture r16float d'un texel par
        zone, qui porte sa baisse. Il ne change qu'une ligne : la netteté vaut `loi − baisse /
        max(force, 0,01)`. Sans `map`, le texte est celui d'avant à l'octet près : les shaders générés
        de `mwsr/` ne bougent pas, seule la copie de `MwsrShader.js` y suit.
      - Le renderer (`WebGpuRenderer`) construit la seconde compilation avec la première, à la
        création. La grille d'une image est gardée par son horodatage (`noteFrameZones`), comme le QP.
        Au dessin, elle est lue si elle convient à l'échelle (`mwsrZonesFit` : à 10 % près, et ×1,3 au
        moins). La texture n'est réécrite que quand les codes changent ; une réduction n'en lit pas.
      - Le chemin de l'image : `onVideo`, `handleVideoFrame`, puis le worker de décodage ou
        `decodeFrame`, AV1 compris, et les images qui attendent le décodeur gardent leur grille.
      - Sur SwiftShader, le vrai renderer, des images du banc (`mix` à ×2 et à ×1,33, `text` à ×2) et
        les grilles que `decideAll` tire des cellules WARP de ces captures :
        - une grille de code 0 rend la loi au bit près ; une grille décidée pour une autre échelle
          (+15 %) aussi ;
        - contre le modèle numpy du banc, à la même carte de netteté : 0,0011 à 0,0070 % des pixels à
          plus d'un niveau, pas plus que la loi elle-même (0,0017 à 0,0086 %) ;
        - une grille qui parcourt les 12 codes : 0,0009 à 0,0057 %.
      - Non vérifié : le module f16 (SwiftShader n'a pas `shader-f16`), le coût de la lecture sur un
        vrai GPU, un vrai stream.
      - Tests : le texte (`MwsrShader.test.js`), les texels et l'échelle (`MwsrZoneBlock.test.js`), le
        renderer sur un faux GPU (`WebGpuRendererZones.test.js`), le chemin de la page
        (`StreamViewZones.test.js`).
   6. ~~V2 dans `MwsrShader.js`, la règle par la mesure, le drapeau~~ : sautée, V2 sort de la v0.4.2.
   7. ~~pourquoi V2 ne dessine pas dans WebKit~~ : sautée, de même.
3. Les créneaux de la gestion des plans, avec la durée annoncée à chaque fois. `xhigh`
   - DualRTX : le coût de la passe de l'hôte sur la RTX et l'iGPU ;
     - **Fait le 30/10 au soir** (22:35-22:41), une carte à la fois : la RTX 5060 Ti du bus 1, puis l'iGPU du
       9900X (2 unités de calcul).
     - Le banc, hors dépôt : la passe du produit (`compileMwsrStatsShader`, les tampons de `MwsrStats`),
       seule sur une capture, entre horodatages GPU, copie des cellules comprise. Sur WARP, ses cellules
       sont celles de `MwsrStats` au bit près.
     - Deux rythmes :
       - en rafale, par lots de 50, la file vidée entre les lots ;
       - une image toutes les 16,7 ms, attendue, sur un bureau au repos.
     - Les conditions : la série est refaite dans l'ordre inverse, avec les mêmes chiffres à 2 % près. Les
       fréquences sont laissées au pilote. HAGS est actif sur la RTX ; l'iGPU ne le gère pas.
     - p50 en ms, copie comprise :

       | Capture | RTX, rafale | RTX, 60 i/s | iGPU, rafale | iGPU, 60 i/s | iGPU légère, rafale | iGPU légère, 60 i/s |
       |---|---|---|---|---|---|---|
       | 1080p | 0,044 | 0,08 à 0,19 | 0,86 | 1,61 à 1,62 | 0,23 | 0,74 |
       | 1440p | 0,063 | 0,21 | 1,58 | 2,32 à 2,36 | 0,41 | 1,13 à 1,16 |
       | 4K | 0,091 | 1,40 | 2,60 | 3,33 à 3,38 | 0,73 | 1,42 à 1,43 |

     - **La « légère » de ce tableau n'est pas celle du banc.** Elle lit 64 × 36 cellules, un pixel sur 4.
       Seul son coût est mesuré : ses moyennes ne sont pas renormalisées. Elle coûte 3,6 fois moins en
       rafale, et 2 à 2,4 fois moins à 60 i/s. Le pixel sur 4 seul, sans réduire les zones, ne divise le
       coût de l'iGPU que par 1,4 à 2,9 : une part fixe par groupe domine.
       - Le « pas 4 » du banc lit un pixel sur 16 (`zonecells.py`, `n4`), ce que fait la passe légère
         écrite ensuite (f.4). Son coût a été mesuré au créneau du 31/10 au matin (f.4).
     - **L'estimation** (0,1 ms sur une RTX, 3 ms sur un GPU intégré) tient pour un GPU à pleine fréquence.
     - **À 60 i/s, le GPU redescend entre deux images,** et la passe coûte plus : 1,3 à 1,9 fois plus sur
       l'iGPU, 2 à 15 fois plus sur la RTX.
       - Sur la RTX, la copie des cellules passe de 0,011 à 0,157 ms. Supposé : le lien PCIe ralentit au
         repos.
       - En 4K, la passe seule passe de 0,08 à 1,24 ms. Supposé : la capture (33 Mo) ne tient plus dans
         le cache L2, et la mémoire est à sa fréquence de repos.
     - **Non mesuré : dans un vrai stream**, le GPU convertit et encode aussi, et sa fréquence est entre
       les deux.
     - **La passe part avant la conversion,** sur la même file (`ColorConvert::convert`). Son coût
       retarde donc chaque image, comme celui du Lanczos-2.
   - ~~l'UM790Pro : la 780M~~ (fait le 30/10, e.3) ; ~~l'iPhone, avec Bruno~~ (V2 sortie).
4. Le verdict à Bruno : le budget de la passe de l'hôte. Puis l'activation de la grille, à son oui. `high`
   - **Proposé à Bruno le 30/10 au soir**, sur les chiffres de 3. Dans tous les cas, la passe est mesurée
     dans le stream, comme le Lanczos-2 : les minuteurs du convertisseur, 10 images d'échauffement, puis
     la médiane de 30 (`ResampleCost.h`). La mesure seule ne dit pas le coût dans le stream.
     - **A, recommandé : 0,5 ms, la passe complète seule.** Au-dessus, plus de grille, et le client
       applique la loi.
       - La RTX garde la grille en 1080p et en 1440p, même au repos (0,21 ms au plus). En 4K, la mesure
         du stream tranche (0,09 à 1,40 ms).
       - L'iGPU du 9900X n'en a pas : la passe complète lui coûte 0,86 ms au moins.
       - Rien de plus à écrire.
       - Pourquoi : la latence d'abord, pour un gain petit (+0,066 sur images fixes) que l'œil de Bruno
         n'a pas vu en mouvement (a).
     - **B : 1,5 ms, le budget du Lanczos-2, et la variante légère** entre la passe complète et rien.
       - Au banc, la légère garde presque tout le gain : +0,064 / +0,062 contre +0,066 / +0,063 sur images
         fixes, 99,6 % contre 102,5 % en mouvement (a, 16 × 9 au pas 4).
       - L'iGPU du 9900X aurait la légère à toutes les tailles, pour 0,74 à 1,43 ms au repos. Ces chiffres
         sont ceux d'une légère qui lisait 4 fois plus de pixels que celle du banc (f.3) : la vraie devrait
         coûter moins (supposé).
       - À écrire : la passe légère de l'hôte, l'arbre en 16 × 9 et leurs tests. `high`
         Le transport et le client lisent déjà une grille de toute taille.
     - **Bruno a choisi B** le 30/10 au soir. Go pour la passe légère, l'arbre en 16 × 9 et leurs tests :
       WARP d'abord, rien d'activé, le créneau GPU demandé à la gestion des plans (pas avant le matin).
     - **Écrit le 30/10 au soir, vérifié sur WARP**, coupé comme le reste : sans la clé de labo, aucun
       client ne demande la grille.
       - **La passe légère** (`MW_LIGHT` dans `kMwsrStatsSource`) :
         - elle écrit les mêmes 128 × 72 cellules que la passe complète, chacune d'un pixel sur 16 ;
         - les voisins restent à 1 px ;
         - un fil par cellule, et un groupe de 8 × 8 fils par zone de la grille légère. Le 31/10, elle
           a pris la forme de la complète (plus bas).

         Le calcul d'un pixel est une fonction commune aux deux passes. La passe complète rend ses
         cellules d'avant au bit près, sur les 22 captures natives du banc (WARP).
       - **L'arbre en 16 × 9** (`MwsrZoneTreeLight.h`, 23 nœuds) : appris par l'export du banc sur tout
         le corpus, en « hôte natif q4 ». Sa décision moyenne 8 × 8 cellules par zone (`kLightGrid`). La
         nouvelle option de l'export (`--light`) laisse l'arbre 32 × 18 identique à l'octet près.
       - **Contre le banc**, sur les 22 captures, sur WARP :
         - aucun code de zone ne change : 0 sur 25 344 décisions, aux 4 échelles, réduction linéaire ou
           bilinéaire ;
         - les cellules sont à 1 pas de f16 du banc, sauf une : un pixel compté autrement sur les 25 de la
           cellule, l'ordre des produits de la luma dans numpy (déjà vu en 2.3).
       - **Le budget** (`D3d11VideoPipeline::takeMwsrZones`, `afterPassCost`) :
         - la passe se chronomètre elle-même sur les images du stream (`PassTimer`, l'anneau de requêtes
           du Lanczos-2 sorti de `ColorConvert` et partagé) ;
         - au-dessus de 1,5 ms, la passe complète laisse la place à la légère, chronométrée à son tour ;
         - si la légère dépasse aussi, plus de grille. Chaque étape est écrite dans le journal.

         Les images du changement partent sans grille.
       - **La grille** : 32 × 18 ou 16 × 9 selon la passe, portée par l'image (`mwsrZonesX`, `mwsrZonesY`).
         Le transport et le client en lisent de toute taille.
       - **Tests** : 392 vérifications des groupes touchés passent (`mwsr_zones`, `mwsr_stats` sur WARP,
         `color_convert`, `resample_cost`, `d3d11_video_pipeline`).
         - Les nouvelles : la passe légère contre une référence processeur (1e-7 au plus) et l'arbre
           léger sur des zones du banc ;
         - une zone légère depuis ses 64 cellules ;
         - la règle du budget, et le chronomètre sur WARP.
       - **Mesuré le 31/10 au matin** (06:35-06:38), au créneau de la gestion des plans. Une carte à la
         fois, le banc et les conditions de f.3. Le banc rend les cellules du produit au bit près, pour
         la passe complète comme pour la légère. La passe complète refait les chiffres de f.3 à 1 % près.
         - p50 en ms, copie comprise :

           | Capture | Carte | Complète, rafale | Légère, rafale | Complète, 60 i/s | Légère, 60 i/s |
           |---|---|---|---|---|---|
           | 1080p | RTX | 0,044 | 0,022 | 0,08 à 0,19 | 0,17 |
           | 1440p | RTX | 0,063 | 0,029 | 0,21 | 0,18 |
           | 4K | RTX | 0,092 | 0,051 | 1,34 à 1,43 | 1,47 à 1,52 |
           | 1080p | iGPU | 0,86 | 0,62 | 1,60 à 1,62 | 1,19 à 1,20 |
           | 1440p | iGPU | 1,58 | 1,22 | 2,34 | 1,67 à 1,68 |
           | 4K | iGPU | 2,60 | 1,83 à 1,88 | 3,36 | 2,34 à 2,36 |

           En 4K, la rafale qui suit la série à 60 i/s coûte 0,21 à 0,25 ms sur la RTX : la fréquence
           n'est pas encore remontée.
         - **La légère ne gagne que 1,4 fois sur l'iGPU.** Elle gagne 2 fois sur la RTX en rafale, et rien
           en 4K au repos sur la RTX. C'est loin d'un pixel sur 16.
           - Deux hypothèses, non tranchées :
             - **La bande passante**, celle qui colle le mieux. Les voisins à 1 px de chaque pixel lu
               touchent les lignes y − 1, y et y + 1 : 3 lignes de l'image sur 4. Dans chacune, un pixel
               tous les 4 touche toutes les lignes de cache. La légère tire donc environ 3/4 des octets
               de la complète. C'est le rapport mesuré sur l'iGPU (0,70 à 0,77), le même de 1080p à 4K.
               C'est aussi ce que donne la RTX en 4K au repos, la mémoire à sa fréquence de repos.
             - **La latence**, à cause de la forme du noyau. Il n'y a que 144 groupes. Chaque fil
               enchaîne jusqu'à 64 pixels (en 4K). Deux fils voisins lisent à 15 à 30 px l'un de
               l'autre, jamais dans la même ligne de cache.
           - Si c'est la bande passante, seul un échantillonnage qui touche moins de lignes ferait
             mieux. Mais il change les statistiques du banc : il faudrait refaire l'arbre, et c'est
             une décision de Bruno.
         - **La règle B sur ces chiffres :**
           - l'iGPU du 9900X :
             - en 1080p, la complète en rafale et la légère au repos ;
             - en 1440p, la légère en rafale, et aucune grille au repos ;
             - en 4K, aucune grille.
           - la RTX : la complète partout, sauf en 4K au repos. Là, elle est au bord des 1,5 ms (p90
             jusqu'à 1,77 ms), et la légère n'aide pas.
         - **Pour trancher, deux variantes du banc**, hors dépôt (go de la gestion des plans le 31/10) :
           - **light-cell**, la légère dans la forme de la complète : un groupe par cellule, un pixel par
             fil, puis la réduction en arbre. Elle lit les mêmes pixels et rend les cellules de la
             légère à l'ordre des sommes près.
             - Vérifiée sur WARP, sur les 22 captures : 4,2e-7 d'écart au plus avec la légère.
               45 valeurs sur 1 622 016 (9 216 cellules et 8 statistiques par capture) arrondissent au
               f16 voisin.
             - Aucune décision de zone ne change, 0 sur 25 344, contre la légère comme contre le banc.
           - **light-rows**, la légère sans ses voisins verticaux, qui ne lit qu'une ligne sur 4. Elle
             ne sert qu'à mesurer le coût.
           - Si light-rows tombe vers le quart de la complète et que light-cell ne gagne rien, c'est la
             bande passante. Si light-cell descend vers 0,1 à 0,3 ms en 4K sur l'iGPU, c'est la latence.
         - **Tranché le 31/10 au matin** (06:45-06:51), au créneau de la gestion des plans : l'iGPU, puis
           la RTX du bus 1. Le banc rend toujours le produit au bit près, et light-cell à 3,6e-7 de la
           légère.
           - p50 en ms, copie comprise :

             | Capture | Carte, rythme | Complète | Légère | light-cell | light-rows |
             |---|---|---|---|---|---|
             | 1080p | iGPU, rafale | 0,86 | 0,62 | 0,48 | 0,42 |
             | 1440p | iGPU, rafale | 1,58 | 1,22 | 0,48 | 0,65 |
             | 4K | iGPU, rafale | 2,60 | 1,85 | 0,69 | 1,35 |
             | 1080p | iGPU, 60 i/s | 1,60 | 1,19 | 1,25 | 0,88 |
             | 1440p | iGPU, 60 i/s | 2,33 | 1,68 | 1,22 | 1,18 |
             | 4K | iGPU, 60 i/s | 3,37 | 2,33 | 1,35 | 1,85 |
             | 4K | RTX, rafale | 0,092 | 0,050 | 0,037 | 0,038 |
             | 4K | RTX, 60 i/s | 1,32 à 1,44 | 1,22 à 1,33 | 1,07 | 0,73 |

             En 1080p et en 1440p, la RTX reste entre 0,019 et 0,064 ms en rafale. Au repos, elle
             reste entre 0,17 et 0,21 ms, dont 0,157 pour la copie.
           - **Sur l'iGPU, c'est surtout la latence.** Dans la forme de la complète, la légère coûte 2,7
             fois moins en 4K. light-rows gagne moins. light-cell a un plancher d'environ 0,48 ms, le même
             en 1080p et en 1440p : le coût fixe de ses 9 216 groupes.
           - **Sur la RTX en 4K au repos, c'est la bande passante** : light-rows divise le coût par 2, et
             light-cell gagne un peu.
           - **La règle B, si la légère prend la forme light-cell :**
             - l'iGPU garde une grille aux trois tailles : 1,25 / 1,22 / 1,35 ms au repos (p90 jusqu'à
               1,53 en 4K) ;
             - la RTX la garde partout : en 4K au repos, la complète cède la place à la légère (1,07 ms).
           - **Proposé** à la gestion des plans et à Bruno : la passe légère du produit dans la forme
             light-cell, un seul noyau pour les deux passes (un pas de 1 ou de 4), WARP d'abord, puis un
             créneau court sur l'iGPU. light-rows n'est pas proposée : elle change les statistiques du
             banc, donc l'arbre.
         - **Bruno a dit oui** le 31/10 au matin. **Écrit le 31/10, vérifié sur WARP**, rien d'activé :
           - un seul noyau dans `kMwsrStatsSource`, avec un pas de 1 ou de 4 (`kStep`). Les deux passes
             ont un groupe par cellule : `Dispatch(128, 72)` ;
           - sur les 22 captures, la passe complète rend ses cellules d'avant au bit près. La légère rend
             celles de light-cell au bit près ;
           - contre l'ancienne légère : 4,2e-7 d'écart au plus, et 45 valeurs sur 1 622 016 au f16
             voisin. Aucune décision de zone ne change, 0 sur 25 344, ni contre elle ni contre le banc ;
           - les 392 vérifications des groupes touchés passent.
         - **Reste** : confirmer le coût sur l'iGPU, à un créneau court de la gestion des plans.
       - **Non fait** : un vrai stream.
5. Ensuite :
   - la passe de l'hôte en D3D12, en EGL et en Vulkan. `high` chacune
   - le jumeau GLSL de la grille. `high`
   - le distribuable `mwsr/`. `medium`

### g. Ce qui n'est pas mesuré

- **Le HDR.** La grille est coupée en HDR. V2, sortie de la v0.4.2, n'y a pas été mesurée.
- **Une capture qui n'a pas la taille d'affichage du client.** La grille est ignorée au-delà de 10 %
  d'écart.
- **La passe de l'hôte en rotation**, un quart des pixels par image : le coût baisserait, mais le gain
  n'est pas mesuré par zone.
- **Le coût de la passe dans un vrai stream.** Mesurée seule (f.3), elle coûte entre son prix à pleine
  fréquence et son prix sur un bureau au repos, et l'écart va jusqu'à 15 fois sur la RTX en 4K.
- **La passe lancée après la conversion** plutôt qu'avant : elle quitterait peut-être le chemin de
  l'image vers l'encodeur. Non mesuré.
- **Le pompage à l'œil.** Sur les 4 extraits de la première série, les coches de Bruno ne suivaient pas
  le pompage mesuré (la loi, qui ne peut pas pomper, a été cochée une fois). Un seul observateur, en une
  séance.
- **Les autres hôtes :** une valeur par image côté client a été mesurée, puis écartée par Bruno.
