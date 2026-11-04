# Journal du noyau `mwsr` (plan `docs/design/upscaler-mwsr.md`, §6.3)

Chaque entrée : hypothèse, mesure, verdict. Les temps viennent des horodatages GPU du labo
(`scaler_lab.py`), la qualité de `--quality` (PSNR, SSIM, dépassements contre la vérité terrain
rendue à la taille de sortie, entrée réduite en Lanczos-3 linéaire ; FLIP dès qu'une roue
Python 3.14 existe). SwiftShader donne les mêmes pixels que le RTX au centième de dB (P0 §4 bis) :
la qualité se mesure sur SwiftShader au fil de l'eau, les temps sur le RTX à la demande de la
session d'orchestration. Règle de salle blanche (§10.2) : les constantes ci-dessous sont les
nôtres, produites par ce journal.

**Correction du 29/10/2026.** Le GPU intégré du DualRTX n'est pas un Radeon 780M, mais celui du
Ryzen 9 9900X : RDNA 2, 2 unités de calcul, id 0x13C0 (plan B §9). Dans les entrées ci-dessous, le
« 780M » du DualRTX désigne ce GPU. Seul l'UM790Pro a un vrai 780M (J36-J37).

## 2026-10-21 — J1, le noyau de base tel qu'écrit (`MwsrShader.js`, P2.1)

**Le texte.** Une passe, feed `texture_2d` d'abord. Cinq échantillons bilinéaires : le centre C,
puis, sur les quads que les dérivées d'écran de la luma désignent comme contour
(`|dpdx| · s_x + |dpdy| · s_y ≥ τ`, luma par texel d'entrée), deux échantillons **le long** du
contour (poids `W_ALONG · a`, lissage) et deux **en travers** (poids `−W_ACROSS · netteté · a`,
accentuation), tous à `R_ALONG` / `R_ACROSS` texels, dans le repère donné par les dérivées
(`ORIENT 1`) ou sur les axes (`ORIENT 0`, repère tiré des échantillons). `a` = cohérence =
`max(marche, crête) / variation` ∈ [0, 1] (marche `|X2 − X1|`, crête `|X1 + X2 − 2C|`, variation =
somme des quatre écarts à C), × `strength` (0 à 1:1, 1 dès `S_FULL`). Résultat borné par le min
et le max des cinq. Luma `(0,25, 0,625, 0,125)`.

**Première table** (J1) : `ORIENT 1`, `W_ALONG 0,35`, `W_ACROSS 0,2`, `R_ALONG 1`, `R_ACROSS 1`,
`S_FULL 1,5`, `W_MIN −0,24`, `τ 0,05`, netteté 1.

**Hypothèse H1.** À ×2, ce noyau se place entre le bilinéaire et FSR1 en PSNR/SSIM sur le texte,
sans dépassement.

**Mesure** (SwiftShader, `--quality`, 720p → 1440p, PSNR / SSIM / dépassement) :

| image | bilinéaire | SGSR1 (dépôt) | FSR1 | **mwsr J1** |
|---|---|---|---|---|
| `text` | 16,99 / 0,816 / 0 % | 18,13 / 0,869 / 4,4 % | 17,57 / 0,845 / 0,3 % | **16,85 / 0,816 / 0,7 %** |
| `code` | 29,98 / 0,977 / 0 % | 29,65 / 0,974 / 2,6 % | 30,36 / 0,975 / 0,7 % | **29,79 / 0,977 / 0,2 %** |
| `game` | 35,01 / 0,880 / 0 % | 35,61 / 0,883 / 0,7 % | 35,55 / 0,887 / 0,03 % | **35,45 / 0,881 / 0,01 %** |

**Verdict H1 : non tenue sur le texte.** J1 est au niveau du bilinéaire (−0,14 dB sur `text`,
+0,44 dB sur `game`), loin de FSR1. Lecture : avec des échantillons à ±1 texel sur une rampe
bilinéaire d'un texel, la paire en travers encadre la rampe entière et son masque flou ne redresse
la rampe que d'un facteur `(1 + 2·W_ALONG) / (1 + 2·W_ALONG + 2·W_ACROSS)` ≈ 1,3 ; la paire le long
dilue l'accentuation et lisse les glyphes de 12 px, dont tout est courbe ou extrémité. Les
constantes de J1 sont trop timides en travers et trop larges le long.

**Suite (J2)** : balayage `W_ALONG ∈ {0 ; 0,15}`, `W_ACROSS ∈ {0,2 ; 0,4}`, `W_MIN` jusqu'à −0,4,
`R_ACROSS ∈ {0,6 ; 1}`, `τ 0,02`, `ORIENT 0` contre `1`, sur `text`, `code`, `game`, `zone` et
l'étalon de Bruno à ×2 et ×1,33.

## 2026-10-21 — J2, le balayage des poids (SwiftShader, `--quality-cells`, ×2)

**Hypothèses.** H2a : sans lissage le long (`W_ALONG 0`), le texte remonte. H2b : plus
d'accentuation en travers (`W_ACROSS 0,4`, `W_MIN −0,4`) rapproche de FSR1 sur le texte. H2c :
les axes (`ORIENT 0`) valent le repère des dérivées sur du texte droit. H2d : `τ 0,02` change peu.

| variante (ORIENT 1 sauf dit) | `text` | étalon, tiers central | `game` |
|---|---|---|---|
| bilinéaire | 16,99 / 0,816 / 0 % | 31,56 / 0,928 | 35,01 / 0,880 / 0 % |
| FSR1 | 17,57 / 0,845 / 0,3 % | 32,26 / 0,940 (2,0 % entier) | 35,55 / 0,887 / 0,03 % |
| J1 (`W_ALONG 0,35`) | 16,85 / 0,816 / 0,7 % | 31,21 / 0,925 | 35,45 / 0,881 / 0,01 % |
| `W_ALONG 0` | 17,33 / 0,837 / 0,6 % | **32,09 / 0,935** | 35,73 / 0,881 / 0,01 % |
| `W_ALONG 0`, `W_ACROSS 0,4` | 17,37 / 0,841 / 1,2 % | 31,67 / 0,932 | **35,86** / 0,882 / 0,03 % |
| … + `W_MIN −0,4` | 17,38 / 0,843 / 1,7 % | 30,48 / 0,922 (3,0 %) | 35,33 / 0,881 / 0,14 % |
| `W_ALONG 0,15`, `W_ACROSS 0,4`, `W_MIN −0,4` | 17,20 / 0,834 / 1,3 % | 31,11 / 0,927 | 35,79 / 0,882 |
| `W_ALONG 0`, `W_ACROSS 0,4`, `W_MIN −0,4`, `R_ACROSS 0,6` | 17,22 / 0,836 / 1,4 % | 31,44 / 0,931 | 35,15 / 0,881 |
| `ORIENT 0`, `W_ALONG 0`, `W_ACROSS 0,4`, `W_MIN −0,4` | **17,62 / 0,854** / 2,3 % | 29,97 / 0,918 (4,0 %) | 35,15 / 0,881 / 0,17 % |
| `W_ALONG 0`, `W_ACROSS 0,4`, `W_MIN −0,4`, `τ 0,02` | 17,39 / 0,843 / 1,7 % | 30,43 / 0,921 (3,7 %) | 35,33 / 0,882 |

**Verdicts.** H2a tenue : le lissage le long à ±1 texel coûte partout (−0,5 dB sur `text`, −0,9 dB
au centre de l'étalon) ; `W_ALONG` passe à 0. H2b tenue sur le texte seulement : la force que le
texte réclame (`W_MIN −0,4`, pente ×5 sur la rampe) casse l'étalon (−1,6 dB au centre, 3 % de
dépassements) et `game` ; le dosage commun est `W_ACROSS 0,2-0,4` borné à `W_MIN −0,24…−0,3`. H2c :
les axes gagnent 0,25 dB sur du texte droit mais perdent 0,5 dB sur l'étalon à force égale : le
repère des dérivées reste la base. H2d tenue : `τ` entre 0,02 et 0,05 ne change rien aux chiffres
(les quads de contour du texte passent les deux seuils).

**Ce que J2 dit du noyau.** Sur ces images, ce qui paie est l'accentuation directionnelle en
travers de la rampe bilinéaire, pas le lissage le long ; en PSNR `mwsr` dépasse FSR1 sur `game`
(35,86 contre 35,55) et le rejoint au centre de l'étalon (32,09 contre 32,26) ; en SSIM FSR1 garde
0,005 sur `game` (EASU reconstruit la structure, un masque flou ne fait qu'affûter). **Les
dépassements** sont le point dur du critère C3 (« ≤ FSR1 ») : la borne des cinq échantillons
couvre trois texels, plus large que l'empreinte 2×2 de la métrique ; `text` est à 0,6 % contre 0,3 %
pour FSR1 dès `W_ALONG 0`. D'où la borne exacte 2×2 (`DERING 1`, quatre `textureLoad` sur les quads
de contour) ajoutée pour J3.

**Suite (J3)** : force intermédiaire (`W_ACROSS 0,3`, `W_MIN −0,3`), lissage le long à un
demi-texel (`R_ALONG 0,5`), `R_ACROSS 0,75`, `τ 0,08`, puis `DERING 1`.

## 2026-10-21 — J3, le dosage, le demi-texel le long, la borne exacte (SwiftShader)

**Hypothèses.** H3a : une force intermédiaire (`W_ACROSS 0,3`, `W_MIN −0,3`) tient le texte sans
casser l'étalon. H3b : un lissage le long **à un demi-texel** (`R_ALONG 0,5`, `W_ALONG 0,1-0,2`)
aide l'étalon sans coûter au texte (contrairement à ±1 texel, J2). H3c : la borne exacte 2×2
(`DERING 1`, quatre `textureLoad` sur les quads de contour) ramène les dépassements à zéro sans
perdre de netteté. H3d : à ×1,33 (1080p → 1440p, le cas courant d'un ordinateur) le noyau vaut FSR1.

Notation : W = `W_ALONG`, R = `R_ALONG`, A = `W_ACROSS`, M = `W_MIN`, D = `DERING 1` ; `R_ACROSS 1`,
`ORIENT 1`, `τ 0,05`, netteté 1 partout. PSNR / SSIM / dépassement ; étalon : tiers central.

| variante | `text` ×2 | étalon ×2 | `game` ×2 | `zone` ×2 | `code` ×2 | étalon ×1,33 | `text` ×1,33 |
|---|---|---|---|---|---|---|---|
| bilinéaire | 16,99 / 0,816 | 31,56 / 0,928 | 35,01 / 0,880 | 10,41 / 0,423 | 29,98 / 0,977 | 36,36 / 0,977 | 20,52 / 0,923 |
| FSR1 | 17,57 / 0,845 / 0,3 % | 32,26 / 0,940 / 2,0 % | 35,55 / 0,887 / 0,03 % | 10,79 / 0,484 / 3,4 % | 30,36 / 0,975 / 0,7 % | 35,94 / 0,976 / 5,0 % | 21,45 / 0,942 / 0,2 % |
| SGSR1 | 18,13 / 0,869 / 4,4 % | 31,28 / 0,930 / 6,8 % | 35,61 / 0,883 / 0,7 % | 10,80 / 0,491 / 0 % | 29,65 / 0,974 / 2,6 % | 34,55 / 0,970 / 5,0 % | 21,64 / 0,946 / 2,7 % |
| W0 A0,3 M−0,3 | 17,40 / 0,842 / 1,1 % | 31,59 / 0,932 / 1,1 % | 35,79 / 0,882 / 0,05 % | — | — | — | — |
| W0,2 R0,5 A0,3 M−0,3 | 17,37 / 0,839 / 0,5 % | 31,99 / 0,934 / 0,2 % | 35,82 / 0,882 / 0 % | 10,64 / 0,456 / 0,03 % | 30,00 / 0,978 / 0,06 % | 36,77 / 0,979 / 0,15 % | 21,45 / 0,942 / 0,4 % |
| **W0,2 R0,5 A0,3 M−0,3 D** | 17,38 / 0,840 / **0 %** | **31,99 / 0,934** / 0 % | 35,82 / 0,882 / 0 % | 10,64 / 0,456 / 0 % | **30,00 / 0,978** / 0 % | **36,79 / 0,979** / 0 % | 21,45 / 0,942 / 0 % |
| **W0,15 R0,5 A0,35 M−0,3 D** | 17,42 / 0,842 / 0 % | 31,82 / 0,933 / 0 % | **35,87 / 0,882** / 0 % | 10,65 / 0,459 / 0 % | 29,89 / 0,978 / 0 % | 36,55 / 0,978 / 0 % | 21,60 / 0,944 / 0 % |
| W0,1 R0,5 A0,4 M−0,3 D | **17,45 / 0,844** / 0 % | 31,67 / 0,933 / 0 % | 35,87 / 0,882 / 0 % | 10,66 / 0,460 / 0 % | 29,80 / 0,978 / 0 % | 36,10 / 0,977 / 0 % | **21,74 / 0,947** / 0 % |
| W0,2 R0,5 A0,4 M−0,4 D | 17,42 / 0,843 / 0 % | 31,51 / 0,931 / 0 % | 35,84 / 0,882 / 0 % | 10,67 / 0,461 / 0 % | 29,74 / 0,978 / 0 % | 36,35 / 0,978 / 0 % | 21,65 / 0,945 / 0 % |
| W0 A0,3 M−0,3, `R_ACROSS 0,75` | 17,22 / 0,835 | 31,73 / 0,932 | 35,39 / 0,881 | — | — | — | — |
| W0 A0,3 M−0,3, `τ 0,08` | 17,38 / 0,841 | 31,58 / 0,931 | 35,77 / 0,882 | — | — | — | — |

**Verdicts.** H3a tenue (la force intermédiaire est le bon milieu : `W_MIN −0,4` perd 0,5 dB sur
l'étalon pour rien sur le texte). H3b tenue : le demi-texel le long rend +0,4 dB au centre de
l'étalon à force égale, +0,2 dB sur `game`, pour −0,03 dB sur le texte, et divise les dépassements
par deux. H3c tenue : `DERING 1` met **tous les dépassements à 0 %** sur toutes les images, les PSNR
et SSIM ne bougent pas au centième ; le critère C3 « dépassements ≤ FSR1 » est acquis par
construction. H3d dépassée : **à ×1,33 `mwsr` bat FSR1** au centre de l'étalon (+0,85 dB, SSIM
+0,003) et sur le texte (+0,15 à +0,3 dB), là où RCAS, réglé pour ×2, sort 5 % des pixels de leur
plage. `R_ACROSS < 1` et `τ 0,08` ne rapportent rien : écartés.

**Bilan contre FSR1, en l'état (J3, `W0,15 R0,5 A0,35 M−0,3 D`)** : PSNR −0,15 dB sur `text` ×2,
−0,44 au centre de l'étalon ×2, +0,32 sur `game`, −0,47 sur `code`, −0,14 sur `zone` ; +0,6 sur
l'étalon ×1,33, +0,15 sur `text` ×1,33 ; SSIM dans −0,007…+0,003 (C3 veut ≥ FSR1 − 0,005 : l'étalon
×2 à −0,007 est le seul point sous la barre, à revoir avec FLIP) ; dépassements 0 % partout. Ce qui
manque encore à ×2 est de la reconstruction (SSIM), pas de la netteté : d'où J4, la courbe bornée
sur les marches (`SHAPE 1`), qui redresse la rampe sans masque flou ni dépassement.

**Première table de P2.1 (provisoire, avant J4)** : `ORIENT 1`, `W_ALONG 0,15`, `R_ALONG 0,5`,
`W_ACROSS 0,35`, `R_ACROSS 1`, `W_MIN −0,3`, `DERING 1`, `S_FULL 1,5`, `τ 0,05`, netteté 1.

## 2026-10-21 — J4, la courbe bornée sur les marches (`SHAPE 1`), rejetée

**Hypothèse H4.** Sur une marche, remplacer le masque flou par une courbe qui pousse le pixel de la
rampe vers son plateau le plus proche, bornée par les deux plateaux (aucun dépassement, aucun
`textureLoad` de plus), redresse la rampe aussi bien que le masque et rend de la structure (SSIM).

**Mesure** (base `W0,2 R0,5 A0,3 M−0,3`, `K_SHAPE` 0,5 / 0,8 / 1,0, avec et sans D) :

| | `text` ×2 | étalon ×2 (centre) | `game` ×2 | `zone` ×2 | `code` ×2 | étalon ×1,33 | `text` ×1,33 |
|---|---|---|---|---|---|---|---|
| J3, masque flou, D | 17,38 / 0,840 | 31,99 / 0,934 | 35,82 / 0,882 | 10,64 / 0,456 | 30,00 / 0,978 | 36,79 / 0,979 | 21,45 / 0,942 |
| `SHAPE 1`, K 0,5, D | 17,07 / 0,828 | 31,61 / 0,930 | 35,92 / 0,882 | 10,57 / 0,446 | 29,78 / 0,979 | 36,06 / 0,976 | 20,77 / 0,932 |
| `SHAPE 1`, K 1,0, D | 17,06 / 0,830 | 30,95 / 0,925 | 35,85 / 0,882 | 10,61 / 0,451 | 29,47 / 0,978 | 35,27 / 0,974 | 20,88 / 0,936 |

**Verdict : rejetée.** La courbe perd 0,3 dB sur le texte et 0,4 à 1 dB au centre de l'étalon à
×2, 0,7 dB sur le texte et l'étalon à ×1,33 ; seul `game` est à égalité. Lecture : `t` est lu sur la
luma de C entre lX1 et lX2 pris à ±1 texel, c'est-à-dire sur une rampe déjà lissée par le
bilinéaire ; la poussée agit donc là où la rampe est la plus pentue et pas sur ses pieds, qu'un
masque flou étire. Sans dépassement et sans `textureLoad`, elle n'a pourtant rien à offrir contre
`DERING 1`. `SHAPE` reste à 0 ; le texte du noyau le garde jusqu'au nettoyage de P2.2 (le
compilateur élimine la branche).

## 2026-10-21 — La première table de P2.1

`ORIENT 1`, `W_ALONG 0,15`, `R_ALONG 0,5`, `W_ACROSS 0,35`, `R_ACROSS 1`, `W_MIN −0,3`, `DERING 1`,
`SHAPE 0`, `S_FULL 1,5`, `EARLY_OUT 1`, luma `(0,25, 0,625, 0,125)`, `τ 0,05`, netteté 1 : les
défauts de `MwsrShader.js` depuis ce jour. Choisie entre la variante qui sert le mieux l'étalon
(`W0,2 A0,3`) et celle qui sert le mieux le texte (`W0,1 A0,4`) : à 0,17 dB de la première sur
l'étalon, à 0,03 dB de la seconde sur le texte, et la seule des trois qui bat FSR1 à la fois sur
`game` ×2, sur l'étalon ×1,33 et sur `text` ×1,33. La netteté (uniforme `sharp`) déplace le point
entre les deux à la demande, sans recompiler. Reste à mesurer : le temps GPU (RTX, puis 780M,
iPhone, realme), le coût de `DERING 1` sur la texture externe (P2.3), FLIP.

## 2026-10-21 — Temps GPU de la table sur le RTX 5060 Ti (`--p2`)

Noyau seul (`tex`) à ×2 vers 1440p : `text` 0,051 ms, `ui` 0,029, `game` 0,028 (plancher 0,028 ;
SGSR1 0,039-0,073 ; FSR1 0,247 ; NIS 0,31-0,44) ; à ×1,33 : 0,046 et 0,036. Sans la borne exacte :
0,039 sur `text`. Sans la sortie anticipée : 0,086 partout (= neuf fetches par pixel). Sur la texture
externe, sans passe 0 : 0,043-0,096 (la borne par `textureLoad` externe y coûte 0,03 : à refaire par
échantillons, P2.3). Le détail est dans `docs/design/upscaler-mwsr-p2.md` §1.4.

## 2026-10-21 — La table sur le Mac M1 Pro (créneau de 30 min, Chrome, `metal-3`)

Mur par itération sous contre-pression (`back`) / GPU `-single` (quantum 65,5 µs), ×2 → 1440p :
`text` : `mwsr` 0,26 / 0,39 ms, plancher 0,13 / 0,13, SGSR1 0,37 / 0,46, FSR1 1,27 / 1,77, NIS
2,6 / 2,0 ; `game` : `mwsr` 0,15 / 0,13 (au plancher), SGSR1 0,37 / 0,33. Sans sortie anticipée
0,42 partout ; borne exacte +1 quantum sur le texte. **Texture externe** : `ext-mwsr` 0,61 (`text`)
contre `blit` + `mwsr` ≈ 0,42 ; 0,23 contre 0,32 sur `game` : sur Apple, passe 0 par défaut (P2.3,
`override`). Chaîne 1:1 du produit (P1) : une passe, 48 copies / 48, `draw()` 1,3-1,5 ms. Détail :
`docs/design/upscaler-mwsr-p0.md` §4 quater.

## 2026-10-21 — P2.2 ouverte (go de Bruno par 59) : J5, le palier qualité

**Hypothèse H5.** Ce qui manque à ×2 est de la reconstruction (SSIM −0,005…−0,007 contre FSR1 sur
l'étalon et `game`), pas de la netteté. Avec les **quatre coins de la grille 3×3 tournée** (`TIER 1`,
neuf échantillons sur les quads de contour), pondérés en fenêtre séparable (poids en travers × poids
le long, `W_CORNER` pour les forcer), un contour diagonal est lissé le long de lui-même et affûté en
travers comme un contour droit : SSIM remonte sur l'étalon sans perdre le texte. À mesurer :
`TIER 1` × `W_ALONG {0,15 ; 0,3}` × `R_ALONG {0,5 ; 1}` × `W_CORNER {1 ; 2}` contre la table, sur les
sept cas ; puis le coût (RTX : `tex-s9` 0,085 ms sur `text` ; M1 : 0,36 ms, au niveau de SGSR1 — le
palier qualité est un choix d'ordinateur). Outil : `mwsr:show=1` dans `--quality-cells` imprime la
part des pixels sur le chemin contour par image.

**Mesure J5** (SwiftShader ; T = `TIER 1`, K2 = `W_CORNER 2`, W/R = `W_ALONG`/`R_ALONG` ; PSNR / SSIM,
dépassements 0 % partout) :

| variante | `text` ×2 | étalon ×2 (centre) | `game` ×2 | `zone` ×2 | `code` ×2 (deux colonnes) | étalon ×1,33 | `text` ×1,33 |
|---|---|---|---|---|---|---|---|
| FSR1 | 17,57 / 0,845 | 32,26 / 0,940 | 35,55 / 0,887 | 10,79 / 0,484 | 27,35 / 0,951 | 35,94 / 0,976 | 21,45 / 0,942 |
| table (5 taps) | 17,42 / 0,842 | **31,82 / 0,933** | **35,87 / 0,882** | 10,65 / 0,459 | **26,88 / 0,956** | **36,55 / 0,978** | 21,60 / 0,944 |
| T | 17,58 / 0,850 | 31,12 / 0,929 | 35,80 / 0,882 | 10,68 / 0,463 | 26,56 / 0,955 | 36,11 / 0,977 | 21,85 / 0,949 |
| T K2 | **17,63 / 0,853** | 30,23 / 0,922 | 35,50 / 0,882 | **10,71 / 0,466** | 26,25 / 0,954 | 35,53 / 0,976 | **21,97 / 0,951** |
| T W0,3 | 17,55 / 0,849 | 31,09 / 0,929 | 35,81 / 0,882 | 10,68 / 0,463 | 26,56 / 0,955 | 36,09 / 0,977 | 21,81 / 0,948 |
| T W0,3 R1 | 17,34 / 0,842 | 30,91 / 0,926 | 35,85 / 0,882 | 10,66 / 0,459 | 26,53 / 0,955 | 35,91 / 0,976 | 21,54 / 0,943 |
| T W0,15 R1 | 17,46 / 0,846 | 31,12 / 0,929 | 35,84 / 0,882 | 10,67 / 0,461 | 26,61 / 0,955 | 36,11 / 0,977 | 21,70 / 0,946 |

Part des pixels sur le chemin contour (`SHOW 1`, `τ 0,05`) : `text` 35 %, étalon 29 % (28 % à ×1,33),
`game` 2 %, `zone` 50 %, `code` 14 %, `text` ×1,33 26 %.

**Verdict H5 : rejetée.** Les coins font gagner le texte (+0,16 à +0,37 dB, SSIM +0,007 à +0,011 ;
`T K2` **dépasse FSR1 sur le texte à ×2 et à ×1,33**, à 0 % de dépassement) mais perdent partout
ailleurs : −0,7 dB au centre de l'étalon à ×2, −0,4 à ×1,33, −0,07 à −0,37 sur `game`, −0,3 sur
`code`. Ce n'est donc pas la couverture des diagonales qui manque à l'étalon. Lecture : 71 % des
pixels de l'étalon passent **sous `τ`** et sortent en bilinéaire pur, là où EASU filtre chaque
pixel ; l'écart de SSIM sur les dégradés (peau, feuillage) serait dans ces 71 %, pas dans les
contours. D'où J6 : `τ` de 0,05 à 0 sur la table, avec la part de pixels traitée et son coût.
`TIER 1` reste une option de texte (un palier « bureau et texte » sur ordinateur), à décider en P4
par classe d'appareil ; `W_CORNER 2` est son meilleur réglage.

**FLIP sur J5** (dès ce jour : `metrics.py` passe par le venv `.flipenv`, Python 3.12 ; FLIP moyen
LDR, plus bas = mieux ; C3 : ≤ FSR1 + 0,01) :

| | `text` ×2 | étalon ×2 (image entière) | `game` ×2 |
|---|---|---|---|
| bilinéaire | 0,090 | — | — |
| FSR1 | 0,068 | 0,045 | 0,017 |
| SGSR1 | 0,067 | — | — |
| **table (5 taps)** | **0,072** | **0,052** | **0,019** |
| T | 0,073 | 0,056 | 0,019 |
| T K2 | 0,076 | 0,059 | 0,020 |

FLIP tranche contre les coins là où PSNR et SSIM les récompensaient sur le texte : la netteté
supplémentaire des diagonales n'est pas perçue comme une amélioration. **La table tient C3 en FLIP
sur les trois images** (texte 0,072 ≤ 0,078, étalon 0,052 ≤ 0,055 de justesse, `game` 0,019 ≤ 0,027) ;
`TIER 1` ne la tient pas sur l'étalon. `TIER`/`W_CORNER` vont au nettoyage avec `SHAPE`.

## 2026-10-21 — J6, le seuil du vote (`τ` de 0,05 à 0), rejeté comme explication

**Hypothèse H6.** Les 71 % de pixels de l'étalon qui passent sous `τ` sortent en bilinéaire pur ;
abaisser `τ` (jusqu'à traiter tout, `τ 0`) referme l'écart de FLIP et de SSIM avec FSR1 au prix
du temps.

| `τ` | étalon ×2 : centre PSNR / SSIM, FLIP entier, part traitée | `text` ×2 : PSNR / SSIM, FLIP, part | `game` ×2 : PSNR / SSIM, FLIP, part | étalon ×1,33 : centre, FLIP |
|---|---|---|---|---|
| FSR1 | 32,26 / 0,940, 0,045, 100 % | 17,57 / 0,845, 0,068 | 35,55 / 0,887, 0,017 | 35,94 / 0,976, 0,035 |
| 0,05 (table) | 31,82 / 0,933, 0,052, 29 % | 17,42 / 0,842, 0,072, 35 % | 35,87 / 0,882, 0,019, 2 % | 36,55 / 0,978, 0,038 |
| 0,03 | 31,84 / 0,935, 0,051 | 17,43 / 0,843, 0,071 | 35,87 / 0,882, 0,019 | 36,58 / 0,979, 0,038 |
| 0,02 | 31,85 / 0,935, 0,051, 48 % | 17,43 / 0,843, 0,071, 37 % | 35,87 / 0,882, 0,019, 4 % | 36,58 / 0,979, 0,038 |
| 0,01 | 31,85 / 0,936, 0,051, 61 % | 17,43 / 0,843, 0,071, 39 % | 35,89 / 0,883, 0,018, 19 % | 36,59 / 0,979, 0,038 |
| 0 | 31,85 / 0,936, 0,051, 100 % | 17,43 / 0,843, 0,070 | 35,90 / 0,884, 0,018 | 36,59 / 0,980, 0,038 |

**Verdict : rejetée.** Traiter tous les pixels rend 0,001 de FLIP et 0,003 de SSIM au centre de
l'étalon, rien en PSNR, pour deux fois plus de quads sur le chemin contour : l'écart avec FSR1
(FLIP 0,052 contre 0,045) tient à ce que le noyau fait **sur** les contours, pas à ce qu'il ne fait
pas à côté. `τ 0,05` reste (il tient le coût de `game` à 2 % de pixels traités ; `τ 0,01` l'enverrait à
19 % pour 0,001 de FLIP). Dernier tour sur la question de l'étalon (règle des trois tours, §8.3) :
J7, le rayon en travers (`R_ACROSS` 0,75-0,85, que l'étalon préférait en J3a) contre la force, jugé
en FLIP.

## 2026-10-21 — J7, le rayon en travers contre la force, jugé en FLIP (troisième et dernier tour sur l'étalon)

| variante (table sauf dit) | étalon ×2 : centre, FLIP | `text` ×2 : PSNR, FLIP | `game` ×2 : PSNR, FLIP | étalon ×1,33 : centre, FLIP | `text` ×1,33 : PSNR, FLIP |
|---|---|---|---|---|---|
| FSR1 | 32,26 / 0,940, 0,045 | 17,57, 0,068 | 35,55, 0,017 | 35,94 / 0,976, 0,035 | 21,45, 0,042 |
| table (`A 0,35`) | 31,82 / 0,933, 0,052 | 17,42, 0,072 | 35,87, 0,019 | 36,55 / 0,978, 0,038 | 21,60, 0,047 |
| **`W_ACROSS 0,3`** | **31,95 / 0,934, 0,051** | 17,41, **0,071** | 35,84, 0,019 | **36,76 / 0,979, 0,038** | 21,50, 0,047 |
| `R_ACROSS 0,75` | 31,83 / 0,933, 0,051 | 17,18, 0,074 | 35,52, 0,019 | 36,64 / 0,979, 0,037 | 21,31, 0,047 |
| `R_ACROSS 0,85` | 31,83 / 0,933, 0,051 | 17,29, 0,073 | 35,65, 0,019 | 36,64 / 0,979, 0,037 | 21,42, 0,047 |
| `R_ACROSS 0,75`, `A 0,45` | 31,79 / 0,933, 0,051 | 17,18, 0,074 | 35,52, 0,019 | 36,28 / 0,978, 0,037 | 21,46, 0,046 |
| `R_ACROSS 0,85`, `A 0,45` | 31,78 / 0,933, 0,051 | 17,29, 0,073 | 35,65, 0,019 | 36,14 / 0,977, 0,038 | 21,59, 0,046 |
| `R_ACROSS 0,75`, `A 0,45`, `M −0,4` | 31,18 / 0,929, 0,056 | 17,06, 0,084 | 35,27, 0,020 | 36,28 / 0,978, 0,037 | 21,46, 0,046 |
| `R_ACROSS 0,75`, `W_ALONG 0,25` | 31,86 / 0,933, 0,050 | 17,15, 0,074 | 35,52, 0,018 | 36,65 / 0,978, 0,037 | 21,23, 0,048 |

**Verdict.** Le FLIP est plat autour de la table (± 0,002 sur toutes les images) : l'écart avec
FSR1 sur l'étalon à ×2 (0,052 contre 0,045) ne se règle pas par le rayon ni par la force, il tient à
la reconstruction à douze texels d'EASU que cinq échantillons bilinéaires n'ont pas. **Trois tours
sur cette question (J5, J6, J7), on s'arrête** (règle du §8.3) ; C3 est tenu (étalon 0,052 ≤ 0,055).
Un gain net et gratuit : `W_ACROSS 0,3` au lieu de 0,35 (+0,13 dB au centre de l'étalon à ×2,
+0,2 dB à ×1,33, FLIP égal ou meilleur partout, −0,01 dB sur le texte) : **la table de P2.2 prend
`W_ACROSS 0,3`**, le reste inchangé. J8 ferme la boucle de qualité : `S_FULL` à ×1,33 et un
contrôle à ×3.

## 2026-10-21 — J8, la constante d'échelle et un contrôle à ×3 ; la table de P2.2

**`S_FULL` à ×1,33** (`W_ACROSS 0,3`) : 1,5 (force 0,66 à ×1,33) donne l'étalon au centre 36,76 /
FLIP 0,038 et le texte 21,50 / 0,047 ; 1,2 ou 1,33 (force 1) : étalon 36,08 / 0,040, texte 21,70 /
0,047 ; 2 (force 0,33) : étalon 36,92 / 0,039, texte 21,13 / 0,051. **`S_FULL 1,5` reste** : le
meilleur FLIP sur l'étalon, le texte à 0,2 dB de son meilleur.

**×3** (848×480 → 2544×1440, le 720p sur un 4K du plan §4.3) : étalon, centre 27,97 / 0,846, FLIP
0,076 contre FSR1 27,92 / 0,845 / 0,073 et SGSR1 27,67 / 0,841 / 0,076 ; `text` 15,02 / 0,723 / 0,111
contre FSR1 15,16 / 0,726 / 0,111 et SGSR1 15,45 / 0,747 / 0,099 (4,3 % de dépassements). Le noyau
tient FSR1 à ×3 sans réglage propre ; 0,3-0,4 % de dépassements y apparaissent (bords de l'image,
où les échantillons bloqués au bord et l'empreinte de la métrique divergent), rien au centre.

**La table de P2.2** (défauts de `MwsrShader.js`, le texte nettoyé des variantes rejetées) :
`W_ALONG 0,15`, `R_ALONG 0,5`, `W_ACROSS 0,3`, `R_ACROSS 1`, `W_MIN −0,3`, `DERING 1`, `S_FULL 1,5`,
`EARLY_OUT 1`, luma `(0,25, 0,625, 0,125)`, `τ 0,05`, netteté 1 ; repère des dérivées seul (la croix
sur les axes, la courbe bornée et les coins sont retirés du texte : J2, J4, J5).

**C3 en FLIP, table contre FSR1** (≤ FSR1 + 0,01) : `text` ×2 0,071 / 0,068 ; étalon ×2 0,051 /
0,045 ; `game` 0,019 / 0,017 ; `zone` 0,116 / 0,107 ; `code` 0,029 / 0,024 ; étalon ×1,33 0,038 /
0,035 ; `text` ×1,33 0,047 / 0,042 ; étalon ×3 0,076 / 0,073 ; `text` ×3 0,111 / 0,111 : **tenu
partout**, au plus serré sur `zone` (+0,009). Dépassements 0 % (hors bords à ×3). SSIM ≥ FSR1 − 0,005
tenu partout sauf l'étalon ×2 (−0,006) et `game` (−0,005, limite) ; PSNR : au-dessus de FSR1 sur
`game` ×2, l'étalon ×1,33 et le texte ×1,33, en dessous de 0,15-0,45 dB ailleurs à ×2.

## 2026-10-21 — P2.3 ouverte (go de Bruno) : J9, la texture externe contre la passe 0 sur le Mac

Créneau éclair sur le Mac M1 Pro (« commence par le Mac, Léo arrive »), cinq cas en 30 s, temps
mur par image sous contre-pression (ms), ×2 → 1440p sauf dit :

| chaîne | `text` | `game` | `ui` | `text` ×1,33 |
|---|---|---|---|---|
| passe 0 seule (`blit`) | 0,10 | 0,09 | 0,12 | 0,24 |
| noyau seul sur RGBA (`mwsr`) | 0,25 | 0,16 | 0,16 | 0,23 |
| **passe 0 + noyau (`blit+mwsr`)** | **0,32** | **0,195** | **0,195** | **0,295** |
| texture externe (`ext-mwsr`) | 0,56 | 0,21 | 0,27 | 0,57 |
| externe sans borne exacte | 0,51 | 0,20 | 0,22 | 0,38 |
| externe sans sortie anticipée | 1,12 | 1,09 | 1,07 | 1,12 |
| passe 0 + SGSR1 (chaîne livrée) | 0,43 | 0,39 | 0,26 | 0,40 |
| passe 0 + FSR1 (chaîne livrée) | 1,30 | 1,31 | 1,21 | 1,33 |

**Verdict.** Sur Apple, **la passe 0 gagne partout** (−8 % sur `game`, −26 % sur `ui`, −43 % sur le
texte, −48 % à ×1,33) : chaque échantillon de la texture externe y lit deux plans, et le chemin
contour en fait jusqu'à neuf. La décision n° 2 du plan (« pas de passe 0 en SDR ») ne tient que là où
l'échantillon externe ne coûte rien de plus que l'échantillon RGBA (NVIDIA, P0). La chaîne
passe 0 + `mwsr` vaut 0,5 à 0,75 × la chaîne SGSR1 livrée et 0,25 × FSR1 : **C1 tenue en chaîne sur
Apple**. Sans sortie anticipée, l'externe coûte 1,1 ms : la branche est vitale sur ce feed. À
confirmer sur le 780M et le RTX avec `blit+mwsr` dans les mêmes cellules ; si le gain de l'externe y
reste de l'ordre de 0,01 ms, **la passe 0 partout** est la règle la plus simple (un seul module, le
même chemin que le HDR et que le contournement 1:1).

## 2026-10-21 — P2.3, le code des couches (avant les passes 780M et RTX)

Ajouté au texte, à mesurer dès que DualRTX est rendue (« à toi » de 59) :

- **`DERING 2`** : la borne exacte par **quatre échantillons aux centres des texels** (un sampler
  linéaire rend le texel exactement là) au lieu de quatre `textureLoad` — pour la texture externe,
  où le chargement coûte deux plans et une conversion par texel (P0 : `ext-l` > `ext-s` sur AMD et
  Apple). Mêmes pixels que `DERING 1` par construction.
- **`half`** (option du module, pas un `override` : `enable f16` doit ouvrir le texte) : le chemin
  contour en f16 — lumas, cohérence, poids, mélange (`alias hf`, `alias h3`, `EPS_H 0,001` pour
  rester hors des dénormalisés) ; le vote, le repère et la borne restent en f32 ; le labo exige
  `f16=1` (fonctionnalité `shader-f16` demandée au device) et refuse la cellule sans elle.
- `QUAD_UNIFORM` : rien à écrire, la branche est uniforme par quad par construction (dérivées
  grossières) ; `subgroupAny` n'y ajouterait rien.
- `EARLY_OUT 0` reste jusqu'aux chiffres du 780M, puis sort du texte (perdant partout jusqu'ici).

Cellules `--p2` : `mwsr`, `mwsr:dering=0`, `mwsr:dering=2`, `mwsr:half=1`, `mwsr:early=0`,
`blit+mwsr`, `ext-mwsr`, `ext-mwsr:dering=0`, `ext-mwsr:dering=2`, les planchers et les références,
avec `f16=1` ; cinq cas. **Hypothèses H10** : (a) `DERING 2` ramène la borne sur l'externe au prix de
quatre échantillons (RTX : 0,096 → ~0,07 ; 780M : à voir) ; (b) `half` gagne peu sur ce noyau
(≤ 5 %, chemin borné par la texture) ; (c) sur le 780M comme sur le RTX, passe 0 + noyau et externe
sont à ± 0,02 ms, et la passe 0 partout est la règle.

## 2026-10-21 — J10, les couches sur le 780M et le RTX ; la décision « passe 0 partout »

Passes `--p2` du 21/10 (10:05-10:20), noyau seul (`tex`) et chaînes, GPU p50 en ms, ×2 → 1440p :

| cellule | 780M `text` | 780M `ui` | 780M `game` | 780M `text` ×1,33 | RTX `text` | RTX `ui` | RTX `game` | RTX `text` ×1,33 |
|---|---|---|---|---|---|---|---|---|
| plancher (`tex-s1`) | 0,385 | 0,366 | 0,365 | 0,381 | 0,028 | 0,028 | 0,028 | 0,028 |
| **`mwsr`** (table) | **1,408** | **0,652** | **0,577** | **1,233** | **0,051** | **0,029** | **0,028** | **0,046** |
| `mwsr:dering=0` | 1,107 | 0,548 | 0,496 | 0,995 | 0,039 | 0,028 | 0,028 | 0,036 |
| `mwsr:dering=2` (borne par échantillons) | 1,506 | 0,659 | 0,581 | 1,309 | 0,054 | 0,029 | 0,028 | 0,048 |
| **`mwsr:half=1`** (f16) | **1,270** | **0,590** | **0,523** | **1,125** | 0,053 | 0,029 | 0,028 | 0,047 |
| `mwsr:early=0` (sans branche) | 2,542 | 2,542 | 2,540 | 2,548 | 0,086 | 0,086 | 0,086 | 0,086 |
| **passe 0 + `mwsr`** | **1,534** | **0,761** | **0,737** | **1,510** | **0,060** | **0,037** | **0,037** | **0,068** |
| texture externe (`ext-mwsr`) | 2,361 | 0,983 | 0,920 | 2,073 | 0,096 | 0,048 | 0,043 | 0,088 |
| externe sans borne | 1,649 | 0,816 | 0,785 | 1,490 | 0,065 | 0,035 | 0,033 | 0,062 |
| externe, borne par échantillons | 2,404 | 1,037 | 0,971 | 2,154 | 0,097 | 0,047 | 0,043 | 0,088 |
| SGSR1 (noyau seul) | 2,476 | 1,116 | 2,477 | 2,131 | 0,073 | 0,039 | 0,066 | 0,066 |
| FSR1 | 9,32 | 9,35 | 9,36 | 9,37 | 0,247 | 0,243 | 0,246 | 0,247 |

Qualité sur le RTX (`--quality`, `f16=1`) : `half=1` et `dering=2` rendent **les mêmes pixels** que le
f32 au centième (texte 17,41 / 0,841 / FLIP 0,072 ; étalon 20,75 / 0,871 / 0,051).

**Verdicts H10.** (a) **La borne par échantillons ne gagne rien** (780M : +7 % sur le texte ; RTX :
égal ; externe : égal) : le surcoût de la texture externe est dans ses échantillons eux-mêmes (deux
plans), pas dans les chargements — `DERING 2` sort du texte. (b) **f16 vaut 9-10 % sur le 780M**,
rien sur le RTX (−4 %) : le module `half` est à prendre quand `shader-f16` est là, hors NVIDIA
(Apple et Adreno/Mali à mesurer, l'attendu est du même ordre ou mieux). (c) **La passe 0 gagne
partout** : 780M −20 à −35 % (1,53 contre 2,36 sur le texte), RTX −12 à −37 % (0,060 contre 0,096),
M1 −8 à −48 % (J9). La décision n° 2 du plan (« pas de passe 0 en SDR ») tenait pour un noyau à un ou
deux fetches par pixel ; avec cinq échantillons et quatre texels sur un tiers des pixels, les deux
plans de la NV12 coûtent plus que la copie RGBA qu'ils évitent. **Décision : passe 0 partout** —
`mwsr` lit `texture_2d` après la passe 0 existante du renderer, comme les autres enhancers et comme
le HDR ; un seul module, un seul bind group layout, le contournement 1:1 inchangé. Le feed `ext`
reste dans le générateur pour le labo (une future classe de GPU pourrait le remesurer). `EARLY_OUT 0`
sort du texte (perdant partout : 780M 2,54 = `tex-s9`, RTX 0,086, M1 ×2,7). Chaînes sur le 780M :
passe 0 + `mwsr` 1,53 contre passe 0 + SGSR1 ≈ 2,6-2,8 (P0) et passe 0 + FSR1 9,5 : **C2 tenue**
(× 0,16 de FSR1) ; avec `half` ≈ 1,38.

## 2026-10-21 — J11, le feed HDR tel qu'il est, et le noyau dessus (C8, qualité)

**Le feed.** Le plan (§4.1) attendait de la lumière de scène linéaire et prévoyait une compression
réversible autour du noyau. La passe 0 HDR du renderer (`tonemapPq`, courbe 2) écrit en réalité des
valeurs **encodées sRGB, prolongées au-delà de 1,0** (`linToSrgb(P * scaled)` sur un canvas
`rgba16float` en `toneMapping: extended`) : 203 nits = 1,0, 1000 nits ≈ 2,0, 10000 nits ≈ 5,3. La
dynamique est déjà compressée par la courbe ; le noyau, borné par ses propres échantillons et sans
aucun `clamp` à 1,0, tourne dessus sans changement. **La compression du plan est sans objet.**

**L'instrument.** `hdr=1` dans la page (textures et cibles `rgba16float`, image `f16:<url>`,
relecture en demi-flottants), mires de `hdr_mires.py` (rampe PQ grise et rouge, douze marches avec un
trait sombre, rayures 1000 / 0,5 nits, dégradé ; texte 0,5 sur 1000 nits, 1000 sur 10, rouge P3 sur
blanc ; l'image `game` de la page poussée à 1000 nits), vérité au format du feed, entrée réduite en
lumière linéaire (Lanczos-3), `metrics_hdr.py` : ΔE ITP (BT.2124, ICtCp depuis des nits BT.2020),
PSNR et SSIM en PQ, maximum, dépassement du max local 2×2 en nits (1 % + 0,5 nit).

Passe SwiftShader du 21/10 (les pixels y sont ceux du RTX : contrôle SDR identique à P2 au centième),
720p → 1440p :

| mire | cellule | ΔE ITP moy | p99 | centre moy | PSNR PQ | max nits (× entrée) | dépassement |
|---|---|---|---|---|---|---|---|
| rampe | bilinéaire | 34,26 | 352 | 29,4 | 17,19 | 10718 (0,972) | 0 % |
| rampe | FSR1 | **30,73** | **331** | 25,9 | **17,87** | 11348 (**1,030**) | **5,16 %** |
| rampe | SGSR1 | 41,06 | 334 | 33,1 | 17,00 | 10073 (0,914) | 1,47 % |
| rampe | **`mwsr`** | 31,32 | 352 | **23,6** | 17,70 | 10791 (0,979) | **0 %** |
| texte | bilinéaire | 15,76 | 251 | 2,26 | 22,72 | 1139 (0,887) | 0 % |
| texte | FSR1 | 16,71 | 252 | 2,61 | 22,52 | 1418 (**1,104**) | **4,81 %** |
| texte | SGSR1 | 34,51 | 267 | 3,04 | 20,24 | 1083 (0,843) | 17,65 % |
| texte | **`mwsr`** | **15,38** | 257 | **2,14** | **22,76** | 1149 (0,894) | **0 %** |
| jeu | bilinéaire | 3,65 | 22,9 | 5,24 | 38,47 | 1078 (0,872) | 0 % |
| jeu | FSR1 | 3,64 | 23,2 | 5,44 | 37,91 | 1354 (**1,095**) | 0,18 % |
| jeu | SGSR1 | 4,07 | 40,4 | 5,95 | 36,69 | 1075 (0,870) | 0,97 % |
| jeu | **`mwsr`** | **3,54** | **20,7** | **4,97** | **39,14** | 1162 (0,940) | **0 %** |

Part du chemin contour : rampe 12 %, texte 28 %, jeu 2 %. Le « max » se lit contre l'entrée : la
réduction Lanczos-3 en lumière linéaire sonne elle-même à 1285 nits sur le texte à 1000 ; `mwsr` garde
le maximum local de l'entrée et ne le dépasse jamais, FSR1 y ajoute 10 % (RCAS n'a pas de borne au-delà
de 1,0). SGSR1 écrête à 1,0 sur son chemin contour (`saturate`) : inutilisable en HDR, comme prévu.

**Verdict H11.** Qualité de C8 **tenue** : ΔE ITP moyen de `mwsr` ≤ FSR1 sur le texte (−8 %) et le jeu
(−3 %), +2 % sur la rampe (mais −9 % au centre) ; maximum conservé ; 0 % au-delà du maximum local
contre 5 % pour FSR1 ; aucun changement de texte. Le temps ajouté (C8, « ≤ FSR1 `linear` ») se mesure
sur le RTX (J14). Le module `half` sur le feed HDR (valeurs jusqu'à 5,3) : pixels à vérifier sur le RTX.

## 2026-10-21 — J12, la réduction (C10) : la lumière linéaire d'abord, la grille ensuite

Mode `down` du texte : grille k×k d'échantillons bilinéaires répartie sur l'empreinte du pixel de
sortie (1/s texel par axe), étendue `R_DOWN`, moyenne en valeurs encodées (`LINEAR_LIGHT 0`), en
gamma 2 (1 : carré, racine) ou en sRGB exact (2 : 15 `pow` par pixel à 4 taps). Référence : le
Lanczos-2 de l'hôte en lumière linéaire (`p3_flows.lanczos_linear`) ; `smooth2d` = `drawImage` de
Canvas2D en qualité `high` (le Smooth2D du critère) ; FSR1 et SGSR1 hors de leur domaine, tels que le
client les ferait tourner aujourd'hui ; `alias` = RMS de l'erreur dans l'anneau de la zone-plate dont
la fréquence dépasse la Nyquist de sortie, `flat` = RMS autour de la moyenne (le moiré lui-même),
`lines` = PSNR de la moitié des traits fins. SwiftShader, 21/10.

| échelle, image | cellule | PSNR | FLIP | alias | flat | lines |
|---|---|---|---|---|---|---|
| ×0,75 texte | bilinéaire = Smooth2D | 26,00 | 0,055 | | | |
| ×0,75 texte | FSR1 / SGSR1 | 23,51 / 23,69 | 0,050 / 0,051 | | | |
| ×0,75 texte | 2×2, encodé | 26,85 | 0,070 | | | |
| ×0,75 texte | **2×2, gamma 2** | **28,55** | 0,054 | | | |
| ×0,75 texte | 2×2, sRGB | 28,90 | 0,051 | | | |
| ×0,75 texte | 2×2 sRGB, étendue 0,75 / 0,5 | 28,47 / 27,20 | 0,050 / 0,048 | | | |
| ×0,75 code | bilinéaire / FSR1 | 32,99 / 31,37 | 0,022 / 0,017 | | | |
| ×0,75 code | 2×2 encodé / gamma 2 / sRGB | 32,95 / 33,91 / 34,08 | 0,029 / 0,023 / 0,022 | | | |
| ×0,75 zone | bilinéaire = Smooth2D | 17,67 | | 0,165 | 0,186 | 17,9 |
| ×0,75 zone | FSR1 / SGSR1 | 15,20 / 16,05 | | 0,235 / 0,209 | 0,294 / 0,255 | 15,9 / 16,5 |
| ×0,75 zone | 2×2 encodé / gamma 2 / sRGB | 18,84 / 20,37 / 20,70 | | 0,133 / 0,109 / 0,104 | 0,105 / 0,103 / 0,104 | 19,1 / 20,9 / 21,3 |
| ×0,5 texte | bilinéaire (= boîte 2×2) | 24,77 | 0,077 | | | |
| ×0,5 texte | FSR1 / SGSR1 | 22,95 / 22,04 | 0,063 / 0,070 | | | |
| ×0,5 texte | **2×2, gamma 2** | **30,63** | **0,015** | | | |
| ×0,5 texte | 2×2, sRGB | 30,76 | 0,025 | | | |
| ×0,5 texte | 3×3 encodé / gamma 2 | 26,35 / 31,46 | 0,093 / 0,054 | | | |
| ×0,5 code | bilinéaire / FSR1 | 30,58 / 29,01 | 0,031 / 0,024 | | | |
| ×0,5 code | 2×2 gamma 2 / sRGB | 32,79 / 32,99 | 0,012 / 0,008 | | | |
| ×0,5 zone | bilinéaire = Smooth2D | 16,86 | | 0,166 | 0,143 | 17,0 |
| ×0,5 zone | FSR1 | 14,41 | | 0,266 | 0,270 | 16,6 |
| ×0,5 zone | 2×2 gamma 2 / sRGB | 20,16 / 20,32 | | 0,094 / 0,090 | 0,133 / 0,134 | 18,8 / 18,9 |
| ×0,5 zone | 3×3 encodé / gamma 2 | 18,23 / 20,46 | | 0,141 / 0,104 | 0,091 / 0,088 | 18,7 / 20,7 |
| ×0,33 texte | bilinéaire / FSR1 / SGSR1 | 19,5 / 19,5 / 19,4 | 0,096 / 0,095 / 0,097 | | | |
| ×0,33 texte | Smooth2D | 25,98 | 0,099 | | | |
| ×0,33 texte | 3×3 encodé / gamma 2 / sRGB | 24,93 / 31,80 / 31,81 | 0,085 / 0,014 / 0,022 | | | |
| ×0,33 texte | **3×3 étendue 1,25, gamma 2** / sRGB | **35,07** / 34,52 | 0,034 / 0,024 | | | |
| ×0,33 texte | 4×4 encodé / sRGB | 25,55 / 33,29 | 0,093 / 0,030 | | | |
| ×0,33 zone | bilinéaire / FSR1 | 9,8 / 9,6 | | 0,367 / 0,375 | 0,354 / 0,363 | 10,6 / 10,2 |
| ×0,33 zone | Smooth2D | 16,67 | | 0,148 | 0,078 | 16,3 |
| ×0,33 zone | 3×3 encodé / gamma 2 / sRGB | 16,09 / 20,30 / 20,63 | | 0,147 / 0,058 / 0,052 | 0,085 / 0,076 / 0,075 | 15,3 / 18,0 / 18,3 |
| ×0,33 zone | **3×3 étendue 1,25, gamma 2** / sRGB | 22,65 / **23,69** | | 0,068 / 0,056 | **0,043** / 0,044 | 22,5 / 23,3 |
| ×0,33 zone | 4×4 sRGB / 4×4 étendue 1,25 sRGB | 20,02 / 20,40 | | 0,084 / 0,101 | 0,063 / 0,041 | 19,0 / 21,2 |

**Verdicts H12.** (a) **La lumière linéaire est le terme décisif** : +2,5 dB (×0,75) à +6 dB (×0,5) et
+12 dB (×0,33 contre le bilinéaire) sur le texte, FLIP divisé par trois à cinq ; la moyenne en valeurs
encodées assombrit toute structure fine (le gris moyen d'un damier sort à 0,5 encodé au lieu de 0,73),
ce que PSNR et FLIP sanctionnent tous deux. Le sRGB exact gagne ≤ 0,35 dB sur le gamma 2 et perd en
FLIP sur le texte ×0,5 : **`LINEAR_LIGHT 1` par défaut**, 2 en option là où quinze `pow` ne coûtent
rien (classe d'ordinateur, J14). (b) **Grilles** : à ×0,5 la grille 2×2 à l'étendue 1 est la boîte
exacte, et FLIP la préfère nettement à la 3×3 (0,015 contre 0,054 sur le texte) ; en dessous de ×0,5,
la 3×3 à l'étendue 1,25 vaut +2,7 dB sur le texte et −40 % de moiré (`flat` 0,044 contre 0,076) à FLIP
égal ; la 4×4 ne fait pas mieux à ×0,33 (régime < ×0,25 non mesuré, laissé au plan). **Table :
0,5 ≤ s < 1 → 2×2, étendue 1 ; 0,25 ≤ s < 0,5 → 3×3, étendue 1,25 ; s < 0,25 → 4×4, étendue 1,25**
(`MWSR_DOWN_TAPS`, `mwsrRegime()`). (c) Contre le critère : énergie d'aliasing ≤ Smooth2D partout
(×0,33 : 0,068 contre 0,148 ; `flat` 0,043 contre 0,078) ; PSNR contre le Lanczos-2 de l'hôte 28,5 à
35 dB sur le texte ; le coût (« ≤ bilinéaire + 30 % ») se mesure au RTX et au 780M (J14). FSR1 et SGSR1
en réduction sont pires que le bilinéaire (FSR1 : 27 à 50 % de dépassements à ×0,33) : c'est ce que le
client WebGPU fait aujourd'hui avec un enhancer allumé sur un flux plus grand que l'écran.

## 2026-10-21 — J13, le 4:4:4 (C9) et la netteté par la luma

Mires `code` (syntaxe VS Code sur fond sombre) et `codeiso` (les mêmes couleurs ramenées à une luma
Rec. 709 à 5/255 près du fond : chaque contour n'est qu'un contour de chroma), entrée brute (4:4:4) et
la même passée par un pipeline 4:2:0 simulé (`p3_flows.chroma_420` : Y'CbCr 709, chroma moyennée 2×2 et
remontée en bilinéaire, ce que fait l'import du navigateur). `CHROMA444 1` : vote sur le canal à la
pente la plus forte, cohérence et poids par canal. `LUMA_SHARP 1` (expérience) : netteté par la luma
seule, teinte des échantillons conservée. ×2, 720p → 1440p, SwiftShader.

| mire, entrée | cellule | FLIP | PSNR Cb / Cr | ΔE00 moy / p99 | dépassement | part contour |
|---|---|---|---|---|---|---|
| code 4:4:4 | bilinéaire | 0,034 | 36,94 / 37,69 | 1,653 / 23,7 | 0 % | |
| code 4:4:4 | SGSR1 | 0,026 | 36,98 / 37,73 | **1,563** / 24,2 | 4,92 % | |
| code 4:4:4 | FSR1 | **0,024** | **37,07** / 37,70 | 1,626 / **23,6** | 1,30 % | |
| code 4:4:4 | `mwsr` | 0,029 | 36,62 / 37,21 | 1,614 / 24,6 | 0 % | 14,4 % |
| code 4:4:4 | `mwsr:c444=1` | 0,029 | 36,58 / 37,18 | 1,623 / 24,7 | 0 % | 16,0 % |
| code 4:4:4 | `mwsr:lsharp=1` | 0,030 | 36,79 / 37,49 | 1,568 / 24,5 | 0 % | |
| code 4:2:0 | bilinéaire | 0,040 | 34,64 / 35,59 | 1,827 / 24,3 | 0 % | |
| code 4:2:0 | SGSR1 | 0,032 | 34,66 / 35,61 | 1,748 / 25,4 | 1,11 % | |
| code 4:2:0 | FSR1 | **0,031** | **34,79** / 35,73 | 1,743 / 24,3 | 1,17 % | |
| code 4:2:0 | `mwsr` | 0,036 | 34,70 / 35,65 | 1,742 / 25,3 | 0 % | 14,5 % |
| code 4:2:0 | `mwsr:c444=1` | 0,036 | 34,71 / 35,66 | 1,737 / 25,2 | 0 % | 15,5 % |
| code 4:2:0 | `mwsr:lsharp=1` | 0,036 | 34,76 / 35,69 | **1,716** / 25,2 | 0 % | |
| codeiso 4:4:4 | bilinéaire = SGSR1 = `mwsr` | 0,014 | 34,57 / 40,62 | 0,752 / 13,0 | 0 % | 0,1 % |
| codeiso 4:4:4 | FSR1 | **0,008** | **34,94 / 42,00** | **0,654** / 11,4 | 1,16 % | |
| codeiso 4:4:4 | **`mwsr:c444=1`** | 0,011 | 34,55 / 41,39 | 0,675 / 12,1 | 0 % | 11,7 % |
| codeiso 4:2:0 | bilinéaire = `mwsr` | 0,027 | 30,17 / 36,17 | 1,415 / 24,6 | 0 % | 0,0 % |
| codeiso 4:2:0 | FSR1 | 0,026 | 30,38 / 36,37 | 1,359 / 24,3 | 0,15 % | |
| codeiso 4:2:0 | `mwsr:c444=1` | 0,026 | 30,30 / 36,28 | 1,367 / 24,6 | 0 % | 8,5 % |

Réglages de `c444` essayés sur `codeiso` (netteté 1,5 et 2, τ 0,03, `W_ACROSS` 0,45, `W_ALONG` 0,25) :
ΔE00 0,665 à 0,680, FLIP 0,010 à 0,011 — aucun levier, la table reste celle de la luma. Contrôle SDR à
×2 (texte, jeu, code) : le défaut rend les pixels de P2 au centième ; `c444` y vaut +0,08 dB sur le
texte (la mire a des parties colorées) et rien ailleurs ; `lsharp` −0,06 dB et FLIP +0,002 sur le texte,
rien sur le jeu et le code ; en HDR (J11) `lsharp` perd 0,1 à 0,2 % de ΔE ITP sur les trois mires.

**Verdicts H13.** (a) Sur la syntaxe colorée ordinaire, la luma voit déjà chaque contour : le vote par
canal n'y change pas les pixels (14,4 → 16,0 % de quads, mêmes chiffres) ; le défaut égale FSR1 en
ΔE00 (4:4:4 1,614 contre 1,626 ; 4:2:0 1,742 contre 1,743), reste 0,1 à 0,4 dB sous lui en PSNR de
chroma, et seul des cinq sans dépassement. (b) Sur les bords **isoluminants**, le cas que le mode
existe pour servir, la luma seule ne voit rien (0,1 % de quads, `mwsr` = bilinéaire = SGSR1) ; `c444`
les voit (11,7 %) : FLIP 0,014 → 0,011, ΔE00 −10 %, SSIM 0,9751 → 0,9785, un cheveu derrière FSR1
(0,675 contre 0,654 ; FSR1 affûte tous les canaux par construction, et dépasse 1,2 %). **C9 : mieux que
SGSR1 partout, égal à FSR1 en ΔE00 sur la syntaxe, derrière lui de 3 % sur les bords isoluminants** :
tenu pour la première moitié, à 3 % près pour la seconde. `CHROMA444` reste dans le texte, pour les flux
4:4:4 (`StreamView._yuv444`, P4) ; coût nul quand il est à 0 (constante de pipeline), à mesurer à 1
(J14). (c) **`LUMA_SHARP` sort du texte** : −0,03 de ΔE00 sur la chroma (marginal), perdant en luma SDR
et en HDR.

## 2026-10-21 — J14, les temps de P3 sur le 780M et le RTX (C8 temps, C10 coût, `c444`)

Passes `--p3` du 21/10 (13:55-13:58, créneau de 59 ; Chrome sans tête épinglé par `--use-adapter-luid`,
adaptateur vérifié par une sonde avant chaque passe : `NVIDIA GeForce RTX 5060 Ti`, `AMD Radeon(TM)
Graphics` RDNA 2), GPU p50 en ms, trois tours. Le feed HDR : textures et cible `rgba16float`, mire
`text` de `hdr_mires.py` chargée en demi-flottants ; les réductions en SDR (`bgra8unorm`).

**Feed HDR (C8 temps)**, 720p → 1440p et 1080p → 1440p :

| cellule | 780M ×2 | 780M ×1,33 | RTX ×2 | RTX ×1,33 |
|---|---|---|---|---|
| plancher (`tex-s1`, cible f16) | 0,642 | 0,860 | 0,056 | 0,056 |
| FSR1 | 9,51 | 9,57 | 0,247 | 0,250 |
| SGSR1 | 2,502 | 2,404 | 0,069 | 0,066 |
| **`mwsr`** | **1,517** | **1,390** | **0,058** | **0,057** |
| `mwsr:half=1` | 1,387 | 1,292 | 0,058 | 0,057 |

Pixels sur le RTX (`--hdr-quality`, f16) : `half=1` rend ceux du f32 au millième de ΔE ITP (rampe
31,335 contre 31,325 ; texte 15,376 = 15,376 ; jeu 3,541 contre 3,540), même maximum en nits (10791 /
1156 / 1159), 0 % de dépassement ; et le RTX rend les chiffres de SwiftShader (J11) au centième.

**Réduction (C10 coût)**, `text` ×0,75 (1440p → 1080p), `text` ×0,5 (1440p → 720p), `zone` ×0,33
(2160p → 720p) ; `mwsr` = la table de J12 (2×2 étendue 1, 2×2 étendue 1, 3×3 étendue 1,25 ;
`LINEAR_LIGHT 1`) :

| cellule | 780M ×0,75 | 780M ×0,5 | 780M ×0,33 | RTX ×0,75 | RTX ×0,5 | RTX ×0,33 |
|---|---|---|---|---|---|---|
| plancher (`tex-s1`) | 0,246 | 0,145 | 0,336 | 0,016 | 0,007 | 0,016 |
| FSR1 | 5,284 | 2,364 | | 0,136 | 0,062 | |
| SGSR1 | 1,284 | | | 0,039 | | |
| **`mwsr`** (table) | **0,541** | **0,246** | **1,059** | **0,022** | **0,009** | **0,026** |
| `mwsr:ll=2` (sRGB exact) | 1,585 | 0,709 | 1,426 | 0,048 | 0,021 | 0,040 |
| `mwsr:taps=9` | 1,174 | 0,559 | (table) | 0,048 | 0,022 | (table) |
| `mwsr:taps=16` | | 0,920 | 1,692 | | 0,037 | 0,039 |
| `mwsr:rdown=1.25;ll=2` | | | 1,425 | | | 0,040 |
| **`mwsr` / plancher** | **2,20** | **1,69** | **3,15** | **1,40** | **1,30** | **1,66** |

**4:4:4**, `code` ×2 (720p → 1440p) : 780M plancher 0,379, `mwsr` 0,899, `mwsr:c444=1` 0,964 (+7 %) ;
RTX 0,028, 0,039, 0,042 (+8 %).

**Verdicts H14.** (a) **C8 temps : tenu**, largement : sur le feed HDR `mwsr` vaut 0,16 × FSR1 sur le
780M (1,52 contre 9,51 ms) et 0,23 × sur le RTX (0,058 contre 0,247). La cible f16 coûte à tous
+0,26 ms sur le 780M (plancher 0,385 → 0,642) et le noyau y ajoute les mêmes ≈ 0,9 ms qu'en SDR ; sur
le RTX l'écriture f16 cache le noyau (plancher +2 %). `half` : −9 % sur le 780M, comme en SDR, et les
mêmes pixels jusqu'à 10 000 nits (valeurs ≤ 5,3) : le module f16 est sûr sur le feed HDR. Il reste à
C8 le regard de Bruno sur le M27Q (P5). (b) **`c444=1` : +7-8 %** sur les deux GPU (cohérence et
poids par canal sur le chemin contour) : à prendre quand `StreamView._yuv444` l'annonce, coût nul
sinon (constante de pipeline, chemin supprimé). (c) **C10 coût : non tenu** à la lettre (« ≤
bilinéaire + 30 % ») : 2,20 / 1,69 / 3,15 × le plancher sur le 780M, 1,40 / 1,30 / 1,66 × sur le RTX,
le seul point dans le critère étant le RTX à ×0,5. En absolu : 0,54 ms pour une sortie 1080p sur le
780M (3 % d'une image à 60 Hz), 1,06 ms pour 720p depuis 2160p en 3×3. Lecture : le plancher est un
seul échantillon bilinéaire ; la grille en fait k² et y ajoute le décodage et l'encodage, et sur le
780M cette arithmétique pèse autant que les échantillons (`ll=2` ×2,9, le 4×4 ×3,7 pour rien, J12).
Contre ce que le client fait aujourd'hui en réduction avec un enhancer allumé : FSR1 5,28 ms (×10) et
SGSR1 1,28 (×2,4), tous deux pires que le bilinéaire en qualité (J12). `LINEAR_LIGHT 2` sort des
défauts partout (×2,2 même sur le RTX) et reste une option. (d) **Piste J15, les vues sRGB** : une vue
`rgba8unorm-srgb` sur la texture de la passe 0 et une vue `bgra8unorm-srgb` sur le canvas font
décoder par l'échantillonneur et encoder par la cible, gratuitement et en sRGB exact (`ll=0` au prix de
`ll=0`) ; et l'unique échantillon bilinéaire du plancher devient lui-même un filtre en lumière
linéaire — à ×0,5 exactement c'est la boîte 2×2 de la table, pixel pour pixel, au coût du plancher.
Qualité sur SwiftShader en J15 ; coût au 780M au prochain créneau. La décision sur le coût de C10
appartient à Bruno (porte P3, rapport §6).

## 2026-10-21 — J15, la réduction par vues sRGB : la lumière linéaire faite par le matériel

Idée : une vue `rgba8unorm-srgb` sur la texture d'entrée et une vue `bgra8unorm-srgb` sur la cible
font décoder chaque texel par l'échantillonneur *avant* le filtrage, et encoder le résultat à
l'écriture — sRGB exact, sans une instruction, sur tout GPU WebGPU (`viewFormats` à la création de la
texture et dans la configuration du canvas). Dans le labo (SDR) : `viewFormats` sur `inTex` et
`outTex`, cellules `tex-s1-srgb` (le bilinéaire du plancher à travers les vues) et `mwsr:srgb=1` (la
grille à travers les vues, `LINEAR_LIGHT 0`). SwiftShader, contre le Lanczos-2 de l'hôte en lumière
linéaire (la référence de C10), mires `text` et `zone` ; `mwsr` = la table de J12 (lumière
logicielle, `ll=1`) ; `mwsr:ll=2` = la même grille en sRGB exact *calculé* (décodage après chaque
échantillon bilinéaire).

| échelle, mire | cellule | PSNR | FLIP | ΔE00 | alias | flat |
|---|---|---|---|---|---|---|
| ×0,75 texte | bilinéaire = Smooth2D | 26,00 | 0,055 | 1,810 | | |
| ×0,75 texte | **bilinéaire, vues sRGB** | **30,18** | 0,027 | **1,060** | | |
| ×0,75 texte | `mwsr` 2×2 (`ll=1`) | 28,55 | 0,054 | 1,446 | | |
| ×0,75 texte | `mwsr:ll=2` | 28,90 | 0,051 | 1,365 | | |
| ×0,75 texte | 2×2, vues sRGB | 27,19 | **0,020** | 1,195 | | |
| ×0,75 zone | bilinéaire = Smooth2D | 17,67 | 0,204 | 7,76 | 0,165 | 0,186 |
| ×0,75 zone | **bilinéaire, vues sRGB** | **22,57** | 0,055 | 3,94 | 0,074 | 0,159 |
| ×0,75 zone | `mwsr` 2×2 (`ll=1`) | 20,37 | 0,210 | 6,12 | 0,109 | 0,103 |
| ×0,75 zone | `mwsr:ll=2` | 20,70 | 0,200 | 5,82 | 0,104 | 0,104 |
| ×0,75 zone | 2×2, vues sRGB | 21,98 | **0,036** | **3,38** | **0,037** | **0,093** |
| ×0,5 texte | bilinéaire = Smooth2D | 24,77 | 0,077 | 2,288 | | |
| ×0,5 texte | **bilinéaire, vues sRGB** = 2×2 vues = `ll=2` | **30,76** | 0,025 | **1,051** | | |
| ×0,5 texte | `mwsr` 2×2 (`ll=1`) | 30,63 | **0,015** | 1,083 | | |
| ×0,5 zone | bilinéaire = Smooth2D | 16,86 | 0,213 | 9,89 | 0,166 | 0,143 |
| ×0,5 zone | **bilinéaire, vues sRGB** = 2×2 vues = `ll=2` | **20,32** | **0,030** | **5,07** | **0,090** | 0,134 |
| ×0,5 zone | `mwsr` 2×2 (`ll=1`) | 20,16 | 0,068 | 5,46 | 0,094 | 0,133 |
| ×0,33 texte | bilinéaire (= vues sRGB : échantillon ponctuel) | 19,48 | 0,096 | 3,60 | | |
| ×0,33 texte | Smooth2D | 25,98 | 0,099 | 1,94 | | |
| ×0,33 texte | `mwsr` 3×3 (`ll=1`) | **35,07** | 0,034 | 0,683 | | |
| ×0,33 texte | `mwsr:ll=2` | 34,52 | 0,024 | 0,736 | | |
| ×0,33 texte | **3×3, vues sRGB** | 32,23 | **0,022** | 0,685 | | |
| ×0,33 texte | 2×2, vues sRGB | 29,46 | 0,043 | 1,49 | | |
| ×0,33 zone | bilinéaire (= vues sRGB) | 9,77 | 0,173 | 21,8 | 0,367 | 0,354 |
| ×0,33 zone | Smooth2D | 16,67 | 0,299 | 10,8 | 0,148 | 0,078 |
| ×0,33 zone | `mwsr` 3×3 (`ll=1`) | 22,65 | 0,197 | 5,16 | 0,068 | 0,043 |
| ×0,33 zone | `mwsr:ll=2` | 23,69 | 0,168 | 4,32 | 0,056 | 0,044 |
| ×0,33 zone | **3×3, vues sRGB** | **30,04** | **0,018** | **1,27** | **0,013** | **0,039** |
| ×0,33 zone | 2×2, vues sRGB | 16,86 | 0,072 | 8,13 | 0,107 | 0,113 |

**Verdicts H15.** (a) **Décoder avant de filtrer n'est pas décoder après** : `ll=2` décode des
échantillons bilinéaires déjà mélangés en gamma, les vues décodent les texels. À ×0,5 les deux
coïncident (les échantillons tombent sur les texels : 30,76 dB, pixel pour pixel) ; dès que les
échantillons sont entre les texels (×0,75, ×0,33) le mélange en gamma à l'intérieur de chaque
échantillon biaise la moyenne : sur la zone-plate à ×0,33, 23,7 dB (`ll=2`) contre 30,0 (vues),
aliasing 0,056 contre 0,013, FLIP 0,168 contre 0,018. (b) **Entre ×0,5 et 1:1, un seul échantillon
bilinéaire à travers les vues suffit** : à ×0,75 il bat toutes les grilles en PSNR (30,18 contre
27,2-28,9 sur le texte, 22,57 contre 20,4-22,0 sur la zone), divise le FLIP du plancher par deux
(0,027 ; 0,055 contre 0,204 sur la zone) et l'aliasing par 2,2 (0,074 ≤ Smooth2D 0,165) ; la 2×2 à
travers les vues fait un peu mieux en FLIP et en aliasing (0,020 ; 0,037) pour 3 dB de moins et
quatre échantillons. À ×0,5 c'est la boîte exacte de la table. **Son coût est le plancher** (c'est
l'échantillon que le client fait déjà) : **C10 (×0,75, ×0,5) tenu en coût (+0 %) et en qualité**,
mieux qu'avec la grille de J12. *Confirmé au GPU (passe de 14:33, trois tours) : les vues ne coûtent
rien (`tex-s1-srgb` = `tex-s1` : 780M 0,241 = 0,241 ms à ×0,75, RTX 0,016 = 0,016) et l'échantillon
unique vaut le plancher (780M 0,250 à ×0,75, 0,146 = 0,146 à ×0,5 ; RTX 0,016 et 0,007, égaux) ; la
2×2 à travers les vues coûte sa grille calculée (0,539 = 0,539), comme `ll=0` : sur le 780M aussi, ce
sont les échantillons qui coûtent, pas le carré et la racine ; seul le `pow` de `ll=2` pèse.* (c) **Sous ×0,5, la 3×3 à travers les vues** : sur la zone-plate
+7,4 dB, aliasing ÷ 5 et FLIP ÷ 11 contre la même grille en lumière logicielle ; sur le texte −2,8 dB
de PSNR (32,2 contre 35,1) mais FLIP 0,022 contre 0,034 à ΔE00 égal — l'œil (FLIP) la préfère, le
PSNR non ; la 2×2 n'y suffit pas (16,9 dB sur la zone). Neuf échantillons sans arithmétique : au GPU
(14:33) la 3×3 à travers les vues coûte sa grille calculée (780M 1,041 contre 1,058 ms ; RTX 0,026
les deux), 3,1 × le plancher ; le plancher y est un échantillon ponctuel (9,8 dB sur la zone,
inutilisable), le « + 30 % » n'y a pas de sens, la référence utile est Smooth2D, que le navigateur
paie sur le CPU. (d) **Décisions** : `MWSR_DOWN_TAPS_SRGB = [[0,5, 1, 1], [0,25, 9, 1,25], [0, 16,
1,25]]`, `mwsrRegime(…, { srgb: true })` rend `taps 1` jusqu'à ×0,5 et `linearLight 0` ; la table
logicielle (`MWSR_DOWN_TAPS`, 2×2 d'abord, `LINEAR_LIGHT 1`) reste pour les feeds sans vue sRGB (le
`rgba16float` du HDR, un navigateur qui refuserait `viewFormats`). Ce que le renderer fait en P4 :
`viewFormats: ['rgba8unorm-srgb']` sur la texture de la passe 0, `viewFormats: [format + '-srgb']`
dans la configuration du canvas SDR, et en mode `down` lier la vue sRGB de l'entrée et dessiner dans
la vue sRGB du canvas (`getCurrentTexture().createView({ format })`) ; en mode `up` et à 1:1 les vues
ordinaires (le noyau travaille en gamma, à dessein). Dorés : `mwsr:srgb=1` ajouté aux trois cas de
réduction, `golden.json` régénéré.

## 2026-10-21 — J16, le realme Note 60 (Mali-G57 MP1, Chrome Android) : le reste de P0, `mwsr`, la réduction

Créneau de Bruno (14:26-14:31) : la page servie en HTTPS depuis DualRTX (`scaler_lab.py --serve
0.0.0.0:8766 --tag realme`, huit liens courts, résultats `bench-out/scaler-lab/phone-realme-*.json` ;
aucune règle de pare-feu à ouvrir : les règles entrantes « Python » existantes couvrent le port sur le
profil public du LAN). Adaptateur `arm valhall` (WebGPU de Chrome Android) ; `shader-f16`,
`timestamp-query`, `subgroups`, `float32-filterable`, `texture-formats-tier2`, ASTC et ETC2 ; décodeur
H.264 matériel à **sortie RGBA** (pas NV12 : un échantillon de la texture externe vaut un échantillon
RGBA, `ext-s1` = `tex-s1`). Les horodatages d'un lot s'additionnent (×2 texte : lot 18,0 ms contre
18,6 à une soumission par itération ; plancher 3,6 contre 4,8) : lecture directe, les cas `-single`
sont inutiles ici. Dispersion ≤ 2 % sauf les cellules sous 2 ms (p90 jusqu'à ×1,6 : bruit d'horloge).
2 tours × 4 lots × 8 itérations. GPU p50 en ms :

| cas | plancher | externe `ext-s1` | SGSR1 | FSR1 | **`mwsr`** | `half=1` | `dering=0` | passe 0 + `mwsr` |
|---|---|---|---|---|---|---|---|---|
| ×2 texte, 720p → 1440p | 3,60 | 3,67 | 24,5 | 83,1 | **18,0** | 16,3 | 13,2 | 18,8 (0,92 + 17,9) |
| ×2 jeu | 3,67 | 3,74 | 23,3 | 83,1 | **8,78** | 7,60 | 7,41 | 9,90 (0,92 + 9,11) |
| ×1,33 texte, 1080p → 1440p | 3,60 | | 22,5 | 83,2 | **16,7** | 14,9 | 12,3 | |
| **l'écran du realme** : texte, 720p → 1600×720 | 1,18 | 1,25 | 8,39 | 26,0 | **5,90** | 5,31 | 4,33 | 6,75 (0,85 + 5,96) |

Chaînes du produit à ×2 texte (`chain-*`, une image par tampon) : `off` 4,00 ms de GPU pour 22,3 ms de
mur par `draw()` ; SGSR1 26,3 / 46,7 ; FSR1 85,3 / 110.

Réduction, texte :

| cas | plancher | plancher, vues sRGB | `mwsr` 2×2 (`ll=1`) | **un échantillon, vues sRGB** | 2×2, vues sRGB | `ll=2` | FSR1 |
|---|---|---|---|---|---|---|---|
| ×0,67, 1080p → 1600×720 | 1,31 (p90 2,10) | 1,18 | 3,80 | **1,18** | 3,80 | | 26,0 |
| ×0,5, 1440p → 720p | 1,05 | 0,92 | 3,08 | **0,98** | | 8,06 | |

**Verdicts H16.** (a) **C1 tenue sur le Mali** : noyau seul `mwsr` / SGSR1 = 0,74 (texte ×2), 0,38
(jeu), 0,74 (texte ×1,33), 0,70 (écran) — le critère dit ≤ 0,9 noyau seul, ≤ 0,8 jeu, ≤ 1,0 texte ;
avec `half` 0,66 / 0,33 / 0,66 / 0,63 ; chaîne passe 0 + `mwsr` 18,8 contre la chaîne SGSR1 26,3
(0,72). C2 au passage : 0,22 × FSR1. (b) À la taille de l'écran (1600×720), passe 0 + `mwsr` =
6,75 ms par image (`half` ≈ 6,1, `dering=0` ≈ 5,2) : tient dans les 11,1 ms d'une image à 90 Hz,
large à 60 Hz ; la chaîne SGSR1 y fait 9,6 ms et FSR1 27 ms — ce que le client propose aujourd'hui sur
ce téléphone fait tomber des images. (c) `half` vaut 10 % sur Mali, comme sur le 780M : à prendre ;
`dering=0` vaut 27 % sur le texte et 16 % sur le jeu : le cran performance existe. (d) **La passe 0
coûte 0,85-0,92 ms** à 720p sur ce téléphone, où la texture externe est déjà RGBA (`ext-s1` =
`tex-s1`) : 13-15 % du noyau à la taille de l'écran ; `ext-mwsr` n'était pas dans les liens (à mesurer
au prochain créneau : si l'externe RGBA vaut la texture, la passe 0 est évitable sur cette famille
d'Android, décision P4 par `frames.format`). (e) **Réduction : l'échantillon unique à travers les vues
sRGB coûte le plancher** (1,18 = 1,18 à ×0,67 ; 0,98 contre 0,92-1,05 à ×0,5) : **C10 tenu en coût sur
la machine du critère** (« téléphone 1080p ») ; la grille 2×2 coûte 3,2 × le plancher, vues ou pas (sur
Mali ce sont les échantillons, pas l'arithmétique), `ll=2` 7,7 ×. (f) Le mur par `draw()` de la chaîne
`off` (22 ms pour 4 ms de GPU) dit le coût CPU du chemin du renderer sur ce SoC (Unisoc T612) : à
regarder en P5 sur le flux réel, hors shader.

## 2026-10-21 — J17, P4.1 : la clé `mwsr` du `WebGpuRenderer`, et la chaîne du produit dans les dorés

Intégration (commit `e965e9f9`) : un pipeline par régime (`mwsrRegimes()`), tous à la création ;
`viewFormats` `-srgb` sur le canvas SDR (`ctx.configure`, dans un `try` : repli lumière logicielle) et
sur `_inputTex` ; la réduction lit la vue sRGB de l'entrée et dessine dans celle du canvas, le noyau les
vues ordinaires ; `half` demandé à la création du device hors NVIDIA ; `CHROMA444` depuis `yuv444`
(StreamView, worker). Labo : cellule `chain-mwsr` (le renderer du produit sur un `OffscreenCanvas`) ;
captures et dumps des chaînes par `transferToImageBitmap` → `copyExternalImageToTexture` vers la
texture de sortie ; avec `codec=none` les chaînes reçoivent des `VideoFrame` RGBA faites des mires (les
texels exacts de la texture d'entrée du labo) ; `dump=all` n'exclut plus les chaînes.

Dorés (SwiftShader, `--golden`, 22 s les neuf cas) : `chain-mwsr` = `mwsr` au bit près sur les six cas
d'agrandissement (texte, jeu, code ; 720p et 1080p → 1440p) et = `mwsr:srgb=1` sur les trois cas à
×0,5 ; les empreintes des autres cellules inchangées (`mwsrRegimes()` n'a pas touché au texte).
Trois tours pour y arriver : la chaîne ne tournait pas (« no VideoFrame » : les mires ne faisaient pas
d'images sans codec), puis n'était pas dumpée (`dump=all` filtrait les chaînes). Verdict H17 : **le
produit fait ce que la cellule faisait**, vues sRGB comprises ; le coût des vues a été confirmé au GPU
en J15.

## 2026-10-21 — J18, P4.2 : le jumeau GLSL (`gl-mwsr`) et la conformité des jumeaux (C11)

`mwsrGlsl()` depuis les mêmes tables (overrides en constantes, deux `vec4` d'uniformes, `mediump` sur
le chemin contour), `WebGlRenderer` sous `gl-mwsr`. Au labo : `chain-gl-<passe>` (jeu `glchain`) ;
mais forcer ANGLE sur SwiftShader (`--use-gl=angle --use-angle=swiftshader`) éteint WebGPU dans ce
Chrome (`requestAdapter` rend `null`, avec ou sans `--ignore-gpu-blocklist`,
`--use-webgpu-adapter=swiftshader`), et sans ce forçage WebGL partait sur le GPU par défaut de la
machine (interdit sans go) et, de plus, **pendait** : une promesse jamais résolue a retenu le pilote dix
minutes (`Runtime.evaluate` avec `awaitPromise`). D'où : un mode `gl=1` de la page (WebGL2 seul, dumps
par un canvas 2D, mêmes noms de fichiers), `--webgl-swiftshader` au pilote (un second Chrome), le flux
`--conformity` qui compare ce qui est sur le disque, et le chien de garde `--page-timeout` (le journal
de la page, le cas échoue).

Premier passage (texte ×2, GLSL contre `mwsr`) : 27,7 dB, 28 % des pixels différents, max 221/255 ;
à ×0,5 : un bloc de 30×30 à 174/255. Trois causes :

1. **le signe de `dFdy`** : fenêtre GL y vers le haut → `dFdy` = −`dpdy` ; sans la négation la normale
   est en miroir et les taps le long / en travers s'échangent sur les diagonales. Corrigé : 34,3 dB,
   25 % différents, 17 % au-delà de 2/255, max 76 ;
2. **l'image dumpée** : la chaîne dessinait l'image du clip où en était le curseur (le carré mobile).
   Corrigé (première image) : la réduction passe à ≤ 1/255 (texte, code), 3/255 sur 0,009 % de la
   zone-plate ;
3. **la ligne de référence des dérivées coarse** : plats concordants (0,6 % différents, vote au
   seuil), contours différents à 44 % mais autant d'un côté que de l'autre de la bilinéaire (écart
   moyen à la bilinéaire 20,55 contre 20,54/255). Essai `mwsr:fine=1` (`dpdxFine`/`dpdyFine`) : coarse
   contre fine = 13,4 % des pixels au-delà de 2/255, max 83 ; GL contre fine 13,1 %, contre coarse
   16,9 % : le GL n'est ni l'un ni l'autre — la ligne du quad que prend une dérivée coarse est le choix
   de l'implémentation et diffère entre les deux espaces fenêtre. Inhérent, pas un bug de port.

Verdict H18 : **C11 reformulé**, tenu sous sa nouvelle forme. Pixels : chaîne WebGPU identique (0/255,
neuf cas) ; jumeau GLSL ≤ 2/255 sur 99,9 % des pixels et ≤ 4 au plus là où aucune dérivée n'intervient
(réduction : 1/255 ; zone-plate 3/255 sur 0,009 % = précision sous-texel des échantillonneurs).
Qualité face à la vérité terrain (`--quality`, ×2) :

| contenu | `mwsr` WGSL : PSNR / SSIM / FLIP | `gl-mwsr` GLSL | bilinéaire | FSR1 |
|---|---|---|---|---|
| texte | 17,41 / 0,8415 / 0,071 | 17,44 / 0,8425 / 0,071 | 16,99 / 0,816 / 0,090 | 17,57 / 0,845 / 0,068 |
| jeu | 35,84 / 0,8816 / 0,019 | 35,84 / 0,8816 / 0,019 | 35,01 / 0,880 / 0,020 | 35,55 / 0,887 / 0,017 |
| code | 26,95 / 0,9555 / 0,029 | 26,96 / 0,9559 / 0,029 | 26,96 / 0,953 / 0,034 | 27,35 / 0,951 / 0,024 |

0 % de dépassement des deux côtés ; le jumeau vaut le noyau à 0,000 FLIP et 0,025 dB près. Le job CI
`scaler-lab` (non bloquant) rejoue les deux côtés sur un runner hébergé.

## 2026-10-21 — J19, la CI : la chaîne WebGPU noire sur le runner Linux, la capture par la texture

Première exécution du job `scaler-lab` (CI 39627353471, signalée par 59) : côté WebGPU (SwiftShader
Subzero sur Ubuntu), `chain-mwsr` hors tolérance sur les neuf cas, ~100 % des pixels, PSNR 0,4 à
15,6 dB ; en qualité, `chain-mwsr` à 0,45 / 7,14 / 14,94 dB (texte, jeu, code) avec SSIM ≈ 0 et 95-99 %
de dépassements, le noyau seul sain (18,43 / 35,93 / 27,08 : les polices d'Ubuntu, pas celles de
DualRTX). Ces PSNR sont ceux d'une **image noire** contre les trois mires (le texte presque blanc à
0,45 dB, le code sombre à 15) : le `transferToImageBitmap()` d'un `OffscreenCanvas` WebGPU ne rend pas
l'image présentée sur ce chemin logiciel Linux, alors qu'il la rendait au bit près sous Windows (J17).

Correctif : pendant le dessin du dump, un crochet sur `GPUCanvasContext.getCurrentTexture` donne au
renderer une texture du labo (même device, format, taille et `viewFormats` que la swap chain, `COPY_SRC`),
relue par `copyTextureToBuffer` sur ce device puis écrite dans la texture de sortie du labo ; la chaîne
WebGL2 garde son tampon de dessin. Sur DualRTX (SwiftShader) : toujours exact sur les neuf cas dorés,
conformité OK des deux côtés. Pour qu'une exécution que personne ne regarde dise ce qui s'est passé :
les lignes console des renderers et tous les avertissements vont dans le journal de la page, les
erreurs non capturées des devices du produit rejoignent celles du passage, le pilote garde le journal de
la page avec chaque résultat (`pageLog`), le flux de conformité imprime le renderer et le GPU de chaque
chaîne. Au passage : une page dont l'import statique du module échouait restait muette jusqu'au
délai (un cas sur douze côté WebGL2, 18:10) — import dynamique rapporté en résultat (rechargé aussitôt
par la règle des transitoires), et une page muette au délai est rechargée une fois. Verdict H19 :
**à confirmer par la prochaine exécution de la CI** ; si la chaîne y reste fausse avec la texture relue
directement, c'est le renderer sur cette plateforme et non plus la lecture.

## 2026-10-21 — J20, la porte P4 sur ordinateur : WebGPU contre WebGL2 à shader égal

**La question** (plan §3.6, §8.1, wiki 15.3) : avec le même algorithme dans les deux renderers du
produit, quelle saveur en SDR par classe d'appareil ? Le jeu `--p4` (`p4_flows.py`) joue les chaînes du
produit `off`, SGSR1, FSR1 et `mwsr` dans les deux saveurs, sur les mêmes images (douze images H.264
décodées), à ×2 (texte, jeu, interface), ×1,33 et ×0,67 (4K → 1440p), saturé (`back`) et à 60 i/s
(`tick`), 48 draws par chaîne en deux tours ABBA. Trois chiffres par draw : **`submit`**, le temps du
thread principal (WebGPU : `lastDraw.submitMs` du renderer, encodage et soumission ; WebGL2 : la durée de
`draw()`, qui ne fait qu'empiler) ; **`done`**, du début du draw à la fin du GPU (WebGPU : `draw()`
lui-même, qui attend `onSubmittedWorkDone` ; WebGL2 : une barrière `fenceSync` sondée tâche par tâche) ;
**`gpu`**, le temps du GPU seul (WebGPU : les passes horodatées, passe 0 et noyau ; WebGL2 :
`EXT_disjoint_timer_query_webgl2` autour du draw, téléversement compris).

Trois pièges de l'instrument, trouvés sur SwiftShader avant tout GPU : (1) **`gl.finish()` de Chrome est
un flush** (« Intentionally a flush, not a finish » dans `webgl_rendering_context_base.cc`) : au premier
essai `done` valait `submit` (0,16 ms) quand l'horodatage lisait 28 ms ; la barrière sondée par
`getSyncParameter` (ce que lit le renderer lui-même pour savoir si le GPU est en retard) rend 38 ms,
cohérent ; (2) le niveau H.264 de l'encodeur du labo (5.0, 22 080 macroblocs) refuse la 4K : niveau 5.1
au-delà ; (3) la page `gl=1` ignorait `pace=tick`. L'horodatage WebGL2 existe sur ANGLE D3D11 (RTX,
780M) et sur ANGLE-SwiftShader.

**Passes** (go de 59, un GPU à la fois, Chrome sans tête épinglé par LUID, sonde WebGPU + WebGL2 avant
chaque jeu) : 780M 21:44-21:45 (dix pages, une minute) puis ×0,67 à 21:49 ; RTX 21:46-21:48 ; aucun
processus restant. À 60 i/s, p50 en ms, `done` WebGPU / WebGL2 (`gpu` entre parenthèses) :

| cas | GPU | `off` | SGSR1 | FSR1 | **`mwsr`** |
|---|---|---|---|---|---|
| ×2 texte | RTX | 4,37 / 2,09 (0,03 / 0,12) | 4,06 / 1,77 (0,09 / 0,21) | 4,65 / 3,23 (0,57 / 1,42) | **4,29 / 2,01** (0,07 / 0,34) |
| ×2 jeu | RTX | 4,24 / 2,08 | 3,61 / 1,64 | 4,50 / 3,06 | **3,99 / 1,77** (0,05 / 0,09) |
| ×1,33 texte | RTX | 5,44 / 3,26 | 4,07 / 3,19 | 4,40 / 3,20 | **4,09 / 2,16** (0,14 / 0,12) |
| ×0,67 texte | RTX | 8,49 / 1,87 | 4,45 / 1,75 | 4,04 / 1,92 | **4,59 / 3,28** (0,21 / 0,23) |
| ×2 texte | 780M | 4,62 / 4,09 (0,53 / 0,66) | 6,84 / 6,06 (2,81 / 2,69) | 13,34 / 10,55 (9,42 / 7,92) | **5,61 / 4,75** (1,59 / 1,38) |
| ×2 jeu | 780M | 4,34 / 3,84 | 6,59 / 5,70 | 13,75 / 11,17 | **5,00 / 4,25** (0,81 / 0,81) |
| ×2 interface | 780M | 4,57 / 3,94 | 6,00 / 4,42 | 13,50 / 10,93 | **5,45 / 4,13** (0,84 / 0,83) |
| ×1,33 texte | 780M | 5,05 / 5,57 | 7,75 / 7,00 | 13,78 / 12,24 | **6,30 / 5,80** (1,57 / 1,40) |
| ×0,67 texte | 780M | 7,10 / 8,88 (0,70 / 1,57) | 9,47 / 10,27 | 16,08 / 15,12 | **7,59 / 9,30** (1,57 / 1,98) |

Saturé : RTX `mwsr` 3,3-3,7 ms (WebGPU) contre 0,81-0,87 (WebGL2), 4 × ; 780M 3,4-4,7 contre 2,6-4,1,
1,15-1,5 ×. Thread principal à 60 i/s : égal, 0,25-0,35 ms p50 des deux côtés (RTX ×1,33 : 0,28 contre
0,44 pour WebGL2) ; le p99 de 48 draws est leur maximum, et des arrêts isolés de 100 à 300 ms
apparaissent dans les deux saveurs (une image sur quarante-huit, pas une tendance).

**Lecture.** Le shader est à égalité sur le GPU : `mwsr` WGSL et GLSL à ±15 % l'un de l'autre, le
WebGL2 comptant en plus le téléversement de l'image. Ce qui sépare les saveurs est la chaîne autour :
l'import de la texture externe, la soumission et l'attente `onSubmittedWorkDone` coûtent 4 ms sur le RTX
quelle que soit la passe (`off` compris, 0,03 ms de GPU pour 4,4 ms de `done`), et 0,5 à 1,3 ms de plus
que WebGL2 sur le 780M. L'observation du wiki 15.3 tient avec le nouveau shader : **sur ordinateur,
WebGL2 reste la saveur SDR** (RTX 1,9-2,3 × à 60 i/s ; 780M 1,1-1,2 ×, « égalité dans les marges, la
production reste », WebGL2 saturé). Une exception, à peser en P6 : la **réduction 4K → 1440p sur le
780M**, où les vues sRGB (un échantillon décodé par le matériel, J15) et un import 4K moins cher donnent
WebGPU 7,6 ms contre 9,3 (0,82 ×, GPU 1,57 contre 1,98) ; sur le RTX, où le GPU n'est pas le goulot, WebGL2
garde l'avantage même là (1,40 ×). Verdict H20 : **WebGL2 en SDR sur RTX et 780M**, la décision par
classe d'appareil reste ouverte pour Apple et Mali (créneaux de Bruno, mêmes liens `--serve --p4`).
Tables complètes : `bench-out/scaler-lab/p4rtx-gate.md`, `p4amd-gate.md` (`--p4-table`).

**Le drapeau `mw_mwsr` sur la `--dev` de DualRTX** (22:04-22:14, créneau de 59, `--dev` de 09e31b38
vérifiée au hachage, deux Chrome sans tête : l'un tout SwiftShader, l'autre ANGLE sur l'AMD épinglé et
WebGPU sur SwiftShader, décodage logiciel, flux de l'écran 1, image vive, aucune erreur console) : `auto`
+ drapeau → « WebGL MWSR » (684×384 → ×1,34) ; `auto` sans drapeau → « WebGL FSR1 » ; `gl-mwsr` choisi →
« WebGL MWSR » ; `mwsr` choisi sans WebGPU → Canvas2D (le repli de toute saveur WebGPU) ; `mwsr` choisi
avec WebGPU → « MWSR », renderer créé avec les vues sRGB ; flux 896×504 dans un canvas 896×504 → « Off
(1:1) ». Le menu debug des deux saveurs n'apparaît qu'en mode debug du serveur (réglage de la `--dev`
non touché) : couvert par les tests unitaires.

## 2026-10-21 — J21, le repli du renderer WebGPU quand l'import d'une image est refusé

Décision de Bruno (relayée par 59, 22:20) : l'écran noir de la CI se corrige avant la v0.4.1. Choix :
**copier l'image dans la texture d'entrée** plutôt que revenir à Canvas2D — l'upscaler continue de
tourner, rien ne change à l'écran ni dans l'overlay. Dès le premier refus d'`importExternalTexture`,
`draw()` fait `createImageBitmap(frame)` puis `copyExternalImageToTexture` dans `inputTex` (`COPY_DST`
ajouté) : la copie tient lieu de passe 0 ; la copie nue vers le canvas (`off`, 1:1) lit `inputTex` par un
blit `texture_2d` (`BLIT_TEX_WGSL`). L'import est retenté tous les 300 draws (un refus passager rend le
chemin rapide de lui-même) ; un seul avertissement console ; un draw qui échoue n'est journalisé qu'une
fois par message distinct. En HDR le bitmap est le rendu SDR du navigateur : une image, pas l'image HDR.

Vérification : test `vitest` à device simulé (`WebGpuRendererImport.test.js`, cinq cas : import
accepté, refusé puis copié avec une seule ligne, retenté 300 draws plus tard, chemin upscaler sans passe
0, journal une fois par message) ; suite complète 1659 tests verts ; labo `importfail=1` (le crochet
refuse l'import aux seuls devices du produit) : **dorés exacts sur les neuf cas** par le repli comme par
l'import, conformité OK. Reste la mesure du coût du repli sur un GPU (créneau à demander à 59) : sur
SwiftShader la chaîne `chain-mwsr` passe par le bitmap sans écart de pixel.

**Le coût du repli, 22/10 00:21-00:23 (780M, créneau de 59 ; même sonde et mêmes conditions que la
porte ; `scaler_lab.py --p4 --extra importfail=1 --tag p4amdbm`, table par
`p4_fallback_compare.py bench-out/scaler-lab p4amd p4amdbm`).** `done` p50 de la chaîne WebGPU `mwsr`,
repli contre import : ×2 texte 5,34 contre 4,11 ms saturé (×1,30) et 6,55 contre 5,61 à 60 i/s
(×1,17) ; ×2 jeu ×1,21 / ×1,22 ; ×2 interface ×0,98 / ×1,02 ; ×1,33 texte ×1,26 / ×1,20 ; réduction
×0,67 (entrée 4K) 11,45 contre 6,42 (×1,78) et 13,34 contre 7,60 (×1,76). Témoin : les cellules WebGL2
des deux passes, que `importfail` ne touche pas, dérivent de 0,91 à 1,21 (médiane 1,03). Lecture :
≈ +1 ms par image à 720p → 1440p (au-dessus du bruit sur la plupart des lignes, pas toutes), +5 à 6 ms
quand l'entrée est en 4K (le bitmap d'une image 4K). Le fil principal n'est pas plus chargé (`submit`
p50 égal ou plus bas : la création du bitmap est asynchrone) ; le temps GPU horodaté baisse de 5 à
15 % (la passe 0 n'est plus une passe, la copie passe par la file) et de moitié à ×0,67. Chemin de
secours seulement, jamais le chemin normal : acceptable, une image vaut mieux qu'un écran noir.

## 2026-10-22 — J22, mw-arm à distance : pas de GPU pour un Chrome lancé par SSH

Créneau de 59 (00:26-00:38). Recette : tunnel SSH (`-L 9222` pour DevTools, `-R 8767` pour le serveur
du labo), `scaler_lab.py --remote-cdp 9222 --http-port 8767 --gl` depuis DualRTX, la page en
`http://127.0.0.1:8767` vue de mw-arm (contexte sécurisé, aucun certificat à accepter). Trois
constats. (1) Une tâche planifiée `Interactive` reste en file (`Queued`) : personne n'est connecté à la
console de mw-arm. (2) Un Chrome `--headless=new` lancé depuis la session SSH (séance 0) n'a **aucun
GPU** (`gl=disabled`, aucun adaptateur énuméré) ; avec `--use-gl=angle --use-angle=d3d11
--ignore-gpu-blocklist`, l'Adreno 618 est énuméré et `webgl: enabled`, mais chaque contexte est perdu à
la création et le domaine finit bloqué (« Web page caused context loss and was blocked ») : la séance 0
n'a pas accès au périphérique. (3) Couper le bac à sable du processus GPU n'est pas une option sans
l'accord de Bruno. La porte `mw-arm` se fait donc avec sa console ouverte (Bruno connecté) : la tâche
`Interactive` du 06/09 marche alors, et la même recette `--remote-cdp` s'applique. Tout rendu à 00:38 :
0 Chrome, tâche et profil effacés, tunnel fermé, aucun GPU de DualRTX pris.

## 2026-10-22 — J23, la porte P4 sur l'iPhone 13 Pro (Safari, iOS 26.5) : WebGPU gagne partout

Créneau de Bruno (08:51-09:31, liens servis depuis DualRTX, `--tag iphone --p4 --screen 2532x1170`,
18 pages, consignes relayées par 59). Les deux chaînes du produit sur WebKit (adaptateur `apple`, WebGPU
avec horodatages, WebGL2 sans timer). `done` p50 en ms, WebGPU / WebGL2, `mwsr` :

| cas | saturé | 60 i/s |
|---|---|---|
| ×2 texte 720p → 1440p | 2,96 / 7,40 | 3,76 / 7,12 |
| ×2 jeu | 2,26 / 7,00 | 3,06 / 7,62 |
| ×2 interface | 2,06 / 6,68 | 3,14 / 7,58 |
| ×1,33 texte | 2,70 / 10,96 | 3,12 / 12,38 |
| ×0,67 texte (entrée 4K) | 2,82 / 29,08 | 3,80 / 29,26 |
| écran ×2 texte 1264×584 → 2532×1170 | 2,58 / 6,32 | 3,52 / 6,80 |
| écran ×2 jeu | 2,42 / 6,34 | 3,44 / 6,78 |
| écran ×1,33 texte | 2,80 / 9,74 | 3,06 / 10,52 |
| écran ×0,67 texte | 2,90 / 23,56 | 3,66 / 23,14 |

Verdict de la règle : **WebGPU** dans les deux régimes (`done` 0,25-0,53 ×, fil principal 0,01-0,05 ×).
La cause est l'envoi de l'image : sur WebKit, `texImage2D` d'une VideoFrame coûte au fil principal 2,5 ms
à 540p, 3,1 à 720p, 6,7 à 1080p et 19-24 ms à 4K (`submit` des `chain-gl-*`), quand l'import WebGPU vaut
0,02-0,16 ms ; le GPU lui-même (`mwsr` 1,3-2,4 ms horodatés à ×2) n'est pas en cause. Conséquence : en
WebGL2, l'iPhone ne tient pas 60 i/s dès une entrée 1080p. Proposition pour Bruno (plan §9, classe Apple
mobile) : la saveur SDR y est WebGPU quand il est là (iOS 26), `gl-mwsr` sinon.

Deux pièges de passe. (1) Chaque page a été postée deux fois : la seconde, plus lente (28-32 s au lieu de
3-7), était le **realme sur les mêmes liens**, ouvert en parallèle par Bruno (reconnu à l'agent
utilisateur, fichiers renommés `phone-realme-*`, journal J26) ; cru d'abord un préchargement de Safari, la
page attend désormais d'être visible avant de mesurer et compte le temps caché (`hiddenMs`), et la table
lit, parmi les fichiers postés d'un cas, une passe complète (toutes les cellules du cas, aucune erreur) au
premier plan, la plus courte à défaut (`_best_posted`) : les fichiers de P0 du même tag ne comptent plus.
(2) Le motif `phone-<tag>-<cas>-*.json` attrapait aussi les fichiers du cas `-tick` : corrigé avant la
passe.

## 2026-10-22 — J24, la porte P4 sur le Mac M1 Pro (Chrome 155 sans tête, `metal-3`)

Créneau de Bruno (09:15-09:16, Mac rendu à 09:18) : Chrome de labo lancé par SSH dans la session de Léo
(`open -na "Google Chrome" --args --headless=new --remote-debugging-port=9223 …`, profil jetable,
`--mute-audio`), piloté depuis DualRTX par `scaler_lab.py --remote-cdp 9223 --http-port 8769 --p4
--screen 3600x2338` à travers `ssh -L 9223 -R 8769` ; mot pour Léo par osascript (launchd gui/501),
retiré à la fin ; 16 cas sur 18 (les deux « réduction à 1,5 × l'écran », entrée 5400×3504, sans encodeur
H.264 au niveau 5.1 : limite du labo). `done` p50 en ms, WebGPU / WebGL2, `mwsr` :

| cas | saturé | 60 i/s |
|---|---|---|
| ×2 texte | 1,46 / 2,43 | 4,48 / 3,21 |
| ×2 jeu | 1,05 / 2,12 | 3,70 / 3,54 |
| ×2 interface | 1,05 / 2,03 | 4,08 / 3,86 |
| ×1,33 texte | 1,48 / 2,76 | 4,75 / 3,97 |
| ×0,67 texte (entrée 4K) | 1,37 / 5,63 | 4,79 / 5,68 |
| écran ×2 texte 1800×1168 → 3600×2338 | 2,23 / 4,14 | 5,57 / 4,51 |
| écran ×2 jeu | 1,77 / 3,17 | 5,04 / 3,45 |
| écran ×1,33 texte | 1,60 / 4,94 | 5,57 / 5,08 |

Verdict de la règle : saturé, **WebGPU** (0,50-0,60 ×, fil principal 0,03 contre 0,10 ms) ; à 60 i/s,
**égalité dans les marges** (WebGPU 1,05-1,40 × WebGL2, son fil principal 0,54-0,67 ms contre 0,19-0,25)
→ la saveur de production, WebGL2, reste sur Mac dans Chrome. Même lecture que sur RTX et 780M : la chaîne
WebGPU cadencée paie l'attente de présentation, pas le shader (GPU horodaté égal). Réduction ×0,67 à
60 i/s : WebGPU 0,84 ×. Safari macOS non mesuré (le labo pilote Chrome).

## 2026-10-22 — J25, la porte P4 sur mw-arm (Adreno 618, WebGL2 seul) : 8 cas, puis hors réseau

Créneau de 59 (09:15-09:26), Bruno connecté à la console : la tâche planifiée `Interactive` lance un Chrome
sans tête avec DevTools, piloté par `--remote-cdp 9222 --http-port 8767 --gl --screen 1920x1080` à travers
le tunnel (la recette de J22 marche dès qu'une session console existe : WebGL2 sur l'Adreno,
`EXT_disjoint_timer_query_webgl2` présent). À 09:26 mw-arm a quitté le réseau (DevTools puis SSH coupés) :
8 cas sur 18, les tailles standard sauf la réduction 4K, aucun cas à la taille de l'écran ; le ménage
(Chrome, tâche, profil, scripts) attend son retour. GPU p50 en ms par image, 720p → 1440p (machine qui
pagine : p99 de 0,3 à 9 s sur toutes les cellules, à lire sur les p50 seulement) :

| cas, saturé / 60 i/s | `gl-off` | `gl-sgsr` | `gl-fsr1` | `gl-mwsr` | mwsr / SGSR1 |
|---|---|---|---|---|---|
| ×2 texte | 2,57 / 2,61 | 10,61 / 10,62 | 21,33 / 21,38 | 8,15 / 7,01 | 0,77 / 0,66 |
| ×2 jeu | 2,60 / 2,59 | 8,24 / 8,37 | 21,43 / 21,50 | 5,20 / 6,26 | 0,63 / 0,75 |
| ×2 interface | 2,55 / 2,57 | 5,70 / 7,69 | 21,40 / 21,36 | 6,35 / 6,42 | 1,11 / 0,83 |
| ×1,33 texte | 2,61 / 2,82 | 10,94 / 10,89 | 21,70 / 21,74 | 6,97 / 7,02 | 0,64 / 0,64 |

`done` p50 : `gl-mwsr` 14-17 ms, `gl-off` 12-15, `gl-fsr1` 29-35. Verdict : pas de WebGPU, donc WebGL2
par nécessité ; `gl-mwsr` y tourne sans erreur, tient 60 i/s (FSR1 non) et coûte 0,63-0,77 × SGSR1 (le
choix Auto actuel sur Snapdragon, `isSnapdragonGpu()`) sur texte, jeu et ×1,33, égal sur l'interface
(1,11 saturé, 0,83 à 60 i/s : le bruit de cette machine) : **C1 tenue sur l'Adreno 618**. Proposition pour
Bruno : l'Auto Snapdragon passe à `gl-mwsr` avec le drapeau. Les cas manquants ne changent pas cette
lecture ; mw-arm n'est pas redemandé pour eux.

## 2026-10-22 — J26, la porte P4 sur le realme Note 60 (Mali-G57 MP1, Chrome Android) : égalité

Bruno a ouvert les liens de l'iPhone sur le realme en même temps (09:14-09:31) : 17 pages sur 18 postées
sous le tag `iphone` (les 21 fichiers Android, reconnus à l'agent utilisateur, renommés `phone-realme-*`),
les cas « écran » donc à la taille de l'iPhone (2532×1170), pas à la sienne (1600×720) ; le serveur
préparé pour lui (port 8768) n'a pas servi. Deux téléphones en parallèle ne se gênent pas : chacun mesure
son GPU et son fil principal, le serveur ne fait que livrer la page et recevoir le JSON. `done` p50 en ms,
WebGPU / WebGL2, `mwsr` :

| cas | saturé | 60 i/s |
|---|---|---|
| ×2 texte 720p → 1440p | 37,3 / 38,0 | 37,3 / 38,4 |
| ×2 jeu | 29,0 / 30,7 | 29,2 / 31,2 |
| ×2 interface | 30,1 / 31,8 | 29,4 / 31,8 |
| ×1,33 texte | 36,4 / 46,8 | 38,0 / 46,4 |
| ×0,67 texte (entrée 4K) | 38,1 / 97,5 | 38,1 / 97,0 |
| 1264×584 → 2532×1170 texte | 32,6 / 34,2 | 32,6 / 34,1 |
| 1264×584 → 2532×1170 jeu | 29,5 / 30,0 | 29,9 / 30,8 |

Verdict de la règle : **égalité dans les marges** dans les deux régimes (WebGPU 0,78-0,98 ×, fil principal
égal, 2,1-2,4 ms des deux côtés) → la saveur de production, WebGL2, reste sur Mali ; la réduction 4K →
1440p est la seule où WebGPU s'impose (0,39 ×, vues sRGB contre la grille 2×2). À lire avec la taille :
une sortie 1440p (3,7 Mpx) vaut 3,2 écrans de ce téléphone, le plancher de la chaîne (`off`, copie et
présentation) y est déjà 22-23 ms et `mwsr` ajoute 7 à 15 ms de GPU (18 ms horodatés sur le texte, 9,7 sur
le jeu) : aucune saveur n'y tient 60 i/s, alors qu'à sa taille `mwsr` coûte 5,9 ms (J16). Manque la page
18 (réduction à 1,5 × l'écran de l'iPhone), sans incidence ; les cas à la taille du realme, refaits
ensuite, sont à l'addendum.

**Addendum (10:12-10:16), les huit cas à la taille du realme.** Bruno a ouvert les liens /11 à /18 sur le
realme seulement, servis par `--tag realme --p4 --screen 1600x720` (port 8768) : huit pages au premier plan
(`hiddenMs` 0), sans erreur, 15-21 s chacune. La table de la porte écarte désormais un fichier posté à une
autre taille ou dans un autre régime que le cas (`_posted_shape`, `p4_flows.py`) : les sept pages « écran »
à la taille de l'iPhone et une page de P0 du 21/10 n'y entrent plus. `done` p50 en ms, WebGPU / WebGL2,
`mwsr` :

| cas | saturé | 60 i/s |
|---|---|---|
| écran ×2 texte 800×360 → 1600×720 | 20,8 / 23,1 | 20,8 / 23,1 |
| écran ×2 jeu | 18,1 / 20,7 | 18,1 / 20,5 |
| écran ×1,33 texte 1200×536 → 1600×720 | 21,4 / 24,8 | 19,3 / 25,4 |
| écran ×0,67 texte 2400×1080 → 1600×720 | 18,6 / 37,3 | 19,0 / 36,7 |

À la taille de son écran, **WebGPU passe devant partout** : 0,87-0,90 × à ×2, 0,76-0,86 × à ×1,33,
0,50-0,52 × en réduction (2,2-2,6 ms de moins par image à ×2, 3,4-6,1 à ×1,33, 18 en réduction), fil
principal égal (1,9-2,3 ms des deux côtés ; 3,1 en WebGL2 sur la réduction). L'avance vient du plancher de
la chaîne, pas du shader : `off` 14,6-15,8 ms en WebGPU contre 18,4-20,3 en WebGL2 (35 ms sur la réduction,
l'envoi d'une image 2400×1080), et au-dessus de son plancher `mwsr` ajoute 2,4-6,3 ms en WebGPU, 1,1-5,8 en
WebGL2 ; GPU horodaté 4,1-6,9 ms, 0,45-0,74 × SGSR1 en agrandissement (C1 tenue). Aucune des deux ne finit
son image sous 16,7 ms. La règle, jugée sur les tailles standard, reste « égalité » ; appliquée à ces cas,
elle le resterait de justesse (texte ×2 à 0,901 et 0,903). Pour la décision par classe : sur Mali, WebGPU
gagne 2 à 6 ms par image à la taille de l'écran, 18 ms quand le flux est plus grand que l'écran.

## 2026-10-22 — J27, la porte P4 relue : le labo s'arrête à la fin du GPU, la saveur se choisira au clic-photon

Question de Bruno (10:33) : pourquoi « WebGL2 reste » sur le realme avec 6 et 18 ms de gain, et que
disait le clic-photon déjà mesuré ? Sa crainte : une régression si l'on ne regarde pas la chaîne entière.

- **La règle** (`GATE_WIN` 0,9, `GATE_MAIN` 1,2 ou 0,25 ms, `p4_flows.py`) a été posée par la session le
  21/10 en écrivant la porte, pas par Bruno. Trois limites : elle est relative (10 % de 2 ms sur le RTX,
  de 20 à 37 ms sur le realme) ; elle juge des sorties 1440p, 3,2 écrans du realme ; elle juge `done`,
  du début du draw à la fin du GPU, avant le compositeur.
- **Le clic-photon déjà mesuré**, sonde `mwLatency`, toujours sur ordinateur. Wiki 15.3 (septembre, RTX) :
  en fenêtre ×1,07, WebGL2 13,9-15,1 ms contre WebGPU 14,0-15,7, égaux à la médiane, WebGL2 meilleur de
  1-2 ms au p90 ; en plein écran ×2, WebGL2 34,2-37,2 contre WebGPU 26,8-29,5, WebGPU devant de 5-10 ms
  avec 20-40 ms de dispersion sur dix clics. Rapport P1 §6 (21/10, 780M, plein écran 1:1) : Canvas2D
  26,9, WebGPU 27,6, WebGL2 30,6 ms, égaux à ± 3 ms ; les 4 ms de soumission WebGPU du labo y sont
  absorbées tant que l'image tient dans la période.
- **Lecture.** Sur ordinateur, le labo (WebGL2 deux fois plus tôt sur le RTX) et le photon plein écran
  (WebGPU devant) se contredisent ; le rapport P4 disait avoir répondu à la question du wiki 15.3, c'est
  corrigé (§5). Sur le realme, une image prend 18 à 37 ms, plus que la période : le dessin y est sur le
  chemin critique et l'écart du labo a plus de chances de passer au photon, sans preuve. Sur mobile,
  `desynchronized` n'aurait pas d'effet d'après `BrowserDetect.js` (les compositeurs mobiles
  resynchronisent) : l'asymétrie WebGL2/WebGPU du dernier saut n'y jouerait pas, et la sonde, qui lit
  l'image dessinée dans la page et pas l'écran, y serait plus juste que sur ordinateur.
- **Décision de Bruno (go 10:42)** : aucune classe ne change de saveur sur la foi du labo seul ; d'abord
  le clic-photon des deux téléphones. Protocole : la `--dev` de DualRTX avec `latency_flag_enabled` et le
  mode debug (`ClientLog` remonte alors la console du téléphone au journal client de l'hôte) ; WebGPU
  `mwsr` contre WebGL2 `gl-mwsr` par `video_enhancement_algo`, Canvas2D pour témoin ; flux 720p (×1,25 sur
  le realme) et 1080p (réduction) ; séries de 10 clics entrelacées ; le realme piloté par adb sans fil et
  DevTools (Bruno autorise la manipulation), l'iPhone par l'inspecteur web de Safari du Mac de Léo en
  Wi-Fi ; caméra au ralenti (repère gris de la sonde) si un écart reste ambigu. Créneau via 59.

## 2026-10-22 — J28, le clic-photon des téléphones : la sonde biaisée, l'échelle qui coupe, pas de régression WebGPU

Créneau de 59 (10:52, priorité donnée par Bruno), `--dev` de DualRTX sur `da7468f0` avec le drapeau et le
mode debug, Display 1 capturé (kiosque `still.html?anim=1`, pointeur parqué sur `click-target`). Scripts et
vidéos dans le scratchpad de la session (`photon/`) ; rien du produit n'a changé.

- **La sonde se mesure elle-même sur téléphone.** Pendant qu'elle guette le drapeau, WebGL2 relit ses
  trois pixels par un `readPixels` synchrone après chaque dessin, Canvas2D par `getImageData`, WebGPU en
  asynchrone (`copyTextureToBuffer` + `mapAsync`). Sur le realme, `mwLatency.run` donnait WebGPU 43-46 ms
  contre WebGL2 74-77 et le présentateur simple 72-90 : un artefact de la relecture, abandonné.
- **Méthode sans relecture.** La page envoie le clic (`mwLatency._sendClick(null)`) et montre au même
  instant un flash coloré dans le DOM (trois rAF) ; l'écran composé est enregistré (`adb shell
  screenrecord` sur le realme, ~52 i/s ; l'enregistrement d'écran d'iOS sur l'iPhone, 60 i/s) et l'on lit,
  par clic, le délai du flash au drapeau (`rec_analyse2/4.py` : zone du drapeau trouvée dans l'image,
  flashs groupés par mesure). Le dernier saut du compositeur, commun au DOM et au canvas, s'annule : les
  absolus sont sous le vrai clic → photon, les écarts valent. Pas de 120 Hz résolu : l'unité est l'image
  d'enregistrement (16,7 ms), lissée par des clics non synchronisés.
- **Piège : la mémoire de l'échelle.** `EnhancerGovernor` retient par appareil où l'échelle est descendue
  (`localStorage.mw_enhancer_rung`) ; les séries de sonde l'avaient fait descendre, et toute la première
  matrice du realme (11:08-11:18) a démarré amélioration coupée : `Enhancer starts at off (setting
  gl-mwsr)` au journal client, rien à l'écran. Cette matrice (v1) compare donc les présentateurs simples.
  Ensuite : clé effacée avant chaque lancement, relue après (les descentes sont muettes dans la console).
- **realme Note 60, Chrome 155, 60 Hz, sortie 1280×720 (v1, présentateurs sans amélioration, 2 × 14
  clics).** WebGL sans passe deux fois (réglage `off`, et réglage `gl-mwsr` démarré à `off`), WebGPU sans
  passe une fois (réglage `mwsr` démarré à `off`). 960×540 : 38,7 ms (séries 38,7 / 38,7) et 42,9 (38,6 /
  47,1) pour WebGL, 43,3 (47,9 / 38,7) pour WebGPU, tous à 52 i/s. 1920×1080 : 43,2 et 45,5 pour WebGL à
  30 i/s, 38,8 pour WebGPU à 53-55 i/s. Le fil principal de WebGL sature en 1080p : une image sur deux
  n'est pas présentée.
- **L'échelle coupe l'upscaler sur le realme, dans les deux saveurs (v2, 12:25).** Avec la mémoire
  effacée, `gl-mwsr` comme `mwsr` sont descendus à `off` pendant la série, en 2 s : la passe ne tient pas
  une image à ×1,33 (le labo P4 mesurait 21-25 ms par image à la taille de l'écran). Dans le produit, sur
  ce téléphone, la saveur ne choisit donc que le présentateur simple qui reste : v1.
- **Upscaler maintenu (v3, 12:29-12:38, labo seulement).** L'échelle neutralisée sur la page de test
  (`EnhancerGovernor.prototype.update` → `null` par un import du même module ; vérifié après chaque série :
  mémoire vide, correctif en place), 2 × 14 clics par saveur, une série témoin. 960×540 (×1,33) :
  `gl-mwsr` 35,7 ms (31,8 / 39,6 ; médiane 32,7), `mwsr` 45,3 (46,0 / 44,5 ; médiane 41,1), témoin 51,3
  (médiane 48,8). 1920×1080 (réduction) : `gl-mwsr` 40,6 (40,6 / 40,6 ; médiane 32,7) à 30 i/s, `mwsr` 68,8
  (93,4 avec deux à-coups de 458 et 280 ms / 44,3 ; médiane 48,7) à 50-58 i/s, témoin 53,0 à 30 i/s. Forcé,
  WebGPU arrive environ une image plus tard que WebGL2, mais montre presque deux fois plus d'images en
  1080p. L'écart entre deux séries identiques atteint lui aussi une image (témoin 960 : médiane 33,5 en v1,
  48,8 en v3) : la phase entre la cadence de l'hôte et l'écran du téléphone tient toute une série.
- **iPhone 13 Pro, Safari d'iOS 26.5, 120 Hz, canvas 1445×813 (11:49-11:54, Bruno au signet).** 120
  drapeaux vus, 100 clics appariés (un flash de trois rAF à 120 Hz échappe parfois à l'enregistrement à
  60 i/s, au hasard). 1280×720 (×1,13) : `gl-mwsr` 34,4 ms (15 clics), `mwsr` 28,1 (19). 1920×1080 (×0,75) :
  WebGL simple 38,6 (19 ; 36,9 sur les réglages de Bruno, 14), `gl-mwsr` 33,3 (14), `mwsr` 37,7 (19).
  WebGPU − WebGL2 : −6,4 ms en 720p (IC 90 % −13,4 à 0), +4,4 en 1080p (−1,0 à +10,0). Pas de régression,
  et aucune descente de l'échelle pendant les six mesures.
- **La mesure du rafraîchissement sous charge.** En 1080p WebGL, le client annonce un écran plus lent que
  le vrai : 35-39 Hz sur le realme (l'hôte, sans diviseur proche, reste à 60 i/s), 112-115 Hz sur l'iPhone
  (l'hôte passe à 56-58 i/s et perd son verrou de cadence). Avec `mwsr` sur WebGPU, l'iPhone reste mesuré à
  120 Hz et le flux à 60 i/s verrouillé. Hors du chantier : signalé à 59 pour la cadence.
- **Verdict.** iPhone : WebGPU confirmé, sans régression au photon et avec une cadence plus stable.
  realme : à ×1,33, l'upscaler ne survit à l'échelle dans aucune saveur (la réduction n'a pas été rejouée
  échelle active) ; il reste le présentateur simple, égal à
  960×540 et meilleur en WebGPU à 1080p (×1,8 images) ; forcé, `mwsr` coûte une image de plus que
  `gl-mwsr`. La décision par classe reste à Bruno (plan §9, P6). Incident : `stream.dev` et
  `stream.moonlightweb.top` injoignables de 11:39 à 11:44 (ligne rendez-vous de la `--dev` perdue puis
  reprise), signalé à 59.

## 2026-10-22 — J29, le troisième repli de l'import, l'abandon du renderer, la CI rejouée sous Linux

Décisions de Bruno du 22/10 (plan §9) : un troisième repli de l'import (`VideoFrame.copyTo` vers RGBA
puis `writeTexture`), la bascule sur `gl-<algo>` puis Canvas2D quand tout échoue N fois de suite, et la
règle de sortie du job CI « Upscaler lab ». Codés au go de Bruno, après le `/compact`.

- **Ce que disait la CI du 22/10 au matin (run 39737555924, la première avec le repli bitmap de J21).**
  Côté WebGPU, 24 problèmes : l'import refusé, puis « Failed to copy content from external image » sur
  la copie du bitmap, puis les cellules du noyau et leurs dumps en « A valid external Instance reference
  no longer exists ». Rejoué à l'identique dans le WSL Debian de DualRTX (Chrome 156 stable, SwiftShader,
  sans GPU : `/dev/dxg` masqué dans un espace de montage à part, le chargeur Vulkan réduit à l'ICD
  SwiftShader de Chrome, `CI=1` comme sur le runner).
- **Le vrai coupable : le canvas, pas la copie.** Une page de sonde nue sur la même plateforme : l'import
  est refusé, mais la copie du bitmap et `copyTo` → `writeTexture` marchent (pixels exacts, toutes les
  tailles du labo, isolation cross-origin ou non). Ce qui casse, c'est le **canvas WebGPU** : à son premier
  `getCurrentTexture`, la swap chain ne trouve aucune mémoire partagée à allouer (stderr : « Could not find
  SharedImageBackingFactory … WebgpuSwapChainTexture », « SharedImageStub: Unable to create shared
  image »), Chrome détruit le device (`lost` : « destroyed », « Device was destroyed. ») et coupe
  l'instance WebGPU de toute la page (tous les autres devices, celui du labo compris) ; un nouveau
  `requestAdapter` repart. Le 21/10, le dessin mourait à l'import, avant le canvas : le noyau était
  épargné. Le repli bitmap de J21 a mené le dessin jusqu'au canvas, et l'instance est tombée avec lui.
  Le renderer du produit ignorait la perte « destroyed » : sur cette plateforme il restait noir pour de bon.
- **Le produit (`WebGpuRenderer`).** Trois voies d'entrée : l'import, puis la copie du bitmap dans
  `inputTex`, puis la lecture (`copyTo` en RGBA, `writeTexture` dans `readbackTex`, que la passe 0 copie
  comme la texture externe ; pipeline fait à la première lecture, au format d'`inputTex`, flottant en HDR).
  Un `copyTo` qui ne sait pas convertir est reconnu par l'accesseur du membre `format` (WebIDL lit tous
  les membres qu'il connaît) : sinon il rendrait les plans NV12 de l'image, vus comme des couleurs. Le
  refus du bitmap se redemande avec l'import (tous les 300 draws, et tout de suite après une image
  perdue). Une image est perdue quand aucune voie ne l'a prise, que le dessin a levé une exception ou
  que son travail ne s'est pas achevé sur un device resté debout ; 30 de suite (une demi-seconde à 60 i/s)
  et le renderer abandonne (`failed`). Il abandonne aussi quand son device est perdu avant d'avoir fini
  une image (le cas Linux : chaque nouveau device subirait le même sort) ou ne revient pas. Une perte
  « destroyed » n'est plus ignorée : elle n'est la sienne qu'une fois le renderer libéré. Pendant que les
  images sont copiées, un dessin à la fois (`serialDrawsOnly`) : deux dessins qui attendent pourraient
  arriver dans le désordre, et la lecture n'a qu'un tampon.
- **La page (`StreamView`).** Au tick de 500 ms, un renderer qui a abandonné passe la main, une fois :
  `rendererAfterFailure` donne la passe qu'il faisait sur son jumeau WebGL2 (le réglage en plafond,
  l'échelle reprise à son barreau), la copie simple sur le présentateur que la page prend pour elle, et
  `createVideoRenderer` répond par Canvas2D si WebGL2 refuse. Fil principal seulement : dans le worker
  (option de debug, coupée par défaut) le renderer abandonne mais reste, son `OffscreenCanvas` est le sien.
- **Le labo.** `importfail=2` refuse aussi la copie du bitmap, `importfail=3` la lecture ; `nopresent=1`
  fait dessiner les chaînes WebGPU dans une texture du labo à chaque image (le crochet des dumps de J19,
  étendu), et la conformité le passe toujours ; une chaîne dont la dernière image n'est entrée par aucune
  voie, ou dont le renderer a abandonné, échoue en le disant. **La règle du job (`p4_flows.conformity`)** :
  sortie 0 quand les seuls problèmes sont des chaînes non applicables, chacune une annotation
  `::warning::` sous GitHub Actions ; sortie 1 pour une vraie différence ou une image manquante, avec une
  annotation `::error::`.
- **Vérifications.** `vitest` : `WebGpuRendererImport.test.js` 17 cas (les cinq de J21, la lecture, ses
  refus, l'abandon par images perdues, par travail inachevé, par device détruit, par reprise impossible,
  la reprise d'un device qui avait dessiné), `rendererAfterFailure` 4, `StreamViewRendererFailed.test.js`
  6 ; suite complète 1686 tests verts, couverture 94,0 % ; deux mutations (limite à 31, « destroyed » de
  nouveau ignoré) attrapées. SwiftShader sous Windows (Chrome 155) : conformité OK des deux côtés ;
  `importfail=1` et `2`, 9 paires dorées exactes sur 9 ; `importfail=3`, non applicable, sortie 0. Sous
  Linux (WSL, Chrome 156, la plateforme du runner) : côté WebGPU, conformité OK, la chaîne entrée par le
  bitmap (import refusé) exacte sur les 9 cas et de qualité identique sur les 3 vérités terrain ; par la
  lecture (`importfail=2`) exacte aussi ; `importfail=3` sous `GITHUB_ACTIONS` : une annotation
  `::warning::`, sortie 0.
- **Une vraie différence, révélée sous Linux.** Les paires du jumeau WebGL2 se comparent pour la
  première fois sous Linux (jusqu'ici les dumps du noyau manquaient) : **zone plate 2560×1440 → 1280×720,
  `chain-gl-mwsr` contre `mwsr`, max 45/255, 1,01 % des pixels au-dessus de 2/255 : hors tolérance**,
  sortie 1 au pas WebGL2 (sous Windows : 3/255, 0,009 %). Les dumps d'une plateforme à l'autre : le
  jumeau GL est identique au bit près sous Linux et sous Windows ; c'est le côté WebGPU qui bouge, et pas
  le noyau : le bilinéaire nu du labo (`tex-s1`) diffère déjà de 36/255 sur 1,4 % des pixels (la moitié
  droite, où la mire approche Nyquist), `mwsr` de 45, les vues sRGB de 49. Chrome 155 et 156 sous Linux
  donnent les mêmes images au bit près (Chrome for Testing 155.0.8059.84) : c'est la plateforme, pas la
  version. Piste : l'interpolation des varyings du triangle plein écran côté WebGPU de SwiftShader
  (le jumeau GL, identique partout, ne dépend pas d'elle de la même façon) ; non vérifiée. Le texte, lui,
  diffère partout entre les deux plateformes : l'entrée elle-même, les polices du système.
- **Verdict.** Le troisième repli et la bascule sont en place et vérifiés ; la chaîne WebGPU du produit
  redevient jugeable sur le runner. Avec la règle décidée, le job sortira en 1 au pas WebGL2 sur la
  seule réduction de la zone plate. À Bruno : la garder comme différence connue, ou juger cette paire
  contre l'écart des deux bilinéaires nus des deux API sur les mêmes pixels (un changement de méthode
  de C11).

## 2026-10-23 — J30, le comparateur côte à côte (`compare.html`), pour l'œil et le téléphone

Demande de Bruno (23/10) : une page qui compare visuellement les algorithmes côte à côte sur son image
1440p, en réduction (1440 → 1080, 1440 → 720) et en agrandissement (720 → 1440, 1080 → 1440, l'entrée
étant une réduction bicubique), avec le choix et l'ordre des algorithmes (natif, mw, fsr1, sgsr1, nis),
des bandes horizontales ou verticales, les réglages derrière un petit bouton, le zoom à deux doigts et
le déplacement à un doigt, et deux chiffres par algorithme : la qualité contre l'original et la moyenne
du temps sur 1000 itérations.

- **La page** (`scripts/bench/scaler-lab/compare.html`, `compare.js`, `compare-worker.js`), servie
  comme le labo depuis la racine du dépôt (`scaler_lab.py --serve` imprime son lien, `/compare`) : les
  renderers du produit eux-mêmes (`WebGpuRenderer`, `WebGlRenderer`, un par API, la passe changée par
  `setAlgo` comme l'échelle le fait), chacun sur un `OffscreenCanvas` à la taille de sortie ; « natif »
  est `drawImage` bilinéaire (qualité `low`), « Smooth2D » la qualité `high`, en option. L'entrée est
  une `VideoFrame` RGBA de l'image (pas de codec). Les sorties sont capturées (`transferToImageBitmap`,
  sinon `createImageBitmap` du canvas) et dessinées dans des bandes sous un même zoom et un même
  déplacement : par défaut la **même zone** dans chaque bande, vue 1:1 (un pixel de sortie par pixel
  d'écran), centrée ; l'autre mode coupe une image continue. Au-dessus de 1:1 le voisin le plus proche
  (les pixels se voient) ; en dessous l'afficheur réduit lui-même, et le dit.
- **Les deux chiffres.** Qualité : SSIM de la luma (fenêtre gaussienne de Wang, σ 1,5, bord coupé : la
  définition de `metrics.py`) et PSNR RGB, contre l'image à 1440p ; à 1080p et 720p, contre sa réduction
  bicubique (Catmull-Rom, support élargi, espace gamma : le `BICUBIC` de Pillow), qui est aussi l'entrée
  des agrandissements, la même référence servant aux deux sens. Temps : la moyenne de `iters` (1000)
  images après 20 de chauffe, de l'appel du dessin à la fin du GPU, le `done` du labo (WebGPU l'attend
  dans `draw()`, WebGL2 par une fence scrutée tâche par tâche, Canvas2D par la lecture d'un pixel), en
  pause pendant les gestes ; le p50 est dans le tableau du panneau. Les réductions et le SSIM (par bandes
  de 64 lignes) tournent dans un worker.
- **API « auto »** : la décision de Bruno du 22/10 (plan §9), WebGL2 partout et WebGPU sur iPhone et
  iPad ; WebGPU ou WebGL2 au choix (WebGL2 quand WebGPU manque ou refuse). Cinq cas, dont 720 → 1080,
  ajouté (la demande écrivait deux fois 720 → 1440). L'état est dans la requête de la page (et le dernier
  dans localStorage) : un lien se partage.
- **Vérifié sur SwiftShader (Chrome 155 headless sous Windows, `00_Background.png`, ×2, 20 itérations)**,
  par un pilote CDP du scratchpad. Côté WebGL2 (ANGLE sur SwiftShader) : cinq sorties, SSIM natif 0,857,
  MW 0,871, FSR1 0,881, SGSR1 0,879, NIS 0,875 (PSNR 21,6 à 22,1 dB sur l'image entière : l'ordre des
  repères du 20/10). Côté WebGPU (adaptateur logiciel, `soft=1`) : les quatre mêmes à 10⁻⁴ près. Molette,
  glisser, pincement à deux points et double appui conformes ; l'émulation 390×844 à 3× en bandes
  horizontales lisible, le panneau utilisable au doigt. Non vérifié : la capture d'un canvas WebGPU sous
  Safari (le repli `createImageBitmap` est là, et un échec se lirait « capture noire » sur la bande).
- **L'image** : `C:\Test\test.png`, servie sous `/test/`, à défaut l'étalon `00_Background.png` ;
  `test.png` n'existait sur aucun disque de DualRTX le 23/10. Le panneau accepte aussi un fichier.
- **Le même matin, sur l'iPhone de Bruno (Safari 26.5, WebGPU)** : la page marche, SSIM à 0,001 près
  celles de SwiftShader (Natif 0,856, MW 0,870, FSR1 0,880, SGSR1 0,879, NIS 0,874), mais zoom et
  déplacement mettaient plusieurs secondes à suivre le doigt. Deux causes, corrigées. Le chrono Canvas2D
  ne rendait la main qu'une image sur dix (un `drawImage` de `VideoFrame` 1440p plus la lecture d'un
  pixel, dix fois de suite : une seconde d'écran figé sur un téléphone) : une tâche entre chaque image,
  hors mesure, pour les trois API. Et l'afficheur, un `drawImage` de bitmap 1440p par bande et par image,
  dessine en WebGL2 : chaque sortie est une texture (mipmappée) envoyée une fois, une image est un clear
  et un quad par bande, `texelFetch` à partir de 1:1 (les pixels tels quels, sans le doute d'un
  échantillon sur une arête de texel) ; le tracé Canvas2D reste le repli sans WebGL2, et sur
  `?viewer=2d`. Vérifié sur SwiftShader par le pilote : même tableau, gestes conformes, émulation
  390×844 identique à 99,8 % au tracé Canvas2D (le reste : les chiffres des pastilles). Au doigt sur
  l'iPhone, 08:04 : « fluidité du zoom : ok ».
- **La butée (Bruno, 08:04)** : on pouvait sortir l'image entière de la bande. Les bords de l'image ne
  passent plus le centre du cadre (la bande en « même zone », la page en « image continue »), à tout
  zoom : au plus une moitié de noir par axe. En butée le glisser suit l'image (l'ancre du geste est
  resuivie : le doigt qui revient la déplace aussitôt) et le pincement va pas à pas depuis le dernier
  événement, pour la même raison. Vérifié par le pilote sur SwiftShader : bureau, ajustement puis un
  glisser trop loin de chaque côté, l'arrêt aux valeurs attendues ; émulation 390×844 en bandes
  horizontales, deux glissers vers le bas et la droite, idem.

## 2026-10-23 — J31, la netteté de `mwsr` à l'œil de Bruno : balayage `--quality`, deux curseurs dans le comparateur

**Le constat (iPhone, Safari 26.5, WebGPU, 08:10-08:15).** 1080p → 1440p, 100 images : MWSR
**3,69 ms** contre FSR1 8,12, SGSR1 5,10, NIS 68,2 (« bien plus rapide que les autres ») ; mais sur le
texte à ×17, à ×1,33 comme à ×2, « moins sharp que FSR1 et SGSR1, un peu trop flou ». Natif
(Canvas2D) y mettait 435 ms par image : Safari dessinait la `VideoFrame` faite de pixels par le CPU ;
le comparateur dessine maintenant le canvas du niveau (543c5cd5), 300 images par défaut.

**Le levier.** La netteté est l'uniforme `sharp` (× `W_ACROSS` 0,3), bornée par `W_MIN` −0,3 : à
netteté 1 la borne n'était jamais atteinte, et toute netteté > 1 n'agissait que sur les contours peu
cohérents (a < 1). `W_MIN` passe à **−0,5** (71b7cebc ; pixels identiques à netteté 1, golden OK),
`setMwsrTuning({tau, sharp})` sur les deux renderers (jamais appelé par le produit), et deux curseurs
dans `compare.html` (netteté 0,5-2, τ 0,01-0,1 ; l'état dans le lien). À ×1,33 la force vaut 0,667
(`S_FULL` 1,5) : la netteté 1,5 y donne les poids de la table à ×2.

**Balayage** (SwiftShader, `--quality`, `text` / `game` / étalon, ×2 = 1280×720 → 1440p et ×1,33 =
1080p → 1440p ; PSNR / SSIM / FLIP, l'étalon au tiers central pour PSNR et SSIM ; « ac » = pente moyenne
de la luma sur les contours de la vérité terrain, rapportée à la sienne, 1 = aussi raide ; dépassements
0 % pour toutes les variantes `mwsr`, `DERING` oblige) :

| variante | `text` ×2 | `game` ×2 | étalon ×2 | `text` ×1,33 | `game` ×1,33 | étalon ×1,33 |
|---|---|---|---|---|---|---|
| FSR1 | 17,57 / 0,845 / 0,068 · ac 0,55 · dép. 0,3 % | 35,55 / 0,887 / 0,017 · 0,88 | 32,26 / 0,940 / 0,045 · 0,58 · dép. 2 % | 21,45 / 0,942 / 0,042 · 0,77 | 37,92 / 0,933 / 0,013 · 1,00 | 35,94 / 0,976 / 0,035 · 0,82 · dép. 5 % |
| SGSR1 | 18,13 / 0,869 / 0,067 · 0,66 · dép. 4,4 % | 35,61 / 0,883 / 0,018 · 0,96 | 31,28 / 0,930 / 0,052 · 0,62 · dép. 6,8 % | 21,64 / 0,946 / 0,050 · 0,86 · dép. 2,7 % | 37,70 / 0,925 / 0,014 · 1,04 | 34,55 / 0,970 / 0,040 · 0,87 · dép. 5 % |
| `mwsr`, netteté 1 (table) | 17,41 / 0,842 / 0,071 · 0,51 | 35,84 / 0,882 / 0,019 · 0,88 | 31,95 / 0,934 / 0,051 · 0,53 | 21,50 / 0,942 / 0,047 · 0,74 | 37,99 / 0,920 / 0,012 · 0,96 | 36,76 / 0,979 / 0,038 · 0,73 |
| netteté 1,25 | 17,44 / 0,844 / 0,072 · 0,52 | 35,84 / 0,882 / 0,019 · 0,91 | 31,53 / 0,931 / 0,054 · 0,55 | 21,65 / 0,945 / 0,047 · 0,76 | 38,09 / 0,920 / 0,013 · 0,97 | 36,40 / 0,978 / 0,039 · 0,75 |
| **netteté 1,5** | 17,43 / 0,844 / 0,075 · 0,53 | 35,66 / 0,882 / 0,020 · 0,94 | 30,93 / 0,927 / 0,057 · 0,57 | **21,76 / 0,948 / 0,047 · 0,77** | **38,13 / 0,920 / 0,013 · 0,99** | 35,84 / 0,976 / 0,040 · 0,76 |
| netteté 1,75 | 17,39 / 0,843 / 0,079 · 0,54 | 35,39 / 0,881 / 0,020 · 0,96 | 30,26 / 0,921 / 0,059 · 0,58 | 21,82 / 0,949 / 0,049 · 0,78 | 38,10 / 0,920 / 0,013 · 1,01 | 35,12 / 0,974 / 0,042 · 0,77 |
| netteté 2 | 17,34 / 0,842 / 0,082 · 0,54 | 35,33 / 0,881 / 0,020 · 0,96 | 29,97 / 0,918 / 0,060 · 0,58 | 21,81 / 0,950 / 0,052 · 0,79 | 38,00 / 0,920 / 0,013 · 1,02 | 34,29 / 0,971 / 0,043 · 0,78 |
| 1,5 + `S_FULL` 1,25 | — | — | — | 21,77 / 0,950 / 0,053 · 0,79 | 37,96 / 0,920 / 0,013 · 1,02 | 33,96 / 0,970 / 0,044 · 0,78 |
| 1,5 + τ 0,03 | 17,43 / 0,844 / 0,075 (36 % des pixels au noyau, contre 35) | 35,67 / 0,882 / 0,020 (2,7 % contre 2,0) | 30,91 / 0,927 / 0,057 (40 % contre 29) | 21,77 / 0,948 / 0,047 | 38,16 / 0,921 / 0,013 (8,2 % contre 1,6) | 35,84 / 0,977 / 0,040 (37 % contre 28) |
| 1,5 + `R_ACROSS` 1,25 | 17,44 / 0,846 / 0,080 · 0,53 | 36,02 / 0,882 / 0,019 · 0,95 | 30,81 / 0,925 / 0,056 · 0,57 | — | — | — |

**Lecture.** La netteté 1,5 amène le texte à l'acuité de FSR1 à ×1,33 (0,77 = 0,77) avec un meilleur
PSNR et un meilleur SSIM que lui (21,76 / 0,948 contre 21,45 / 0,942), le même FLIP qu'à 1, et
`game` au niveau de FSR1 (0,99 contre 1,00) avec le meilleur PSNR de la table ; à ×2 elle reste à 0,015
de FSR1 sur le texte, le dépasse sur `game`. Son prix, c'est l'étalon (une scène douce, où FSR1 tire
sa netteté de 2 à 5 % de dépassements, des halos) : −1 dB au centre à ×2 et −0,9 dB à ×1,33 par
rapport à la table, mais toujours **au-dessus de FSR1 à ×1,33** (35,84 contre 35,94 au centre, FLIP
0,040 contre 0,035) ; à ×2 le critère C3 déborde de peu (FLIP 0,057 contre 0,055 admis, SSIM −0,0065
contre −0,005), avec 0 % de dépassements contre 2 % pour FSR1. 1,25 tient C3 partout mais ne change
presque rien à l'œil (acuité +0,02) ; 1,75 et 2 sortent de C3 sur le texte à ×2 (FLIP 0,079-0,082).
`S_FULL` 1,25 (force pleine à ×1,33) : −2,8 dB au centre de l'étalon, rejeté ; τ 0,03 : rien aux
chiffres pour cinq fois plus de pixels au noyau sur `game` à ×1,33, rejeté ; `R_ACROSS` 1,25 : +0,36 dB
sur `game` mais le FLIP du texte à 0,080, à revoir seulement si Bruno trouve 1,5 insuffisant.
**Proposition : netteté 1,5 par défaut** (une constante, `MWSR_UNIFORMS.sharp`), à appliquer après le
signal de 59 (un banc C4 sert l'arbre de travail), golden à refaire ; l'œil de Bruno tranche, par les
curseurs du comparateur (1 / 1,25 / 1,5 / 1,75 sur l'iPhone, bande contre bande).

**Où va le PSNR (09:50, sur les dumps du balayage, le masque `mwsr:show=1`).** Étalon à ×1,33 : 72 % des
pixels sont sur le chemin plat, où `mwsr` **est** le bilinéaire (31,10 dB = `tex-s1`), FSR1 y fait 31,45 ;
sur les 28 % de contours, FSR1 19,20, `mwsr` 18,97 (netteté 1), 18,92 (1,5), 18,79 (2), bilinéaire 18,77.
L'avance de FSR1 sur l'image entière (+0,25 dB au labo, +0,6 dB dans le comparateur de Bruno avec
l'entrée bicubique, 25,1 contre 24,5) vient donc **pour l'essentiel du chemin plat** (douze texels
d'EASU contre un échantillon bilinéaire, le prix de la vitesse) et pour le reste de la reconstruction
des contours ; la netteté ne peut pas l'acheter (elle baisse le PSNR des contours, comme RCAS). Sur la
mire `text`, les plats sont à 40,1 (= bilinéaire) contre 42,4 pour FSR1, les contours montent avec la
netteté (15,75 → 16,02 → 16,07). Le comparateur sur SwiftShader donne les chiffres de l'iPhone au
centième (natif 24,20, `mwsr` 24,54 / 24,57 / 24,52 à 1 / 1,5 / 2, FSR1 25,17, SGSR1 24,83).

**À l'œil (09:50, captures du comparateur sur SwiftShader, WebGL2, les libellés du coin haut gauche de
l'étalon à ×4 puis ×4 au plus proche voisin).** « Ori.png », blanc sur sombre : natif mou avec un halo
gris ; 1,0 un peu plus défini ; **1,5 nettement plus net** (bords plus blancs et plus sombres, les
boucles du n et du g lisibles) ; 2,0 plus net encore, près de FSR1 ; FSR1 le plus propre ; SGSR1 le plus
dur, mais en escalier sur le O. « 1440p.png », sombre sur clair avec les franges colorées du ClearType :
tous proches, 1,5 et 2 un peu plus contrastés que 1, FSR1 ≈ 2 ; seul SGSR1 paraît « plus net », par ses
blocs ; la cohérence `a` prend ces franges pour de la texture et retient le noyau. Verdict : la netteté
1,5 se voit sur le texte, 2 rapproche de FSR1 sur fond sombre pour −0,1 dB et +0,003 de FLIP ; **le PSNR,
lui, ne bougera qu'avec un chemin plat mieux reconstruit** (un Catmull-Rom à quatre échantillons
bilinéaires en palier qualité, +3 échantillons partout : à chiffrer, au go de Bruno).

**Décision de Bruno (09:55), devant les planches : netteté 2,0 par défaut**, et un chemin plat mieux
reconstruit « en paramètres » (J33). Appliqué à 10:05 : `MWSR_UNIFORMS.sharp` 2,0, le test unitaire,
`golden.json` refait (chaîne = noyau sur les neuf cas), conformité OK des deux côtés (jumeau WebGL2 :
FLIP 0,000, PSNR ≤ 0,038 dB, 0 % de dépassement ; réductions au mètre des bilinéaires), `npm run check`
vert, 1717 tests verts. Le curseur du comparateur monte à 3. Chiffres de 2,0 (J31) : texte ×1,33 21,81 /
0,950 / FLIP 0,052 (FSR1 21,45 / 0,942 / 0,042), acuité 0,79 (FSR1 0,77) ; étalon ×1,33 au centre 34,29
(table 36,76, FSR1 35,94), FLIP 0,043 (0,035) ; `game` 38,00 (FSR1 37,92).

**Appliqué (09:10, signal de 59 à 08:55)** : `MWSR_UNIFORMS.sharp` 1,5 dans `MwsrShader.js`, le test
unitaire qui épinglait 1 mis à jour, `golden.json` refait sur SwiftShader (la chaîne du produit égale
au noyau sur les neuf cas, `chain-mwsr` = `mwsr`), conformité C11 OK des deux côtés (WebGPU 0/255
partout ; jumeau WebGL2 contre le noyau : FLIP 0,000, PSNR à 0,027 dB, 0 % de dépassement, les écarts
de pixels restant ceux des dérivées, connus), `npm run check` vert, 1717 tests verts. C'est un
changement du rendu du produit, à dire dans les notes de version ; le comparateur part désormais de 1,5
et le bouton « Défauts du produit » y ramène. Le verdict de Bruno à l'œil reste attendu.

## 2026-10-23 — J32, C11 option 2 : la réduction du jumeau jugée contre les bilinéaires nus

Décision de Bruno (23/10, par 59) sur la question de J29 : **option 2**, juger l'écart
`chain-gl-mwsr` / `mwsr` d'une réduction contre l'écart des deux bilinéaires nus des deux API sur les
mêmes pixels, au lieu de garder la zone plate comme différence connue. Entre-temps la CI de ac6d705b
(run 39912544431) sortait rouge au pas WebGL2 sur **les trois** réductions 2560×1440 → 1280×720, pas la
seule zone plate : `code` max 4/255, 0,173 % au-dessus de 2/255 ; `text` max 5/255, 2,80 % (18,6 % de
pixels différents) ; `zone` max 45/255, 1,01 %.

**La méthode (`p4_flows.py`).** Sur chaque cas de réduction, les deux côtés dumpent aussi leur
bilinéaire nu : `tex-s1` (la cellule du labo, WebGPU) et `chain-gl-off` (la copie simple du produit,
WebGL2), un échantillon bilinéaire au centre du pixel de sortie dans les deux cas. Leur écart est
imprimé (« the samplers' own difference ») et sert de mètre à la paire du jumeau : part de pixels
au-delà de 2/255 ≤ 4 × la leur, maximum ≤ 2 × le leur, jamais sous la tolérance fixe (0,1 %, 4/255).
Le 4 : un pixel de la grille 2×2 diffère dès qu'un de ses quatre échantillons diffère ; le 2 : la
lumière gamma-2 (carré, racine) élargit l'écart d'un échantillon près du noir. Les agrandissements
restent « reported (derivatives) » et jugés par la qualité ; `ci.yml` ne change pas (mêmes commandes).

**Rejeu de la plateforme du runner (WSL Debian, Chrome stable, SwiftShader, sans GPU, la recette de
J29), les deux côtés, sortie 0 et 0 :**

| réduction 2560×1440 → 1280×720 | bilinéaires nus (`chain-gl-off` / `tex-s1`) | jumeau (`chain-gl-mwsr` / `mwsr`) | verdict |
|---|---|---|---|
| `code` | 2/255, 3,80 % de pixels différents, 0 % au-delà de 2 | 2/255, 3,95 %, 0 % | OK |
| `text` | 1/255, 4,96 %, 0 % | 1/255, 4,48 %, 0 % | OK |
| `zone` | **36/255, 1,44 %, 0,854 %** | 45/255, 1,47 %, 1,013 % | OK (≤ 4 × 0,854 %, ≤ 2 × 36) |

La zone plate donne au bit près les chiffres de la CI (45/255, 1,47 %, 1,013 %, 42,5 dB) : c'est bien
la plateforme, et l'écart du jumeau y est celui des échantillonneurs (36/255 sur 0,85 % pour un seul
échantillon). `text` et `code` passent ici sous la tolérance fixe (1 à 2/255, 0 %), là où le runner
voit 5/255 sur 2,8 % : sur le runner, la différence est donc dans l'**entrée** que chaque Chrome
dessine lui-même (la mire de texte, par Canvas2D : rastérisation CPU d'un côté, ANGLE sur SwiftShader
de l'autre, un bord de glyphe à quelques 255ᵉ près), pas dans le noyau ; le mètre des bilinéaires la
porte par construction, puisqu'il voit la même entrée que le jumeau. À vérifier sur le runner au
prochain push de Bruno ; si `text` y restait hors du mètre, ce serait une vraie différence à lire.

**Côté Windows (SwiftShader, les deux côtés, sortie 0 et 0)** : bilinéaires nus `code` 1/255 (2,12 % de
pixels différents, 0 % au-delà de 2), `text` 1/255 (4,89 %, 0 %), `zone` 4/255 (0,48 %, 0,011 %) ; le
jumeau 1/255, 1/255 et 3/255 (0,009 %) : la différence du jumeau est bien celle des échantillonneurs,
ici comme sous Linux. Qualité du jumeau contre le noyau inchangée (FLIP 0,000, PSNR ≤ 0,027 dB).

## 2026-10-23 — J33, le chemin plat en paramètres : un cubique d'échantillons bilinéaires

Décision de Bruno (09:55) : « un chemin plat mieux reconstruit, oui, en paramètres ». Le constat de J31
(le chemin plat **est** le bilinéaire, et c'est là que FSR1 prend son avance sur les pixels plats, 72 %
de l'étalon à ×1,33) appelait une reconstruction meilleure qu'un échantillon, sans changer le produit.

**Les paramètres (`MwsrShader.js`, 3c42d89d).** Deux constantes de pipeline, dans les deux jumeaux :
`FLAT_TAPS` (1 : l'échantillon bilinéaire, le défaut ; 5 ou 9 : le cubique de Mitchell-Netravali à
B 0 et C `FLAT_C` sur les 4×4 texels autour de la position) et `FLAT_C` (0,5 = Catmull-Rom ; plus haut,
lobe négatif plus profond). Le cubique est lu comme les échantillons bilinéaires que le GPU pèse pour
rien (Jimenez, Pettineo) : sur chaque axe les deux texels du milieu par un échantillon à leur point
médian pondéré (`tc + w2 / (w1 + w2)`), les deux texels extérieurs par un échantillon chacun ; neuf
échantillons, ou cinq sans les quatre coins (les plus petits poids), le reste renormalisé ; le résultat
borné par les échantillons eux-mêmes, comme le chemin contour. Pour le comparateur, `setMwsrOverrides()`
sur les deux renderers reconstruit les pipelines ou les programmes (jamais appelé par le produit) ; la
page a un sélecteur « chemin plat » et un curseur « lobe C » ; les cellules du labo prennent `flat=` et
`fc=`. Vérifié : golden identique (le défaut ne change pas un pixel), conformité OK des deux côtés,
1718 tests, `npm run check`.

**Balayage (SwiftShader, `--quality`, PSNR entier / plat / contours par le masque `show=1`, SSIM,
dépassements ; FSR1 en repère) :**

| ×1,33 | étalon : entier / plat / contours · SSIM · dép. | `game` | `text` |
|---|---|---|---|
| FSR1 | 24,15 / 31,45 / 19,20 · 0,941 · 5,0 % | 37,92 / 39,80 / 24,47 · 0,933 · 0,4 % | 21,45 / 42,37 / 15,69 · 0,942 · 0,2 % |
| `mwsr` (netteté 2) | 23,74 / 31,10 / 18,79 · 0,936 · 0 % | 38,00 / 39,44 / 25,46 · 0,920 · 0 % | 21,81 / 40,11 / 16,07 · 0,950 · 0 % |
| CR5 (C 0,5) | 23,78 / 31,35 / 18,79 · 0,938 · 0,05 % | 38,16 / 39,67 / 25,46 · 0,925 · 0 % | 21,83 / 41,51 / 16,07 · 0,951 · 0 % |
| **CR9 (C 0,5)** | **23,78 / 31,35 / 18,79 · 0,938 · 0,05 %** | **38,24 / 39,79 / 25,46 · 0,928 · 0 %** | **21,83 / 41,60 / 16,07 · 0,951 · 0 %** |
| CR9, C 0,75 | 23,78 / 31,38 / 18,79 · 0,939 · 0,18 % | 38,27 / 39,83 / 25,46 · 0,929 · 0,02 % | 21,83 / 41,89 / 16,07 · 0,951 · 0,01 % |
| CR9, C 1 | 23,78 / 31,36 / 18,79 · 0,938 · 0,37 % | 38,28 / 39,84 / 25,46 · 0,929 · 0,04 % | 21,83 / 41,98 / 16,07 · 0,951 · 0,01 % |

| ×2 | étalon | `game` | `text` |
|---|---|---|---|
| FSR1 | 20,79 / 22,84 / 17,99 · 0,874 · 2,0 % | 35,55 / 38,03 / 22,02 · 0,887 · 0,03 % | 17,57 / 35,13 / 13,04 · 0,845 · 0,3 % |
| `mwsr` (netteté 2) | 20,52 / 22,80 / 17,55 · 0,862 · 0 % | 35,33 / 37,96 / 21,61 · 0,881 · 0 % | 17,34 / 33,56 / 12,82 · 0,842 · 0 % |
| CR5 | 20,53 / 22,83 / 17,55 · 0,866 · 0,01 % | 35,37 / 38,04 / 21,61 · 0,884 · 0 % | 17,35 / 34,57 / 12,82 · 0,843 · 0 % |
| **CR9** | **20,54 / 22,84 / 17,55 · 0,867 · 0,03 %** | **35,39 / 38,07 / 21,61 · 0,886 · 0 %** | **17,35 / 34,61 / 12,82 · 0,843 · 0 %** |

**Lecture.** Sur les pixels plats, CR9 à C 0,5 **rejoint FSR1** : étalon 31,35 contre 31,45 et 22,84 =
22,84, `game` 39,79 contre 39,80 et 38,07 contre 38,03, `text` à 0,5-0,8 dB ; CR5 en prend les deux
tiers ; C au-delà de 0,5 n'ajoute que des dépassements. Sur l'image entière le gain est ce que la part
d'erreur des plats permet : +0,24 dB sur `game` à ×1,33 (MWSR passe devant FSR1 : 38,24 contre 37,92),
+0,04 sur l'étalon, +0,02 sur le texte ; SSIM +0,003 à +0,008 (étalon ×2 : 0,862 → 0,867, FSR1 0,874).
Pourquoi si peu sur l'étalon : 28 % de ses pixels sont des contours et portent **87 % de l'erreur**
(18,8 dB contre 31 sur les plats) ; là, c'est la reconstruction à cinq échantillons contre les douze
texels d'EASU (J7, trois tours, on s'arrête) et la netteté 2 (18,79 contre 18,97 à netteté 1) qui
comptent, pas le chemin plat. Le comparateur sur SwiftShader (entrée bicubique, ×1,33) dit la même
chose : CR9 0,9415 / 24,57 contre 0,9386 / 24,52 (FSR1 0,948 / 25,17) ; coût CPU ×3,7 sous SwiftShader,
qui ne dit rien d'un GPU. **À Bruno** : le temps et l'œil sur l'iPhone (⚙ → chemin plat CR9, C 0,5 ;
la texture externe y paie deux fetches par échantillon, donc neuf au lieu d'un sur 72 à 98 % des
pixels) ; si le temps passe, CR9 à C 0,5 est la valeur à retenir, en palier qualité ou par défaut.

## 2026-10-23 — J34, un chemin plat quadratique à trois échantillons (`FLAT_TAPS 3`), et la zone du regard en paramètres

**La question de Bruno (10:50, après l'iPhone).** CR5 à netteté 1,85 lui plaît, mais sa capture de
11:02 (1080p → 1440p, WebGPU, 100 images) dit le prix : MWSR **5,66 ms** avec CR5 contre 3,9 ms
nu, SGSR1 5,70, FSR1 8,96 — le chemin plat à cinq échantillons sur 72 à 98 % des pixels mange
l'avance. « Un Catmull-Rom 3, si ça existe ? » Il n'existe pas : le cubique lit quatre texels par
axe, trois échantillons bilinéaires par axe (la paire du milieu fusionnée, les deux extérieurs
seuls), neuf en deux dimensions, cinq sans les coins — le plancher de Jimenez ; en dessous il
faudrait lâcher un axe. Ce qui existe en trois, c'est le **quadratique interpolant de Dodgson
(1997)** : trois texels par axe, le plus proche et son voisin du côté de l'échantillon fusionnés en
un échantillon bilinéaire à leur barycentre, le lointain, de poids négatif (jusqu'à −1/16), en un
échantillon ponctuel ; le coin lâché et le reste renormalisé : trois échantillons. Sa faiblesse,
connue d'avance : à mi-chemin entre deux texels (`h = ±0,5`) le poids lointain s'annule et il
redevient bilinéaire, précisément où le cubique apporte le plus. Go de Bruno (11:0x, par 59) :
en option de labo, le défaut du produit inchangé. Fait (6fbc3627) : `FLAT_TAPS 3` dans les deux
jumeaux, borné par ses échantillons ; « quadratique, 3 échantillons » dans le comparateur
(`flat=3`) ; vérifié par les deux Chromes de SwiftShader : mêmes chiffres des deux côtés (SSIM
0,9403, PSNR 24,56 sur l'étalon à ×1,33), golden intact (le défaut ne change pas un pixel).

**Balayage (SwiftShader, `--quality`, netteté 1,85, PSNR entier / plat / contours par le masque
`show=1` ; FSR1 en repère) :**

| ×1,33 (1080p → 1440p) | nu (bilinéaire) | Q3 | CR5 | FSR1 |
|---|---|---|---|---|
| étalon (28 % de contours) | 23,78 / 31,10 / 18,84 | 23,80 / 31,20 / 18,84 | 23,82 / 31,35 / 18,84 | 24,15 / 31,45 / 19,20 |
| `game` (1,6 %) | 38,07 / 39,44 / 25,70 | 38,18 / 39,60 / 25,70 | 38,24 / 39,67 / 25,70 | 37,92 / 39,80 / 24,47 |
| `text` (26 %) | 21,82 / 40,11 / 16,07 | 21,82 / 40,32 / 16,07 | 21,83 / 41,51 / 16,07 | 21,45 / 42,37 / 15,69 |
| SSIM étalon / game / text | 0,937 / 0,920 / 0,950 | 0,938 / 0,924 / 0,950 | 0,939 / 0,925 / 0,950 | 0,941 / 0,933 / 0,942 |

| ×2 (720p → 1440p) | nu | Q3 | CR5 | FSR1 |
|---|---|---|---|---|
| étalon (29 %) | 20,53 / 22,80 / 17,58 | 20,55 / 22,84 / 17,58 | 20,55 / 22,83 / 17,58 | 20,79 / 22,84 / 17,99 |
| `game` (2,0 %) | 35,34 / 37,96 / 21,65 | 35,39 / 38,04 / 21,65 | 35,39 / 38,04 / 21,65 | 35,55 / 38,03 / 22,02 |
| `text` (35 %) | 17,37 / 33,56 / 12,86 | 17,38 / 34,50 / 12,86 | 17,38 / 34,57 / 12,86 | 17,57 / 35,13 / 13,04 |
| SSIM étalon / game / text | 0,863 / 0,881 / 0,843 | 0,866 / 0,884 / 0,843 | 0,867 / 0,884 / 0,844 | 0,874 / 0,887 / 0,845 |

**Lecture.** À ×2, Q3 **vaut CR5** sur les pixels plats (et FSR1 : étalon 22,84 = 22,84, `game`
38,04 contre 38,03) : les centres des pixels de sortie tombent à un quart de texel (`h = ±0,25`),
là où le quadratique est au mieux, à un cheveu du cubique. À ×1,33, un pixel sur quatre par axe
tombe à mi-chemin et Q3 prend la **moitié** du gain de CR5 sur l'étalon et `game` (31,20 entre
31,10 et 31,35 ; 39,60 entre 39,44 et 39,67), un cinquième sur `text` (40,32 : les plats du texte
sont les bords des lettres, à toutes les phases). Dépassements 0 % partout. Pour trois
échantillons au lieu de cinq : à mesurer sur l'iPhone (lien `flat=3` à netteté 1,85, le même
réglage que son CR5 : chemin plat et netteté sont indépendants, les contours ne bougent pas d'un
centième de dB entre nu, Q3 et CR5).

**La zone du regard (go de Bruno, §15.2 du plan), en paramètres.** Faite le même jour dans les deux
jumeaux : la constante `GAZE` (0 par défaut, le texte d'avant au pixel près) et la table
`MWSR_ZONE` (centre 0,5 / 0,5, demi-axes 0,2 / 0,45, anneau 0,75, τ 0,02 et netteté 2,5 au
centre) ; `z = (1 − smoothstep(1, 1 + anneau, d)) · gaze`, `τ' = mix(τ, τc, z)`, `netteté' =
mix(n, nc, z)` ; `FLAT_ZONE` : le chemin plat CR ou Q3 dans la zone seule ; `SHOW 2` peint z. Le
poids `gaze` suit le mouvement de l'image dans le produit (`GazeZone.js` : bits par pixel et par
seconde des images delta, 0,25 → 0 et 1,0 → 1, montée 1 s, descente 3 s, par pas de 1/64 ; test
unitaire : un bureau immobile reste à 0, un flux 1080p à 19 Mbit/s passe 0,9 en 2,5 s, retombe
sous 0,1 en 9 s), ou est tenu à 1 par le comparateur. Produit : rien d'allumé, clé de labo
`mw_mwsr_gaze` (téléphones et tablettes, jamais un client de bureau), coupée par défaut. Jumeaux :
d'accord sur le comparateur avec la zone et `FLAT_ZONE` (SSIM 0,93671 / 0,93670, PSNR 24,458 des
deux côtés, SwiftShader).

**Mesure (SwiftShader, `--quality` à ×1,33, netteté 1,85 à la table, poids tenu à 1 ; PSNR dedans
l'ellipse verte / sur l'anneau / dehors, 28,3 % / 36,7 % / 35,0 % des pixels, poids moyen 47,9 %) :**

| ×1,33 | table | zone (τc 0,02, nc 2,5) | table + CR5 | zone + CR5 | zone + CR5 dans la zone seule | FSR1 |
|---|---|---|---|---|---|---|
| étalon | 35,22 / 25,46 / 20,59 | 32,82 / 25,31 / 20,59 | 35,32 / 25,48 / 20,63 | 32,84 / 25,33 / 20,63 | 32,84 / 25,31 / 20,59 | 36,35 / 25,81 / 20,94 |
| `game` | 37,83 / 38,80 / 37,59 | 37,34 / 38,72 / 37,59 | 37,96 / 39,01 / 37,75 | 37,40 / 38,88 / 37,75 | 37,40 / 38,80 / 37,59 | 37,44 / 39,06 / 37,33 |
| `text` | 21,82 / 21,64 / 22,02 | 21,65 / 21,60 / 22,02 | 21,83 / 21,65 / 22,03 | 21,66 / 21,61 / 22,03 | 21,66 / 21,60 / 22,02 | 21,44 / 21,28 / 21,66 |
| SSIM entier | 0,950 / 0,920 / 0,937 | 0,950 / 0,921 / 0,932 | 0,950 / 0,925 / 0,939 | 0,951 / 0,925 / 0,933 | 0,951 / 0,923 / 0,932 | 0,942 / 0,933 / 0,941 |
| part de pixels au noyau | 26,3 / 1,6 / 27,7 % | 27,1 / 9,5 / 37,3 % | | | | |

(SSIM et parts dans l'ordre `text` / `game` / étalon.)

**Lecture.** Dehors, rien ne bouge au centième : la périphérie est la table, comme voulu, et
`FLAT_ZONE` la rend au bilinéaire (20,59 contre 20,63 avec CR5 partout). Dedans, le PSNR baisse —
comme toute netteté ici (J31) : à 2,5 les contours du noyau s'écartent de la vérité terrain, et
τ 0,02 fait passer six fois plus de pixels de `game` par le noyau (1,6 → 9,5 %) ; le SSIM monte
un peu sur `text` et `game`, baisse sur l'étalon, dont le centre est un dégradé doux que τ 0,02
commence à « accentuer ». Les chiffres ne tranchent pas une préférence de netteté : c'est l'œil de
Bruno sur l'iPhone, avec les anneaux et les six curseurs (`gaze=1` dans le lien) ; les premières
valeurs sont celles du labo. Le temps sur un vrai GPU est à lire dans le coin de la bande (un
`length`, un `smoothstep`, deux `mix` de plus par pixel, et les pixels de plus au noyau au centre).

## 2026-10-23 — J35, le noir plus pâle des traits fins : la borne des cinq échantillons du chemin contour

**Constat de Bruno (iPhone, 12:13-12:16, 1080p → 1440p, ×16 sur l'emblème sombre de l'étalon) :**
« J'ai essayé différentes approches, toutes ont une couleur plus pâle sur le noir. » MWSR à
netteté 2,00, nu, CR5, Q3, avec ou sans zone : les traits sombres de l'emblème sortent gris-brun là
où FSR1 et SGSR1 les gardent noirs (ses captures : MWSR SSIM 0,936-0,940 · PSNR 24,4-24,5 ·
4,4-5,1 ms ; FSR1 0,948 · 25,2 · 7,9-8,3 ms ; SGSR1 0,946 · 24,8 · 4,4-5,0 ms).

**Pistes.** L'espace de couleur, non : le mode `up` lit et écrit les valeurs encodées (les vues sRGB
ne servent qu'à la réduction, J15), comme FSR1 et SGSR1. La cause est la borne du chemin contour
(plan §3.2, point 9) : `blend()` borne son résultat entre le minimum et le maximum de ses **cinq
échantillons bilinéaires**, puis `DERING 1` par les quatre texels. Sur une marche, les deux bornes se
valent (les échantillons en travers, à ±1 texel, atteignent les plateaux). Sur un **trait d'un
texel** — une crête — l'échantillon central est l'extrême des cinq, et la première borne épingle le
pixel à son mélange bilinéaire : à ×1,33 les colonnes de sortie tombent à 0, 0,75, 1,5 et 2,25
texel, et un trait noir D sur un fond O ne sort jamais plus sombre que 0,5·D + 0,5·O et
0,75·D + 0,25·O, quelle que soit la netteté (à netteté 2, le mélange voudrait 1,33·D − 0,33·O au
point milieu ; la borne le lui interdit). EASU et SGSR1 ne bornent que par les quatre texels voisins
(`min4`/`max4`), et leurs lobes négatifs, puis RCAS, font descendre le trait vers D : le noir « plus
profond » de leurs bandes. Le chemin plat n'est pas en cause : un trait noir sur orange passe par le
vote, CR5 et Q3 n'y sont jamais appelés (leurs bornes épingleraient de même, au demeurant).

**Mesure (SwiftShader, `--quality` à ×1,33 ; « traits fins » = pixels sombres de la référence,
Y < 0,25, avec un pixel clair, Y > 0,4, à deux pixels de part et d'autre : 5,55 % des pixels de
`text`, 0,32 % de l'étalon ; biais = luma de sortie − référence, + = plus pâle ; p1 = premier centile
de luma sur les pixels du chemin contour) :**

| ×1,33 | biais traits étalon / `text` | p1 contour étalon / `text` | PSNR étalon / `text` / `game` | SSIM étalon / `text` / `game` | FLIP étalon |
|---|---|---|---|---|---|
| MWSR table (2,0 ; cinq échantillons, puis texels) | +0,139 / +0,197 | 0,002 / 0,059 | 23,74 / 21,81 / 38,00 | 0,936 / 0,950 / 0,920 | 0,043 |
| `dering=2` à 2,0 (texels seuls) | +0,091 / +0,090 | 0,000 / 0,000 | 24,08 / 22,18 / 37,56 | 0,938 / 0,958 / 0,920 | 0,044 |
| `dering=2` à 1,5 | +0,102 / +0,121 | 0,000 / 0,000 | 24,30 / 22,20 / 37,83 | 0,942 / 0,954 / 0,920 | 0,038 |
| `dering=2` à 1,5 + CR5 | +0,102 / +0,121 | 0,000 / 0,000 | 24,33 / 22,22 / 37,98 | 0,944 / 0,955 / 0,925 | 0,037 |
| `dering=2` à 1,0 | +0,118 / +0,161 | 0,000 / 0,000 | 24,29 / 21,86 / 37,81 | 0,942 / 0,947 / 0,920 | 0,033 |
| FSR1 | +0,124 / +0,189 | 0,000 / 0,014 | 24,15 / 21,45 / 37,92 | 0,941 / 0,942 / 0,933 | 0,035 |
| SGSR1 (source) | +0,114 / +0,178 | 0,000 / 0,016 | 23,75 / 21,73 / 37,65 | 0,921 / 0,947 / 0,925 | 0,042 |

Dépassements : 0 % pour tous les MWSR (la borne 2×2 tient la sonnerie), FSR1 5,0 % sur l'étalon.
En moyenne sur tous les pixels sombres (Y < 0,25), MWSR n'était pas plus pâle que FSR1 (biais
+0,0148 contre +0,0144 sur l'étalon) : l'écart est tout entier dans les traits et les extrêmes,
que le zoom ×16 de Bruno regarde.

**Lecture.** La borne des cinq échantillons, « faible et gratuite » (§3.2), est surtout fausse sur
les crêtes. Levée (`DERING 2`, aucune instruction de plus : les quatre texels sont déjà chargés), les
traits reviennent à la profondeur de la référence (p1 0,059 → 0,000 sur `text`, biais des traits
divisé par deux) et tout monte sur `text` (+0,37 dB, SSIM 0,950 → 0,958) et sur l'étalon (+0,34 dB à
2,0, +0,56 à 1,5) ; `game` perd 0,44 dB à netteté 2,0 (ses rares pixels sombres au noyau sont
maintenant poussés sous la référence : biais −0,043 sur la bande 0-0,1), 0,17 dB à 1,5, et rien en
SSIM. La netteté se redose : à 1,5 avec CR5, MWSR passe FSR1 sur les trois mesures de l'étalon
(24,33 / 0,944 / 0,037 contre 24,15 / 0,941 / 0,035) et de `text`, à égalité de PSNR sur `game`
(37,98 / 37,92 ; le SSIM 0,925 / 0,933 reste à FSR1). Sur le comparateur même (SwiftShader, image
par défaut, 1080p → 1440p, `dr=2&sharp=1.5&flat=5&fc=0.5`) : MWSR SSIM 0,951 · PSNR 25,52 contre
FSR1 0,948 · 25,17 et SGSR1 0,946 · 24,83 — là où les captures de Bruno donnaient 0,937-0,940 ·
24,4-24,5 à MWSR.

**Jumeaux** : d'accord sur le comparateur (WebGPU, adaptateur logiciel : 0,95124 · 25,518 ; WebGL2
SwiftShader : 0,95130 · 25,524). Test unitaire J35 (les deux textes, la constante cuite dans le
GLSL) ; golden de la table inchangé, conformité C11 refaite des deux côtés : OK (jumeau WebGL2 contre
noyau : FLIP 0,000, 0,007 dB) ; 1726 tests, `npm run check`. Les cellules `chain-*` du labo n'acceptent pas d'options — elles les ignorent en silence et
rendent la cellule nue : à savoir avant de s'y fier. **Produit** : `DERING` reste 1 ; dans le
comparateur, `dr=2` et ⚙ → « borne (arêtes) » ; à l'œil de Bruno, avec la netteté (1,5 ?) et CR5.
S'il valide, le défaut du produit change, et la netteté avec, sur son oui explicite.

**Faux pas de banc.** La première passe « WebGPU » du comparateur a été lancée sans `api=webgpu` ni
`soft=1` dans le lien ; le Chrome du pilote, qui n'épingle WebGL que par la page, a pris la
RTX 5060 Ti en WebGL2 pendant quelques secondes (figures 0,95131 · 25,525, cohérentes avec
SwiftShader ; aucun mode d'écran touché ; C4 de f1 était fini). Le pilote force désormais ces deux
paramètres en mode `gpu`.

## 2026-10-23 — J36, le comparateur sur le parc : six configurations par appareil, pour le choix de Bruno

**Demande de Bruno (12:46)** : « un benchmark sur tous les PC (Mac et realme compris) avec la même
image, en Q3, Q5, dering 2, avec et sans focus, à la taille en pixels de l'iPhone comme mes captures ».
Lu comme : l'étalon `00_Background.png`, le cas 1080p → 1440p de ses captures (×1,33), 300 images par
bande, et trois réglages — Q3 (`flat=3`, netteté 2,0), CR5 (`flat=5`, 2,0) et « borne texels » (`dr=2`,
netteté 1,5, CR5 : le candidat de J35) — chacun sans et avec la zone du regard (`gaze=1`, valeurs de la
table, poids tenu à 1) : six pages par appareil, la saveur « auto » du produit (WebGL2 sur PC, Mac et
Mali ; WebGPU sur l'iPhone). FSR1 et SGSR1 dans les mêmes pages, en repère.

**Outillage** : `scratchpad/fleet/compare_fleet.py` (DevTools de l'appareil par tunnel SSH ou `adb
forward`, le lab servi en http sur `127.0.0.1:8767` par tunnel inverse ou `adb reverse`, la table lue
dans `window.mwCompare`, les lignes d'adaptateur du renderer gardées, une capture par page) ;
`dualrtx.py` (Chrome sans tête épinglé par `--use-adapter-luid`, sonde WebGL2 + WebGPU sur une page du
lab avant toute mesure — `navigator.gpu` n'existe pas sur une URL `data:` — ; les LUID lus dans
`HKLM\SOFTWARE\Microsoft\DirectX`, 780M `0,88263`, RTX `0,77960` toujours bons) ; `summary.py`
(médiane des passes). Sur le realme, `/json/new` répond 500 : les onglets sont faits par le point
d'entrée navigateur (`Target.createTarget`, `activateTarget`, `closeTarget`), Chrome mis au premier
plan par `am start -n`. Conditions de 59 : UM790Pro écarté (test de Qwergin), N95 et mw-arm avec une
tâche planifiée Interactive — que mon classifieur a refusé d'enregistrer à distance : Bruno a lancé
lui-même les deux scripts par ssh depuis son poste (`ssh mw-arm powershell -NoProfile -Command - <
start-arm.ps1`, 14:15), un Chrome sans tête collé à sa console mourant avec elle ; Mac libre (dit par Bruno), mot pour Léo affiché puis retiré, Chrome dédié
`--mute-audio` ; DualRTX annoncé début et fin.

**Mesures (ms par image, moyenne de 300 images ; médiane des passes ; la saveur entre parenthèses) :**

| appareil (saveur) | passes | Q3 | Q3 + zone | CR5 | CR5 + zone | borne texels 1,5 CR5 | borne texels + zone | FSR1 | SGSR1 |
|---|---|---|---|---|---|---|---|---|---|
| iPhone 13 Pro, Safari (WebGPU `mwsr`, captures de Bruno 12:13-12:16) | 1 | 4,41 | 4,82 (100 images) | 4,82 | 5,13 | — | — | 7,94-8,26 | 4,39-5,04 |
| Mac M1 Pro, Chrome 155 (WebGL2 `gl-mwsr`) | 3 | 3,28 | 4,38 | 3,70 | 3,37 | 3,35 | 3,43 | 4,20 | 3,44 |
| DualRTX RTX 5060 Ti, Chrome sans tête (WebGL2 `gl-mwsr`, comparateur corrigé, 24/10 08:18, J38) | 1 | 0,98 | 0,94 | 0,96 | 1,03 | 0,98 | 0,93 | 0,91-1,99 | 0,94-1,78 |
| DualRTX 780M, Chrome sans tête (WebGL2) | 1 | 6,13 | 5,86 | 5,85 | 9,46 | 5,32 | 5,67 | 14,93 | 6,60 |
| UM790Pro, Radeon 780M, Chrome 155 sans tête (WebGL2 `gl-mwsr`, 18:09, J37) | 1 | 5,06 | 4,27 | 4,15 | 4,12 | 4,09 | 4,07 | 5,73-7,15 | 4,07-7,28 |
| N95 (mw-intel), Intel UHD, Chrome 155 sans tête (WebGL2) | 1 | 19,3 | 17,5 | 16,5 | 17,2 | 17,2 | 16,3 | 33,3 | 18,2 |
| mw-arm, Snapdragon 7c, Adreno 618, Chrome 156 sans tête (WebGL2 `gl-mwsr`, comparateur corrigé, 24/10 10:47, J38) | 1 | 21,6 | 18,3 | 20,4 | 20,0 | 21,5 | 18,1 | 31,5-80,3 | 19,3-69,5 |
| realme Note 60, Mali-G57, Chrome 155 (WebGL2) | 1 | 51,6 | 57,3 | 59,2 | 62,1 | 52,6 | 59,3 | 125 | 73 |

Passe par passe : Mac Q3 3,52 / 3,13 / 3,28, Q3 + zone 3,53 / 4,53 / 4,38, CR5 3,24 / 3,70 / 4,74,
CR5 + zone 8,01 / 3,22 / 3,37, borne texels 3,09 / 4,60 / 3,35, avec zone 3,30 / 4,38 / 3,43 (machine
chargée : load 5, les jeux de Léo en fond) ; 780M FSR1 de 11,4 à 17,2 et SGSR1 de 6,0 à 10,7 d'une page
à l'autre. Qualité (la même sur tous les appareils, au millième : les shaders sont déterministes) :

| réglage | SSIM | PSNR |
|---|---|---|
| Q3 | 0,940 | 24,5 |
| Q3 + zone | 0,937 | 24,5 |
| CR5 | 0,941 | 24,6 |
| CR5 + zone | 0,938 | 24,5 |
| borne texels, 1,5, CR5 | 0,951 | 25,5 |
| borne texels + zone | 0,945 | 25,3 |
| FSR1 / SGSR1 | 0,948 / 0,946 | 25,2 / 24,8 |

**Lecture.** (1) Le réglage ne change le temps que sur les téléphones : Q3 contre CR5, +9 % sur l'iPhone
(4,41 → 4,82) et +15 % sur le realme (51,6 → 59,2) ; sur le Mac, le 780M et le N95 l'écart est dans le
bruit des passes (N95 : Q3 19,3 contre CR5 16,5, dans l'autre sens, une passe). La borne texels ne coûte rien de plus que CR5, comme prévu (780M 5,3, realme 52,6, Mac 3,35).
(2) La zone du regard à poids 1 coûte +0,3-0,4 ms sur l'iPhone (+7-9 %), +3 à +7 ms sur le realme (+5 à
+11 %), rien de lisible sur le Mac et le 780M ; sur toute l'image elle retire 0,003-0,006 de SSIM et
0,1-0,2 dB (la netteté 2,5 au centre, J34). (3) Partout, MWSR reste loin sous FSR1 (iPhone 4,4-5,1
contre 8, Mac 3,4 contre 4,2, 780M 5-6 contre 15, N95 16-19 contre 29-36, realme 52-62 contre 125) et
au niveau ou sous SGSR1 (4,4-5,0 ; 3,4 ; 6,6 ; 17-22 ; 73). (4) À qualité égale ou meilleure que FSR1 (0,951 / 25,5 contre 0,948 / 25,2)
pour le temps de CR5, le candidat de J35 est la configuration à proposer à Bruno ; Q3 reste l'économie
des téléphones (-9 à -15 %) pour un dixième de SSIM en moins au millième.

**Manques et ratés.** Le N95 est fait (14:20-14:23, quatre connexions ssh, tâche retirée et vérifiée).
**Le mw-arm est invalide** : avec le même pilote, les trois algorithmes sortent à 310-350 ms par image
en médiane (MWSR 312, FSR1 350, SGSR1 340 ; moyennes 372, 359 et 1 574), dix fois un tracé 1440p sur
son Adreno 618 — un micro-banc dans le même Chrome donne 30 ms par tracé plein écran trivial, la fence
du comparateur correcte (300 sondages de 0,1 ms), `gl.finish()` 0,2 ms (un flush), et un tracé sur
huit figé 18 s. Un surcoût commun aux trois, donc hors shader : le portable à 3,6 Go qui pagine (893 Mo
libres, fichier d'échange en service, Discord et Parsec dans la session de Martin), ou l'import d'une
VideoFrame par tracé sur le pilote Qualcomm ; à reprendre avec le lab (recette du 22/10, lire les p50),
pas avec le comparateur. Le pilote a reçu `--ws-timeout` (une réponse DevTools peut prendre des minutes
sur une machine qui pagine) et la colonne p50. L'UM790Pro attend la fin du test de Qwergin et l'accord
explicite de Bruno. La passe RTX (13:12) est **invalide** : MWSR
mesuré à 125 ms par image, FSR1 jamais fini, le processus GPU de ce Chrome avec un compteur de
plantage récent (`--gpu-recent-crash-count`), et le pilote resté pendu 18 min sur sa socket DevTools
morte au lieu d'abandonner (délai de socket de 60 s ajouté depuis) ; tuée à 13:30:50 à la demande de
59, qui tenait f1 en attente ; un créneau RTX de deux minutes est demandé pour plus tard. Les deux
premiers essais DualRTX avaient échoué avant toute mesure (`navigator.gpu` absent sur `data:`, `wmic`
absent de Windows 11 26200) : les LUID n'étaient pas en cause.

## 2026-10-23 — J37, la borne des texels seuls par défaut (`DERING 2`) ; le comparateur sur NVIDIA, trois régimes

**Décision de Bruno (15:14, par 59)** : `DERING 2`, la borne des arêtes par les quatre texels seuls (J35),
devient le défaut du produit ; CR5, la netteté et la zone du regard restent comme avant (bilinéaire, 2,0,
zone coupée), en attente de ses décisions. Sa préférence à l'œil à ×1,33 (iPhone, 14:51) : plat Q3,
netteté 1,85, texels seuls, sans zone, 4,02 ms, SSIM 0,948, PSNR 25,4 dB contre FSR1 0,947 / 25,1 à
9,23 ms. À ×2 (720p → 1440p), MWSR lui paraît « baveux » à côté de FSR1, sur l'iPhone comme sur le DualRTX
(0,874 / 21,9 dB contre 0,880 / 22,1) : c'est structurel (J31 : raideur des contours 0,53-0,54 contre 0,55,
FLIP 0,057 contre 0,045 sur l'étalon ; la netteté sature sur `W_MIN`, le chemin plat n'affûte pas la
texture, RCAS si). Trois voies lui sont proposées : accepter ; FSR1 au-dessus de ×1,6 par le gouverneur là
où l'appareil le paie (FSR1 à 1440p : iPhone 8-9 ms, Mac 4,2, 780M 14,9, N95 33) ; un affûtage borné dans
MWSR au-delà de ×1,6 (étude). Décision attendue.

**Appliqué (16:33-16:45)** : `DERING: 2` dans `MWSR_OVERRIDE_SPEC` (les deux jumeaux lisent la même
table), le test unitaire (le défaut à 2 ; le GLSL cuit `DERING = 1u` sur demande), le comparateur
(`MWSR_DEF.dr` suit le produit : « texels seuls » par défaut, `dr=1` rend l'ancienne borne, étiquetée
« borne cinq puis texels »), le design §3.2 (9). `golden.json` refait sur SwiftShader (`chain-mwsr` =
`mwsr` au bit près sur les neuf cas) et revérifié par `--golden-check` ; conformité : côté WebGPU
`chain-mwsr` = `mwsr` exact partout ; côté WebGL (SwiftShader, « renderer on ANGLE (Google, Vulkan 1.3.0
(SwiftShader Device …)) » dans le journal de la passe) jumeau contre noyau text 0,057 dB, game 0,010,
code 0,020, FLIP 0,001 / 0,000 / 0,000 : OK ; 1726 tests, `npm run check` vert ; commit du jour sur
l'index privé. Pilote du parc : `dr=1` épinglé sur q3 / q3f / q5 / q5f, pour garder la borne mesurée en J36.

**Faux pas et fausse alerte.** La passe « côté WebGPU » (`--swiftshader --conformity`) crée un contexte
WebGL2 sur l'adaptateur par défaut, ici la RTX de l'écran, avant que le garde-fou `soft=1` de la page ne
le rejette : quelques millisecondes, aucune cellule rendue ; signalé à 59. Puis la table de conformité a
comparé au noyau d'aujourd'hui des clichés `conf-q-*-chain-gl-mwsr.png` **du 21/10** (noms sans étiquette
ni date ; leurs FLIP 0,071 / 0,019 / 0,029 sont ceux de J20) et conclu « pas la même qualité » : une heure
de fausse alerte sur « la RTX rend autre chose ». Une relance avec `--use-angle=swiftshader` côté WebGPU
coupe WebGPU (`requestAdapter returned null`), comme le commentaire du pilote le dit ; seule la passe
`--webgl-swiftshader` produit les clichés du jumeau. À faire un autre jour : l'âge de chaque cliché dans
la table, et « périmé » pour ce qui précède la passe.

**Le comparateur sur NVIDIA, en cours.** Trois régimes vus sur le DualRTX, aucun n'étant le temps GPU :
(1) le Chrome de Bruno, fenêtre du comparateur recouverte par une autre (14:49) : 110 ms par image pour
les trois shaders, Natif 0,46 ms, mesuré en premier, fenêtre encore visible ; (2) le même Chrome, fenêtre
seule au premier plan sur DISPLAY5 à 120 Hz (59, par l'extension, 16:29) : Natif 8,03 ms, MWSR 11,04,
FSR1 8,47, SGSR1 12,38, la cadence de l'écran : chaque itération attend une image du compositeur ; (3) un
Chrome sans tête épinglé sur la RTX 0 (bus 01:00 ; épinglage vérifié par la croissance des compteurs WDDM
du processus GPU pendant un dessin, le cumul comptant l'énumération des adaptateurs au démarrage) :
« chrono 225/300 » après 45 s, ~180 ms par itération, et au micro-banc (8 tours) : dessin trivial 1440p +
barrière 1,61 ms, import VideoFrame de canvas / VideoFrame CPU / ImageBitmap / canvas + dessin + barrière
4,77 / 2,67 / 0,94 / 1,12 ms, mais `WebGlRenderer` gl-mwsr sur `OffscreenCanvas(16,16)` redimensionné au
premier draw : 128 ms (110-132), 0,17 ms sans barrière ; avec une VideoFrame CPU 4 tours à 125 ms puis
0,9-3,9 ; avec une ImageBitmap 0,2-4. Le produit n'est pas en cause : la porte P4 (J20) a mesuré gl-mwsr
à 0,81-0,87 ms sur la RTX avec le labo, qui donne au renderer un OffscreenCanvas à la taille de sortie,
et le stream dessine dans le canvas visible. Second micro-banc prêt : trois sortes de canvas sur 30 tours,
requête de temps GPU, FSR1 et SGSR1, empreinte SHA-256 de la sortie sur une entrée déterministe, relevée
d'abord sur SwiftShader ; sur la RTX au go de Bruno (scripts de séance, hors dépôt).

**Second micro-banc (16:53, RTX 0 épinglée et vérifiée, 35 s, référence SwiftShader relevée avant).** La RTX
rend juste : PSNR contre SwiftShader sur la même entrée déterministe, gl-mwsr 63,1 dB (écart max 58/255 sur
0,10 % des pixels), gl-fsr1 61,9, gl-sgsr 62,9, toutes au-dessus du seuil de 45 dB fixé d'avance ; même
empreinte pour les trois sortes de canvas. Et les 125 ms ne sont pas du GPU : la requête de temps GPU donne
0,076 ms par dessin gl-mwsr 1440p. Tours (p50) : renderer sur OffscreenCanvas(16,16) 12 tours à 122-163 ms
puis 18 à 3-15 ; sur OffscreenCanvas(2560,1440) 30 tours sur 30 à 117-142 ; sur un canvas d'élément 3-8 dès
le troisième ; un renderer neuf sur OffscreenCanvas(16,16) avec begin/endQuery autour du dessin 0,9 ms ;
gl-fsr1 et gl-sgsr sur OffscreenCanvas 117-143. Lecture : la barrière attend une soumission D3D que le dessin
et le flush du comparateur ne provoquent pas sur ce pilote NVIDIA ; un canvas d'élément (commit du
compositeur) ou une requête de temps (endQuery) la forcent. C'est la clé des trois régimes : une soumission
par image d'écran dans une fenêtre visible, une soumission périodique sinon. Le produit dessine dans le
canvas visible du stream : non concerné. Troisième micro-banc prêt (barrière seule, finish, readPixels 1×1,
endQuery, flushs explicites, deux témoins nus sur OffscreenCanvas) : la gagnante deviendra le « done » du
comparateur sur WebGL2 ; UM790Pro : recette du N95 avec le LUID du 780M lu dans le registre, au go de 59.

**UM790Pro (18:09, au go de 59 après le « fait » de Bruno ; 50 s pour les six pages, 3 connexions ssh).** Chrome 155
sans tête dans la session Minis par une tâche planifiée Interactive lancée par Bruno lui-même (`ssh mw-um790win
powershell -NoProfile -Command - < start-um790.ps1`), épinglé sur le Radeon 780M par son LUID lu dans
`HKLM\SOFTWARE\Microsoft\DirectX` (l'entrée Radeon vue en dernier : la clé garde les vieux adaptateurs), vérifié
avant toute mesure (renderer WebGL2 `ANGLE (AMD, AMD Radeon 780M Graphics, D3D11)`, le pilote s'arrête sinon) ;
Chrome arrêté et tâche retirée à la fin (« chrome left: 0 », « task left: 0 »), prod, DEV, Sunshine, Parsec et les
deux mstsc intacts, aucun écran touché. Ligne ajoutée au tableau de J36 : Q3 5,06 ms, Q3 + zone 4,27, CR5 4,15,
CR5 + zone 4,12, borne texels 1,5 CR5 4,09, avec zone 4,07 ; FSR1 5,73-7,15, SGSR1 4,07-7,28 selon la page (la
première mesure d'une page neuve, pas le GPU : même écart que sur le 780M du DualRTX). Qualité au millième près
celle du tableau (0,951 / 25,5 dB pour la borne texels, 0,940 / 24,5 pour Q3) : les shaders sont déterministes.
Lecture : sur ce 780M sous Windows, MWSR coûte 4,1 ms quel que soit le réglage, FSR1 6 et SGSR1 4 à 7 ; la borne
texels, défaut depuis 15:14, est la meilleure des six en qualité au même temps. Piège levé au passage, après la
question de Bruno (« Je dois lancer quoi comme commande ? ») : un script lu sur l'entrée standard par
`powershell -Command -` n'exécute un bloc multi-lignes qu'après une ligne vide et le jette en fin de fichier sans
erreur ; la première version du script avait une boucle multi-lignes et se serait arrêtée avant la tâche. Réécrit
en une instruction par ligne et répété à blanc en local avant la ligne (les scripts N95 et mw-arm n'avaient pas
de bloc). Restent le troisième micro-banc RTX et la correction du « done » du comparateur, au go de 59.

**Troisième micro-banc (19:14, RTX 0 épinglée et vérifiée, 29 s ; go de Bruno par 59).** Dix façons de finir un
dessin gl-mwsr 1080→1440 du `WebGlRenderer` sur OffscreenCanvas, dix tours chacune après deux de chauffe (p50) :
barrière seule 122 ms ; `gl.finish()` puis barrière 125 ; `readPixels` 1×1 puis barrière 122 ; requête de temps
(`beginQuery`/`endQuery` TIME_ELAPSED) autour du dessin puis barrière **4,7 ms** (temps GPU 1,7-2,7 ms, horloges au
repos) ; canvas d'élément, barrière seule 8,0 ms sur cinq tours puis 0,8-1,3 ; offscreen de nouveau 129 ;
flush-barrière-flush 114 ; flush, tâche, flush, barrière 130 (pics 300-870) ; contexte nu sur
OffscreenCanvas(2560×1440), attributs du renderer, dessin trivial + barrière 0,90 ms ; attributs par défaut 0,84.
Lecture : ni `finish`, ni `readPixels`, ni les flushs ne font soumettre sur ce pilote ; seule la requête de temps le
fait (et le commit du compositeur sur un élément, quantifié à la période d'écran) ; et un contexte nu n'a pas le
problème : c'est un pas du chemin du renderer avec une VideoFrame (banc 1 : contexte nu + même VideoFrame 2,7 ms ;
renderer + ImageBitmap 0,2-4 ms ; renderer + VideoFrame CPU lent quatre tours puis rapide) qui laisse le travail
non soumis ; candidat à vérifier un autre jour, la barrière que le renderer pose lui-même à chaque dessin
(`_setFence`, lue au dessin suivant). Produit non concerné (canvas visible, porte P4 0,8 ms).

**Correction du comparateur (19:20, commit local du soir).** `compare.js` : sur WebGL2, quand
`EXT_disjoint_timer_query_webgl2` existe, `timeAlgo` enveloppe chaque dessin (chauffe comprise) dans une requête
TIME_ELAPSED créée, ouverte, fermée après le dessin et supprimée, puis la barrière comme avant ; sans l'extension
(Safari, Firefox) rien ne change, et là la soumission est immédiate (Mac, Intel, Mali, 780M mesurés sains). Aucun
changement d'UI, `frontend/` intact (gel de 59 jusqu'à 02:30). Test sur SwiftShader (Chrome sans tête, aucun moteur
3D n'a bougé, `softcompare.py` de séance) : q5 MWSR 44,4 ms, FSR1 90,4, SGSR1 69,8 ; d2 39,9 / 107,2 / 48,8 ;
qualité 0,941 / 24,6 et 0,951 / 25,5, chemin `gl-mwsr:frame` : les chiffres d'avant la correction (référence du
banc 2 : 40 / 89 / 48). Restent, au go : la RTX à remesurer au comparateur (sa ligne du parc) et mw-arm à refaire ;
la boucle de mesure du labo (`scaler_lab`) n'a pas ce « done » et ses chiffres RTX de P4 étaient sains (0,8 ms) ;
à faire un autre jour : la colonne du temps GPU par la même requête.

## 2026-10-24 — J38, la RTX au comparateur corrigé : les trois shaders au plancher d'une passe

**Contexte.** Go de Bruno à 07:5x par 59, « à toi » à 08:18 après le banc c2 de dualgpu (les deux cartes rendues
à 180 W, topologie d'origine). Recette épinglée de J36 (`dualrtx.py --gpu rtx0`) : Chrome sans tête, profil
propre, muet, `--use-adapter-luid=0,77960` (LUID 0x13088 inchangé : pas de redémarrage depuis le 19/10), sondes
WebGL2 et WebGPU « NVIDIA », croissance des compteurs WDDM pendant un dessin 21 043 sur 0x13088 et 0 sur le 780M,
NVENC 0 session, plafond 130 s ; 32 s au total, « chrome left: 0 », ni --dev ni écran touchés.

**Mesures (ms par image, 300 images, chemin `gl-mwsr:frame`)** : Q3 0,98 (p50 0,85), Q3 + zone 0,94, CR5 0,96,
CR5 + zone 1,03, borne texels 1,5 CR5 0,98, avec zone 0,93 ; FSR1 0,91-1,99 et SGSR1 0,94-1,78 selon la page ;
qualité au millième celle de la table (0,951 / 25,5 pour la borne texels). Ligne ajoutée au tableau de J36 ; la
passe du 23/10 à 15:20 (180 ms par itération, le régime 3 de J37) est remplacée.

**Lecture.** Avec la requête de temps autour du dessin, la barrière du comparateur revient en 1 ms sur NVIDIA, le
chiffre du labo à P4 (0,8 ms) : les trois shaders sont au plancher d'une passe plein écran sur cette carte, et
les écarts entre réglages (0,93-1,03) sont le bruit d'une page à l'autre, comme ceux de FSR1 et SGSR1 (première
mesure d'une page neuve). Le comparateur donne maintenant sur NVIDIA ce qu'il donnait déjà sur AMD, Intel et
Apple ; reste mw-arm (Qualcomm, Adreno 618), à la ligne de Bruno après le test dualgpu.

**mw-arm (10:26-10:47, au mot de 59 après le test dualgpu ; Chrome 156 sans tête lancé par la ligne de Bruno,
tâche Interactive ; serveur du lab relancé sur 8767 après un arrêt pour mémoire basse ; 4 connexions
ssh).** D'abord la sonde CR5 avec le comparateur corrigé de la veille : toujours 298 ms par image (p50 210), FSR1
341, SGSR1 1 098, la machine saine cette fois (949 Mo libres, échange 464 Mo, charge 1) : la correction de J37 ne
mord pas sur ce pilote. Quatre micro-bancs dans le même Chrome (`cdpeval.py` à travers le tunnel : `rtxbench.js`,
`armbench2.js` à `armbench4.js`, 6 à 16 s chacun) :
(1) contexte nu : dessin trivial + barrière 2,8-8 ms, envoi d'une VideoFrame 1080p + dessin 11-14 ms (35 pour une
VideoFrame CPU), bitmap 9 ; renderer : dessin + barrière 167-215 ms quel que soit l'intrant (VideoFrame, bitmap),
l'appel lui-même 0,7 ms ; (2) le redimensionnement du canvas hors de cause : nu, né à 2560×1440 ou redimensionné
comme le renderer (16 → 1920×1080 → 2560×1440), 3-5 ms ; renderer sans redimensionnement (720p → 1080p) 196 ms ;
(3) les suspects un à un dans le renderer : **sa barrière par dessin retirée (`_setFence` / `_checkFence`) → 26 ms** ;
requête de temps gardée jusqu'après la barrière → 21 ms (temps GPU 12-15 ms) ; `gl.finish()` 201 ; gl-fsr1 224,
gl-sgsr 232, copie simple `off` 175 : ce n'est pas le noyau ; (4) la durée de vie de la requête : supprimée aussitôt
fermée (la correction de J37) → 195 ms, barrière du renderer retirée ou non (189) ; gardée jusqu'après la barrière →
23 ms ; lue → 19 ms ; sans requête ni barrière du renderer → 16 ms.

Lecture : sur le pilote D3D11 de Qualcomm comme sur celui de NVIDIA, un objet de synchronisation laissé en attente (la
barrière que le renderer pose après chaque dessin et lit au dessin suivant ; ou une requête fermée puis supprimée
avant d'être achevée) retarde de 120 à 190 ms l'achèvement vu par la barrière suivante ; une requête de temps vivante
jusqu'à la barrière lève l'attente sur les deux pilotes. La boucle du labo n'a pas le problème parce qu'elle mesure
par lots. **Correction** : `compare.js` garde la requête jusqu'après `glDone` et la supprime ensuite (un dessin qui
lève laisse la sienne, supprimée au suivant) ; prettier et `node --check` verts ; SwiftShader inchangé (q5 46,7 /
93,6 / 70,2 ; d2 40,2 / 112,2 / 48,2 ; qualité 0,941 / 24,6 et 0,951 / 25,5). Sonde CR5 ensuite : 18,5 ms (p50 16,8),
FSR1 34,7, SGSR1 23,7, les chiffres du labo de J25 (`done` p50 14-17 et 29-35). **Les six pages (300 images)** :
Q3 21,6 (p50 15,8), Q3 + zone 18,3, CR5 20,4, CR5 + zone 20,0, borne texels 1,5 CR5 21,5, avec zone 18,1 ; FSR1
31,5-80,3 et SGSR1 19,3-69,5 selon la page (machine bruyante, première mesure d'une page neuve) ; qualité au millième
celle de la table. Ligne ajoutée au tableau de J36 : sur l'Adreno 618 sous Windows, MWSR coûte 18-22 ms par image
à 1080p → 1440p quel que soit le réglage (p50 15-18), sous SGSR1 et loin sous FSR1, comme au labo (0,63-0,77 ×
SGSR1 en J25). Ménage vérifié (« chrome left: 0 », « task left: 0 »), tunnel fermé, 0 ssh.

Question pour le produit (P5, à Bruno ; rien changé) : la barrière par dessin du renderer sert au gouverneur
(`takeGpuBehind`, part des dessins dont le GPU n'a pas fini au dessin suivant). Sur un canvas d'élément le
compositeur soumet à chaque image et la barrière du RTX signale en 1 ms (banc 3 de J37) ; sur Qualcomm en flux réel,
vérifier au `/bench` avec `mwsr` qu'elle ne compte pas de faux « GPU en retard » (ce serait une descente d'échelle
injustifiée sur mw-arm).

**P6, le chemin plat par classe d'appareil (11:20-11:50, go de Bruno par 59 ; le gel de e7 sur l'arbre, 11:00-11:35,
respecté).** Code : `MWSR_OVERRIDE_SPEC.FLAT_TAPS` passe de 1 à 5 (CR5 ; `FLAT_C` 0,5 inchangé) ; `MwsrShader.js`
exporte `mwsrClassOverrides({ handheld })`, `{ FLAT_TAPS: 3 }` pour un téléphone ou une tablette et vide sinon ; les
deux renderers la posent à la construction des pipelines sous les surcharges du labo, avec `IS_HANDHELD` de
`BrowserDetect` (téléphone ou tablette, jamais une TV), si bien qu'un jeu nul rend les défauts de la classe ;
`compare.js` nomme toujours `FLAT_TAPS` et `FLAT_C` dans ses surcharges (un jeu nu ne veut plus dire bilinéaire), lit
la classe de l'appareil pour « Défauts du produit » et appelle Q3 le quadratique ; test vitest. Vérifications :
`npm run check` vert, vitest 162 fichiers et 1 740 tests ; dorés régénérés sur SwiftShader (les quatre cellules MWSR
des six cas d'agrandissement changent, les trois réductions et `tex-s1` intactes, `chain-mwsr` = `mwsr` sur chaque
cas), `--golden-check` OK ; conformité OK des deux côtés (jumeau contre noyau : FLIP 0,000-0,001, 0,01-0,06 dB,
dépassement ≤ 0,035 %). Comparateur, page des défauts (CR5, netteté 2, borne texels ; `compare_fleet.py` page `def`) :
SwiftShader 0,950 / 25,4 dB ; RTX 0 épinglée (croissance 133 708 sur 0x13088, 0 ailleurs) 1,94 ms par image (p50
1,41), FSR1 3,54, SGSR1 0,80 ; 780M épinglé (204 579 sur 0x158c7, 0 ailleurs) 5,73 ms (p50 4,92), FSR1 12,32, SGSR1
9,91 ; la même qualité au millième sur les trois (la table de J36 donnait 0,941 / 24,6 pour CR5 à la borne 1, et
0,951 / 25,5 pour la borne texels à netteté 1,5). Incident sans suite : une perte de contexte WebGL, perdue puis
restaurée par Chrome, sur SwiftShader à la première page des défauts (11:37, trois bandes noires), non reproduite sur
six pages suivantes ; le renderer rebâtit ses programmes à la restauration (`_buildResources`). Reste en option : les
deux TV Android (Mi TV, Freebox POP) pour Android, si créneau. Produit : MWSR reste derrière `mw_mwsr` ; ce défaut
vaut dès maintenant pour la --dev de 59, qui sert l'arbre (B0 à 15:20). « Chrome left: 0 » après chaque passe.

**Netteté 1,85 et la borne (11:50-12:05, message de Bruno).** Borne : les texels seuls, confirmés (le défaut depuis
J37). Netteté : `MWSR_UNIFORMS.sharp` passe de 2,0 à 1,85 pour toutes les classes (son œil à ×1,33 sur l'iPhone, J37) ;
test ajusté (`toBeCloseTo` : le bloc d'uniformes est en f32), dorés régénérés (les mêmes 24 cellules), `--golden-check`
OK, conformité OK des deux côtés ; une passe côté WebGPU a donné `code` à 0,145 dB entre jumeau et noyau (« pas la
même qualité »), la passe suivante 0,018 et le côté WebGL2 0,018 : du bruit du chemin WebGL2 du Chrome à drapeau
WebGPU sur SwiftShader, à surveiller en CI. Page des défauts du comparateur sur SwiftShader : 0,951 / 25,5 dB à ×1,33
(0,950 / 25,4 à 2,0). `npm run check` et vitest verts ; commit du midi.

**Le ×2 « baveux » (720p → 1440p), question de Bruno : la cause est connue (J31, J37), rien n'est corrigé.** Mesure du
jour au comparateur sur SwiftShader, 720p → 1440p, la netteté seule : 1,85 → SSIM 0,877 / 21,9 dB ; 2,5 → 0,875 /
21,9 ; 3 → 0,875 / 21,9 ; 4 → 0,875 / 21,8 (FSR1 0,880 / 22,1 en J37). La netteté ne mord plus au-delà de 1,85 : le
poids des deux échantillons en travers est plafonné par `W_MIN` (−0,5 ; `W_ACROSS` 0,3 × 1,85 = −0,555, au plafond
dès la pleine force que `S_FULL` donne à partir de ×1,5), et le chemin plat, CR5 ou Q3, reconstruit sans affûter là où
RCAS affûte toute l'image après EASU. **Proposition pour la netteté dynamique voulue par Bruno** (plan §12) : une loi
par l'échelle qui agit sur ce qui mord, pas sur l'uniforme seul : au-delà de ×1,5, faire suivre à l'échelle le plafond
`W_MIN` et la distance `R_ACROSS` des échantillons en travers (1,0 texel = 2 pixels de sortie à ×2), et ajouter un
affûtage borné du chemin plat (masque flou sur les quatre texels, borné par eux comme `DERING 2` : aucun halo), nul à
×1,5 et plein à ×2 ; en réduction, le même affûtage borné après la grille. Le point d'entrée existe : `mwsrUniforms()`
reçoit les deux tailles, et `strength` par `S_FULL` est déjà une loi d'échelle dans le noyau. Étude au labo
(`--quality`, FLIP, 720p → 1440p et 1080p → 1440p, SwiftShader puis RTX et 780M), puis l'œil de Bruno sur l'iPhone et
le PC ; les deux autres voies de J37 restent (FSR1 au-delà de ×1,6 par le gouverneur, ou accepter).

## 2026-10-24 — J39, la netteté selon l'échelle : la loi ×2 en paramètres, le levier est l'uniforme

**Le go de Bruno (12:25) : « étude de la netteté selon l'échelle ».** L'hypothèse de J38 : à ×2 la netteté sature
sur `W_MIN` et le chemin plat n'affûte pas ; faire suivre l'échelle à `W_MIN` et `R_ACROSS`, affûter le plat.

**Fait (12:30-12:45) : la loi ×2 en paramètres du noyau, dans les deux jumeaux.** Un poids `x2 = clamp((s −
S_FULL) / (S_X2 − S_FULL))`, 0 jusqu'à ×1,5 et 1 dès `S_X2` (2,0), et cinq constantes qu'il fait entrer :
`X2_BASE` (le chemin de contour part de l'image du chemin plat, CR5 ou Q3, au lieu de l'échantillon bilinéaire, ce
qu'une texture perd quand sa cohérence l'envoie au contour), `X2_COH` (un plancher sous la cohérence de la paire en
travers : la texture affûtée d'au moins autant), `X2_FLAT` (masque flou sur le chemin plat : la crête en travers du
gradient à `R_ACROSS`, retirée puis bornée par les quatre texels), `X2_R_ACROSS` et `X2_W_MIN` (les valeurs au plein
poids, par une différence de constantes, pas un `mix` : exact aux défauts). Toutes neutres par défaut, prouvé :
`--golden-check` au pixel, conformité WebGL2 OK (FLIP 0,000-0,001, ≤ 0,055 dB). La borne par les texels devient une
fonction (`texelBound`) des deux côtés. Clés du labo `sx2`, `x2base`, `x2flat`, `x2racross` (et `x2coh`, `x2wmin`,
retirées plus bas) ; 16 tests, `npm run check`.

**Balayage 1 (SwiftShader, `--quality`, ×2 = 1280×720 → 1440p ; parts du chemin de contour : `text` 35 %, `game`
2 %, étalon 29 % ; PSNR / SSIM / FLIP, l'étalon entier / centre / FLIP ; dépassements 0 % partout) :**

| ×2 | `text` | `game` | étalon |
|---|---|---|---|
| FSR1 | 17,57 / 0,845 / 0,068 | 35,55 / 0,887 / 0,017 | 20,79 / 32,26 / 0,045 (dép. 2 %) |
| `mwsr` (CR5, texels seuls, netteté 1,85 : le produit) | 17,78 / 0,864 / 0,079 | 34,92 / 0,883 / 0,020 | 20,27 / 28,79 / 0,067 |
| `X2_BASE` 1 | 17,92 / 0,870 / 0,079 | 34,82 / 0,883 / 0,020 | 20,24 / 28,64 / 0,068 |
| `X2_COH` 0,3 ; 0,5 | = ; = | = ; = | = ; = |
| `X2_COH` 0,7 | 17,73 / 0,864 / 0,080 | 34,91 / 0,883 / 0,020 | 20,25 / 28,72 / 0,067 |
| `X2_FLAT` 0,25 | 17,79 / 0,865 / 0,079 | 34,95 / **0,888** / 0,020 | 20,25 / 28,79 / 0,070 |
| `X2_FLAT` 0,5 ; 1 | 17,79 / 0,865 / 0,079 | 34,92 / 0,888 / 0,020 ; 34,85 / 0,887 / 0,022 | 20,23 / 28,70 / 0,073 ; 20,21 / 28,58 / 0,075 |
| `X2_R_ACROSS` 0,75 | 17,05 / 0,838 / 0,092 | 34,53 / 0,883 / 0,020 | 20,36 / **29,50** / 0,064 |
| `X2_R_ACROSS` 1,25 | 17,84 / **0,875** / 0,082 | **35,25** / 0,883 / 0,020 | 20,18 / 28,08 / 0,069 |
| `X2_W_MIN` −0,6 ; −0,75 | 17,78 / 0,865 / 0,080 | 34,81 / 0,883 / 0,020 | 20,25 / 28,62 / 0,067 |
| `X2_BASE` + `X2_FLAT` 0,5 | 17,93 / 0,870 / 0,079 | 34,81 / 0,888 / 0,020 | 20,20 / 28,55 / 0,074 |

**Lecture.** L'hypothèse de J38 tombe : le plafond `W_MIN` n'est pas le levier (−0,6 et −0,75 rendent les mêmes
pixels ou pires), un plancher de cohérence non plus (0,3 et 0,5 : pas un pixel ne change ; 0,7 : pire). La raison
est dans la forme du noyau : l'affûtage en travers est un laplacien à l'échelle du texel (`X1 + X2 − 2C`), nul sur une
rampe linéaire — il ne raidit pas un contour anticrénelé, il amplifie la courbure à ses deux bouts, puis la borne
épingle le pixel au texel. Les constantes qui mordent sont celles du contenu : `X2_R_ACROSS` 1,25 sert le texte et
`game` (+0,33 dB), 0,75 l'étalon (+0,7 dB au centre), aucune valeur commune ; `X2_BASE` gagne +0,14 dB sur le texte
et perd 0,03-0,10 ailleurs ; `X2_FLAT` 0,25 porte le SSIM de `game` au-dessus de FSR1 (0,888 contre 0,887) pour un
FLIP partagé. Rien n'explique le « baveux ». À l'œil sur les dumps (étalon à ×2, trois régions ×3) : MWSR n'est pas
plus flou que FSR1, il est plus dur — la brique du « 03 » et les mèches du prince se font par **paquets épinglés aux
texels** (des taches, des escaliers), là où EASU dessine des bords propres et RCAS les relève. « Baveux » = le
rendu empâté de cet épinglage, pas de la douceur.

**Balayage 2 (mêmes conditions, la netteté par l'uniforme et la borne, à défauts égaux par ailleurs) :**

| ×2 | `text` | `game` | étalon |
|---|---|---|---|
| `mwsr` netteté 1,85 (le produit) | 17,78 / 0,864 / 0,079 | 34,92 / 0,883 / 0,0198 | 20,27 / 28,79 / 0,0665 |
| netteté 1,5 | 17,84 / 0,863 / 0,072 | 35,28 / 0,884 / 0,0194 | 20,44 / 30,03 / 0,061 |
| **netteté 1,25** | 17,82 / 0,860 / **0,068** | 35,49 / 0,884 / 0,0188 | 20,57 / **31,02** / **0,056** |
| netteté 1 | 17,73 / 0,854 / 0,066 | 35,57 / 0,884 / 0,0182 | 20,68 / 31,84 / 0,051 |
| `DERING` 1 | 17,38 / 0,844 / 0,079 | 35,39 / 0,884 / 0,0194 | 20,55 / 30,18 / 0,058 |
| `DERING` 1 + netteté 1,25 | 17,45 / 0,844 / 0,070 | 35,89 / 0,885 / 0,0184 | 20,71 / 31,65 / 0,052 |
| 1,25 + `X2_FLAT` 0,25 | 17,83 / 0,860 / 0,068 | 35,52 / 0,888 / 0,0185 | 20,54 / 31,01 / 0,059 |
| 1,25 + `X2_R_ACROSS` 0,75 | 17,33 / 0,842 / 0,074 | 35,18 / 0,883 / 0,0187 | 20,60 / 31,46 / 0,053 |
| 1,25 + `X2_BASE` | **17,96** / 0,866 / 0,067 | 35,41 / 0,884 / 0,0192 | 20,51 / 30,66 / 0,059 |
| nu, `DERING` 1, netteté 1 (la table de J8) | 17,41 / 0,842 / 0,071 | 35,84 / 0,882 / 0,0186 | 20,74 / 31,95 / 0,051 |
| FSR1 | 17,57 / 0,845 / 0,068 | 35,55 / 0,887 / 0,0171 | 20,79 / 32,26 / 0,045 |

**Lecture : le levier est la netteté elle-même.** La force du noyau monte de 0 à 1:1 à 1 dès `S_FULL` (×1,5) : à
×1,33, où l'œil de Bruno a réglé 1,85, le poids en travers vaut 1,85 × 2/3 = **1,23** ; à ×2 il vaut **1,85**, la
moitié de plus que ce qu'il a validé, et c'est ce surcroît qui épingle. Revenir à ce poids à ×2 (netteté 1,25) :
l'étalon au centre 28,8 → 31,0 dB (FSR1 32,3), FLIP 0,067 → 0,056 (0,045), SSIM 0,862 → 0,873 (0,874) ; `game` 34,9 →
35,5 (35,55), FLIP 0,0198 → 0,0188 ; `text` garde son avance de PSNR et de SSIM sur FSR1 (17,82 / 0,860 contre 17,57 /
0,845) et son FLIP rejoint celui de FSR1 (0,068 = 0,068). La netteté 1 va plus loin sur l'étalon et `game` mais le
texte cède (17,73 / 0,854). La borne : `DERING` 1 à ×2 gagne sur `game` et l'étalon mais rend 0,4 dB sur le texte (les
traits fins de J35) ; les texels seuls restent. Les constantes ×2 avec 1,25 : `X2_BASE` +0,14 dB sur le texte pour
−0,1 à −0,4 au centre de l'étalon, `X2_FLAT` 0,25 le SSIM de `game` à 0,888 et de l'étalon à 0,875 (FSR1 0,887 /
0,874) pour un FLIP d'étalon moins bon : des leviers de contenu, gardés en paramètres de labo (et en réglages du
comparateur : `x2b`, `x2r`, `x2f`), sans proposition de défaut ; `X2_COH` et `X2_W_MIN`, morts, retirés du noyau
(golden-check et conformité des deux côtés refaits : OK). À ×1,33 (1080p → 1440p), vérifié : les trois constantes
réunies rendent les pixels de `mwsr` au millième (22,23 / 0,957 ; étalon 24,21 / 0,941), `x2` y vaut 0. Au passage,
à ×1,33 la netteté 1,25 est aussi la préférée des mesures (étalon 24,35 / 0,945, centre 36,33 contre 33,97 à 1,85 et
35,94 pour FSR1 ; texte 22,10 / 0,952, FLIP 0,045 contre 0,049) : l'œil de Bruno a choisi 1,85 là, c'est son choix.

**Proposition à Bruno : la loi d'échelle sur l'uniforme**, `mwsrSharpness(s) = 1,85 × min(1, force(4/3) / force(s))` :
1,85 jusqu'à ×1,33, puis le poids en travers tenu à 1,23 (1,85 à ×1,33, 1,54 à ×1,4, 1,23 dès ×1,5 et à toute
échelle au-delà) — le même mélange qu'à ×1,33 partout où l'on agrandit plus. Écrite, exportée, testée (`MWSR_SHARP_REF_SCALE`
4/3 ; 17 tests), **pas câblée** : `mwsrUniforms()` garde la constante jusqu'à son oui. Le comparateur reçoit un bouton
« Netteté de la loi d'échelle (J39) à ce facteur » (le curseur prend 1,23 à 720p → 1440p, 1,85 à 1080p → 1440p) et les
trois constantes ×2 en réglages. Pour son œil : à 720p → 1440p, la bande MWSR à 1,85 (défaut) contre 1,25 (bouton)
contre FSR1 ; à 1080p → 1440p rien ne change. Les deux autres voies de J37 restent (FSR1 au-delà de ×1,6 par le
gouverneur ; accepter). En réduction (< 1) l'uniforme n'est pas lu (la grille n'affûte pas) : une loi y passerait par
un affûtage après la grille, à étudier seulement s'il le demande. Coût de la loi : nul (une valeur par dessin). Rien
n'a tourné sur GPU : la qualité se mesure au pixel sur SwiftShader (conformité), le créneau RTX + 780M de 14:15 rendu à 59.

## 2026-10-24 — J40, la loi d'échelle de la netteté câblée (oui de Bruno)

**Le oui (13:20) et le go (21:00, relayé par 59).** `mwsrUniforms()` prend `mwsrSharpness(min(outW/inW, outH/inH))`
quand l'appelant ne nomme pas de netteté : 1,85 jusqu'à ×1,33, 1,542 à ×1,4, 1,233 dès ×1,5 (le poids en travers de
×1,33 tenu au-delà, J39). Calculée à la reconstruction du bloc d'uniformes, pas par pixel ; en réduction l'uniforme
reste 1,85 (la grille ne le lit pas). Aucune UI touchée : le labo et l'appel nommé (`{ sharp }`) gardent la main.
Le comparateur part de la loi : `state.sharp` vaut `null` (= la loi au cas affiché), le réglage affiche « (loi) »,
le bouton devient « Netteté par la loi d'échelle (défaut du produit) », la remise à zéro rend la loi, un `sharp=`
du lien prime.

**Vérifié.** Tests de `MwsrShader` (17) : le bloc à ×2 porte 1,85 × 2/3, à ×1,33 et en réduction 1,85, `{ sharp: 1,85 }`
rend 1,85 ; vitest 1742 / 1742, `npm run check` vert. `golden.json` refait sur SwiftShader : seules les cellules
MWSR des trois cas ×2 changent (`mwsr`, `chain-mwsr`, `mwsr_dering_0`, `mwsr_c444_1` : 12 cellules ; PSNR contre
bilinéaire `text` 19,97 → 22,43, `game` 37,98 → 40,21, `code` 29,13 → 31,38 dB), ×1,33 et réduction au bit près ;
`--golden-check` OK. Conformité côté WebGL2 (`--webgl-swiftshader`) OK, côté WebGPU OK (jumeau contre noyau : FLIP
0,000, 0,023-0,047 dB) ; la première passe côté WebGPU, lancée avant la passe WebGL2, avait lu les clichés du jumeau
d'avant la loi (« pas la même qualité » sur `code`, 0,98 dB) : le faux pas de J37, l'ordre est WebGL2 d'abord.
Rien n'a tourné sur GPU matériel hors le contexte WebGL2 éphémère de la passe WebGPU (J37). Chrome de labo : 0 resté.

## 2026-10-24 — J41, MWSR par défaut (P6) ; la Mi TV au comparateur : Q3 devant CR5 sous Android aussi

**Le go (21:05, mot pour mot : « MWSR défaut en v0.4.1 »).** Bruno avance la bascule de P6 avant la campagne P5.
`pickAutoEnhancer({ hdr })` rend désormais la saveur, selon ses classes du 22/10 (plan §9) : `gl-mwsr` sur PC,
Mac, Snapdragon (à la place de SGSR1) et les GPU mobiles qu'aucun banc n'a vus (SGSR1 avant : MWSR coûte moins sur
tout GPU mesuré) ; `mwsr` sur iPhone et iPad quand WebGPU est là (`gl-mwsr` sinon), et en HDR partout ; `off` sur
un Mali d'entrée de gamme. La classe est lue dans la chaîne du renderer WebGL (`gpuRendererString`, une sonde par
page, qui sert aussi `isSnapdragonGpu`) : `isEntryMaliGpu` = G31, G51, G52, G57, G310 et la série T ; les Mali plus
gros (G68, G7x, G510, G610 et au-delà, Immortalis), jamais mesurés, prennent l'upscaler des téléphones. La Mi TV
répond `Mali-G31`, sans nombre de cœurs : la règle se fait sur le modèle seul. Frontière proposée, à confirmer par
Bruno. Sur cette classe, le présentateur simple passe à WebGPU, amélioration activée ou non
(`plainPresenterIsWebGpu`, Canvas2D sans WebGPU), sauf sur une TV (puits `<video>`) et un appareil d'un gigaoctet
(jamais mesuré). Les règles TV et mémoire basse passent avant, inchangées. Le drapeau `mw_mwsr` est retiré. La
mémoire du gouverneur ne demande pas de migration : elle n'est lue que sous le même plafond, et un verdict gardé sous
FSR1 ou SGSR1 est ignoré par une session MWSR, dont le seul cran plus bas est `off`. Tests (`BrowserDetect`, 35) :
chaque classe, le HDR, l'iPhone avec et sans WebGPU, les Mali des deux côtés de la frontière, le présentateur, une
sonde par page ; vitest 1746 / 1746. README.next, wiki (ch. 1, 3, 4, 7), brouillon des notes v0.4.1 ; le chapitre
du wiki sur MWSR reste à écrire.

**La Mi TV (Mali-G31, TV Bro = WebView 153, WebGL2) au comparateur, demande de Bruno (23:00).** La Freebox POP est
en veille profonde (adb muet) : la Mi TV la remplace, par `adb forward` des DevTools et `adb reverse` du labo
(8767), son coupé d'origine et laissé tel quel, TV remise en veille à la fin. 1080p → 1440p, étalon, 300 images
par bande, une passe chacun (23:11, 23:17, 23:24), ms par image (moyenne / p50) :

| réglage | MWSR | FSR1 | SGSR1 | SSIM / PSNR de MWSR |
|---|---|---|---|---|
| défaut du produit (une TV n'est pas un téléphone : CR5) | 208,8 / 195,4 | 621,4 | 225,0 | 0,951 / 25,5 |
| `flat=3` (Q3) | **176,6** / 163,6 | 623,7 | 231,6 | 0,949 / 25,5 |
| `flat=5&fc=0.5` (CR5) | 208,1 / 194,1 | 623,9 | 222,2 | 0,951 / 25,5 |

**Lecture.** Q3 est 15 % plus rapide que CR5 sur la Mali-G31, dans le sens du realme (−13 %) et de l'iPhone
(−9 %) : Android ne se comporte pas autrement, et Q3 sur téléphone et tablette tient. MWSR y coûte 0,34 × FSR1 et
0,78-0,93 × SGSR1. À ~200 ms par image, aucun upscaler n'a sa place sur ce GPU : l'Auto d'une TV reste sans
upscaler (règle TV, et la Mi TV est aussi sous un gigaoctet), et le Mali d'entrée de gamme sans upscaler est
cohérent avec cette mesure.

## 2026-10-25 — J42, P7.0 : la mémoire des images mesurée, la porte reste fermée

**Le cadre.** C'est l'étude du plan §14.3. Elle répond au mandat de nuit de Bruno du 24/10 : « utiliser un historique
d'image pour améliorer le shader actuel, aussi bien d'un point de vue qualité, que d'un point de vue performance ».
Elle s'est faite le 25/10 au matin, aucun réveil n'ayant été programmé pour la nuit. Le travail s'est fait dans un
worktree de brouillon, sur SwiftShader seul (règle de 59). Les passes Chrome ont attendu le « libre » de 59, après
les tests réseau E1-E3. `mwsr` n'est pas touché, et rien n'est activé dans le produit.

**Le mode séquence.** Il vit dans `seq-lab.js` (le `seq=1` de la page) et se pilote par `p7_flows.py`
(`scaler_lab.py --p7`, `--p7-table`).

- **Les séquences.** Une image fixe de 3840×2160 (le texte du labo, ou la capture de 0 A.D.) devient une séquence :
  - un panoramique à 0,3, 1, 2 ou 6 px du flux par image, en direction (1 ; 0,25) ;
  - puis un arrêt, puis une coupure vers l'autre image ;
  - ou bien un bloc rayé qui traverse l'image fixe à 8 px par image.
- **La vérité.** Chaque image est tirée de la même fenêtre de la source, à la taille du flux et en 1440p (Lanczos-3
  en lumière linéaire, comme `make_input`). La vérité est donc exacte, image par image.
- **Le flux.** Il passe par WebCodecs en logiciel (`prefer-software` sous `soft=1` : OpenH264, `avc1.640033`
  accepté), à débit constant et en `realtime`, à 20 et 10 Mbit/s, encodé puis décodé.
- **Les variantes.** Chaque image décodée passe par `mwsr` seul et par sept variantes côte à côte :
  - le tirage borné `pull` (H' = C + a·clamp(H − C, ±t)) ;
  - le poids doux `soft` ;
  - le bornage 3×3 `clamp`, qui est un TAA sans vecteurs ;
  - `mcpull` et `mcclamp`, les mêmes avec le mouvement exact de la séquence : des oracles, lus en Catmull-Rom au
    point d'origine ;
  - `outmc` : la sortie accumulée en 1440p avec le mouvement exact, bornée par son voisinage 3×3. C'est le plafond
    d'une accumulation, la seule voie vers la résolution (§14.2).
- **Les mesures.** Elles se font sur le GPU, par image et par variante :
  - le PSNR ;
  - |ΔE|, l'instabilité de l'erreur, sur toute l'image et sur les contours de la vérité ;
  - |Δout|, le changement de la sortie pendant l'arrêt, où l'image vraie est fixe ;
  - l'erreur dans le sillage du bloc.

  FLIP se calcule côté pilote, sur des recadrages.

**Deux réglages en cours de route, avec le go de 59.**

- **La région d'intérêt (`roi=`).** La vérité, le noyau et les mesures ne tournent que sur un quart de l'écran 1440p :
  le centre, ou la trajectoire du bloc pour `obj`. L'historique reste calculé sur toute l'image à la taille du flux.
  L'accumulation `outmc` couvre la région plus 128 px autour : un pixel dont l'historique vient d'au-delà repart de
  zéro, comme au bord de l'écran.
  - Le coût passe de 22 s à 6,6 s par image.
  - Le contrôle contre l'écran entier (`--p7-roi full`, même page) donne, sur le panoramique, les mêmes écarts à
    0,01-0,1 dB près et les mêmes rapports d'instabilité à 0,01-0,04 près.
  - Après la coupure, sur le jeu, les écarts sont 10-15 % plus grands, car la région est plus dense que l'image
    moyenne (MWSR seul : 22,2 contre 22,8 dB). Le classement et les signes ne changent pas.
- **L'échauffement de l'encodeur (`warm=60`).** Sans lui, l'image clé du départ fait à elle seule le débit d'une
  séquence courte : à 10 Mbit/s, on a mesuré plus de bits qu'à 20. Une image fixe en échauffement ne marche pas non
  plus : l'encodeur met des bits de côté, puis les dépense (74 Mbit/s mesurés pour 10 demandés).
  - L'échauffement retenu (`warmFrames`) rejoue le premier mouvement à l'envers jusqu'à la première image : 60 images,
    ou 31 et 47 sur le panoramique rapide, faute de place dans la source.
  - Le débit est alors tenu dès la première image mesurée : 9,8 et 20,1 Mbit/s.

**La campagne `p7c`** (25/10, de 09:15 à 12:35). Elle couvre ×2 (720p → 1440p) et ×1,33 (1080p → 1440p), sur les
deux images et cinq séquences :
- lent, moyen et rapide : 60 images de panoramique, 20 d'arrêt, 20 après la coupure ;
- très rapide : 30, 10 et 10 ;
- le bloc : 40 images.

Chaque image est mesurée par huit chaînes, à deux débits.

- **Le coût.** 20 pages, 3 h 20 de mur, 102 000 s de CPU (8 à 9 des 24 fils, Chrome de labo en BelowNormal) et
  aucun Chrome resté.
- **Un incident.** Un « Failed to fetch » sur le post d'un recadrage a coûté un rechargement de page (12 min) ; la page
  renvoie maintenant un post raté jusqu'à trois fois.

La porte, en moyenne sur les deux cas, les deux images et les deux débits. Qualité et stabilité se lisent sur le lent
et le moyen ; les traînées sur le très rapide, le bloc et les cinq images après chaque coupure :

| variante | Δ PSNR | Δ FLIP | instabilité des contours | pire traînée | passe ? |
|---|---|---|---|---|---|
| `pull` a 0,5, t 6 | −0,01 | +0,0006 | ×0,99 | −0,11 | non |
| `pull` a 0,75, t 10 | −0,13 | +0,0067 | ×1,00 | −0,88 | non |
| `soft` a 0,75, t 16 | −0,02 | +0,0009 | ×1,00 | −0,07 | non |
| `clamp` a 0,75 | −1,71 | +0,0387 | ×1,28 | −8,08 | non |
| `mcpull` (oracle) | −0,03 | −0,0013 | ×0,98 | −0,88 | non |
| `mcclamp` (oracle) | −0,21 | +0,0001 | ×0,99 | −8,08 | non |
| `outmc` (oracle) | +0,11 | −0,0023 | ×0,92 | −6,39 | non |

Les seuils sont ceux du plan : +0,3 dB ou −0,005 de FLIP, l'instabilité au plus ×0,5, aucune traînée sous −0,1 dB.
Le débit : à 10 Mbit/s, aucune variante ne rattrape `mwsr` seul à 20. `mwsr` seul y perd −0,61 dB, la meilleure
variante (`outmc`, un oracle) −0,41.

**Lecture.**

1. **À l'arrêt, il n'y a rien à filtrer.** Le flux décodé ne bouge plus, car l'encodeur répète ses blocs : `mwsr` seul
   donne |Δout| = 0,007/255 sur le jeu lent à 20 Mbit/s. L'historique, lui, se recale encore après le panoramique :
   l'image bouge de 5 à 60 % de plus qu'avec MWSR seul, et le rapport monte à ×14-×70 là où `mwsr` seul est presque
   immobile. Le §14.1 l'annonçait (« il n'y a rien à accumuler »).
2. **En panoramique, sans mouvement connu, rien ne se gagne.** `pull` et `soft` font de −0,47 à +0,07 dB selon le cas,
   avec une instabilité inchangée. `clamp` sans vecteurs perd de 1,5 à 6 dB : le texte se dédouble.
3. **Avec le mouvement exact, l'accumulation en 1440p gagne un peu.**
   - Le gain va de +0,04 à +0,28 dB en ×2 et de +0,20 à +0,58 dB en ×1,33 (le haut sur le texte), et le FLIP baisse
     de 0,006 à 0,010 sur le jeu.
   - Ce gain est plat le long du panoramique : +0,21, +0,20 puis +0,19 dB sur les images 1-9, 10-29 et 30-59 du jeu
     lent. C'est donc un débruitage, pas une résolution accumulée.
   - Les oracles qui travaillent à la taille du flux (`mcpull`, `mcclamp`) perdent ou font jeu égal : le Catmull-Rom
     adoucit un peu à chaque image.
4. **Les traînées touchent tout le monde.** Toutes les variantes perdent à la coupure (jusqu'à −6,5 dB sur cinq
   images) et derrière le bloc (jusqu'à −8,1 dB), oracles compris : elles ne connaissent que le mouvement de la
   caméra.
5. **Le débit ne se gagne pas non plus.** Aucune variante ne compense la moitié du débit.

**Verdict : non.** P7 se ferme à P7.0, comme le §14.3 le prévoyait si la porte restait fermée. `mwsr` et le §3.5
restent tels quels : rien dans le produit, pas de cran `mwsr-t`.

Pour la troisième version imaginée par Bruno (l'hôte natif enverrait un drapeau de scène, des vecteurs ou une image
légère), les oracles donnent le plafond :
- des vecteurs parfaits rendraient +0,2 à +0,6 dB sur un panoramique ;
- il faudrait encore le drapeau de scène et des vecteurs par bloc contre les traînées ;
- le tout pour une passe de plus en 1440p et 59 Mo de mémoire.

Le déblocage côté client reste une piste mesurable avec ce même labo, avec ou sans le découpage et le QP des blocs
venus de l'hôte. Il est proposé à Bruno.

La page de comparaison (privée, hors dépôt) montre recadrages, cartes d'erreur, courbes par image et coûts.

**Limites.**
- OpenH264 n'est pas NVENC : il a placé une image clé toutes les 17 images sur le panoramique rapide du jeu à
  10 Mbit/s.
- Deux images sources seulement.
- Un quart de l'écran.
- Deux réglages par famille d'historique, choisis d'avance.

## 2026-10-25 — J43, P8.0 : le déblocage côté client mesuré, la porte reste fermée

**Le cadre.** C'est le premier pas mesurable du §14.5, la question posée à la fin de P7 ; Bruno a répondu « Go »
le 25/10. Le travail s'est fait en deux étapes, sur le processeur puis sur SwiftShader, aux règles de 59 :
BelowNormal, 8 fils au plus (le Chrome de labo épinglé sur les CPU 16 à 23), son propre profil, `--mute-audio`,
aucun Chrome resté. Les passes Chrome ont attendu le « libre » de 59. `mwsr` n'est pas touché, et rien n'est activé
dans le produit.

**L'étape hors ligne** (`scripts/bench/scaler-lab/p8/`, ffmpeg 9.0.1, numpy).

- **Les sources.** Le clip CoD des bancs (`~/.mw-bench/content/cod.webm`, 2560×1440 à 59,94 i/s, une capture déjà
  compressée en VP9) donne trois fenêtres de 180 images :
  - 38 s, une explosion contre un mur, avec une coupure dedans ;
  - 121,5 s, le ciel, puis un vol rapide au-dessus d'une ville ;
  - 33 s, une scène calme.

  Le texte du labo et la capture de 0 A.D. y ajoutent un panoramique de 3 px de la source 4K par image. Tout passe
  en 720p et en 1080p (Lanczos).
- **Les encodages.**
  - x264 : veryfast, zerolatency, High, sans images B, une seule tranche (les tranches parallèles laissaient des
    coutures).
  - x265 : veryfast, zerolatency.
  - libaom : temps réel, cpu-used 8.

  Tous ont un tampon d'une image (`bufsize` = débit/60, comme `RateControl.h`). Débits : 4, 8 et 16 Mbit/s en 720p,
  6, 12 et 24 en 1080p, deux seulement en HEVC et en AV1. Les 60 premières images chauffent l'encodeur, puis 40 sont
  mesurées, une sur trois.
- **Les mesures**, sur la luminance, contre l'entrée de l'encodeur :
  - le PSNR, sur toute l'image, les aplats et les textures ;
  - R, l'énergie des marches de l'erreur sur la grille de 8, rapportée à celle des phases 2, 3, 5 et 6 ;
  - TR, le même rapport sur les moyennes des blocs.

  La part des marches qui reste vaut (R − 1)/(R sans filtre − 1).
- **Sans filtre.** x264 laisse une grille : R de 1,2 à 1,35, et de 1,3 à 1,4 dans les aplats. x265 et libaom n'en
  laissent presque aucune dans les aplats. Les images fixes n'en ont pas : leur erreur est du détail perdu.
- **Les filtres**, aveugles, sur la luminance :
  - les bords façon H.264 (`edge`, force s) ;
  - le seuillage DCT sur des grilles 8×8 décalées : 16 décalages au pas de 2, 4 au pas de 4, ou 8 en damier. Il
    efface les coefficients sous le seuil T et garde la composante continue. Une variante bornée limite la retouche
    d'un pixel à ±c ;
  - un champ des moyennes de blocs (`dc_field`), abandonné car nuisible : sur une image déjà déblocée par le
    décodeur, changer l'ajustement d'un bloc ajoute des marches ;
  - ffmpeg : `deblock`, et `spp`, `pp7`, `fspp` nourris du QP réel de chaque macrobloc
    (`-export_side_data venc_params`).

Le premier balayage (`p8_sweep.py`) a écarté plusieurs pistes :
- les seuils au-delà de 8, de −0,5 à −4 dB ;
- le DCT à 4 décalages, qui fait sa propre grille de 4 px ;
- `deblock` de ffmpeg, à −0,35 dB, qui multiplie la grille par 4 ;
- `spp`, `pp7` et `fspp`, de −3,9 à −7 dB même avec le QP réel : réglés pour MPEG-2, bien trop forts sur du H.264.

Une force aveugle, seuil = 3 + 4 × la marche visible, vaut le seuil fixe 3 : la marche visible prédit mal le
meilleur seuil.

Le second balayage (`p8_sweep2.py`, `p8_policy.py`) donne les moyennes contre le flux sans filtre, en dB, avec entre
parenthèses la part des marches qui reste :

| réglage | CoD H.264 | CoD HEVC | CoD AV1 | texte | jeu |
|---|---|---|---|---|---|
| DCT, seuil 3 | +0,33 (0,58) | +0,33 | +0,26 | +0,03 | −0,04 |
| DCT, seuil 4 | +0,28 (0,46) | +0,28 | +0,22 | +0,04 | −0,09 |
| DCT, seuil 6 borné à 2 | +0,18 (0,35) | +0,17 | +0,11 | +0,02 | −0,26 |
| DCT, seuil 8 borné à 2 | +0,04 (0,27) | +0,04 | −0,01 | −0,03 | −0,43 |
| DCT, seuil 12 borné à 4 | −0,40 (−0,05) | −0,42 | −0,45 | −0,22 | −1,18 |
| DCT en damier (8 décalages), seuil 4 | +0,26 (0,47) | +0,26 | +0,20 | +0,03 | −0,11 |
| bords façon H.264, s 2 | +0,09 (0,72) | +0,12 | +0,09 | −0,06 | −0,00 |
| seuil = 0,25 × Qstep(QP de l'image) | +0,35 (0,40) | +0,37 | – | +0,03 | −0,05 |
| meilleur seuil de chaque image | +0,39 (0,30) | +0,38 | +0,34 | +0,04 | −0,04 |

Le QP de chaque image, que l'hôte pourrait envoyer, ajoute +0,02 dB au seuil fixe 3 et efface 20 % de marches en
plus, près du plafond.

**Le labo.** Le mode séquence de P7 (`seq-lab.js`) reçoit trois ajouts.

- **Les variantes de déblocage.**
  - `dbe:s` est le filtre des bords, en deux passes de rendu.
  - `dbd:t;n;c` est le seuillage DCT : un dispatch de calcul par décalage, dans un tampon, puis une passe qui ajoute
    la retouche de la luminance aux trois canaux, bornée par c.

  Les forces sont en niveaux de Y du flux (plage limitée). Les passes ne travaillent que sur la région du flux que
  voit la région mesurée, plus 16 px. Le calcul a été vérifié avant toute passe Chrome : une transcription fil par
  fil du WGSL en numpy (`p8_check_wgsl.py`) redonne les filtres à 4e-5 près pour le DCT, exactement pour les bords.
- **La lecture d'un clip** (`vid=<préfixe>`). Les images PNG d'une fenêtre du clip sont copiées dans la texture
  source avant chaque image, et la fenêtre reste fixe sur l'image entière. L'image vraie en 1440p est donc l'image
  du clip elle-même, ce que vérifie au bit près un recadrage. Le pilote extrait ces images
  (`--p7-video cloud=121.5,boom=38`, ffmpeg en BelowNormal).
- **Les séquences** `vid` (60 images après 60 d'échauffement) et `tryvid` (6).

**Les essais.**
- **Sur les images fixes** (`p8try`, x2), les passes compilent et tournent : 89 s par page, environ 570 s de CPU.
  Mais aucun réglage ne bouge le PSNR de plus de 0,01 dB, faute de grille.
- **Sur le clip** (`p8vtry`, x2 sur le ciel, x133 sur l'explosion), le gain croît avec la force.
- **La porte a donc été amendée avant la campagne.**
  - Le critère des blocs se juge sur deux fenêtres du clip, avec les mêmes seuils.
  - Le filtre des bords, à +0,002 dB à l'essai et le plus faible hors ligne aussi, cède sa place à un réglage plus
    fort, `dbd:t=12;n=16;c=4`.
  - Les images fixes ne passent qu'ensuite, et seulement si un réglage franchit le critère des blocs.

**La campagne `p8v`** (25/10, de 15:25 à 16:25). Quatre pages : ×2 (720p → 1440p) à 4 et 10 Mbit/s, ×1,33
(1080p → 1440p) à 6 et 12 Mbit/s, sur les deux fenêtres.
- Le débit est tenu : 3,9 à 4,3 Mbit/s pour 4 demandés.
- Le coût : 23 200 s de CPU, environ 6,5 des 8 fils permis, aucun Chrome ni ffmpeg resté.

Le tableau donne les moyennes des deux fenêtres sur les images du panoramique (rôle `pan`, t ≥ 3), en Δ PSNR (dB)
/ Δ FLIP contre `mwsr` seul :

| réglage | 720p · 4 Mbit/s | 1080p · 6 Mbit/s | 720p · 10 Mbit/s | 1080p · 12 Mbit/s | instabilité | passe ? |
|---|---|---|---|---|---|---|
| DCT, seuil 3 | +0,02 / −0,0001 | +0,02 / −0,0001 | +0,05 / −0,0002 | +0,06 / −0,0001 | ×0,99 | non |
| DCT, seuil 4 | +0,04 / −0,0002 | +0,04 / −0,0002 | +0,06 / −0,0002 | +0,08 / −0,0002 | ×0,99 | non |
| DCT, seuil 6 borné à 2 | +0,05 / −0,0003 | +0,06 / −0,0002 | +0,07 / −0,0002 | +0,11 / −0,0002 | ×0,99 | non |
| DCT, seuil 8 borné à 2 | +0,06 / −0,0004 | +0,07 / −0,0003 | +0,06 / −0,0001 | +0,12 / −0,0003 | ×0,99 | non |
| DCT, seuil 12 borné à 4 | +0,09 / −0,0003 | +0,13 / −0,0005 | −0,01 / +0,0006 | +0,17 / −0,0000 | ×0,98 | non |

La porte, fixée avant la campagne :
- à bas débit, +0,1 dB ou −0,002 de FLIP, en 720p et en 1080p chacun ;
- à haut débit, rien sous −0,05 dB, ni au-delà de +0,001 de FLIP ;
- l'instabilité au plus ×1,05.

Le seuil 12 borné à 4 passe en 1080p, pas en 720p ; les autres ne passent nulle part. Les images fixes (`p8c`)
n'ont pas été lancées.

**Lecture.**

1. **Ce que le filtre efface sur le flux ne se retrouve presque pas après `mwsr`.** En 1440p, l'erreur vient surtout
   du détail que l'encodeur a perdu, et le filtre ne le rend pas. Le meilleur réglage choisi image par image est un
   oracle de la force, ce qu'une aide parfaite de l'hôte donnerait. Il fait +0,100 dB en 720p à 4 Mbit/s, +0,105
   à 10, +0,129 en 1080p à 6 et +0,204 à 12.
2. **Le classement s'inverse entre le flux et l'écran.**
   - Hors ligne, le seuil 3 est en tête et les forts perdent.
   - Au labo, le seuil 12 borné à 4 est en tête, sauf à haut débit sur l'explosion : là, il efface du vrai détail
     (−0,14 dB, FLIP +0,0024).
   - Plusieurs causes sont possibles, non séparées : l'encodeur (celui de Chrome n'est pas x264), l'affinage de
     `mwsr`, qui ferait ressortir le bruit que le filtre efface, et la vérité (l'image en 1440p plutôt que l'entrée
     de l'encodeur).
3. **Les grandes marches restent.** Dans le ciel, les marches entre blocs de 16 px dépassent les seuils : le filtre
   les prend pour de vraies arêtes, et la borne c les laisse.
4. **FLIP ne bouge pas** (au mieux −0,001). À l'œil, la différence se voit à peine sur les recadrages.
5. **Le coût.** Les 16 grilles décalées font environ 500 multiplications par pixel du flux : 0,5 milliard par image
   720p, 28 milliards par seconde à 60 i/s. C'est peu pour une RTX, beaucoup pour un Mali d'entrée de gamme. Rien
   n'est chronométré sur GPU. Sur SwiftShader, une image 1440p demande 10 s en 720p et 15 s en 1080p, pour `mwsr`
   seul et cinq réglages à deux débits.

**Verdict : non.** P8 se ferme à P8.0 : pas de passe de déblocage dans le produit, `mwsr` inchangé.

L'aide de l'hôte pour le déblocage (découpage et QP des blocs) est écartée : même une force parfaite ne donne que
+0,10 dB après `mwsr`. De la troisième version imaginée par Bruno restent le drapeau de scène et les vecteurs (P7.0,
§14.5), à sa décision.

La page de comparaison (privée, hors dépôt) montre recadrages, cartes d'erreur, courbes par image et tableaux.

**Pour rejouer.**
- Hors ligne, dans `scripts/bench/scaler-lab/p8/` : `python p8_offline.py prep`, puis `python p8_offline.py encode`,
  puis `p8_q1.py`, `p8_sweep.py`, `p8_sweep2.py` et `p8_policy.py`.
- Au labo :
  `scaler_lab.py --tag p8v --swiftshader --p7 --p7-case x2 --p7-seq vid --p7-video cloud=121.5,boom=38 --p7-content cloud,boom --p7-kbps 10000,4000 --p7-hist "none,dbd:t=3;n=16,…"`,
  puis `--p7-table p8v` et `p8_gate.py p8v`.

**Limites.**
- Un seul clip de jeu, lui-même déjà compressé en VP9.
- L'encodeur de Chrome en logiciel n'est pas NVENC.
- Un quart de l'écran.
- Des forces choisies d'avance.
- Des mesures (PSNR, FLIP), sans comparaison à l'œil en aveugle.

## 2026-10-25 — J44, les versions 2 et 3 de MoonlightWeb dans le menu de débogage

**La demande.** P8.0 fermé, Bruno garde `mwsr` tel quel pour la production. Il demande les deux autres versions
dans la liste déroulante des builds de débogage, avec les réglages qui me semblent les meilleurs. La version 3 doit
s'appuyer sur ce que l'hôte sait. Le plan les décrit au §14.7.

**Les réglages.**
- **Version 2 : seuil 8 borné à 2.** Sur les huit cas du labo de J43 (deux fenêtres du clip, deux tailles, deux
  débits), c'est le seul réglage qui ne perd nulle part : +0,03 à +0,16 dB, +0,08 en moyenne. Le seuil 12 borné à 4
  fait +0,10 en moyenne, mais −0,14 dB et FLIP +0,0024 sur l'explosion en 720p à 10 Mbit/s.
- **Version 3 : seuil = 0,4 × 2^((QP − 4)/6), plafonné à 12, borné au tiers (2 au moins), aucune passe sous QP 20.**
  - Hors ligne, sur le flux seul, le meilleur seuil suivait le QP avec un facteur de 0,25 à 0,3 (sweep2.json : QP
    34 à 37 → seuils 6 à 12, QP 28 à 31 → 4 à 12, QP 22 à 25 → 3 à 8).
  - Au labo, après `mwsr`, les forces plus grandes gagnaient, d'où 0,4.
  - Les QP des encodages du labo (l'encodeur de Chrome) ne sont pas connus. Leur PSNR, rapproché des encodages x264
    hors ligne, les place vers 28 à 29 à haut débit et vers 31 à 39 à bas débit : le facteur donne alors des seuils
    de 6 à 7, et de 9 au plafond de 12. C'est une estimation, pas une mesure de ce réglage.
- **Le QP de l'hôte.** Il est dans l'en-tête de chaque tranche, écrit par l'encodeur de l'hôte. `SliceQp.js` le lit
  avant le décodage, en H.264 et en HEVC, jusqu'à `slice_qp_delta`. Le reste de l'en-tête est lu au passage : jeux
  d'images de référence (prédits ou non), images de long terme, modification des listes, prédiction pondérée,
  marquage des références. La lecture s'arrête à la première tranche. En AV1, ou sur un en-tête illisible, la
  version 3 prend la force de la version 2.

**Le produit.**
- Les valeurs du menu `mwsr-db` et `mwsr-dbq` deviennent `mwsr` plus l'option `deblock` 'fixed' ou 'qp' du
  `WebGpuRenderer`, dans les deux chemins (fil principal et worker). L'échelle de l'enhancer voit `mwsr`.
- `DeblockShader.js` fait d'abord un dispatch de calcul : un groupe par bloc 8×8 et par grille (z = la grille,
  16), les sommes ajoutées par atomiques en virgule fixe (1/256 de niveau). Une passe de rendu écrit ensuite
  l'image que le noyau lit.
- Le labo faisait un dispatch par grille, avec des additions simples : ce sont les mêmes sommes, à l'arrondi près.
- SDR seulement : sur les intermédiaires HDR, la passe se coupe d'elle-même.
- Le QP de chaque image suit son horodatage jusqu'au dessin. L'image de QP 18 saute les deux passes.
- Coût nul quand rien n'est choisi : `wantsFrameQp` vaut faux, aucun en-tête n'est lu et aucune ressource n'est
  construite. `test/DeblockShader.test.js` le vérifie sur un faux GPU.
- Le panneau de statistiques affiche « MWSR 2 », ou « MWSR 3 (QP n, tx) » quand le rendu tourne sur le fil
  principal.

**Les contrôles.**
- `test/SliceQp.test.js` : six flux x264 et x265 de huit images (P seules à tampon d'une image, B en pyramide avec
  prédiction pondérée et deux tranches, Baseline en CAVLC, HEVC P seules, HEVC B pondérées en deux tranches, HEVC
  4:4:4 avec listes de quantification et GOP ouvert). Chaque QP est égal à celui que lit l'analyseur d'en-têtes de
  ffmpeg (`trace_headers`). S'ajoutent les cas d'un jeu de paramètres manquant, d'un en-tête coupé, de l'AV1 et
  d'un corps de tranche brouillé.
- `deblock_check.py` sur SwiftShader, une image 720p du clip des nuages à 4 Mbit/s (x264) :
  - les passes seules contre numpy (`p8_filters.dct_shrink`) : au plus un niveau d'écart, 0,0009 à 0,0012 en
    moyenne, aux forces t8c2, t12c4 et t3,2c2, environ 1 s chacune ;
  - le `WebGpuRenderer` avec `mwsr` : sans erreur WebGPU ; son image déblocée est identique aux passes seules ;
    l'image 1440p s'écarte de `mwsr` seul à 51,5 dB (version 2) et à 48,0 dB (version 3, QP 36) ; l'image de QP 18
    est identique à celle de `mwsr` seul. chrome left: 0.
- `npm run check` et vitest verts.

**Limites.** Rien n'est chronométré sur GPU. Le facteur 0,4 de la version 3 n'est pas mesuré tel quel. Le gain
mesuré par P8.0 reste petit (au mieux +0,1 à +0,3 dB) : ces versions servent à ce que Bruno juge à l'œil.

**Pour rejouer.** `python -X utf8 scripts/bench/scaler-lab/deblock_check.py`, puis `npx vitest run
test/SliceQp.test.js test/DeblockShader.test.js` dans `frontend/`. Les flux de test viennent d'un script
(ffmpeg, `trace_headers`) gardé hors du dépôt ; leur contenu est dans `test/fixtures/slice-qp.json`.

**Le verdict de Bruno (25/10, au soir).** Il garde la version 1 pour l'auto : « J'adore le résultat, plus rapide et
plus propre que FSR1, c'est une réussite. » Les versions 2 et 3 restent dans le menu de débogage. Le plan le
consigne au §14.7, avec ses questions sur l'historique, l'aide de l'hôte et la fusion dans le noyau.

## 2026-10-26 — J45, le HDR dans le chemin du produit : agrandissement et réduction

**La demande.** Bruno, le 25/10 au soir : MWSR n'avait pas été essayé en HDR dans le produit. J11 avait mesuré le
noyau seul sur le feed HDR du labo (720p → 1440p, 21/10), avant les défauts d'aujourd'hui (`DERING 2`, CR5 et Q3,
netteté 1,85 et sa loi). Il demande un test avec une image HDR, en agrandissement et en réduction.

**L'instrument (`hdr_check.py`, `hdr-check.html`, `hdr-check.js`).**
- **Les images.** Les mires HDR du labo (`hdr_mires.py` : rampe PQ jusqu'à 10 000 nits, texte sur 1000 nits),
  la capture 4K de 0 A.D. (`C:\Test\01_4K.png`) aux hautes lumières montées à 1000 nits, et des rayures
  verticales de 1000 et 0,5 nit, larges d'un et de trois pixels.
- **Le format.** Chaque image est faite en numpy dans le format que le décodeur donne au renderer : PQ, BT.2020,
  Y'CbCr limité sur 10 bits. Le I420P10 sert partout, le I444P10 pour un cas.
- **Le rendu.** Le vrai `WebGpuRenderer` dessine chaque image sur ses deux chemins HDR :
  - `linear` (écran HDR) : canvas `rgba16float` en `extended`, MWSR sur des intermédiaires flottants ;
  - `tonemap` (écran SDR) : ACES en passe 0, puis MWSR sur 8 bits.
- **Les variantes.** `off` (la copie du produit) et `mwsr`, en 720p et 1080p → 1440p, 2160p → 1440p, 1440p → 720p,
  plus une image à 1:1. En réduction sur `linear` s'ajoute un diagnostic : la réduction en sRGB exact
  (`LINEAR_LIGHT 2`) au lieu du carré du produit.
- **La lecture.** La texture du canvas est remplacée par une texture de son format, relue entière : demi-flottants
  sur `linear`. SwiftShader seul, la page refuse tout autre adaptateur. Le module f16 n'y est pas accordé :
  J14 l'avait comparé au f32 sur le RTX.

**Ce qui va.**
- À 1:1, l'image est celle de la passe 0 refaite en numpy (YUV → PQ → P3), à 0,002 près.
- Aucune valeur infinie ni négative.
- Les hautes lumières sont gardées : la part des pixels au-dessus de 600 nits vaut 0,92 à 1,05 de celle de la
  vérité (la rampe en 2160p → 1440p mise à part, voir (a)), et 10 000 nits restent 10 000.
- Le noyau tient ses bornes dans son propre espace : 0 % de pixels hors des 4×4 texels voisins, en agrandissement
  et à ×0,5.
- Sur le chemin `tonemap`, la réduction passe par les vues sRGB et elle est juste : les rayures gardent 1,001 et
  0,987 de leur lumière.

**Le mode « off » en HDR était au plus proche voisin, corrigé (1d6f92f5).** La passe 0 YUV lit ses plans texel par
texel. Dessinée à la taille du canvas, elle mettait l'image à l'échelle au plus proche voisin : pixellisée à
l'agrandissement, crénelée à la réduction. À ×0,5, les rayures ne gardaient qu'un tiers de leur lumière, et sur le
chemin `tonemap` la réduction faisait 4,5 dB de PSNR, contre 40,5 pour MWSR.

Au « corrige-le » de Bruno, une image mise à l'échelle passe maintenant d'abord par la passe 0 à sa taille, puis par
la copie bilinéaire des images SDR. Chaque cas « off » est la copie bilinéaire de son entrée, à 0,004 près.

Au passage, `_ensureInputTex` construisait pour « off » le groupe de liaison de SGSR, dont la disposition n'existe
pas. Dans un navigateur, cet appel lève une exception : après un import refusé, la copie par ImageBitmap retombait
donc toujours sur la relecture.

**(a) La réduction HDR assombrit le contraste fin.** Part de la lumière des rayures gardée :

| réduction | `off` (bilinéaire) | `mwsr` (`LINEAR_LIGHT 1`, le produit) | `LINEAR_LIGHT 2` | `tonemap`, `mwsr` |
|---|---|---|---|---|
| 1440p → 720p (×0,5) | 0,605 | 0,918 | 0,988 | 0,987 |
| 2160p → 1440p (×0,667) | 0,786 | **0,724** | 0,742 | 1,001 |

- **Deux causes.** À ×0,5, les prises tombent sur les centres des texels : seule la lumière de la moyenne compte,
  et le carré (gamma 2) assombrit un mélange de 1000 et 0,5 nit de 8 %. Ailleurs, chaque prise bilinéaire mélange
  déjà deux texels sur les valeurs encodées, avant tout décodage : le sRGB exact n'y change presque rien.
- **La même cause** fait tomber à 0,853 la part des pixels au-dessus de 600 nits sur la rampe en 2160p → 1440p, à
  cause de sa bande de rayures.
- **Sur des images réelles, l'écart est petit**, et MWSR reste devant la copie (ΔE ITP du jeu en 2160p → 1440p :
  8,82 contre 9,33).
- **Le remède proposé**, exact à tout rapport et sans coût en plus : sur un feed flottant, la passe 0 écrirait de la
  lumière linéaire quand on réduit. L'échantillonneur mélangerait alors en lumière linéaire, comme les vues sRGB le
  font en SDR, et le noyau n'encoderait qu'à la sortie.
- **Le coût de `LINEAR_LIGHT 2`**, déjà mesuré par J14 (feed SDR, même arithmétique) : 780M 0,54 → 1,59 ms à ×0,75
  et 0,25 → 0,71 ms à ×0,5 ; RTX 0,022 → 0,048 ms. Il ne règle que le cas ×0,5.
- À la décision de Bruno.

**(b) L'agrandissement HDR a régressé depuis J11.** ΔE ITP moyen contre la vérité, chemin `linear` :

| cas | `off` (bilinéaire) | `mwsr` |
|---|---|---|
| texte 720p → 1440p | 16,49 | **20,86** |
| texte 720p → 1440p, 4:4:4 | 15,99 | **20,07** |
| jeu 720p → 1440p | 20,87 | 21,80 |
| jeu 1080p → 1440p | 15,44 | 15,99 |
| rampe 720p → 1440p | 34,36 | 29,63 |
| rampe 1080p → 1440p | 22,69 | 14,47 |

- **La comparaison avec J11.** J11 avait, sur la même mire de texte, 15,76 pour le bilinéaire et 15,38 pour
  `mwsr`. MWSR y était 2 % devant ; il est aujourd'hui 27 % derrière. Le bilinéaire, lui, n'a presque pas bougé
  (16,0 en 4:4:4 contre 15,8).
- **Ce qui a changé depuis** : le bornage par les quatre texels (`DERING 2`, J37), le chemin plat en Catmull-Rom à
  cinq prises (7264afd4), la netteté 1,85 et sa loi d'échelle (J40). Tous ont été choisis à l'œil sur du SDR.
- **Sur le chemin `tonemap`**, le PSNR de `mwsr` est aussi sous celui du bilinéaire sur le texte (16,60 contre
  17,13). Le labo SDR le voit aussi sur `code` (26,20 contre 27,27, CI de 248aae8e) : le PSNR pénalise l'affûtage,
  et l'œil tranche.
- **Ce qu'il faut maintenant** : une étude réglage par réglage au labo, puis l'œil de Bruno sur un écran HDR.

**(c) Sur du texte HDR en 4:2:0, MWSR fait des hautes lumières.** Le pic monte à 1489 nits, contre 1295 pour
l'entrée (× 1,15). Le noyau borne chaque canal séparément, et les franges de chrominance du 4:2:0 lui permettent de
réunir le rouge d'un texel et le vert d'un autre. En 4:4:4, le pic reste à 0,979.

**Les seuils, corrigés entre les passes.** Quatre passes : à 23:50 (17 contrôles hors limites), à 00:16 (15), à
00:20 (11), puis à 00:24 (9, tous réels : (a) quatre fois, (b) quatre fois, (c) une fois).
- **Le pic des réductions.** Je l'ai d'abord comparé à celui de l'entrée, puis à celui de la vérité. Or le Lanczos
  de la vérité sonne au-delà de l'entrée : 10 991 nits sur une rampe qui s'arrête à 10 000, et plus de 1100 nits au
  bord du texte. Ce contrôle compte maintenant la part des pixels au-dessus de 600 nits.
- **Les dépassements.** La mesure de J11 (BT.2020 linéaire, 2×2) prend le bilinéaire lui-même en défaut : 0,05 %,
  car on interpole en P3 encodé. Les bornes se jugent maintenant dans l'espace du noyau, et l'ancienne mesure reste
  affichée.
- **Le PSNR du chemin `tonemap`** est affiché seulement.

**Limites.**
- SwiftShader, dont les pixels sont ceux du RTX d'après J11. Aucun temps n'est mesuré.
- Le f16 n'est pas comparé ici, faute de `shader-f16` sur SwiftShader.
- Chrome seulement : le chemin `tonemap-ext` de WebKit n'est pas couvert.
- Le 4:2:0 est synthétique (chrominance moyennée 2×2 en PQ).

**Pour rejouer.** `python -X utf8 scripts/bench/scaler-lab/hdr_check.py`, avec `--only` (des cas par leur nom) ou
`--keep` (garder les relectures). Il faut `C:\Test\01_4K.png` pour le jeu ; sans elle, ces cas sont sautés. Environ
trois minutes ; la sortie va dans `bench-out/scaler-lab/hdr/`.

## 2026-10-26 — J46, la conformité des jumeaux sur une seule image d'entrée

**Le résidu.** Depuis le 23/10, le job « Upscaler lab » sort rouge au pas WebGL2, sur un seul cas : la réduction
2560×1440 → 1280×720 de `code`. Le jumeau y diffère du noyau de 4/255 au plus, sur 0,173 % de pixels au-delà de
2/255, pour une limite de 0,1 %. Les bilinéaires nus, eux, diffèrent de 3/255 sur 0,014 % (CI 39912544431 et
40233795656, mêmes chiffres). Bruno a dit oui le 26/10, par 59, pour le corriger dans la v0.4.1.

**La cause.** Chaque côté dessinait sa mire lui-même, en Canvas2D. Sur le runner, les deux Chromes ne tracent pas
les mêmes bords de glyphes. Les bilinéaires nus de `code` y diffèrent sur 7,09 % des pixels, contre 3,80 % dans le
rejeu WSL de la même plateforme. J32 l'avait vu sur `text` : la différence est dans l'entrée, avant tout renderer.
Le mètre des bilinéaires la porte, mais mal : un pixel de la grille 2×2 lit quatre points de l'entrée, le bilinéaire
un seul.

**Le correctif (`p4_flows.py`).**
- Le côté WebGPU dessine une fois l'image de chaque cas doré, à sa taille d'entrée (la page `gt=1`), et la poste en
  PNG.
- Les deux côtés relisent ce PNG comme entrée (`content=img:`), comme la jambe de qualité le faisait déjà avec ses
  entrées réduites.
- Le côté WebGL2 attend ce fichier (« pending ») si le côté WebGPU n'a pas tourné.
- Le mètre, les tolérances et `ci.yml` ne changent pas.

**Vérifié sous Windows** (SwiftShader, les deux côtés, sortie 0 et 0) :
- Les trois réductions sont exactes, bilinéaires nus et jumeau compris : 0/255. Avant, sur les mires que chaque
  Chrome dessinait : `code` 1/255, `text` 1/255, zone plate 4/255. Ces écarts venaient donc tous de l'entrée, même
  sur la zone plate.
- La chaîne WebGPU reste égale au noyau (0/255) sur les neuf cas.
- La qualité du jumeau contre le noyau ne change pas : FLIP 0,000, PSNR à 0,047 dB au plus, 0,001 % de dépassements.

**Ce qui reste.** Seul le runner peut dire si ses réductions passent, au prochain push de Bruno. Le rejeu WSL ne
reproduisait pas ses chiffres de `text` et de `code`. Sur la zone plate, J32 avait attribué les 36/255 des
bilinéaires à l'échantillonneur WebGPU de Linux. Si c'était aussi l'entrée, cet écart tombera à 0.

## 2026-10-26 — J47, la réduction HDR en lumière linéaire

**La décision.** Bruno, le 26/10 au matin, par 59, sur les constats de J45 : « 1 puis 2, et vérifie qu'il n'y a pas de
régression en SDR (perf et couleur) ». Le 1 est le constat (a) : la réduction HDR assombrissait le contraste fin.

**Le changement (`WebGpuRenderer.js`, `MwsrShader.js`).**
- **Où il s'applique** : sur le chemin `linear` (le canvas flottant d'un écran HDR), quand MWSR réduit l'image.
- **La passe 0** : elle écrit alors la lumière linéaire, sans l'encodage sRGB (`tonemapPq`, courbe 3).
- **Le noyau** : il lit cette lumière sur les grilles des vues sRGB du SDR. L'échantillonneur mélange donc de la
  lumière, comme les vues sRGB le font en SDR, et le noyau encode une seule fois, à la sortie (`LINEAR_LIGHT 3` :
  `dec` laisse la valeur, `enc` prend le sRGB exact). C'est un échantillon jusqu'à ×0,5, puis 9 et 16 en dessous.
- **Ce qui ne change pas** :
  - un agrandissement, le mode « off », une image à 1:1 et le chemin `tonemap` gardent la passe 0 encodée ;
  - une image que la passe 0 YUV n'a pas prise garde la lumière logicielle (`LINEAR_LIGHT 1`) et ses grilles, qui
    restent construites à côté ;
  - le texte GLSL : WebGL2 ne dessine pas de HDR.
- **Pour le labo** : `_mwsrLinearDown` à `false` rend la réduction d'avant, sous le nom `mwsr-enc` (`hdr-check.js`).

**La couleur (SwiftShader).**
- **La lumière des rayures de 1000 et 0,5 nit**, MWSR après contre avant (`mwsr-enc`) :

  | réduction | après | avant | `tonemap` (vues sRGB) |
  |---|---|---|---|
  | 1440p → 720p (×0,5) | **0,987** | 0,918 | 0,987 |
  | 2160p → 1440p (×0,667) | **1,004** | 0,724 | 1,001 |
  | 2160p → 720p (×1/3, 9 échantillons, cas nouveau) | **1,004** | 0,680 | 1,002 |

- **Les hautes lumières** : la rampe en 2160p → 1440p garde 0,999 de sa part de pixels au-dessus de 600 nits
  (0,853 avant).
- **Les bornes** : toujours 0 %.
- **ΔE ITP, après contre avant** :
  - rampe en 2160p → 1440p : 8,43 contre 12,14 ;
  - texte en 2160p → 1440p : 8,84 contre 9,20 ;
  - texte en 1440p → 720p : 13,03 contre 13,15 ;
  - jeu en 2160p → 1440p : 8,86 contre 8,82, égal ;
  - rayures à ×1/3 : 0,40 contre 50,0.
- **Une exception** : les rayures à ×0,667 perdent en ΔE (69,9 contre 48,0). Un seul échantillon y crénèle des
  rayures d'un pixel, que la grille 2×2 floutait. C'est le choix du SDR depuis J15 (un échantillon à travers les vues
  jusqu'à ×0,5), et le chemin `tonemap` fait pareil.
- **Ce qui n'a pas bougé** : les agrandissements HDR et tout le chemin `tonemap` sont au chiffre près ceux de J45.
  Restent donc (b) et (c).

**La couleur en SDR (SwiftShader).**
- Le contrôle des dorés est OK, au bit près, chaîne du produit comprise. Le texte WGSL des grilles a changé d'une
  branche (`LINEAR_LIGHT >= 2u` dans `enc`), morte en SDR.
- 1811 tests verts, `npm run check` vert.

**La vitesse (RTX 5060 Ti, `rtx0`, créneau de 59 de 06:44 à 06:52).**
- **Le banc** : un Chrome sans tête épinglé par `--use-adapter-luid`. Son épinglage a été vérifié avant chaque
  mesure : le moteur 3D de son processus GPU croît sur ce LUID seul. Les temps sont ceux du GPU (horodatages sur chaque
  passe du renderer), en ms, p50/p99.
- **HDR** : `hdr_check.py --time`. Un seul renderer par cas, la réduction d'avant allumée et éteinte par blocs,
  400 tirages par variante en quatre tours ABBA.

  | cas | après | avant | écart |
  |---|---|---|---|
  | témoin 720p → 1440p (même chemin) | 0,2178 / 0,349 | 0,2206 / 0,386 | −1,3 % (le bruit) |
  | 2160p → 1440p (×0,667) | 0,3065 / 2,914 | 0,3456 / 0,356 | **−11,3 %** |
  | 2160p → 720p (×1/3) | 0,3225 / 0,333 | 0,3482 / 0,354 | **−7,4 %** |
  | 1440p → 720p (×0,5) | 0,0995 / 0,105 | 0,1121 / 0,114 | **−11,2 %** |

  - La passe 0 gagne 11 à 14 % sur chaque réduction : elle n'encode plus.
  - Le noyau passe de 4 échantillons à 1 à ×0,667 (−13 %). Il coûte autant à ×1/3 et 0,0008 ms de plus à ×0,5.
  - Le p99 de 2,9 ms à ×0,667 est une traîne de quelques tirages. Elle est apparue dans 2 des 3 passes, sur
    « après » seulement, et n'est pas localisée : Bruno a lancé un stream avant la passe ciblée, et le banc s'est
    arrêté. À revoir au prochain créneau.
- **Le témoin** : une première passe avec deux renderers (deux devices) l'avait mis à 13 % d'écart. Le chrono les
  compare donc sur un seul renderer.
- **SDR** : les cellules `chain-mwsr` de la porte P4, `codec=none`, ni encodeur ni décodeur. Avant = 87286046
  exporté hors du dépôt, après = l'arbre de travail, deux tours ABBA après une passe de chauffe ; médiane des quatre
  passes, `gpu` p50/p99.

  | cas | après | avant |
  |---|---|---|
  | ×2 texte | 0,1049 / 0,110 | 0,1040 / 0,109 |
  | ×2 texte, 60 Hz | 0,1049 / 0,132 | 0,1047 / 0,132 |
  | ×1,33 texte | 0,1156 / 0,120 | 0,1494 / 0,403 (deux passes à une horloge plus basse) |
  | ×1,33 texte, 60 Hz | 0,1162 / 0,141 | 0,1158 / 0,138 |
  | ×0,67 texte | 0,1814 / 0,193 | 0,1824 / 0,191 |
  | ×0,67 texte, 60 Hz | 0,1867 / 0,196 | 0,1866 / 0,208 |

  - L'écart est sous 1 % là où les passes sont stables : pas de régression SDR.

**Pour rejouer.** `python -X utf8 scripts/bench/scaler-lab/hdr_check.py` pour la couleur (SwiftShader). Pour la
vitesse : `hdr_check.py --time N --cdp PORT --only …` dans un Chrome qu'on a soi-même épinglé et vérifié sur le GPU.
Le script n'en lance aucun ; la page refuse SwiftShader, et tout adaptateur dont le nom ne contient pas `--vendor`.

## 2026-10-26 — J48, l'agrandissement HDR réglage par réglage

**La demande.** Le 2 de Bruno (26/10, par 59) : trouver quel défaut a fait régresser l'agrandissement HDR depuis J11.
J45 l'avait mesuré : ΔE ITP du texte 27 % au-dessus du bilinéaire, là où J11 le mettait 2 % en dessous. C'est une
étude de labo : rien ne change dans le produit.

**Les réglages.** Quatre défauts ont changé entre J11 (21/10) et aujourd'hui :
- `DERING` 1 → 2 (J37) ;
- le chemin plat, bilinéaire → Catmull-Rom à cinq prises (`FLAT_TAPS 5`) ;
- `W_MIN` −0,3 → −0,5 ;
- la netteté, 1 à toute échelle → 1,85 avec sa loi par l'échelle (J40), soit 1,23 à ×2 et 1,85 à ×1,33.

`hdr_check.py --ablate` les passe un par un :
- chacun seul sur les réglages de J11 (`j11_…`) ;
- chacun retiré seul de ceux d'aujourd'hui (`today-…`).

C'est le vrai renderer, sur les six agrandissements de J45, chemins `linear` et `tonemap`, SwiftShader.

**J11 est reproduit.** Ses réglages font 16,22 sur le texte, contre 16,49 pour le bilinéaire (−1,6 % ; −2,4 % en J11).

**Le coupable : `DERING 2`.** ΔE ITP sur `linear` :

| cas | bilinéaire | aujourd'hui | J11 | J11 + `DERING 2` | aujourd'hui avec `DERING 1` |
|---|---|---|---|---|---|
| texte 720p → 1440p | 16,49 | 20,86 | 16,22 | 19,97 | **16,24** |
| texte 4:4:4 | 15,99 | 20,07 | 15,60 | 19,24 | **15,60** |
| jeu 720p → 1440p | 20,87 | 21,80 | 20,75 | 21,56 | **20,72** |
| jeu 1080p → 1440p | 15,44 | 15,99 | 15,05 | 15,12 | **15,27** |
| rampe 720p → 1440p | 34,36 | 29,63 | 31,44 | 31,50 | 29,57 |
| rampe 1080p → 1440p | 22,69 | **14,47** | 20,04 | 17,56 | 19,93 |

- **Les autres réglages pèsent peu.** Retirés un par un d'aujourd'hui, sur le texte :
  - le chemin plat : 0 % ;
  - `W_MIN` : −1,8 % ;
  - la netteté à 1 : −4,3 %. Elle amplifie le même effet : à ×2, la loi vaut 1,23.
- **Pourquoi `DERING 2` coûte en HDR.** Il laisse un pixel de contour dépasser son mélange bilinéaire jusqu'au texel
  voisin : c'est ainsi qu'un trait d'un texel garde sa profondeur (J35). Or en HDR, les valeurs encodées vont de 0 à
  2,0 autour d'un texte à 1000 nits : un bord poussé jusqu'au texel est un grand écart en ITP.
- **C'est aussi lui qui fait (c).** Le pic du texte 4:2:0 vaut 1,150 de l'entrée aujourd'hui, 1,015 avec
  `DERING 1`. En 4:4:4 : 0,979 contre 0,888.
- **La contrepartie** : la rampe en 1080p → 1440p, dont la bande de rayures d'un pixel est le cas pour lequel
  `DERING 2` a été choisi. Elle perd 38 % sans lui (19,93 contre 14,47).

**Sur le chemin `tonemap`** (le noyau SDR sur une image SDR), le sens est le même mais en petit. FLIP du texte : 0,1067
aujourd'hui, 0,1053 avec `DERING 1`, 0,1025 avec les réglages de J11. Le PSNR et FLIP pénalisent l'affûtage ; en SDR,
l'œil de Bruno a tranché pour `DERING 2` (J37).

**Ce que cela propose, à la décision de Bruno.**
- **`DERING 1` sur le seul chemin `linear`** (écran HDR, canvas flottant ; le SDR et `tonemap` gardent `DERING 2`).
  - Pour : le texte et le jeu redeviennent plus fidèles que le bilinéaire, et le pic de (c) disparaît.
  - Contre : les traits d'un pixel perdent leur profondeur en HDR, comme en SDR avant J37.
- **Ou garder les défauts** : la mesure dit l'écart à une vérité Lanczos, pas ce que l'œil préfère.
- **Dans les deux cas** : l'œil de Bruno sur un écran HDR, sur du texte, un jeu et des traits fins. Le comparateur
  ne sait pas encore afficher le HDR ; c'est à faire si Bruno veut l'A/B.

**Pour rejouer.** `python -X utf8 scripts/bench/scaler-lab/hdr_check.py --ablate`, environ trois minutes sur
SwiftShader. La sortie va dans `bench-out/scaler-lab/hdr/hdr-ablate.json`.

**L'A/B pour l'œil (`hdr-compare.html`, 26/10, à la demande de Bruno).**
- **Ce qu'elle montre** : le vrai renderer sur le chemin `linear` (canvas flottant). D'un côté `DERING 2`, de l'autre
  `DERING 1`, tirés au sort derrière A et B ; un bouton révèle la clé. Les images sont celles de `hdr_check.py` :
  texte, jeu, rampe aux rayures d'un pixel.
- **La vue** : deux volets qui défilent ensemble, à un pixel de canvas par pixel d'écran, ou un seul volet en
  plein écran, que la barre d'espace bascule.
- **Pour la servir** : `scaler_lab.py --serve 127.0.0.1:PORT --no-tls --dir bench-out/scaler-lab/hdr`.
- **Les deux M27Q** étaient en SDR à 07:1x (dxdiag : `ADVANCED_COLOR_MODE_SDR`). C'est à Bruno d'activer leur HDR.
  Elles annoncent 315 nits au plus : un texte à 1000 nits y est comprimé par l'écran.

## 2026-10-26 — J49, DERING 1 contre 2 : la vitesse sur l'iGPU AMD, l'étalon en SDR et en HDR

**La demande** (Bruno, 26/10, dans la session) : la différence entre les deux bornes, des zooms en SDR et en HDR, la
plus rapide sur le N95 ou l'iGPU du DualRTX, la différence de qualité en SDR, et les dB de l'étalon
`C:\Test\00_Background.png` agrandi depuis 720p et 1080p, en SDR et en HDR. Étude de labo : rien ne change dans le
produit, et la décision HDR de Bruno reste ouverte (J48).

**Ce qui les sépare.** Les deux bornent par les quatre texels voisins. `DERING 1` borne d'abord par les cinq
échantillons du mélange : sur un trait d'un texel, l'échantillon central est déjà l'extrême, et le pixel reste à son
mélange bilinéaire. Un trait noir sur blanc agrandi ×2 donne alors 25 % de gris avec `DERING 1` et du noir avec
`DERING 2`, à netteté 1,23. `DERING 2` fait donc moins de calcul : il n'a ni les min et max des cinq, ni leur clamp.

**La vitesse : `DERING 2` est plus rapide**, sur l'iGPU AMD du DualRTX (Radeon Graphics RDNA 2, LUID 0x15e5b). Le
créneau de 59 a duré de 08:47 à 08:49. Chrome sans tête était épinglé par `--use-adapter-luid`, ce que la croissance
des compteurs WDDM sur ce seul adaptateur a vérifié. C'est le `WebGpuRenderer` du produit, sur l'étalon :
horodatages GPU, 400 images par variante en tours ABBA, deux passes. Les médianes bougent de 0,02 ms au plus d'une
passe à l'autre, et le p99 reste à 0,03 ms du p50.

| passe du noyau, p50 (ms) | `DERING 2` | `DERING 1` | écart |
|---|---|---|---|
| SDR 720p → 1440p | 3,016 | 3,117-3,139 | −3,2 à −3,9 % |
| SDR 1080p → 1440p | 3,024 | 3,145 | −3,8 % |
| HDR `linear` 720p → 1440p | 3,004 | 3,136 | −4,2 % |
| HDR `linear` 1080p → 1440p | 3,042 | 3,175 | −4,2 % |

Sur l'image entière, passe 0 comprise, l'écart va de −2,8 à −3,5 %, soit 0,12 à 0,13 ms. Le N95 garde la passe
unique de J36 (16,5 ms contre 17,2), dans son bruit ; 59 n'en a pas demandé d'autre.

**La qualité sur l'étalon** (SwiftShader, `hdr_check.py --dering`). Vérité : l'étalon à 1440p. Entrée : sa réduction
Lanczos-3 en lumière linéaire. Le SDR est dessiné comme un flux ordinaire, image RGBA et vote sur la luma. Le HDR est
l'étalon aux hautes lumières poussées à 1000 nits, comme le cas `game`, en PQ BT.2020 10 bits 4:2:0. Les PSNR sont
en dB, image entière puis tiers central ; ceux du HDR sont en PQ.

| | `DERING 2` | `DERING 1` | bilinéaire |
|---|---|---|---|
| SDR ×2 (720p) | 20,57 / 31,09, SSIM 0,8730, FLIP 0,0558 | **20,71 / 31,68**, 0,8742, 0,0521 | 20,78 / 31,56 |
| SDR ×1,33 (1080p) | **24,21** / 33,97, SSIM 0,9413, FLIP 0,0417 | 23,82 / **34,89**, 0,9390, 0,0417 | 23,72 / 36,36 |
| HDR ×2 (720p) | 21,33 / 33,01, ΔE ITP 25,97, pic ×1,149 | **21,58 / 34,78**, 25,28, ×0,955 | 21,46 / 35,24 |
| HDR ×1,33 (1080p) | **23,90** / 35,86, ΔE ITP 17,92, pic ×1,099 | 23,62 / **37,33**, 18,75, ×0,912 | 23,40 / 38,69 |

- **En SDR**, l'écart est faible et son sens dépend de la zone. À ×1,33, `DERING 2` gagne l'image entière (+0,39 dB)
  et perd le centre (−0,92 dB). Son gain vient donc d'ailleurs que du centre : bureau, panneau de texte, autres
  fenêtres. À ×2, `DERING 1` gagne de peu, sur toutes les mesures.
- **En HDR**, `DERING 1` gagne tout à ×2. À ×1,33, `DERING 2` gagne l'image entière et le ΔE ITP, et perd 1,5 dB
  au centre.
- **Le pic au-dessus de l'entrée** n'existe qu'avec `DERING 2` en 4:2:0 : ×1,149 et ×1,099, le (c) de J48. En
  4:4:4, il tient à ×1,000.
- **En 4:4:4, le sens est le même.** À ×2 : 21,38 / 32,99 contre 21,64 / 35,65. À ×1,33 : 24,12 / 36,01 contre
  23,79 / 38,53.
- **Les mires de J48 sont reproduites au chiffre.** Texte 4:4:4 : ΔE ITP 20,07 contre 15,60. Rampe à 1080p : 14,47
  contre 19,93.

**Les zooms** (×5, régions de l'étalon et des deux mires, référence et bilinéaire à côté) sont réunis avec les
tableaux dans une page privée, hors dépôt. Les vues HDR y passent par une courbe fixe sur un écran SDR, 1000 nits
servant de blanc. On y voit le trait sombre de 2 px de la rampe, noir avec `DERING 2` et gris avec `DERING 1`, et
les rayures de 2 px, contrastées avec l'un et grisées avec l'autre.

**Faux pas de banc, corrigé avant les chiffres.** La première version passait `yuv444` au renderer SDR du cas
étalon 4:4:4. Elle activait ainsi `CHROMA444`, le vote par canal des flux 4:4:4. Le SDR bougeait de 0,02 à 0,13 dB
et le bilinéaire pas du tout : ce n'était pas un indéterminisme. L'image SDR est dessinée comme un flux ordinaire ;
revérifié, elle redonne les chiffres de la première passe au centième.

**L'outillage.** `hdr_check.py` gagne les cas `up720-etalon`, `up1080-etalon` et leurs jumeaux 4:4:4. Ce sont des
cas de `--dering` seul : ni la passe par défaut ni `--ablate` ne les prennent. Il gagne aussi `--dering`, qui
mesure qualité et dumps, gardés pour l'œil, et `--time N --dering`, qui chronomètre dans un Chrome épinglé (`--cdp`).
`--dry` fait tourner ce code sur SwiftShader, figures sans valeur. La page bascule `DERING` par blocs sur un seul
renderer (`ab=dering`). `metrics_hdr.measure` rend aussi le PSNR PQ du tiers central.

## 2026-10-26 — J50, le verdict de Bruno : `DERING 2` partout

**La décision** (Bruno, 26/10 au soir, par 59) : « Je confirme, je veux Dering 2 partout ». Le chemin HDR `linear`
garde donc `DERING 2`, comme le SDR et `tonemap`. C'est le défaut du produit depuis J37 (ecac6ae2) : rien ne change
dans le code. La question que J48 laissait ouverte est close.

**L'A/B qui l'a tranchée.** La page `hdr-compare.html` de J48 : le vrai `WebGpuRenderer` sur la voie `linear`, les
cinq agrandissements de `hdr_check.py` (texte à 1000 nits, jeu, rampe aux rayures d'un pixel, depuis 720p et 1080p),
A et B tirés au sort.
- **L'écran** : elle a été préparée pour le MacBook Pro M1 Pro de Léo, dont l'écran Liquid Retina XDR est en HDR
  (EDR) sans réglage. Navigateur : Chrome 155 sous macOS 15.6.1. Le Safari 18.6 de ce Mac n'a pas WebGPU.
- **Le service** : une page privée, hors dépôt. Ni le réseau local ni un tunnel n'étaient ouverts depuis DualRTX.
- **L'essai préalable** : sur SwiftShader sans tête, la page donnait un canvas `rgba16float` et le chemin
  `mwsr+yuv`.

**Ce que le choix garde, et ce qu'il accepte.** Les mesures de J48 et J49 restent vraies.
- **Gardé** : les traits d'un pixel gardent leur noir (rampe 1080p, ΔE ITP 14,47 contre 19,93), et la passe du noyau
  reste 3 à 4 % plus rapide (J49).
- **Accepté** : en HDR, `DERING 1` est plus près de la vérité Lanczos sur le texte (ΔE ITP 16,24 contre 20,86), et
  il n'a pas le pic du texte 4:2:0 (×1,15 de l'entrée). Ce pic devient une propriété connue du défaut.

**Vérifié dans le code.** Aucun chemin du produit ne pose `DERING` : `MwsrShader.js` le déclare à 2, et
`setMwsrOverrides` n'est appelé que par les pages du labo. `DERING 1` ne vit plus qu'en option de labo : les cellules
`mwsr:dering=1`, `hdr-compare.html`, `hdr_check.py --ablate` et `--dering`.

## 2026-10-28 — J51, la vitesse de J47 refaite au calme, et la traîne p99 localisée

**La demande.** V10, go de Bruno le 28/10 au matin, par 59 : refaire la vitesse HDR et SDR de J47 sur la RTX qui ne
décode pas, et localiser la traîne p99 que J47 avait laissée sur la réduction HDR à ×0,667.

**Le banc (créneau de 59, 08:47 à 08:59).**
- **La carte** : la RTX 5060 Ti du bus 01 (DISPLAY1), celle de J47.
  - LUID revérifiés le 28/10 par `D3DKMTOpenAdapterFromGdiDisplayName` : DISPLAY1 → 0,79382 (bus 01), DISPLAY5 →
    0,84633 (bus 03), 780M 0,89691.
  - Ni encodeur ni décodeur actif sur la carte pendant tout le banc.
- **Le Chrome** : celui de J47, sans tête, épinglé par `--use-adapter-luid`, l'épinglage vérifié par les compteurs
  WDDM avant toute mesure.
- **Ce qui est nouveau** :
  - la page date chaque tirage. Le `seq` de `hdr-time.json` porte maintenant, pour chacun, la passe 0, le noyau et
    son début ;
  - à côté de la page, deux relevés. nvidia-smi lit la carte toutes les 50 ms (horloges, P-state). Les compteurs WDDM
    lisent, toutes les deux secondes environ, le moteur 3D de chaque processus sur la carte. Pendant les trois passes,
    seuls dwm et Discord y ont dessiné, sous 3,5 %. Pendant la passe ciblée, dwm est monté une fois à 20 %.
- **HDR** : trois passes de 800 tirages par variante, sur les cas de J47.
- **SDR** : les cellules `chain-mwsr` de J47, deux tours ABBA. « Avant » = 87286046.

**HDR : les gains de J47 tiennent.** Médiane des trois passes, total des passes en ms, p50 / p99 ; « après » est
`mwsr`, « avant » `mwsr-enc`.

| cas | après | avant | écart | J47 |
|---|---|---|---|---|
| témoin 720p → 1440p (même chemin) | 0,1216 / 0,123 | 0,1215 / 0,123 | +0,1 % | −1,3 % |
| 2160p → 1440p (×0,667) | 0,3050 / 0,311 | 0,3451 / 0,354 | **−11,6 %** | −11,3 % |
| 2160p → 720p (×1/3) | 0,3231 / 0,333 | 0,3498 / 0,357 | **−7,6 %** | −7,4 % |
| 1440p → 720p (×0,5) | 0,0994 / 0,102 | 0,1124 / 0,114 | **−11,5 %** | −11,2 % |

- Le p99 de ×0,667 est de 0,311 ms, contre 2,914 en J47.
- Sur les 2400 tirages mesurés par variante, un seul dépasse trois fois la médiane (0,97 ms).

**La traîne vient de l'état d'énergie de la carte, pas du shader.**
- **Ce qu'on voit** : au premier passage, les 8 premiers tirages de ×0,667 prennent 2,5 à 3,5 ms (passe 0 1,4 à 2,5,
  noyau 0,7 à 1,2). C'est la grandeur exacte de la traîne de J47.
  - Ces 8 tirages tombent tous en P5, mémoire à 810 MHz.
  - Dès que la carte passe en P3 (7001 MHz), les mêmes tirages prennent 0,31 ms.
- **Pourquoi** : la réduction 4K est bornée par la mémoire. À un huitième de son horloge, elle va neuf fois moins
  vite.
- **Ce que fait la carte** :
  - le banc la charge peu et par à-coups : un tirage, puis l'attente de la relecture des horodatages ;
  - elle reste donc en P3, mémoire à moitié, et redescend en P5 ou P8 dès une pause ;
  - le pilote met environ 0,5 s à remonter.
- **La preuve, sur les deux variantes** : une passe ciblée, `--gap 4000`, avec 4 s d'arrêt avant chaque bloc. Deux
  passes de 160 tirages sur ×0,667. Les deux variantes paient pareil :
  - tirages lents : 28 et 9 pour « après », 47 et 51 pour « avant » ;
  - il y a trois paliers, et l'écart tient à chacun (ms) :

  | mémoire | après | avant |
  |---|---|---|
  | 14 001 MHz (P0) | 0,24 | 0,28 |
  | 7001 MHz (P3) | 0,31 | 0,35 |
  | 810 MHz (P5) | 2,5 à 3,5 | 2,5 à 3,5 |

- **Pourquoi seulement « après » en J47 : non établi.** Il n'y avait pas de relevé d'horloge ce jour-là. La grandeur,
  elle, est celle du P5.
- **Dans le produit** :
  - un stream charge la carte sans pause : décodage, dessin et présentation, à chaque image ;
  - une image en P5 coûterait au pire 3,5 ms, sous les 8,3 ms d'une image à 120 Hz, le temps que le pilote remonte ;
  - rien à changer.
- **Pour les bancs** :
  - les temps absolus de ce banc sont ceux de P3, pas du maximum ;
  - seules les comparaisons par blocs ABBA sont sûres ;
  - un temps absolu doit dire son P-state, ce que `seq` et le relevé nvidia-smi permettent maintenant.

**SDR : pas de régression.** `gpu` p50 des cellules `chain-mwsr`, passes faites entièrement à 7001 MHz.
- **Les écarts** :
  - ×2 : −1,1 % (0,1036 contre 0,1048), et +0,2 % à 60 Hz ;
  - ×1,33 : 0,1154 contre 0,1156, et +0,3 % à 60 Hz ;
  - ×0,67 : −0,1 %, et +0,3 % à 60 Hz.
- **Les passes qui s'écartent ont changé de palier en cours de route** :
  - en partie à 810 MHz : ×1,33 « avant », de 0,17 à 0,22 ; ×1,33 « après », 0,245 une fois ;
  - en partie à 14 001 MHz : ×0,67 « après », 0,12.
- **J47 vérifié** : J47 attribuait son écart de ×1,33 à « une horloge plus basse ». C'est vérifié cette fois.

**L'outillage.**
- `hdr-check.js` : chaque entrée de `seq` porte la passe 0, le noyau et l'heure de début ; `&gap=MS` met la pause
  avant chaque bloc (`hdr_check.py --gap MS`).
- Le banc lui-même (Chrome épinglé, relevés nvidia-smi et WDDM) reste hors dépôt, comme celui de J47.

**Pour rejouer.** Lancer `hdr_check.py --time 800 --cdp PORT` dans un Chrome qu'on a soi-même épinglé et vérifié sur
le GPU, avec nvidia-smi à côté (`-lms 50`). Ajouter `--gap 4000` pour voir les paliers.

## 2026-10-28 — J52, MWSR pour les autres programmes : le jumeau HLSL, et `X2_FLAT` d'un GPU à l'autre

**La demande.** Bruno, le 28/10 : distribuer le shader à d'autres développeurs. Ses choix :
- la licence MIT, pour le seul shader : le reste du dépôt reste sous GPL-3 ;
- un dossier `mwsr/` dans le dépôt ;
- le nom MWSR ;
- les shaders et le module, plus du C++ pour l'intégrer comme dans sa PR de moonlight-qt (`d3d12va_shaders.cpp`).

**Ce que contient `mwsr/`** (cb766889, puis ce commit) :
- `generate.mjs` l'écrit tout entier depuis `MwsrShader.js` ;
- `mwsrDist.test.js` échoue dès qu'un fichier retarde sur le module ;
- les fichiers : WGSL, GLSL ES 3.00, HLSL (Direct3D 11 et 12), `cpp/mwsr.h` (le côté processeur, C++17 sans
  dépendance), `cpp/example_d3d11.cpp` (une intégration complète en un fichier) et un README anglais.

**Le jumeau HLSL (`mwsrHlsl`).**
- **Écrit à partir des mêmes tables que le WGSL**, ligne à ligne. Chaque constante devient une définition `MWSR_<nom>`,
  que le compilateur reçoit comme NIS reçoit les siennes dans moonlight-qt. Les registres aussi sont des définitions.
- **Les dérivées gardent leur signe** : l'axe y de l'écran descend sous Direct3D comme sous WebGPU.
- **Compilé avec FXC** (`ps_5_0`, `vs_5_0`) **et DXC 1.8** (`ps_6_0`, HLSL 2021 ; `ps_6_2` avec les types 16 bits pour
  la variante `min16float`), avec les valeurs par défaut et avec les variantes.
- **Vérifié par les tests du module** : chaque constante avec sa valeur par défaut, les dérivées avant le branchement,
  les mêmes grilles de réduction, aucune constante des noyaux voisins.

**La conformité, sans GPU** : le HLSL sur WARP, par l'exemple D3D11, contre le WGSL sur SwiftShader (la page refuse
tout autre adaptateur). Il y a 16 cas : le jeu 0 A.D., l'étalon de Bruno et une mire de texte, agrandis à ×2 et ×1,33,
réduits à ×0,667, ×1/3 et ×0,21, avec ou sans vues sRGB.
- **Le filtrage bilinéaire seul** (tout en chemin plat, `FLAT_TAPS 1`) : 1/255 au plus. C'est l'écart des deux
  rastériseurs entre eux.
- **Les agrandissements**, par défaut comme avec `FLAT_TAPS 3`, `CHROMA444 1` et `DERING 1` :
  - de 0,0001 à 0,02 % des pixels au-delà de 2/255, pour un PSNR de 66 à 73 dB ;
  - ce sont des quads proches du seuil, que le vote envoie de l'autre côté : les votes ne diffèrent que sur 0,0008 %
    des pixels.
- **Les réductions** : 1/255 au plus.
- **La variante `min16float`** sur WARP : 2/255 au plus de la version f32.
- **`mwsr.h` contre le module** : mêmes modes et mêmes 16 flottants, au bit près, sur 484 combinaisons de tailles.

**`X2_FLAT` n'est pas stable d'un GPU à l'autre.**
- **L'écart** : avec `X2_FLAT 0,5` (une constante de labo, neutre par défaut), 1,1 % des pixels diffèrent de plus de
  8/255.
- **Le mécanisme** : ce n'est pas le jumeau, les deux textes sont identiques.
  - Sur un pixel du chemin plat, le gradient de luma est presque nul.
  - WARP l'arrondit à 0 exactement, SwiftShader en garde une trace (≥ 1e-6).
  - La normalisation de `n` fait de cette trace une direction unité, et `X2_FLAT` affûte le long de cette direction :
    ici à la verticale, en travers des traits d'un pixel.
- **Ce qui a été écarté** : les dérivées « fines » à la place des « grossières » ne changent rien ; `X2_BASE` et
  `X2_R_ACROSS`, seuls, restent dans le bruit des agrandissements.
- **Conséquence** : aucune pour le produit, qui ne pose pas `X2_FLAT`. Le README le déconseille sans mesure sur ses
  cibles.
- **Correctif possible, à la décision de Bruno** : doser l'affûtage du chemin plat par la taille du gradient, au lieu
  de sa seule direction.

**Pour rejouer** (hors dépôt, scratchpad `mwsrdist/`) :
- `conform.py` dessine sur WARP et sur SwiftShader, puis compare ;
- `check.py` compile chaque texte dans deux Chrome SwiftShader (WebGPU, puis WebGL2) ;
- `cppcheck` compare `mwsr.h` au module.

## 2026-10-28 — J53, MWSR face à FSR1, NIS et SGSR1 : le comparatif public, chacun à sa meilleure précision

**La demande.** Bruno, le 28/10 :
- une section du README de `mwsr/` qui compare MWSR à FSR1, NIS et SGSR1 : des zooms, des graphiques de qualité et de
  temps, un tableau de ce que chacun sait faire ;
- une image 1080p → 1440p pour le Discord de Moonlight (hors dépôt).

Ses choix : les images publiques sont 0 A.D. (CC BY-SA 3.0) et la mire de texte du labo ; les temps sont pris sur la
RTX 5060 Ti et le 780M, l'iPhone 13 Pro plus tard. Puis : « Prépare [FSR1 en fp16] avant, pour avoir un comparatif
juste. »

**FSR1 et SGSR1 en 16 bits, dans le labo seulement.**
- `fsr1-half.js` porte le chemin 16 bits d'AMD (`FsrEasuH`, deux échantillons à la fois, et `FsrRcasH`) ;
  `sgsr1-half.js` porte SGSR1 tel que Qualcomm l'écrit (`half` partout, coordonnées en `float`). Les notices MIT et
  BSD-3 suivent.
- Nouvelles cellules : `easuh`, `fsr1h`, `sgsrh`.
- SwiftShader n'a pas `shader-f16`, et Chrome ne propose aucun adaptateur WARP à WebGPU.
  - La logique se vérifie donc abaissée en f32 (`asF32`, cellules `easuh32`, `fsr1h32`, `sgsrh32`). FSR1 tombe à
    1-2/255 de son port fp32 sur 0,004 % des pixels ; SGSR1 l'égale au bit près.
  - Un premier contrôle de SGSR1 était faux : les littéraux à exposant n'étaient pas convertis, le pipeline était
    invalide, et la cellule montrait la sortie de la précédente. Il a été corrigé et refait.
- Sur GPU, chaque texte 16 bits est d'abord comparé à son texte 32 bits, et rien n'est chronométré au-delà de 4/255 sur
  0,1 % des pixels. Résultats : FSR1 0,001-0,006 %, SGSR1 0,004-0,045 %, au pire 42/255 sur quelques pixels, SSIM au
  millième.

**Trois créneaux de 59** (14:12, 14:29, 14:49 ; RTX du bus 01 puis 780M).
- Chrome sans tête épinglé par `--use-adapter-luid`, vérifié par les compteurs WDDM (0x13616 seul ; 0x15e5b, 0 sur
  0x14a99).
- La RTX était en P0 pendant chaque chronométrage, après 1,8-1,9 s de P8 au chargement de la page.
- Bus 03, NVENC, --dev et modes d'écran : pas touchés. « chrome left: 0 » à chaque fois.
- Temps GPU par lots de 100 dessins, 3 tours de 6 lots, médiane par dessin.

| 1080p → 1440p, 0 A.D. | RTX f32 | RTX f16 | 780M f32 | 780M f16 |
|---|---|---|---|---|
| MWSR | 0,116 | 0,133 | 3,91 | 3,80 |
| FSR1 | 0,278 | 0,265 | 9,46 | 6,33 |
| SGSR1 | 0,118 | 0,156 | 4,44 | 2,97 |
| NIS (notre portage) | 0,904 | — | 13,78 | — |
| bilinéaire | 0,029 | — | 0,449 | — |

- Le 16 bits ne gagne rien sur NVIDIA ; il gagne 33 % pour FSR1, 33 % pour SGSR1 et 3 % pour MWSR sur le 780M.
- Le 780M donne les mêmes ordres de grandeur qu'au P0 (FSR1 9,4 ms) et en J49 (MWSR 3,0 ms sur l'étalon). Comme
  son horloge n'est pas fixée, le README invite à lire ses chiffres comme des rapports.
- **SGSR1 en half est le plus rapide sur l'iGPU** (2,97 contre 3,80 ms sur 0 A.D., 1,50 contre 2,85 sur le texte).
  MWSR reste au niveau de SGSR1 sur la RTX, et 1,7 à 2,3 fois moins cher que FSR1.
- NIS est marqué « notre portage » : NVIDIA livre un compute shader à mémoire partagée, plus rapide que notre
  transcription par fragment.

**Qualité** (SSIM de la luma ×100 / dépassement, mêmes chiffres sur la RTX, le 780M et SwiftShader) :

| | jeu ×1,33 | texte ×1,33 | jeu ×2 | texte ×2 |
|---|---|---|---|---|
| MWSR | 85,9 / 0,03 % | 96,0 / 0 % | 66,2 / 0,01 % | 86,1 / 0 % |
| FSR1 | 87,5 / 13,7 % | 94,4 / 0,2 % | 68,0 / 2,9 % | 84,7 / 0,3 % |
| NIS | 82,8 / 42,6 % | 95,6 / 4,4 % | 69,5 / 41,7 % | 87,6 / 7,1 % |
| SGSR1 | 85,6 / 6,5 % | 95,3 / 2,7 % | 66,9 / 6,3 % | 87,6 / 4,4 % |

**Lecture**, telle que le README la donne :
- FSR1 est le plus fidèle sur le jeu, au prix d'un halo ;
- MWSR est le plus fidèle sur le texte à ×1,33, sans aucun halo, et le plus doux à ×2 sur le jeu ;
- SGSR1 est proche de MWSR, et le plus rapide sur l'iGPU ;
- MWSR est seul à réduire et à lire le 4:4:4. Pour le HDR sans tone map, ils sont deux : MWSR et NIS.

**Dans le dépôt** :
- la section « Next to FSR1, NIS and SGSR1 » de `mwsr/README.md` : texte, zooms, deux graphiques clair/sombre, deux
  tableaux, ce que chacun sait faire, la méthode, les marques et la licence de l'image ;
- `mwsr/docs/` : `zoom.png` (les recadrages de 0 A.D., sous CC BY-SA 3.0) et les quatre SVG.

**Après le tag** :
- l'iPhone 13 Pro, par 59 quand Bruno est disponible ;
- la page interactive `moonlightweb.top/mwsr/`, déployée avec le site, avec le temps GPU par lots (le comparateur
  mesure par image, au plancher de 1 ms sur NVIDIA, J37) ;
- un NIS en compute shader, si Bruno veut le comparer à son vrai coût.

## 2026-10-28 — J54, la loi ×2 retirée (A29)

**La décision.** Bruno, le 28/10 au soir, par 59 : « Si on doit faire quelques tests avec le Shader, alors je
souhaiterai en profiter pour supprimer X2 pour la v0.4.1 ». A29, prévue pour la v0.4.2, passe donc avant le tag.

**Ce qui part.**
- Les quatre constantes de la loi ×2 de J39 : `S_X2`, `X2_BASE`, `X2_FLAT` et `X2_R_ACROSS`. Elles étaient
  neutres par défaut, et le produit ne les a jamais réglées.
- Leur code, dans les trois jumeaux (WGSL, GLSL, HLSL) :
  - le chemin plat rend son cubique tel quel ;
  - la paire en travers est à `R_ACROSS` ;
  - la base du mélange et la cohérence lisent le même échantillon bilinéaire (`Ch`) ;
  - la normale de l'arête n'est plus calculée sur le chemin plat.
- Leurs réglages : dans le labo (`mwsr:sx2`, `x2base`, `x2flat`, `x2racross`), dans le comparateur (`x2b`, `x2r`,
  `x2f`, leurs curseurs et leurs liens), et dans le README de `mwsr/` (le tableau des constantes, les deux
  paragraphes qui le suivaient, la valeur conseillée « texte à ×2 », l'exemple 4:4:4).
- L'instabilité de `X2_FLAT` d'un GPU à l'autre, notée en J52, disparaît avec elle.

**Vérifié, sur SwiftShader et WARP seulement** :
- dorés (`--golden-check`) : OK avant et après, au bit près ;
- conformité des jumeaux WebGPU et WebGL2 (`--conformity`, les deux côtés) : OK, FLIP 0,000, au plus 0,05 dB ;
- jumeau HLSL sur WARP contre WGSL sur SwiftShader (`mwsrdist/conform.py`, les 12 cas hors loi ×2) : les mêmes
  chiffres qu'en J52, cas par cas ;
- `node mwsr/generate.mjs --check`, `node scripts/website/build-site.mjs --check` (la page `/mwsr/` copie le
  module), 184 fichiers de tests et 1962 tests verts.

Aucun GPU réel n'a servi : le code retiré ne tournait pas aux valeurs par défaut, et les dorés le montrent au bit
près. À l'œil de Bruno, comme avant, le ×2 (720p → 1440p) sur texte et sur jeu.

## 2026-11-02 — J55, le réglage livré change : une loi mesurée en LPIPS, puis en VMAF NEG

**La demande de Bruno (01/11).** Remplacer les défauts livrés par le meilleur réglage en LPIPS, puis en VMAF NEG, dans une
tolérance de 5 % du gain LPIPS sur le livré, avec une loi par taille du stream et de l'écran, et non un point fixe. Le SSIM
quitte le README et le comparateur. Le test à l'aveugle S/F/P est abandonné : Bruno juge à l'œil, sur le comparateur en ligne.

**La mesure.**
- 15 images de recherche (jeux récents, 0 A.D., interfaces, texte), chacune propre et passée par HEVC au débit d'un stream.
  4 images de contrôle, jamais vues pendant les recherches. L'écran 2160 n'a que 4 images de recherche : il est moins sûr.
- LPIPS AlexNet v0.1, sur la RTX pour la vitesse : même réseau qu'en processeur, écart de 3e-8 au plus sur 276 rendus.
  VMAF NEG par `vmaf_v0.6.1neg`.
- Recherches locales aux échelles ×1,15, ×1,333, ×1,5, ×2, ×2,5 et ×3 à l'écran 1440, et ×1,333 et ×2 aux écrans 1080 et 2160,
  avec les zones de l'hôte natif et sans. Puis des grilles sur `racross`, `walong` et `sharp`, et la mesure de chaque loi candidate
  dans le vrai shader, avec les échelles ×1,4 et ×2,25 tenues à part.

**Ce que disent les mesures.**
- `sharp` et `walong` s'échangent sur une crête : à ×2,5, `sharp` ≈ 1,25 + 2,5 `walong` tient le LPIPS. Une fois sur cette crête,
  **`walong` 0 suffit partout**, sans zones comme avec, à condition de baisser `sharp` aux grandes échelles.
- Le VMAF NEG du texte suit `racross` (à ×1,333 : 0,625 donne environ 57, 0,75 environ 59, 1,0 environ 64), mais le LPIPS moyen s'en
  éloigne. À ×1,333, égaler le livré sur le texte coûte un tiers du gain LPIPS (`sharp` 0,875, `racross` 0,9375 : 67 % du gain,
  62,45). À ×2, le compromis existe : 24,81 avec zones, 25,13 sans, contre 25,27.
- La taille de l'écran ne donne pas d'effet qu'un terme linéaire puisse suivre : l'optimum de `sharp` reste à 1,125 à ×1,333 et ×2
  sur les trois écrans, avec zones, et `racross` varie sans suivre l'écran. Les termes d'écran de l'essai précédent dégradaient
  ces écrans (76 % du gain à ×1,333 sur 1080). Ils restent dans la forme, à zéro.

**La loi.** `value = c0 + c1 x + c2 x² + c3 x³ + c4 x⁴ + c5 y + c6 x y`, x = ln(échelle) tenu dans ×1,15..×3, y = ln(h / 1440) tenu dans
1080..2160. `sharp` et `racross` suivent x, deux tables (avec les zones et sans). `tau` 0,0025, `walong` 0, `ralong` 0, `fc` 0,5, et
`wmin` constante, qui ne mord plus. Le x⁴ est là pour le creux de `racross` vers ×1,5 et la remontée à ×2, qu'un degré 3 ne suit pas
(texte à ×2 : 20,89 au degré 3, 24,81 au degré 4). Les zones sont lues dès ×1,15, où elles gagnent encore.

**Les chiffres**, écran 1440, images de recherche (LPIPS / VMAF NEG ; écart de VMAF NEG au livré ; part du gain LPIPS gardée) :

| Échelle | Avec zones | Sans zones | Livré, zones | Livré, sans | FSR1 |
|---|---|---|---|---|---|
| ×1,15 | 0,02510 / 87,54 ; −0,06 ; 95 % | 0,02541 / 87,63 ; −0,16 ; 97 % | 0,03206 / 87,59 | 0,03145 / 87,79 | 0,02950 / 86,64 |
| ×1,333 | 0,03937 / 83,87 ; +0,21 ; 95 % | 0,04083 / 84,20 ; +1,03 ; 96 % | 0,04808 / 83,65 | 0,05056 / 83,17 | 0,04590 / 84,52 |
| ×1,4 (à part) | 0,04534 / 82,77 ; −0,10 | 0,04722 / 83,11 ; +0,73 | 0,05510 / 82,86 | 0,05716 / 82,37 | 0,05390 / 83,28 |
| ×1,5 | 0,05227 / 81,19 ; −0,27 ; 100 % | 0,05464 / 81,74 ; +0,74 | 0,06464 / 81,47 | 0,06633 / 81,01 | 0,06200 / 81,68 |
| ×2 | 0,10638 / 72,16 ; +0,67 ; 95 % | 0,11086 / 72,80 ; +2,42 ; 95 % | 0,12318 / 71,50 | 0,12993 / 70,38 | 0,12075 / 72,54 |
| ×2,25 (à part) | 0,13256 / 67,97 ; +0,33 | 0,13951 / 68,13 ; +1,68 | 0,14371 / 67,63 | 0,15017 / 66,45 | 0,15026 / 67,86 |
| ×2,5 | 0,15325 / 63,78 ; +0,20 ; 101 % | 0,16156 / 63,56 ; +1,15 | 0,16745 / 63,59 | 0,17309 / 62,41 | 0,18215 / 63,58 |
| ×3 | 0,19283 / 56,30 ; +0,82 ; 96 % | 0,20221 / 54,80 ; +0,51 ; 94 % | 0,20853 / 55,47 | 0,21491 / 54,29 | 0,23548 / 55,13 |

Le texte, en VMAF NEG (avec zones / sans ; livré ; FSR1) : ×1,15 72,27 / 70,92 ; 69,88 ; 64,86. ×1,333 57,52 / 58,87 ; 62,65 ; 57,31.
×1,5 46,39 / 48,72 ; 53,21 ; 48,64. ×2 24,81 / 25,13 ; 25,27 ; 24,60. ×2,5 16,91 / 14,46 ; 13,42 ; 7,67. Son LPIPS bat le livré et FSR1
à toutes ces échelles.

**Hors de la règle, ou en perte.**
- Avec zones, les écrans 1080 et 2160 gardent de 89 à 93 % du gain (1080 : ×1,333 91 %, ×2 90 % ; 2160 : ×1,333 93 %, ×2 89 %). Le
  LPIPS y bat quand même le livré et FSR1. Sans zones à l'écran 1440, ×3 garde 94 %.
- VMAF NEG moyen contre le livré, images de recherche : ×1,5 −0,27, ×1,4 −0,10 et ×1,15 −0,06 avec zones, ×1,15 −0,16 sans.
- Images de contrôle : le LPIPS bat le livré et FSR1 partout. VMAF NEG contre le livré : ×1,15 −0,32 avec zones et −0,25 sans, ×2,5
  −0,20 avec zones, partout ailleurs au moins à égalité. Contre FSR1, il reste le plus souvent derrière (89,75 contre 90,34 à
  ×1,333, 81,24 contre 81,81 à ×2, sans zones).

**Le coût.** Le seuil 0,0025 envoie 78 à 97 % d'une image de jeu sur le chemin de bord, contre 8 à 63 % au livré, et 39 à 47 % du
texte, contre 26 à 35 %. Une première mesure sur la RTX, sur la version précédente de la loi au même seuil, donnait de −24 % à +35 %
selon l'image, bruitée : la carte change d'horloge pendant la mesure (plancher bilinéaire à 0,032 ou 0,049 ms). Aucun téléphone n'a été
mesuré. Les temps du README restent ceux du 28/10 jusqu'à l'optimisation, qui pourra retirer les lectures le long des bords :
`walong` 0 et `ralong` 0 les rendent inutiles.

**Un incident.** Une image de sortie a porté exactement un bit retourné (6,8 Mo, CRC du bloc faux, prouvé en la redessinant), et un
autre PNG avait fait échouer ffmpeg la veille, relu sans erreur au deuxième essai. Aucune erreur matérielle ni disque au journal de
Windows ; barrettes sans ECC, vitesse de base 5600, réglées à 6000. La mémoire vive est la piste principale, sans preuve. Une image
corrompue fait échouer le calcul au lieu de fausser un score, et le balayage reprend depuis son cache.

**Vérifié.**
- dorés réécrits (`--golden`) puis relus (`--golden-check`) sur SwiftShader : OK ;
- conformité des jumeaux WebGPU et WebGL2 (`--conformity`, les deux côtés) : OK, FLIP 0,000 sur le cas relevé ;
- jumeau HLSL sur WARP contre WGSL sur SwiftShader, 5 cas : PSNR de 66 à 69 dB, moins de 0,001 % des pixels au-delà de 1/255, écart
  maximal de 21/255 sur quelques pixels ;
- `node mwsr/generate.mjs --check`, `node scripts/website/build-site.mjs --check`, 197 fichiers de tests et 2090 tests verts ;
- `website/mwsr/lab-scores.json` : 75 cas (`cmp_lab.py`), figures du README par `mwsr_charts.py`, zooms redessinés sur la RTX.

**Une relecture de ce changement** a trouvé un piège réel : avec `walong` 0, le mélange divise par 1 + 2 fois le poids en travers, et un `wmin` nommé à −0,5 avec une netteté de 1,67 ou plus annulait ce diviseur. La loi n'y va pas ; `mwsrUniforms()` et le C++ tiennent maintenant un `wmin` nommé au-dessus de −0,49 − `walong`. Les lignes `j11*` de `hdr-check.js` nomment aussi toutes les valeurs de J11, que la loi aurait remplacées, et le curseur τ du labo descend jusqu'à la valeur de la loi.

**Non fait, à la décision de Bruno.** L'activation par défaut des zones (elles restent derrière la clé de labo), le coup d'œil sur le
comparateur en ligne, l'optimisation du shader : zéro perte mesurée d'abord, puis les chiffres de ce que 2 % de perte apporteraient.

## 2026-11-02 — J56, le shader allégé : zéro perte, puis une seule lecture sur le chemin plat

**La demande de Bruno (02/11).** « Optimisation sans régression » : d'abord ce qui ne perd rien de mesurable, ensuite les chiffres de ce
que 2 % de perte apporteraient. Mesures de temps sur la RTX du bus 3 et l'iGPU du 9900X, dans des créneaux donnés par 59 ; la qualité
sur SwiftShader, 4 cœurs en priorité basse, LPIPS sur la RTX, contre la loi de J55.

**Ce qui part sans perte (`efd72cbb`).** Avec `ralong` 0, les deux échantillons le long des bords sont sur le centre, avec un poids nul.
Et la cohérence vaut S / (S + EPS), car max(|p + q|, |p − q|) = |p| + |q| : elle n'est plus qu'un garde. Les trois jumeaux perdent donc
deux échantillons, deux produits scalaires et une division par pixel de bord. Le poids en travers et le diviseur du mélange deviennent
des valeurs du bloc, calculées une fois par image (le bloc passe de 20 à 24 flottants) ; la zone du regard et les zones de l'hôte les
recalculent par pixel. Le vote prend la dérivée de la luma seule hors `CHROMA444`, la taille de l'entrée vient du bloc, la réduction à
un échantillon perd son `span`. Au passage, la borne suit l'empreinte exacte aux bords de l'image.

Sur tout le corpus (19 images, 12 géométries, avec et sans zones) : LPIPS moyen de −0,000009 à +0,000006, VMAF NEG de −0,0015 à +0,0029 ;
pire image +0,00005 et −0,0047. De 0,04 à 0,34 % des pixels changent, de 1/255, sauf quelques bascules du vote (7/255 au plus) et les bords
de l'image (28/255 au plus). Aucune réduction ne change dans les dorés. WebGL2 et la f16 avaient un garde 100 fois plus grand, qui leur
coûtait environ 1 % de LPIPS sur les écrans 1080 et 1440 : ils rejoignent la loi mesurée. En WebGPU contre la v0.4.1 : RTX −30 à −33 %,
iGPU −37 % ; avec les zones −31 % et −35 %.

**Essayé, puis écarté.**
- `textureGather` : RTX −29 % contre −33 %, iGPU −39 % contre −37 % : aucun gain net.
- `s12 = C` dans le cubique : +0,00009 de LPIPS à ×2 : pas zéro perte.
- `DERING 0` : +35 % de LPIPS à ×1,333.
- Une borne sans lecture de texel (`DERING 3`, `ridge` 2) : ×1,333 LPIPS −1,8 à −2,7 % mais VMAF NEG −0,37 à −0,52, ×2 LPIPS +1 à +2,1 % et
  VMAF NEG −0,32 à −0,47 ; RTX −41 %, iGPU −36 %, soit rien de gagné sur l'iGPU, et des traits qui dépassent leurs texels.
- Une sortie rapide du chemin plat (`FLAT_EARLY` 0,002 : une lecture du texel le plus proche, et C rendu s'il en est tout près) : qualité
  tenue (LPIPS −0,000027 à +0,000013), iGPU −48 %, mais re9-street ne passe que de 2,14 à 2,01 ms.

**Ce que cette sortie rapide a appris.** Un GPU exécute les pixels par paquets, et un paquet paie un chemin dès qu'un seul de ses pixels le
prend (TexBench le mesure : écarter un pixel sur deux ne gagne rien). Éviter le cubique à la plupart des pixels gagne donc peu. Il faut
retirer ses échantillons.

**Le chemin plat à une lecture (`FLAT_TAPS 2`).** Ce que le cubique faisait d'utile : à ×2 exact, chaque quad est centré sur un texel, donc
un trait d'un texel a une pente nulle et passe par le chemin plat, où le cubique garde sa profondeur. Une lecture y suffit, le texel n le
plus proche du pixel :

    R = C + k (n − C),    k = (1 − f) (1 − 1,5 f),    f = la plus grande distance par axe au centre de ce texel

Sur un tel trait, à la distance f de son centre, le Catmull-Rom donne h(f) L, avec h(f) = 1,5 f³ − 2,5 f² + 1, là où C vaut (1 − f) L, et n
vaut L : k = (h(f) − (1 − f)) / f. R reste entre C et le texel, donc rien ne sonne, et sur une cellule vraiment plate n vaut C. Un point
isolé relevé sur le texte, vu en diagonale, sur fond blanc (texel 227 en rouge) : le cubique à cinq échantillons, sans ses coins, donne 254 ;
celui-ci 239, près du Catmull-Rom complet (240).

Sur tout le corpus, contre la loi : images de recherche, LPIPS moyen de −0,29 à +0,01 %, VMAF NEG de −0,0087 à +0,0026, pire image +0,00018
(re9-street ×2 hevc) et −0,117 (texte ×2, écran 1080). À ×2 sur l'écran 1440, le LPIPS s'améliore : 0,10638 → 0,10610 avec zones, 0,11086 →
0,11055 sans. Images de contrôle, de −0,09 à +0,07 %, VMAF NEG de −0,0038 à +0,0041, pire image +0,00058 (re9-grace ×2 hevc) et −0,011.
Contre le chemin à cinq échantillons, sur 4 images, 0,6 à 3 % des pixels changent, presque tous de 1 ou 2 niveaux ; moins de 0,05 % bougent
de 4 niveaux ou plus (33 au maximum), des points isolés et de petits détails de texte, qui se rapprochent du pixel source.

**Les temps (créneau du 02/11, 09:19-09:24 ; 0 A.D., texte, re9-street ; 1080 et 720 vers 1440 ; même page, blocs entrelacés), contre la v0.4.1.**
- WebGPU, RTX : −48,1 % (−26 à −62), contre −29,6 % au chemin à cinq échantillons ; iGPU : −57,1 % (−44 à −66), contre −37,2 %.
- Avec les zones : −42,4 % et −54,3 %.
- Face à FSR1 à sa meilleure précision : 4,82 fois plus rapide sur la RTX (3,5 à 6,5), 4,99 fois sur l'iGPU (4,3 à 5,9) ; avec zones 4,35 et
  4,69 fois. Le chemin à cinq échantillons était à 3,64 et 3,58 fois, la v0.4.1 à 2,43 et 2,11.
- WebGL2, la passe seule, v0.4.1 → cinq échantillons → une lecture : iGPU texte 1,590 → 1,124 → 0,483 ms, re9-street 1,501 → 1,263 → 0,768 ms ;
  RTX re9-street 0,0596 → 0,0452 → 0,0252 ms ; RTX texte 0,0704 → 0,0445 ms puis trop de bruit pour conclure. Image finie sur l'iGPU : texte 4,28
  → 3,83 → 3,44 ms, re9-street 3,71 → 3,46 → 2,82 ms.
- La RTX reste bruitée (horloge bimodale, J51) ; l'iGPU stable (0,2 % entre passes).

**Une erreur de banc, corrigée.** Le premier temps WebGL2 de ce chantier (créneau de 06:52, −16 à −44 %) était faux : les copies de labo
gardaient le `WebGlRenderer.js` de la loi, qui ne pose pas les nouvelles valeurs du bloc, et le shader dessinait avec elles à 0 (0,47 et 13 dB
contre la même image en WebGPU). WebGPU prend le bloc entier et n'est pas touché, ni les mesures de qualité. Avec le renderer du dépôt : 32 et 48 dB,
au lieu de 25 et 36 dB pour le texte d'avant : l'écart de dérivées entre WebGL2 et WebGPU, déjà connu, se réduit.

**Monter le seuil du vote, ou retirer aussi cette lecture.** Les chiffres des 2 %, contre −48,1 % et −57,1 % à `tau` 0,0025 :
- `tau` 0,005 : RTX −50,5 %, iGPU −57,7 %, LPIPS au pire +0,32 % ;
- `tau` 0,0075 : −51,7 %, −58,5 %, au pire +0,92 % ;
- `tau` 0,015 : −54,7 %, −60,0 %, au pire +3,19 %, hors de la règle. Le gain vient surtout de re9-street (iGPU 1,42 → 1,16 ms), et 0 A.D. ralentit
  (1,27 → 1,41 ms), sans doute parce que ses paquets paient alors les deux chemins.
- Aucune lecture sur le chemin plat (`FLAT_TAPS 1`) : −54,5 % et −62,1 %, mais un trait d'un texel s'y affadit à ×2 ; pas mesuré sur tout le corpus.

**Les décisions de Bruno (02/11).** Le chemin à une lecture par défaut, partout, téléphones et tablettes compris. Le seuil reste à 0,0025. Les
chemins à 3, 5 et 9 échantillons et `fc` restent jusqu'à la lecture sur téléphone et 780M : le comparateur en a besoin pour comparer les temps. La
f16 reste pour l'instant : sur l'iGPU elle ne gagne plus rien (−56,0 % contre −57,1 %), elle attend la 780M et l'iPhone.

**Sa question sur la Mi TV.** Elle n'a pas d'adaptateur WebGPU (relevé le 06/10) : la f16 ne s'y teste pas. Son WebGL2 peut comparer le mélange en
`mediump`, le jumeau de la f16, à `highp`, et les chemins entre eux, sur une puce où les écarts se voient : à faire sur un créneau de 59.

**Vérifié.** Dorés réécrits puis relus, conformité des jumeaux WebGPU et WebGL2, jumeau HLSL sur WARP, cellules du labo, `node mwsr/generate.mjs --check`,
`node scripts/website/build-site.mjs --check`, 200 fichiers de tests et 2211 tests verts. Le texte appliqué rend les mêmes pixels que la copie de labo mesurée, sur 6 cas, avec et sans zones. Aucune réduction ne change dans les dorés. Le contrôle HDR garde les cinq alertes de la loi commitée, aux mêmes valeurs à 0,005 près, et n'en ajoute aucune.

**Les temps définitifs (02/11, 10:37-10:41, le texte de ce commit, même page).** RTX : 0 A.D., texte, re9-street et gta-city ; iGPU : les trois
premiers. Contre la v0.4.1 : RTX −47,1 % (−37 à −59), iGPU −57,0 % (−45 à −66) ; avec les zones −40,2 % et −54,2 %. FSR1, à sa meilleure précision, coûte
4,79 fois MWSR sur la RTX (4,3 à 5,1) et 4,98 fois sur l'iGPU (4,3 à 5,9) ; avec les zones 4,23 et 4,68 fois ; la v0.4.1 était à 2,54 et 2,11 fois. MWSR
devient le plus rapide des quatre sur ces deux GPU. La RTX était à P0 d'un bout à l'autre, et la mesure confirme celle de 09:19.

Le README, la page et le graphique prennent ces temps. La colonne de la Radeon 780M reste celle du 30/10 et est dite ancienne : l'UM790Pro sert au banc de
sécurité. Les scores (`cmp_lab.py`, 75 cas) sont refaits sur ce texte, les figures par `mwsr_charts.py`, et la colonne MWSR des zooms est redessinée sur la RTX.

**La Radeon 780M (02/11, 12:44-12:48, à la demande de Bruno).** Un Chrome lancé par SSH tourne en session 0 : il voit la carte, mais `requestAdapter` rend null,
comme sur mw-arm le 22/10. Sur l'accord de Bruno, une tâche `Interactive` l'a lancé dans la session console de Minis, sans fenêtre, muet, sur un profil à part, et
WebGPU y répond. Six images, deux passes de 3 tours de 6 lots de 100 dessins, `lab-time` identique au dépôt, 2,4 % d'écart entre passes au pire, aucun autre programme
au-dessus de 2 % du moteur 3D. Contre la v0.4.1 : −20,3 % (−14,5 à −31,5), avec les zones −18,0 %. L'écart avec la RTX et l'iGPU vient du plancher : une bilinéaire seule
y prend 0,21 à 0,32 ms, et ce que le shader ajoute au-dessus baisse de 72 % (60 à 90), contre 68 % et 66 %. FSR1 à sa meilleure précision 3,21 fois MWSR (2,65 à 3,54),
SGSR1 1,21 fois (1,08 à 1,39), NIS 4,68 fois (4,03 à 5,06) : MWSR est le plus rapide des quatre sur les six images. Le chemin à une lecture contre celui à 5 : −6,4 %
(−0,7 à −10,9). La f16 n'apporte rien : +1,0 % (−0,2 à +1,6). Chaque texte 16 bits est tenu à son texte 32 bits sur cette carte : MWSR à 1/255 au plus, FSR1 et SGSR1 à
0,045 % de pixels au pire au-delà de 4/255. La colonne du 30/10 venait d'Ubuntu, et ses valeurs ne se comparent pas à celles-ci : la bilinéaire y valait 0,110 ms, contre
0,322 ms ici. Le README, la page et le graphique prennent cette colonne, et la réserve sur la précision de cette carte disparaît.

**Reste.** Les téléphones (Bruno lit le temps sur le comparateur en ligne, après le signal de 59), la Mi TV ; puis le retrait des anciens chemins et
la décision sur la f16.

## 2026-11-03 — J57, le noyau change : J, sa loi réglée à l'œil de Bruno

**La décision de Bruno (03/11).** Après ses comparaisons de la journée, il adopte J, le noyau rapide des recherches de la nuit : un cubique lu en cinq échantillons bilinéaires, le détail mesuré en travers du bord, et une borne à marge sur la plage de la lecture du milieu et de la paire en travers. Ses repères : le niveau C à ×1,33 (force 1 de la loi), le niveau D à ×2 (force 1,35), tous deux sur `kg` et `kl`. À ×1,5, six images sur six en C (force 1,10), qui est exactement la droite en ln(ratio) entre les deux. Limite : cette page-là n'était pas à l'aveugle et C y était la valeur centrale de cinq forces ; cela soutient la courbe sans la prouver. L'essai à l'aveugle J contre FSR1 de la journée donne J 11, FSR1 0, aucune différence 1 sur 12 séries, J étant seul réglé à son œil et sur ces mêmes images.

**Le texte.** Sept lectures sur un bord, une seule sur un quad plat. La loi (`MWSR_LAW`) est le polynôme de degré 2 en ln(ratio) de `kc`, `kg`, `kr`, `kl` ajusté de ×1,07 à ×4 sur les recherches de la nuit, la force par-dessus, `ft` 0,005 et `kwc` 0,1 constants. Plus de terme de surface, et aucune montée vers la 4K mesurée avec cette loi. Au-delà de ×2 la force reste à 1,35 : rien de plus haut n'a été vu. `DERING`, `FLAT_TAPS`, `FLAT_ZONE`, `fc`, `wmin`, `sharp`, `tau` et `racross` quittent le bloc : la borne et le chemin plat sont ceux de J. Le bloc reste de 24 flottants. La zone du regard suit : son `kg` au centre est un multiple de la loi (`boost` 1,5), une valeur que ni l'œil ni un score n'ont vue sur ce noyau, derrière sa clé de labo.

**Vérifié, en logiciel (cœurs 16-21, priorité basse) :**
- le texte du produit contre celui du labo jugé par Bruno, aux mêmes valeurs : 6 à 15 pixels sur 3,7 millions, 1/255 au plus, sur 9 cas ;
- WebGL2 : conformité `--conformity` OK ;
- HLSL sur WARP contre WGSL sur SwiftShader, 9 cas : 60 à 67 dB, 0,01 à 0,07 % de pixels au-delà de 1/255 (des quads près du seuil basculent) ; FXC et DXC compilent ; `mwsr.h` donne les flottants du module au bit près sur 6 tailles ;
- dorés réécrits puis relus (`--golden-check`).

**HDR.** La marge de la borne fabrique des hautes lumières : du texte à 1000 nits monte vers 1400, soit 1,08 à 1,36 fois le pic d'entrée, et le dE ITP sur le texte et le jeu à ×2 passe au-dessus de la simple copie (`hdr_check.py`). Limiter la marge au-dessous du blanc supprime le pic mais pas le dE. Sans marge (`kl` 0, option `hdr` de `mwsrUniforms`, posée par `WebGpuRenderer` sur le chemin `linear`), tous les contrôles passent. Bruno n'a pas vu J en HDR. Le chemin `tonemap` reste le noyau SDR, sa marge voulue : le contrôle des texels n'y est plus que rapporté.

**Sous ×1 et à ×1.** Le noyau ne s'y applique pas : ×1 est une copie, et jusqu'à ×0,5 la réduction est un échantillon bilinéaire en lumière linéaire, sans netteté. FSR1 n'est pas fait pour réduire ; il tourne tel quel. Moyennes sur 148 cas, images à 2160 ou 1440 selon le ratio (LPIPS, VMAF NEG, PSNR), jeux après HEVC, MWSR puis FSR1 : ×0,5 .0075 92,32 43,17 ; .0210 93,20 39,24. ×0,67 .0077 92,73 44,44 ; .0231 93,75 40,69. ×0,75 .0171 89,00 40,05 ; .0234 91,19 38,06. ×1 (copie) .0133 95,77 41,53 ; .0254 93,10 38,30. MWSR gagne partout sauf en VMAF NEG sous ×1. Contrôle : 30 cellules redessinées, 0 écart.

**La vitesse (`final_time.py`, créneau de 59, 03/11 16:39-16:53, bus 3 et iGPU, ms par image).** Sur la RTX, J 0,08 à 0,11, FSR1 0,29 à 0,39 (2,7 à 4 fois J). Sur l'iGPU en f16, J 1,34 à 2,01, FSR1 6,24 à 6,34 (3,2 à 4,7 fois). Le noyau d'avant J valait 0,063 et 1,29 sur 0 A.D. à ×1,33 ; J y vaut 0,085 et 2,01 : J coûte plus cher, c'est le prix de la netteté retenue.

**L'optimisation, sans perte.** La forme du texte vient des recherches de la nuit (`fa=1;fc=1;fb=1`). Trois essais de plus :
- la f16 hors NVIDIA reste : −14 à −15 % sur l'iGPU pour 2/255 au plus ;
- le calcul du cubique écrit autrement : −1,5 à −2 % sur la RTX, −1 % sur l'iGPU en f32, mais +1,6 % en f16 : gardé tel quel ;
- un seuil de vote plus haut : à 0,0075, 9 % de pixels en moins sur le chemin à sept lectures, mais une image perd jusqu'à 4,8 % de LPIPS : refusé.

**Zones contre loi sur J final (60 cas par ratio).** À ×1,33 sur les jeux : LPIPS −3,3 %, NEG +0,31, PSNR +0,25 dB ; à ×2 : −7,7 %, +1,28, +0,58 dB. Sur les pages rien ne bouge (+0,03 % au plus). NEG et PSNR ne passent jamais sous la loi ; le pire LPIPS est +2,81 %. Après HEVC le gain moyen est plus faible (−1,6 % et −4,8 %). Tout vient du labo, images figées : l'arbre a été réglé sur le noyau d'avant, et le passage de sa baisse à J (`zf`, sur `kg` et `kwc`) est nouveau. Coût de la passe hôte sur la RTX : 45, 64 et 94 µs en rafale de 1080p à 4K ; 80, 215 et 1359 µs à 60 Hz sur un bureau calme. Les zones restent coupées, derrière leur clé de labo.

**Reste.** La colonne de la Radeon 780M, le tableau du site et du README sur ce texte, l'essai en vrai stream, les téléphones.

## 2026-11-03 — J58, la réduction devient nette : la Mixte, au choix de Bruno

**La décision de Bruno.** Sous ×1, MWSR ne faisait qu'une moyenne en lumière linéaire. Trois tours de comparateur, sur pages privées hors dépôt, noms affichés, un seul observateur, écran M27Q. Premier tour, 18 images : l'actuelle n'est choisie sur aucune, une version nette sur 12, FSR1 sur 4, une stricte sur 2. Deuxième tour, 6 images : une version moyenne sur 5, la nette sur 1. Troisième tour, 11 images : la Mixte sur les 11 (58 vues relues sur le canevas, 0 pixel différent, 0 empreinte fausse), « J'ai refait le test, je préfère Mixte. »

**Le texte.** Le vote des dérivées grossières de la luma, puis, où il voit du détail, une croix de 4 lectures à un pixel de sortie donne le détail et la borne. Le gain et la marge suivent l'étendue locale en racine carrée (`smoothstep(0.2, 0.5, ...)`) : plus doux sur un bord franc. `MWSR_DOWN_LAW` : à ×0,5, gain ,25 vers ,05, marge ,1 vers 0 ; de ×2/3 à ×0,93, gain ,45 vers ,2, marge ,25 vers ,1 ; une droite en ratio entre ×0,5 et ×2/3 ; le gain s'éteint de ×0,93 à 1:1 ; aucune netteté sous ×0,5 ; en HDR, gain 0 et les pixels d'avant. Soit 5 lectures à travers les vues sRGB, 8 avec la grille 2×2 de la lumière logicielle. Les grilles 3×3 et 4×4 ne changent pas. Les textes de l'agrandissement non plus : WGSL et HLSL au bit près, GLSL un commentaire.

**Vérifié, en logiciel (cœurs 16-21, priorité basse) :**
- le produit contre ce qu'il a vu : 0 pixel sur ses 11 images ; 0 d'écart de score à ×0,75, ×0,67, ×0,5, ×0,6, ×0,85 et ×0,9 ; à ×0,95, 0,01 au plus, l'arrondi du paramètre du labo ;
- HLSL sur WARP contre WGSL sur SwiftShader, 9 images : 0,04 % de pixels au-delà de 1/255 au plus, 58/255 au plus sur du texte ; `mwsr.h` donne les flottants du module au bit près sur 13 tailles ;
- WebGL2 : `--conformity` refusait la réduction (35/255 au plus, 0,6 % de `code`). Le vote retiré des deux textes, les deux images sont identiques au bit près sur les trois cas : l'écart est le seul vote, GLSL ES n'a pas de dérivée grossière. Le contrôle tient désormais ce couple par un plancher de 45 dB (51 dB au pire ce jour-là), et non plus par l'écart des bilinéaires ;
- dorés réécrits puis relus : 12 différences avant réécriture, toutes des réductions de 2560×1440 à 1280×720 (`mwsr`, `mwsr:srgb=1`, `mwsr:ll=2`, `chain-mwsr`) ; `taps=9`, `tex-s1` et les agrandissements intacts.

Sur carte, l'image relue contre SwiftShader, qui dessinait ce que Bruno a vu : RTX à travers les vues sRGB, 3 % de pixels au-delà de 1/255 et 0,19 % au-delà de 4/255 sur le texte à ×0,75 (17/255 au plus), 0,11 % et 48/255 au plus sur le jeu ; iGPU, 0,06 % au plus, 40/255 au plus. Personne ne l'a regardée à l'œil sur ces cartes.

**Les scores, après HEVC, contre Lanczos-3 en lumière linéaire (LPIPS, VMAF NEG, dépassement en %) ; avant → Mixte (FSR1).**
- ×0,75 jeux, 24 cas : ,0171 → ,0113 (,0234) ; 89,00 → 93,38 (91,19) ; 0 → 2,30 (4,84).
- ×0,75 texte, 6 cas : ,0179 → ,0141 (,0290) ; 71,97 → 82,77 (72,96) ; 0 → 2,83 (2,12).
- ×0,67 jeux, 11 cas : ,0077 → ,0104 (,0231) ; 92,73 → 94,48 (93,75) ; 0 → 1,56 (3,47).
- ×0,67 texte, 3 cas : ,0108 → ,0093 (,0260) ; 69,71 → 87,97 (69,41) ; 0 → 2,00 (,15).
- ×0,5 jeux, 11 cas : ,0075 → ,0058 (,0210) ; 92,32 → 94,49 (93,20) ; 0 → ,57 (3,58).
- ×0,5 texte, 3 cas : ,0138 → ,0157 (,0266) ; 82,15 → 82,69 (71,06) ; 0 → ,11 (,10).

La Mixte perd donc en LPIPS sur les jeux à ×0,67 (11 sur 11, +51,7 % au pire) et sur le texte à ×0,5 (3 sur 3, +15,9 % au pire) : c'est l'œil qui a tranché, et la référence favorise les filtres proches de Lanczos-3.

Entre les ratios, valeurs jamais vues à l'œil, Bruno n'en demandant plus le contrôle : ×0,6 jeux ,0073 → ,0102, 92,29 → 93,88, texte ,0220 → ,0219, 70,72 → 74,67 ; ×0,85 jeux ,0203 → ,0098, 87,43 → 93,08, texte ,0186 → ,0137, 60,43 → 76,48 ; ×0,9 jeux ,0229 → ,0106, 87,16 → 93,15, texte ,0190 → ,0132, 59,54 → 77,53 ; ×0,95 jeux ,0255 → ,0137, 86,78 → 91,79, texte ,0204 → ,0153, 58,60 → 72,16 (FSR1 80,06 : il la dépasse là).

**La lumière logicielle, jamais vue à l'œil.** C'est le chemin de WebGL2, donc d'Auto sur un ordinateur. Sa Mixte s'écarte de ce que Bruno a vu : jusqu'à 21 % de pixels au-delà de 1/255 et 18 % au-delà de 4/255 sur du texte à ×0,75, 89/255 au plus. En scores, après HEVC, avant → Mixte sur ce chemin : ×0,75 jeux ,0269 → ,0093, 87,60 → 92,91 ; texte ,0301 → ,0168, 78,59 → 82,32 ; ×0,67 jeux ,0128 → ,0046, 91,07 → 95,14 ; texte ,0220 → ,0119, 78,09 → 84,91 ; ×0,5 jeux ,0076 → ,0067, 92,35 → 93,06 ; texte ,0131 → ,0142, 85,46 → 82,24, moins bon. Le FLIP du texte y reste au-dessus du chemin sRGB : ,0519 contre ,0372 à ×0,75.

**La vitesse (`time_final2.py`, créneau de 59, 03/11 23:42-23:48, bus 3 et iGPU ; 780M 23:50-23:53 ; ms par image, jeu/texte).**
- RTX, vues sRGB : ×0,75 ,033/,023 ; ×0,67 ,063/,040 ; ×0,5 ,035/,024. Lumière logicielle : ,047/,036 ; ,086/,065 ; ,046/,033. FSR1 : ,149/,144 ; ,278/,264 ; ,152/,145.
- iGPU, vues sRGB : 1,045/,648 ; 1,914/1,092 ; 1,073/,652. Lumière logicielle : 1,494/1,051 ; 2,683/1,809 ; 1,524/1,037. FSR1 en f16 : 3,561/3,525 ; 6,326/6,254 ; 3,572/3,546.
- 780M, vues sRGB : ,255/,215 ; ,442/,371 ; ,347/,287. Lumière logicielle : ,260/,227 ; ,452/,386 ; ,355/,293. FSR1 en f16 : ,546/,518 ; ,919/,866 ; ,601/,537. Horloge non fixée : à lire en rapports.
- WebGL2, GPU seul : RTX ,078/,068 ; ,185/,163 ; ,115/,103 (FSR1 ,162/,160 ; ,309/,300 ; ,199/,195). iGPU 1,675/1,232 ; 3,127/2,251 ; 2,177/1,636 (FSR1 4,870/4,788 ; 8,792/8,580 ; 5,394/5,194).

La copie simple reste plus rapide (RTX ,016 à ,029, iGPU ,258 à ,523). Une forme moins chère de l'étendue (`sqrt(dot(mx, LUMA)) - sqrt(dot(mn, LUMA))`) gagne 2 à 3,5 % sur la RTX et 3,4 à 6 % sur l'iGPU, mais change des pixels (25/255 au plus, 0,02 % au-delà de 2/255) : refusée, Bruno a choisi les autres.

**Zones contre loi sur J final, plage centrée (0,64 à 1,15 fois la loi), après HEVC.** Jeux à ×1,33 : LPIPS -1,46 %, pire +7,0 %, NEG -0,26, PSNR -0,20 dB, dépassement 2,47 → 3,08. Jeux à ×2 : -1,72 %, pire +5,5 %, NEG +0,07, PSNR +0,04. Pages : -2,92 % et -3,30 %, NEG +0,32 et +0,27. Une plage plus large (force de 0,87 à 1,55) va plus mal à ×1,33 : +3,01 %, pire +55,2 %, NEG -1,71. Bruno, à l'œil : à ×1,33 la loi sur 3 images, la plage large sur 3, la centrée sur aucune ; à ×2 la loi sur 6 images sur 6. Les seuils fixés d'avance ne sont pas tenus : pas de section au site, aucune activation, pas de code de plus. Le code actuel reste dans le produit, coupé. Le coût côté client, le pompage et le vrai stream n'ont pas été mesurés pour cette plage.

**Limites.** Le choix de la Mixte est une préférence d'un observateur sur des images fixes, noms affichés. Les ratios intermédiaires, le HDR à gain nul et la lumière logicielle n'ont pas été vus à l'œil. Les téléphones ne sont pas mesurés.

**La page et le README, sur ce texte.** Quatre tableaux à ×1,33 et ×2, jeu et texte, six scores, dépassement et trois GPU, en `mwsr/README.md` et dans « Measured on three GPUs ». La page calcule désormais ses six scores dans l'onglet, chacun dès qu'il est prêt, sans serveur : LPIPS sur le GPU de la page (9,9 Mo de poids chargés par elle), VMAF NEG, PSNR, SSIM, MS-SSIM et FLIP en JavaScript. Coût mesuré : LPIPS 0,34 à 0,40 s par image sur la RTX, 6,1 à 7,3 s sur l'iGPU, 0,85 à 0,95 s sur la 780M ; VMAF NEG 0,6 à 0,9 s ; PSNR, SSIM et MS-SSIM 0,8 s ; FLIP 0,85 s. Écart au labo : LPIPS moins de 4e-8 sur la RTX et l'iGPU sur 6 images, 3,4e-8 sur la 780M sur 6 images ; VMAF NEG 1e-4 au plus sur 6 ; FLIP 2e-7 au plus sur 9. Sur SwiftShader, de bout en bout, la page rend les valeurs de `lab-scores.json` à leur précision d'affichage sur les cas contrôlés (×1,33 texte, ×0,75 0 A.D., ×0,5 Junk Shop), et l'HDR ne garde que le dépassement. `lab-scores.json` passe à côté de `cmp_lab.py` : il ne sert plus qu'au README et au contrôle. Plus de plan de mesure au choix : 3 tours de 6 lots de 100 images. Refaits les 75 cas : hors réduction de MWSR, aucun score ne bouge. La FAQ disait « sans halo » dans les quatre langues : « peu de halo », puisqu'un petit dépassement est laissé exprès. La figure de zoom et la carte de partage sont redessinées, la colonne de MWSR sur la RTX du bus 3.

**Reste.** Les téléphones, le test mémoire de DualRTX, puis le redémarrage prévu.

**La réduction à une lecture sur WebKit (04/11).** Sur un iPhone 13 Pro (iOS 26.5, Safari et Chrome), les réductions WebGPU par `mwsr-down-1` ne dessinaient rien : 0 ms de GPU. WebKit refuse un pipeline à qui l'on donne une constante que son texte ne lit pas (« Fragment library could not be created »). Ce texte ne lit plus `R_DOWN` depuis `efd72cbb` (02/11), et le renderer, le moteur de la page et le labo la passaient encore.

Essais sur Safari. Refusés : le texte à une lecture avec `R_DOWN`, sur une cible avec ou sans `-srgb`, et l'ancien texte réduit à son retour. Acceptés : le texte à une lecture sans `R_DOWN`, ou avec `_ = R_DOWN;` dans son corps. Les textes à 4, 9 et 16 lectures et le noyau en f16 compilent. WebGL2 n'est pas touché : ses constantes sont écrites dans le GLSL.

Rien ne prenait le relais : `submit` et `onSubmittedWorkDone` réussissent, `failed` reste faux. D'après le code, l'image restait noire ou figée ; personne ne l'a vu en stream. Un iPhone en Auto y passait dès qu'un hôte 1440p ou 4K se réduisait à son écran. La v0.4.1 n'a pas ce défaut.

`mwsrDownConstants(reg)` rend les constantes que le texte lit, et un test refuse le couple. Aucun texte de shader ne change, et le contrôle doré passe sur SwiftShader. Sur l'iPhone, Safari et Chrome dessinent désormais l'image, vue à l'œil dans les deux, sans erreur, avec du travail GPU sur chaque réduction essayée.

**La Radeon 780M à horloge tenue (04/11, 06:06-06:13).** Les temps du 03/11 tournaient à l'horloge libre et ne valaient que comme rapports. Cette fois `ID3D12Device::SetStablePowerState` tient l'horloge ; il demande le mode développeur de Windows, posé par Bruno pour la mesure, retiré ensuite et vérifié absent. Relevé ADL : 135 lectures sur 135 à 938 MHz pour le cœur, 2800 pour la mémoire, 1028 pour le SoC, charge jusqu'à 98 %, 35 W au plus. Laissée à elle-même, la puce va de 800 à 2800 MHz : 938 MHz n'est pas la fréquence d'usage.

En ms par image, 0 A.D. et texte, MWSR f32/f16 puis FSR1 f32/f16 : ×1,33 jeu ,610/,656/2,18/1,99 ; ×1,33 texte ,418/,449/2,16/1,97 ; ×2 jeu ,594/,647/2,16/1,98 ; ×2 texte ,408/,443/2,16/1,97. Réductions par vues sRGB, MWSR/FSR1 f16 : ×0,75 jeu ,314/1,14 ; ×0,75 texte ,251/1,14 ; ×0,67 jeu ,558/1,99 ; ×0,67 texte ,432/1,98 ; ×0,5 jeu ,371/1,16 ; ×0,5 texte ,303/1,16.

Cellules seules, 1080p vers 1440p, horloge tenue contre libre, jeu : `tex-s1` ×1,01, `tex-s2` ×1,01, `tex-s5` ×1,45, `tex-s9` ×2,26, MWSR ×1,83, MWSR f16 ×1,91, FSR1 ×2,23, FSR1 f16 ×2,20. Texte : MWSR ×1,47, FSR1 ×2,31. Le plancher bilinéaire ne suit donc pas le cœur et ne bouge pas quand les lectures doublent ; il varie avec l'image et l'entrée (,19 à ,42 ms), alors que FSR1 reste à 2,16 ms. La mémoire est à 2800 MHz dans les deux cas : la circulation en mémoire est l'explication probable, déduite, la mémoire n'ayant pas varié. L'iGPU du 9900X reste à son horloge : à lire en rapports.

À cette horloge le texte f32 de MWSR est 7 à 9 % plus rapide que le f16 ; à l'horloge libre ils sont égaux à 2,3 % près. FSR1 f16 gagne environ 9 %. Le produit garde le f16 hors NVIDIA : rien n'a changé, la question reste à trancher.

**Les téléphones (04/11, une passe par cas, horloges libres).** Temps de `draw` jusqu'à la fin du GPU, sans filtre/MWSR/FSR1, en ms.

iPhone 13 Pro, moyennes sur une minuterie à 1 ms, page non isolée, WebGPU. Agrandissements, relevés avant le correctif, qui ne les touche pas : Chrome écran ×2 texte 2,04/3,00/5,63 ; ×2 jeu 2,27/3,19/5,77 ; ×1,33 2,46/3,00/6,90. Safari écran ×2 texte 2,06/2,75/4,85 ; ×2 jeu 2,08/3,02/6,56 ; ×1,33 2,37/2,85/6,37 ; 720p vers 1440p texte 2,46/3,40/6,71, jeu 2,52/3,37/6,08 ; 1080p vers 1440p 2,35/3,48/5,75.

Réductions, avec le correctif. Safari : écran ×0,67 2,90/4,48/6,50 ; à 60 i/s 3,06/5,17/8,42 ; 4K vers 1440p 3,48/4,77/6,92 ; 1440p vers l'écran 2,60/3,83/7,48 ; témoin écran ×2 2,48/3,62/6,02. Chrome : à 60 i/s 4,71/5,29/9,56 ; 4K vers 1440p 3,23/4,63/7,15 ; 1440p vers l'écran 2,90/4,10/6,85 ; témoin 2,48/4,10/6,48.

Écartée : Chrome à écran ×0,67, 3,08/2,90/6,83. MWSR y passe sous « sans filtre », ce qui ne peut pas être vrai, et son temps GPU lit 2,50 ms contre 4,12 sur la passe suivante de la même image, Safari lisant 3,94 sur les deux. La cause de cet écart n'est pas établie : une passe, téléphone en charge, horloge inconnue, et la page n'a gardé que les moyennes.

Realme RMX3933 (Mali-G57 MP1, Android 14, Chrome 156, page isolée, médianes, 18 pages sans erreur, 47 à 45 % de batterie, 19,9 à 22,6 °C), WebGPU puis WebGL2 : écran ×2 texte 14,6/19,3/43,4, 17,9/21,7/44,1 ; écran ×2 jeu 14,3/19,3/43,9, 17,7/21,7/44,3 ; ×1,33 15,5/20,1/42,7, 19,3/23,0/46,5 ; ×0,67 15,5/23,0/45,8, 35,2/41,1/62,0 ; 720p vers 1440p texte 22,3/35,7/108,0, 23,1/35,3/114,6 ; 1080p vers 1440p 22,5/36,3/111,4, 32,3/44,6/122,1 ; 4K vers 1440p 29,2/47,3/124,2, 88,5/103,7/180,6. MWSR n'y est pas plus lourd qu'en octobre (37,3 et 36,4 ms) et n'y tient pas 60 i/s ; Auto n'y met aucun filtre (`isEntryMaliGpu`). « Assez léger pour un téléphone » n'était donc vrai que d'un iPhone : le README et la FAQ donnent désormais les deux.

**Reste.** Le test mémoire de DualRTX, puis le redémarrage prévu.

**La défense contre un pipeline refusé (04/11, pour la v0.4.2).** Le défaut de la réduction n'a pris aucun relais : un pipeline refusé ne fait échouer ni `submit` ni `onSubmittedWorkDone`. Chaque création de pipeline passe maintenant dans une portée d'erreurs, et le refus est gardé. Si la passe d'une image utilise un pipeline refusé, le renderer abandonne (`failed`) et la page passe sur le jumeau WebGL2. Un pipeline refusé qu'aucune image n'utilise ne déclenche rien, et sans refus rien ne change. La portée retire l'erreur des `uncapturederror` : le renderer l'écrit en console et le labo la relit dans ses erreurs. Le correctif a d'abord été confirmé sur iPhone sans cette défense, qui ne le remplace pas. Dans Chrome sur SwiftShader, avec un pipeline que Dawn refuse vraiment (constante inconnue sur les réductions) : un agrandissement continue sans abandon, une réduction abandonne, et le jumeau WebGL2 dessine l'image. Elle n'a pas été essayée sur WebKit.

**La page de comparaison : tout au bouton, et ce qu'elle envoie (04/11, décisions de Bruno).** Au chargement, la page dessine l'image et rien d'autre : le tableau reste en tirets, aucun fichier de score n'est chargé, ni les 9,9 Mo de LPIPS. Elle s'ouvre sur Junk Shop, ×1,33, 4:2:0, côte à côte, Native et MWSR, zoom 1×, centre (1610, 629). Un lien qui porte un état garde son ancien sens : sans `chroma`, c'est 4:4:4, sans `view`, c'est « All ».

« Measure » lance les scores qui manquent, puis le temps GPU. Stop garde ce qui est connu et n'envoie rien, et un nouvel appui reprend. « Measure again » refait le temps GPU seul. Un changement de menu arrête tout et vide le tableau. Le badge couvre les chiffres, jamais les titres, tant que le tableau est vide ; dès qu'une valeur s'affiche, c'est une ligne sous le bouton.

Ce que l'œil attend passe d'abord. Le tableau ne travaille que sur une page chargée et visible, après 500 ms sans dessin ni défilement. Un lot touché par un dessin est jeté et refait, à chaud, et l'envoi le compte. Un lot lent est coupé en tranches d'environ 100 ms de GPU. Les scores ont leur worker, terminé avec le cas, et LPIPS attend entre deux soumissions. La page ne voit que son propre usage du GPU : ce qu'un autre onglet ou programme lui demande reste dans les chiffres.

L'envoi est un POST à `/v1/mwsr` à la fin de la mesure : familles et versions majeures du navigateur et du système, fabricant et architecture du GPU, le cas, les scores, les temps, et l'échec s'il y en a un (`draw`, `score`, `warmup`, `batch`). Au plus 3 par 5 minutes, comptés en mémoire, sans nouvel essai, sans cookie ni référent. Pour une image du visiteur, ni son nom ni ses tailles. Pas de bouton de consentement, une note sous le bouton : choix de Bruno, sans avis de juriste.

Rien ne bouge pendant une mesure : la place des chiffres est réservée dans les légendes, la largeur des colonnes dans les cellules, et le bouton garde une taille pour ses trois libellés. Avant cela, à 390 px, le bouton descendait de 60 px quand les légendes se remplissaient. Les noms de lignes restent figés sur tous les tableaux de la page.

Sur l'iPhone de Bruno, au clic, le libellé du bouton changeait mais son dégradé n'était pas peint, puis le bouton revenait après un Stop. Ce n'est pas un cache : le nouveau libellé s'affichait, dans sa couleur. La cause n'est pas établie, et le rendu n'a pas pu être reproduit ici. Le bouton a maintenant une taille fixe, une couleur sous le dégradé et plus de transition sur toutes ses propriétés, ce qui n'a pas encore été revu sur un iPhone.

Essayé sur SwiftShader seulement, avec une image de 320×180 pour les mesures entières : rien au chargement (0 soumission en 3 s, aucun POST), mesure entière (0 dessin de vignette, 0 lot jeté, pourcentage sans recul), Stop et reprise, changement de menu, limite d'envois, échecs provoqués dans les scores et le temps GPU, image en entrée, début d'une mesure HDR, bouton et tableau au même pixel de 390 à 1400 px. Vue de départ comparée à la capture de Bruno à densité 2× : corrélation 0,9991, au plus 1 pixel d'écart. Pas essayé : iPhone, vraie carte, vrai serveur, mesure entière sur une image du site, branche `uncapturederror` sur un vrai refus.

**MWSR en f32 partout, son f16 masqué sur la page (04/11, décision de Bruno).** Le produit ne demande plus `shader-f16` et le noyau tourne sur son texte f32, sur tous les GPU et en HDR. Le f16 ne servait en production que là où WebGPU porte MWSR : iPhone, iPad et le HDR. Le texte `half` reste dans le module, au labo, dans `mwsr/` et sur la page. Le `mediump` de la saveur WebGL2 n'a pas changé. Revenir en arrière tient à quelques lignes de `WebGpuRenderer.js`.

Ce que disent les mesures, f16 contre f32 (ms par image, ×1,33 et ×2, jeu et texte) : la Radeon du 9900X prend 11 à 15 % de temps en moins en f16 (2,01 contre 2,35 ; 1,34 contre 1,52 ; 2,00 contre 2,36 ; 1,36 contre 1,53), la 780M à horloge tenue 7 à 9 % de plus, et à horloge libre elles sont égales à 2,3 % près. Sur Mali, avec l'ancien noyau, `half` gagnait 10 %. Sur le texte d'aujourd'hui, ni iPhone ni Mac ne sont mesurés dans les deux précisions : l'iPhone est pourtant l'appareil le plus concerné. Le f32 n'est donc pas « le plus rapide » partout, il est le choix d'attente de Bruno, et les envois de la page trancheront.

**La page.** Le tableau et les légendes donnent le f32 de MWSR, sans étiquette, même si son f16 est refusé par la vérification d'image. FSR1 et SGSR1 gardent la plus rapide de leurs deux précisions. Le f16 de l'agrandissement de MWSR reste vérifié et chronométré à chaque mesure, et part dans la ligne `half: true` de l'envoi, déjà acceptée par le serveur : aucune valeur n'en est affichée, et le texte de la page le dit. Un f16 refusé ne donne pas de ligne : `f16: true` sans ligne `half` de MWSR, en agrandissement, veut dire refusé.

**Les chiffres publiés.** Les temps MWSR de la Radeon du 9900X passent en f32 dans `index.html`, le README et `speed-*.svg` : 2.35, 1.52, 2.36, 1.53 ms. Ce sont les cellules `mwsr` de `final-time-igpu.json` (03/11), le même passage que les cellules `mwsr:half=1` publiées avant, avec une seule série par cas sur cette carte. L'écart à FSR1 y devient « 2.7 to 4.1 », un rapport calculé comme le précédent (`min(fsr1, fsr1h) / mwsr`).

**Deux passes sur la Radeon intégrée, créneaux de 59, ni l'une ni l'autre ne donne le tableau.** La première, dans un Chrome normal : 122 s, allée au bout, aucun événement du pilote, mais mon script a planté avant d'écrire le tableau et l'envoi. La seconde, avec `--disable-gpu-compositing` ajouté pour réduire le débord ci-dessous : à 53 s, pendant LPIPS, `DXGI_ERROR_DEVICE_HUNG`, puis `LiveKernelEvent` 141 (deux) et a1000001 (trois) dans le journal de Windows, les seuls en 30 heures. Le LPIPS du 03/11 sur cette carte, sans ce drapeau, n'en a laissé aucun. Cause non établie : une passe de chaque côté, le dump n'a pas pu être lu. **Ce drapeau ne va plus sur un vrai GPU.** Un visiteur dont Chrome compose en logiciel pourrait se trouver dans ce cas : ni vérifié, ni écarté. Sous SwiftShader, avec ce même drapeau, les volets de la page sortent noirs dans une capture d'écran : à regarder aussi, si ce mode compte.

Prouvé sur la vraie carte : le texte f16 de MWSR, FSR1 et SGSR1 compile et passe sa vérification, l'envoi porte les tailles d'une image du site, et la branche d'échec marche sur une vraie perte de device (`fail.step: 'score'`). Pas prouvé : un temps f16 réel, et l'affichage du tableau avec de vraies valeurs. Cette logique est essayée sous SwiftShader, qui n'a pas `shader-f16` : le service de test lui dit qu'il l'a, dessine le texte 32 bits et divise le temps 16 bits par deux, pour qu'une confusion se voie. MWSR montre son f32, FSR1 et SGSR1 leur f16 étiqueté, un refus forcé se voit sur FSR1 et pas sur MWSR, et « Measure again » suit la même règle.

**Le bon repère pour le débord de Chrome.** Depuis le dernier redémarrage, le bus 1 est l'adaptateur 0,86442, le bus 3 le 0,91721, la Radeon le 0,96679. Un Chrome de labo sans fenêtre pose du temps 3D sur le bus 1 même en SwiftShader : 46,6 ms en 35 s, 170 ms en 125 s. Il suit la durée et pas WebGPU : c'est sans doute le compositeur. Sous SwiftShader, `--disable-gpu-compositing` le ramène à 5 ms en 33 s. Essais de la page : scénarios habituels de `page_check.py` avec ce drapeau, sur les fichiers de ces commits. L'image du wiki est dessinée sans lui ; son volet « Native » est la source au pixel près.

**Les mesures de Bruno sur ses appareils (04/11, page d'essai, Junk Shop, ×1,33, 4:2:0, une passe par appareil).** MWSR, temps GPU par dessin en f32 et en f16 : RTX 5060 Ti 0,076 et 0,081 ms ; Apple M3 0,390 et 0,367 ; iPhone 0,816 et 0,749. Le f16 prend donc 6,6 % de plus sur le RTX, 5,9 % de moins sur le M3 et 8,2 % de moins sur l'iPhone. Sur l'iPhone, le gain est de 0,067 ms par image. Deux mesures du f32 sur le M3 ont donné 0,368 puis 0,390 ms : cet écart entre deux passes vaut celui entre les précisions, même si, dans une passe, les lots sont entrelacés. Ces passes donnent aussi le premier affichage réel du tableau, sur iPhone, M3 et RTX : MWSR montre son f32 sans libellé, et le vrai texte f16 y compile, passe sa vérification d'image et se chronomètre. L'envoi au vrai serveur reste à essayer.

SGSR1, à sa meilleure précision, n'est pas derrière MWSR partout sur cette image : M3 0,354 ms contre 0,390, iPhone 0,749 contre 0,816, RTX 0,077 contre 0,076. La phrase du README, « the fastest of the four on these GPUs », reste vraie de ses trois GPU sur 0 A.D. et le texte, en f32 compris : elle ne s'étend ni aux appareils Apple ni à cette image.

Le tableau de la page : les barres partent d'un même point, la précision se lit sous le nom (« Half precision », « Half ≈ full », « Half refused »), toutes les lignes ont une hauteur, et « port » quitte la ligne de NIS, puisque la section « The ports » le dit. Reste la note de son volet, « MoonlightWeb's per-fragment port ».

**Bruno confirme le f32 partout (04/11), au vu de ces mesures.** La question reste fermée jusqu'à ce que les envois de la page la rouvrent, l'iPhone et le Mac d'abord. Dans le tableau de la page, deux scores qui se lisent de la même façon sont tous les deux en gras, ce que les tableaux fixes faisaient déjà : la comparaison se fait sur la valeur arrondie, et non plus sur la valeur brute.
