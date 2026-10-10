# « Ultra » en LAN — le POC d'un codec intra sur GPU

> Une question, chiffres à l'appui : un mode réservé au LAN, où chaque image est
> compressée seule sur le GPU de l'hôte, décodée sur le GPU du navigateur et
> présentée à l'arrivée, bat-il nettement le HEVC d'aujourd'hui en latence de
> bout en bout, à qualité acceptable et à débit raisonnable ? Et où se place-t-il
> face à PyroWave, que Steam Remote Play embarque en bêta depuis le 21/09/2026 ?
>
> Un POC : un « non » chiffré est un résultat. Le plan détaillé (phases, commits,
> efforts) vit hors du dépôt ; ce document en garde les hypothèses, le modèle,
> les portes et les résultats.

## 1. Où il se fait, et ce qui protège le produit

- Sur `main`, comme le reste du travail (décision de Bruno du 02/10/2026).
- Les labos vivent hors du produit : `tools/` (côté hôte), `scripts/bench/ultra/`
  (côté navigateur), `docs/`.
- Le code du POC dans le produit reste derrière deux clés cachées :
  `MW_NATIVE_TUNING=ultra=…` à l'hôte, `mw_ultra=1` au client. Ultra n'est jamais un
  défaut. Un refus ou un échec retombe sur le HEVC d'aujourd'hui, avec une ligne de log.
- Hôte natif Windows, route D3D12 seulement. Sunshine, Apollo, Wolf, MultiSeat et
  les hôtes Linux et macOS ne changent pas.
- Un « oui » à la dernière porte ouvre un plan de mise en produit à part. Il
  couvrirait aussi macOS (Metal, d'après le portage Metal de l'amont) et Linux
  (Vulkan, les shaders de l'amont presque tels quels). Un « non » retire le code
  produit du POC par un commit et garde les labos et le rapport.
- Code tiers : PyroWave (MIT) est porté depuis l'amont, avec sa notice, sous
  `third_party/pyrowave`, version épinglée. Les dérivés GPL (Vibepollo, clients
  Moonlight modifiés) ne sont pas lus : ni leur source, ni leurs shaders.
- Corpus : les images de jeux restent hors dépôt. Seuls les outils et les chiffres
  y entrent.

## 2. Les hypothèses du départ, relues contre les mesures

- **Le codec ne fait pas le gros de la latence partout.** Sur NVIDIA, il pèse
  ~3 ms (encodage 1,9, décodage 1,1) sur un clic → drapeau de ~35 ms à 60 Hz
  (banc §8l, §6c). Sur l'Arc, il pèse 5,4 ms en oneVPL ; sur le N95 7 à 11, sur
  l'iGPU AMD 9 à 17. Le reste vient de la cadence de capture et de la
  présentation chez le client.
- **Une image ne se décode qu'à son dernier octet.** Les formats de texture
  perdent sur le fil ce qu'ils gagnent au décodage : BC1 / ETC2 (4 bpp) font
  8,3 ms par image 1080p sur 1 GbE, contre 0,3 ms en HEVC à 20 Mbit/s. Et aucun
  n'existe à la fois sous Windows et sur mobile : Chrome sous Windows n'expose
  que BC. Ils sont mesurés hors ligne, sans encodeur temps réel.
- **Le transport est la première inconnue.** Le média passe par un DataChannel
  WebRTC. usrsctp démarre lentement après chaque pause (burst max 10, bridage par
  la fenêtre), le débit de Chrome en DataChannel n'a pas de mesure publiée, et le
  produit plafonne à 150 Mbit/s. D'où un labo transport avant tout codec.
- **La présentation peut tout reprendre.** WebGPU coûte 4 ms de rendu sous
  Windows et 10 sous macOS, contre 0,2 à 0,6 ms pour Canvas2D et WebGL
  `desynchronized` (03/09). Les présentateurs sont départagés au photon avant
  d'écrire un décodeur.
- **Le candidat principal** est un codec intra par ondelettes, porté de PyroWave
  (MIT) : CDF 9/7 sur 5 niveaux, blocs de 32×32 coefficients décodables seuls,
  sans codage entropique. Moins de 0,1 ms d'encodage et de décodage en 1080p sur
  un GPU récent ; 170 Mbit/s en 1080p60 pour une image jugée sans défaut. Aucun
  portage navigateur n'existe : le décodeur est à écrire, en WGSL ou en shaders
  WebGL2 selon le présentateur qui gagne.

## 3. Le modèle de latence — la boussole

Latence d'une image = attente de la capture + acquisition + conversion +
**encodage** + **sérialisation** (taille ÷ débit du lien) + pile réseau et
navigateur + **décodage** + attente de présentation + dalle. Le codec agit sur
les termes en gras, et sur la présentation si son décodeur impose une API de rendu.

Sérialisation d'une image 1080p au débit ligne, sans en-têtes (borne basse) :

| 1080p | Kio / image | Mbit/s à 60 i/s | 1 GbE | Wi-Fi ~400 Mbit/s réels |
|---|---|---|---|---|
| HEVC 20 Mbit/s | 41 | 20 | 0,33 ms | 0,8 |
| PyroWave, seuil subjectif (1,37 bpp) | 346 | 170 | 2,8 | 7,1 |
| ASTC 6×6 (3,56 bpp) | 900 | 442 | 7,4 | 18,4 |
| BC1 / ETC2 RGB (4 bpp) | 1 013 | 498 | 8,3 | 20,7 |

Les paquets SCTP de 1 280 octets ajoutent ~11 % ; un DataChannel qui ne tient
que 80 % du lien, 25 % de plus : PyroWave au seuil passe à 3,2-3,8 ms. Le labo
transport remplace ces facteurs par la mesure.

Segment codec prédit (encodage + sérialisation + décodage, 1080p60, 1 GbE, client
de bureau) :

| Hôte | HEVC aujourd'hui | Ultra PyroWave 170 Mbit/s |
|---|---|---|
| RTX (NVENC P1) | ≈ 3,4 ms | ≈ 3,6-4,2 |
| Arc (D3D12 VE) | ≈ 3,8 | ≈ 4,1-4,7 |
| N95 (oneVPL) | ≈ 10,5 | ≈ 6,5 |
| iGPU AMD (AMF) | ≈ 10,5-18,5 | ≈ 6,5 |

Lecture honnête : légère perte sur NVIDIA et sur l'Arc, gain de 4 à 12 ms sur
les iGPU. Côté client, un décodeur matériel mobile lent ferait pencher vers
Ultra, un présentateur plus lent que Canvas2D vers HEVC. Hors codec, la cadence
de capture coûte une demi-période (8,3 ms à 60 Hz, 2,1 à 240) : sur NVIDIA, elle
pèse plus que le codec. La barre à battre est donc « HEVC réglé Ultra »
(120 i/s, écran virtuel à 120 ou 240 Hz, P1, tearing, « Auto » avec détection),
pas le HEVC par défaut. Un codec intra paie chaque image : son débit double avec
la cadence, celui du HEVC beaucoup moins.

## 4. Le socle hérité du plan D3D12 (relevé du 02/10/2026)

- **Chaîne par GPU** (§32.19) : Intel en D3D12 (conversion D3D12 sur la file
  DIRECT, D3D12 Video Encode, débit maison, deux images en vol sur un GPU à
  mémoire propre) ; NVIDIA et AMD en D3D11 (NVENC, AMF), D3D12 n'y fait pas mieux
  (G4). Tout refus ou panne revient à D3D11 sans couper le stream.
- **Files** (G1, banc §8n.2) : `CreatorID` propre et priorité `GLOBAL_REALTIME`
  quand le processus tient REALTIME, repli en HIGH. Sur la file COMPUTE, la v1
  était préemptée par le jeu sur l'Arc et affamée sur l'iGPU AMD (117 / 249 ms) :
  un encodeur en shaders partage les unités du jeu, et se mesure sous RE9 et sous
  charge, sur cette politique de files.
- **Temps GPU** : `gputiming=1` encadre chaque soumission d'encodage de deux
  horodatages (`gpu_encode_us`). L'Arc les écrit avant que l'image soit codée et
  ne rapporte rien. La poignée de main DDA reste `ddasync=gpu`.
- **Cadence** : l'« Auto » avec détection (§33.10 de `native-capture-encoder.md`)
  porte déjà le stream au-dessus de la fréquence du client quand l'image y
  rajeunit. Il fait partie de la barre.

## 5. Les phases et les portes

| Phase | Ce qu'elle fait | Porte |
|---|---|---|
| UA | « Auto » avec détection, dans le produit | porte UA (Bruno, 02/10 : par défaut, avec une sûreté) |
| U0 | Budget de latence : E2E par image, âge du contenu, « HEVC réglé Ultra », borne Steam, les deux TV | **U0** |
| U1 | Labo transport : DataChannel à haut débit, réglages usrsctp | **U1** |
| U2 | Labo hôte : corpus, qualité, formats de texture hors ligne, PyroWave de référence, portage HLSL, sous charge | — |
| U3 | Labo navigateurs : planchers natifs, capacités, présentateurs au photon, décodeur, sonde TV | **U2** |
| U4 | Intégration derrière les clés cachées | — |
| U5 | Banc comparatif HEVC / Ultra / Steam PyroWave | **U3** (verdict) |
| U6 | Clôture | — |

Chaque porte donne un rapport chiffré et une recommandation ; Bruno tranche.
Chacune peut arrêter le POC. Les deux portages lourds (encodeur HLSL, décodeur
navigateur) ne commencent qu'après U0 et U1.

- **U0** : on continue sur les couples hôte × client où le gain prédit, sous des
  hypothèses favorables à Ultra, vaut au moins 2 ms ou 20 % de l'E2E instrumentée.
  Aucun couple : le POC s'arrête, et les gains hors codec partent dans la liste
  du plan natif.
- **U1** : sur 1 GbE, Chrome de bureau, 60 i/s, images de 350 Kio, étalement +
  RTT min / 2 au p50 ≤ 1,08 × débit ligne + 1 ms et p99 ≤ + 4 ms ; aucune erreur
  de réception UDP ; transport ≤ 10 % du fil principal du client. Le plafond
  mesuré devient l'enveloppe du codec. Sous 170 Mbit/s en filaire : plan B RTP
  sondé, puis arrêt ou décision de Bruno.
- **U2** : qualité ≥ HEVC P1 à 20 Mbit/s sur le contenu animé, à ≤ 200 Mbit/s ;
  lisibilité du texte fixe jugée par Bruno. Encodage p99 ≤ 1 ms sur la RTX,
  ≤ 2 ms sur l'Arc, ≤ 3 ms sur les iGPU, sous charge. Décodage p50 ≤ 1 ms sur un
  bureau, ≤ 3 ms sur M1 et iPhone ; présentateur sans vsync de plus.
- **U3** : E2E par image, médiane ≤ « HEVC réglé Ultra » − 2 ms (ou − 20 %) avec
  un p99 pas pire ; clic → drapeau pas pire ; qualité acceptée à l'œil par Bruno ;
  30 min sans gel ; 1 % de pertes sans gel de plus d'une image. Issues : « oui »,
  « oui limité » (par exemple hôtes à iGPU et clients filaires), « non ».
- Les portes se jugent en Ethernet 1 GbE ; le Wi-Fi est mesuré et rapporté,
  jamais bloquant.
- **Les TV** (décision du 02/10) : budget en U0, sonde du décodeur Ultra en U3.
  Elles deviennent une cible de U4 seulement si, en 720p, décodage + présentation
  tiennent sous 14 ms par image à plus de 30 images affichées par seconde. Elles
  ne bloquent aucune porte.

## 6. Résultats

*(rempli porte par porte)*

### 6.1 U0.2 — l'E2E par image (03/10/2026, `ea3d313c`)

Le POC a besoin d'une latence mesurée d'un bout à l'autre, image par image, et
non d'une somme d'étapes. La latence de l'overlay additionne des étapes, chacune
chronométrée sur sa propre horloge et moyennée sur sa propre fenêtre : une étape
que personne ne chronomètre n'y figure pas.

- **L'horloge commune existait** (plan 1, cadence de l'hôte) :
  - le pong porte les µs de l'hôte (`DataChannelRelay`, champ `host`) ;
  - `util/ClockEstimator.js` ajuste un décalage et une dérive sur les échanges
    proches du RTT le plus court ;
  - ses tests (dérive, RTT asymétrique, saut d'horloge, valeur aberrante) sont
    dans `ContentAgeProbe.test.js`.
- **Ce que U0.2 ajoute** : `stream/FrameLog.js`. Il met l'horodatage de chaque
  image (`backendTs`) sur l'horloge du client, avec sa propre estimation sur
  60 s, nourrie par le ping de 2 s. Il en tire l'âge de l'image à la fin de son
  dessin. On le retrouve :
  - dans le détail de latence, en ligne « Mesurée (hôte → dessin) » (moyenne et
    p99 sur 2 s). Elle est affichée, pas additionnée : c'est ce que la somme
    devrait lire, et l'écart entre les deux est une étape que personne ne
    chronomètre ;
  - dans la ligne `[perf]` (`e2e measured`) ;
  - dans un journal de toutes les images : un anneau de colonnes de
    16 384 images (une minute à 240 i/s), lu par CDP avec `mwFrameLog.csv()` ou
    `summary()`. `age.py run` le vide au départ et l'enregistre en
    `<tag>.frames.csv` à côté du JSON de la passe.
- **Où le trajet commence** :
  - hôte natif : à la présentation de l'image à l'écran, à la milliseconde. Il
    lit 0 à 2 ms de trop, parce que les deux moitiés de l'horodatage sont
    arrondies à la milliseconde ;
  - hôte GameStream : à l'arrivée de la première image du stream au backend.
    Sa capture, son encodage et son trajet jusqu'au backend manquent, du même
    montant sur chaque image.
- **Limites** :
  - un lien plus lent dans un sens que dans l'autre (la montée en Wi-Fi) fausse
    l'âge de la moitié de l'écart, sur chaque image ;
  - seul le décodage sur le fil principal est couvert, pas le worker (option) ;
  - AV1 n'a pas d'horodatage d'hôte sur ce chemin.
- **Vérifié** :
  - Vitest : 7 tests (âge, horloge pas encore prête, dérive de 50 ppm sur 3 min
    avec un ping toutes les 2 s, bouclage 32 bits, horodatage aberrant,
    anneau et CSV, résumé) ; suite complète 1192/1192.
  - En vrai stream (03/10, 08:37) : deux passes locales sur DualRTX, hôte Arc
    (D3D12 VE), HEVC 2560×1440 à 60 i/s, écran virtuel à 240 Hz, client sur
    l'iGPU AMD. Toutes les images dessinées ont un âge (1200 et 1199) :

    | Passe | Médiane | Moyenne | p99 | Somme des étapes (overlay) |
    |---|---|---|---|---|
    | r0 | 10,5 ms | 13,3 ms | 28,4 ms | 11,3 ms |
    | r1 | 10,2 ms | 12,8 ms | 27,2 ms | 11,7 ms |

    - Les médianes des étapes du journal (présent → arrivée 8,2-9,0 ms,
      décodage 0,6, attente 0,0, dessin 0,2) s'additionnent à 9,0-9,8 ms, sous
      la médiane de bout en bout : c'est cohérent.
    - La somme des étapes de l'overlay lit 1,1 à 2 ms sous la moyenne mesurée.
      Une partie de cet écart est l'arrondi de l'horodatage (0 à 2 ms) ;
      l'autre reste à attribuer.
    - L'horloge : 226-227 échanges (le ping de 2 s et ceux de la sonde d'âge),
      RTT minimal 0,3 ms, dérive estimée 0,4-0,9 ppm (0 en vrai : une seule
      machine), aucun saut. L'estimation sœur de la sonde d'âge, faite sur les
      mêmes pongs, tombe à +0,02 / +0,03 ms de l'horloge vraie.
    - Le contrôle croisé (09:50, deux passes de plus, `ua-u02d-*`) : la bande
      était illisible dans la matinée parce qu'une invite pare-feu de Windows,
      ouverte à 06:38 par le banc T7 pour un exe sans règle, restait posée en
      0,0 de l'écran capturé. Bruno l'a autorisée, et la bande se relit. Image
      par image, le journal et la colonne `capture` de la sonde d'âge
      concordent : −0,09 ms de médiane (p10 −0,19, p90 +0,01) sur 296 et 285
      images appariées. Même horodatage, même horloge : c'est le contrôle
      attendu.
    - **Ce que le contrôle a montré en plus : la sonde d'âge retarde les images
      qu'elle lit.** Elle en lit une sur quatre (`every 4`) ; ces images-là
      attendent 12,2 ms entre leur décodage et leur dessin, les autres 0,0.
      Médianes de bout en bout : 22,0 ms pour les images lues, 8,9 pour les
      autres. La sonde ne calcule ses âges que sur les images lues : ses
      chiffres absolus (`drawn`, `shown`, `capture`) portent ce retard, sur ce
      client du moins (iGPU AMD de DualRTX, Chrome). Les écarts entre modes,
      mesurés avec la même sonde, le portent des deux côtés. Le coût sur les
      autres clients (Mac, N95, UM790Pro) est à mesurer de la même façon,
      avec le journal.
    - Pièges de la matinée : l'instance dev écoute maintenant sur 8080/8443
      (et non plus 18080/18443), et une première passe a été arrêtée par
      Claude Code, faute de mémoire.

### 6.2 U0.2 bis — la sonde d'âge ne retarde plus ce qu'elle mesure (03/10/2026)

Le contrôle de U0.2 (§6.1) a montré que la sonde d'âge du contenu retardait le
dessin des images qu'elle lisait. Elle copiait la bande sur le fil principal,
avant le dessin de l'image. Elle passe maintenant l'image à un worker
(`bandReadWorker.js`, `b83f3dac`), qui fait la copie. Si le navigateur refuse
de transférer l'image, ou si le worker meurt, la sonde copie comme avant. Le
résumé de chaque passe dit par quel chemin la copie est passée (`reader`), et
ce que le fil principal a encore payé par lecture (`readCost`, affiché par
`age.py`). Avec `MW_BENCH_INLINE_READ=1`, une passe copie comme avant : c'est
ce qui a servi à mesurer l'avant et l'après (`659a017b`).

Mesure du 03/10 (12:43-13:00) : hôte Arc, stream 60 i/s, une passe par mode,
deux sur DualRTX. On compte l'attente entre le décodage d'une image lue et le
début de son dessin, donnée par le journal par image ; les autres images
attendent 0,0 à 0,1 ms.

| Client | Avant (fil principal) | Après (worker) | `capture` de la sonde, avant → après |
|---|---|---|---|
| DualRTX, client local (iGPU AMD) | 11,3 / 13,2 ms | 0,1 / 0,1 ms | 19,4 / 25,5 → 8,9 / 9,1 ms |
| N95, Wi-Fi | 7,3 ms | 0,2 ms | 49,8 → 34,8 ms |
| UM790Pro sous Windows, Ethernet | 4,0 ms | 0,1 ms | 16,9 → 12,1 ms |

- **Les âges du contenu mesurés jusqu'ici étaient trop hauts, à peu près de ce
  coût.** La sonde ne calcule ses âges que sur les images qu'elle lit, et
  chacune portait son retard : jusqu'à 12 ms sur le client local, 7 sur le N95,
  4 sur l'UM790Pro. Sur le Mac, ce coût n'a pas été mesuré.
- **Les écarts entre modes** (UA, §8t du banc) portent ce retard des deux
  côtés. Ils restent comparables, tant que le coût ne change pas d'un mode à
  l'autre. Une image lue de plus par seconde, à 240 i/s, aurait pu en ajouter :
  ce n'est pas vérifié pour les passes passées.
- La colonne `capture` de la sonde rejoint maintenant la médiane du journal
  par image (8,9 contre 9,2 ms en local) : la sonde ne mesure plus son propre
  retard.

### 6.3 U0.3 — les séries du 03/10/2026 (N95, UM790Pro, iPhone, Mac)

Hôte DualRTX, client le N95 (Chrome, Wi-Fi), de 15:57 à 16:50 : 20 passes,
chacune avec 30 s d'âge du contenu, le journal par image et 30 clics. Deux modes
alternés D U D U dans chaque case, l'écran virtuel rendu par le GPU de la case :

- **D, « HEVC par défaut »** : le produit d'aujourd'hui, Auto avec détection ;
- **U, « HEVC réglé Ultra »** : la barre (§2), 120 i/s demandés, écran virtuel
  à 240 Hz, tearing, détection ;
- **A** : U en AV1. Les cases AV1 alternent U A U A.

`pass.py --codec` vient de `69475ba6`. Médianes de deux passes par mode (l'âge
montré de A sur l'Arc, d'une seule : l'autre n'a rien pu lire) :

| Hôte | Mode | Âge montré | E2E par image | Clic → drapeau (clics mesurés) | Plus longue coupure du lien |
|---|---|---|---|---|---|
| RTX (NVENC) | D | 44,9 ms | 28,0 ms | 83,0 ms (53 / 60) | 0,8 s |
| RTX (NVENC) | U | 270 ms | 242 ms | 135 ms (22 / 60) | 8,8 s |
| Arc (D3D12 VE) | D | 58,2 ms | 39,9 ms | 88,6 ms (54 / 60) | 1,2 s |
| Arc (D3D12 VE) | U | 130 ms | 114 ms | 204 ms (12 / 60) | 4,4 s |
| iGPU AMD (AMF) | D | 51,7 ms | 31,3 ms | 93,9 ms (46 / 60) | 2,4 s |
| iGPU AMD (AMF) | U | 320 ms | 295 ms | 101 ms (17 / 60) | 14,8 s |
| Arc (oneVPL) | A | 448 ms | non mesuré | aucun (0 / 60) | 31 s |
| RTX (NVENC) | A | 671 ms | non mesuré | 185 ms (8 / 60) | 20,8 s |

- **Sur le N95 en Wi-Fi, demander 120 i/s fait décrocher le lien.** Le lien se
  coupe plusieurs secondes et le débit retombe de 7 à 5 Mbit/s. Le stream ne
  livre que 41 à 82 i/s, et plus d'un clic sur deux n'obtient pas de drapeau. Le même
  écart se répète sur les trois GPU, passe après passe : ce n'est pas un
  incident. Le mode par défaut, lui, reste à la cadence du client : sa détection a
  essayé 116 i/s et y a renoncé (la file du décodeur se remplissait, puis
  l'image arrivait trop tard).
- **La cause : la vidéo attend dans usrsctp, sur l'hôte.** Une série de plus
  (18:00, RTX, D U D U, `relaylog=1`, `scripts/bench/wifi/flagpath.py`) coupe
  le trajet de chaque image. Sur toutes les images de la minute des clics :

  | Mode | Réseau p50 / p90 | dont avant de quitter usrsctp, p50 / p90 | Décodage d'une image de drapeau |
  |---|---|---|---|
  | D | 18-22 / 43-67 ms | 12-13 / 35-54 ms | 2-3 ms |
  | U | 38-49 / 204-263 ms | 25-32 / 186-232 ms | 18-22 ms |

  L'air (la moitié du RTT de SCTP) ne prend que 24-28 ms au p90, et SCTP ne
  retransmet rien. C'est la fenêtre de congestion d'usrsctp qui retient la
  vidéo à 120 i/s, comme dans W1 du plan Wi-Fi. Le décodeur du N95 ralentit
  aussi, mais il ne compte que pour une quinzaine de millisecondes. Pour
  corriger, il faut agir sur l'envoi (le plan Wi-Fi), pas sur le décodeur.
- **L'AV1 est pire encore sur ce client.** Le décodage prend 0,9 à 1,1 s par
  image, et la bande sort gris-violet, illisible sur l'Arc. Le N95 ne décode
  pas l'AV1 1080p à cette cadence. Il n'est pas vérifié si son décodeur AV1
  est matériel ou logiciel.
- **Trou de l'outil, corrigé ensuite.** Le journal par image (U0.2) n'avait
  vu aucune image AV1. La voie AV1 donne au décodeur un horodatage inventé et
  un tampon d'hôte nul, pour que le pacer présente au décodage, et le journal
  lisait ce zéro. Depuis `1cdb4eb9`, le tampon suit l'image jusqu'au journal
  seul. Le pacer, la grille de vsync, la détection et la sonde d'âge ne voient
  toujours rien en AV1 : **la détection de l'« Auto » ne mesure donc pas
  l'AV1**. C'est noté, non corrigé. La correction n'est pas encore vérifiée en
  vrai stream.
- **Pas de case sous charge GPU dans cette série.** `mw-gpu-load` s'arrête au
  bout de 60 s, plus court qu'une passe. Il faudra le brancher dans `pass.py`,
  entre la calibration et la mesure.

**L'UM790Pro en Ethernet** (même jour, 18:33-19:20). Client UM790Pro sous
Windows (Chrome, 780M, écran virtuel à 120 Hz, câble 1 Gbit/s), même hôte et
mêmes cases, `relaylog=1` sur toutes les passes. Le `build\` de 18:02 contient
`e0324f4e` (clé `retrcut` du plan Wi-Fi, éteinte par défaut : rien ne change
sans elle). Médianes de deux passes par mode :

| Hôte | Mode | Cadence | Âge montré | E2E par image, médiane / p99 | Clic → drapeau (clics mesurés) |
|---|---|---|---|---|---|
| RTX | D | 239 i/s | 23,8 ms | 8,4 / 126 ms | 34,8 ms (58 / 60) |
| RTX | U | 120 i/s | 24,7 ms | 9,8 / 24,7 ms | 32,9 ms (57 / 60) |
| Arc | D | 238-239 i/s | 27,1 ms | 10,2 / 25,3 ms | 38,5 ms (58 / 60) |
| Arc | U | 120-121 i/s | 25,0 ms | 12,9 / 30,3 ms | 36,4 ms (56 / 60) |
| iGPU AMD | D | 227-234 i/s | 24,6 ms | 11,4 / 33,2 ms | 42,5 ms (58 / 60) |
| iGPU AMD | U | 119-121 i/s | 28,9 ms | 13,9 / 27,2 ms | 40,0 ms (58 / 60) |
| Arc | A (AV1) | 120-124 i/s | 27,1 ms | 15,5 / 41,0 ms | 44,0 ms (28 / 60) |
| RTX | A (AV1) | 120-121 i/s | 28,3 ms | 11,8 / 30,3 ms | 35,3 ms (58 / 60) |

- **En Ethernet, l'Ultra tient, mais le mode par défaut fait déjà aussi bien.**
  La détection monte à 230-240 i/s et y reste. U, à 120 i/s, fait jeu égal :
  son âge médian par image est de 1 à 3 ms plus haut, mais sa queue est plus
  courte (p99 de 25-30 ms, contre 25-126 ms pour D, le pire étant la RTX à
  240 i/s). Le N95 en Wi-Fi est deux à trois fois plus lent, même en D (âge de
  45-58 ms, clic de 83-94 ms).
- **La fenêtre de congestion d'usrsctp ne retient la vidéo qu'en Wi-Fi.** Ici,
  le réseau a un p90 de 7 à 15 ms, dont 5 à 12 ms avant de quitter usrsctp,
  quel que soit le mode, contre 186-232 ms sur le N95 en U.
- **L'AV1 tient sur le 780M, sans rien gagner** : 1 à 4 ms de plus que le HEVC
  à la même barre, pour 20 à 50 % de débit en plus (36-47 contre 29-32 Mbit/s).
- **Outil.** `1cdb4eb9` est vérifié en vrai stream : les passes AV1 ont leur
  âge par image. Une passe est illisible (Arc, AV1, A1) : tous les pixels lus
  de la bande et du drapeau sont blancs (255), alors que le décodage et l'âge
  par image étaient normaux. La passe suivante sur le même hôte est passée, et
  ses clics manquent au tableau (28 / 60). Cause probable : la fenêtre blanche
  du kiosque de contenu, déjà vue le 22/09 (`acceptance/run.py`,
  `content_start`). Le banc d'âge l'ouvre sans la vérification d'image qui
  relance un kiosque resté blanc (`probe=False`). Le calage de la bande, par
  DevTools, réussit quand même. Ce n'est pas vérifié.

**L'iPhone de Bruno** (même soir, 21:26-21:37, Safari, Wi-Fi). L'hôte est la
`--dev` de DualRTX, avec son écran virtuel au défaut du produit (rendu par
l'Arc, D3D12 VE Intel). Quatre passes en alternance Auto, 120, Auto, 120 (la
page de banc défile, l'iPhone reste immobile 60 s), relevées sur les captures du
détail de la latence. Sur iOS, ni la sonde d'âge ni le clic → drapeau ne
tournent sans console : seule la ligne « Mesurée » de U0.2 donne l'âge.

| Mode | Cadence, taille | « Mesurée » moyenne / p99 | Décodage | Réseau |
|---|---|---|---|---|
| Auto | 60 i/s, 2532×1170 | 41,2 / 69,1 puis 47,9 / 80,4 ms | 5,6 puis 9,7 ms | 4,5 à 40 ms (une coupure de 0,37 s) |
| 120 | 99 i/s, 2336×1080 | 39,8 / 67,2 puis 30,5 / 56,4 ms | 7,3 puis 4,7 ms | 5,3 à 17 ms |

- **Le 120 gagne de 5 à 10 ms sur l'iPhone**, avec deux passes par mode
  seulement, en Wi-Fi.
- **Le décodage HEVC matériel de l'iPhone est mesuré pour la première fois** :
  5 à 10 ms en moyenne, 8 à 21 ms au pire.
- **À creuser** : 38 à 47 % d'images comptées « dropped (jitter) » dans les deux
  modes, alors qu'aucune n'est perdue sur le réseau.
- **Lien** : sur iOS, la `--dev` ne se joint pas à son adresse du LAN
  (`https://192.168.1.66:8443/`). Safari n'étend pas au WebSocket de
  signalisation l'exception de certificat acceptée pour la page, et le journal
  de l'hôte dit « certificate unknown » toutes les 4 s. Son lien de staging
  (`stream.dev.moonlightweb.top/<id>`, vrai certificat, vidéo restée en direct
  sur le LAN) marche.

**Le Mac M1 en Wi-Fi** (même nuit, 22:49-23:19, Chrome de banc, capot fermé).
Même hôte, mêmes cases en HEVC (le M1 ne décode pas l'AV1), `relaylog=1`.
Médianes de deux passes par mode :

| Hôte | Mode | Âge montré | E2E par image, médiane / p99 | Clic → drapeau (clics mesurés) |
|---|---|---|---|---|
| RTX | D | 29,4 ms | 14,1 / 165 ms | 63,7 ms (58 / 60) |
| RTX | U | 36,2 ms | 22,4 / 304 ms | 60,9 ms (55 / 60) |
| Arc | D | 37,1 ms | 22,4 / 157 ms | 72,2 ms (58 / 60) |
| Arc | U | 43,9 ms | 26,4 / 184 ms | 71,0 ms (56 / 60) |
| iGPU AMD | D | 32,7 ms | 17,0 / 145 ms | 74,9 ms (55 / 60) |
| iGPU AMD | U | 43,2 ms | 24,6 / 228 ms | 71,2 ms (57 / 60) |

- **Le défaut monte déjà à 114-125 i/s**, la cadence de l'écran du Mac, et fait
  mieux que U de 7 à 11 ms d'âge. Le clic est le même dans les deux modes.
- **Le Mac tient les 120 i/s, contrairement au N95.** Son p90 avant de quitter
  usrsctp est de 50 à 79 ms dans les deux modes, contre 186-232 ms sur le N95
  en U et 5-12 ms en Ethernet. Le Wi-Fi du Mac fait attendre, sans décrocher.
- **Bilan provisoire de U0.3** : sur ordinateur, la barre « HEVC réglé Ultra »
  ne bat l'« Auto » détecté sur aucun client. Le N95 en Wi-Fi décroche à
  120 i/s, et l'UM790Pro et le Mac font jeu égal ou mieux en Auto. Sur
  l'iPhone, le 120 gagne 5 à 10 ms. Restent l'iPad, RE9 sur la RTX, les cases
  sous charge GPU et Android.

**L'iPad de Bruno sous RE9** (04/10, 19:17-19:36, Safari, Wi-Fi). RE9 (la copie
de banc) tourne sur l'écran de la RTX (DISPLAY5, M27Q 120 Hz, NVENC), streamé
par la tuile de cet écran (pas d'écran virtuel). La `--dev` est celle du build
`ded56fc6`, jointe par stream.dev. Relevé sur les captures du détail :

| Passe | Taille, cadence | « Mesurée » moy. / p99 | Décodage moy. / max | Réseau | « dropped (jitter) » |
|---|---|---|---|---|---|
| Auto | 2162×1216, 50 i/s | 82 / 145 ms | 21,6 / 75 ms | 13,8 ms | 44 % |
| 120 | 1920×1080, 62 i/s | 65 / 127 ms | 16,4 / 43 ms | 19,5 ms | 45 % |
| Auto, 1080p imposé | 1920×1080, 26 i/s | 31 / 49 ms | 8,0 / 11 ms | 2,3 ms | 3 % |

- **Le décodeur de l'iPad sature à 50-60 i/s.** Il prend 16 à 22 ms par image
  en moyenne, et jusqu'à 75 ms. À 26 i/s, il descend à 8 ms, et l'image n'a
  plus que 31 ms.
- **Safari annonce un écran à 32-51 Hz**, sans mode Économie d'énergie
  (vérifié par Bruno). L'« Auto » a suivi ce chiffre : 26 i/s à la passe 3
  (« 26 fps stream for a 51 Hz client… every 2nd refresh » dans le journal),
  alors que RE9 présentait environ 65 images par seconde. Sur cet iPad, c'est
  ce qui le sert le mieux, mais par accident. La mesure du rafraîchissement
  sous Safari est à revoir.
- **Le compteur « dropped (jitter) » suit la cadence** : 44-45 % à 50-62 i/s,
  3 % à 26 i/s, sans aucune perte réseau. Comme sur l'iPhone, ce sont des
  images que le décodeur n'a pas pu suivre, pas des pertes du lien. C'est une
  hypothèse, non vérifiée.
- DualRTX a fait un écran bleu (0x133, `DPC_WATCHDOG`) à 18:58, pendant un RE9
  sur la RTX juste avant ces passes, et ne l'a pas refait ensuite.

### 6.4 U0.3 — synthèse provisoire (04/10/2026)

**La barre à battre n'est pas « HEVC réglé Ultra », c'est l'« Auto » détecté.**
Sur les trois clients d'ordinateur, la barre du plan (§2 : 120 i/s, écran
virtuel à 240 Hz, tearing, P1) ne fait jamais mieux que l'« Auto » avec
détection (UA, sur `main`). Sur un client qui suit, la détection monte d'elle-même
au-dessus : 230-240 i/s sur l'UM790Pro, la cadence de l'écran (120 i/s) sur le
Mac. Sur un client qui ne suit pas (le N95 en Wi-Fi), elle reste à la cadence
de l'écran, là où forcer 120 i/s fait décrocher le lien.

| Client, lien | « Auto » détecté : âge montré / clic | Barre Ultra : âge montré / clic | File usrsctp p90 (D / U) |
|---|---|---|---|
| UM790Pro, Ethernet | 24-27 / 35-43 ms | 23-29 / 33-40 ms | 5-12 / 5-12 ms |
| Mac M1, Wi-Fi | 29-37 / 61-75 ms | 36-44 / 61-71 ms | 50-67 / 55-79 ms |
| N95, Wi-Fi | 45-58 / 83-94 ms | 130-320 ms / un clic sur deux perdu | 35-54 / 186-232 ms |
| iPhone, Wi-Fi (« Mesurée ») | 41-48 ms | 31-40 ms | — |

**Le budget d'un clic sur l'UM790Pro en Ethernet** (`flagpath.py`, médianes
des 20 passes, en ms) : montée du clic 1,5-3, le drapeau dessiné sur l'hôte
12-16 (la boucle de messages de l'overlay de banc, pas le produit), jusqu'à la
capture 1-5, encodage 2-6, envoi 0,2, réseau 6-10, décodage 0,4-5, dessin
0,2-6. Hors drapeau de banc, il reste 20 à 27 ms. Un codec intra ne peut
gagner que sur l'encodage et le décodage, soit 3 à 10 ms à se partager, et la
cadence (déjà à 240 i/s) n'a plus de marge.

**Ce que cela change pour la suite du POC (proposition pour la porte U0)** :
- La référence de U3 et U5 devient l'« Auto » détecté, plus la barre §2. Le
  critère de §2 (− 2 ms ou − 20 % de médiane, p99 pas pire) se lit contre
  elle.
- Le Wi-Fi n'est pas un terrain pour Ultra. Même en HEVC à 20-30 Mbit/s, la
  fenêtre de congestion d'usrsctp retient déjà la vidéo à 120 i/s (plan Wi-Fi,
  W1). À 150-200 Mbit/s, elle ne passerait pas. Ultra reste « Ethernet
  seulement », comme prévu en §1.
- L'AV1 ne gagne rien sur le HEVC à la même barre (+1 à 4 ms, +20 à 50 % de
  débit, UM790Pro), et il casse sur le N95. Il ne sert pas de référence.

**Le 04/10 (UM790Pro, build `2ef56bfe` : `sctpburst=0` et `retrcut=3`
par défaut depuis ce build, à dire en comparant aux passes d'avant)** :
- **`6a7c3b43` est vérifié.** En « Auto » sur l'Arc, un stream AV1 monte à
  240 i/s comme un HEVC : décision prise vers 5-18 s, gardée. Le filet joue
  aussi en AV1 : une fois, une file de décodeur qui tient a fait redescendre la
  détection, qui a ensuite regagné 240 i/s. L'AV1 reste derrière : clic de
  43 ms contre 37, 40-46 Mbit/s contre 25-27.
- **Sous charge GPU** (`mw-gpu-load` à ~45 i/s, `4ba3f0d1`), deux passes par
  mode :

  | GPU chargé | Mode | Âge montré | E2E hôte → dessin | Avant la capture | Clic → drapeau |
  |---|---|---|---|---|---|
  | Arc | D | 126-127 ms | 38 ms | ~67 ms | 84-86 ms |
  | Arc | U | 129-137 ms | 39-52 ms | ~67 ms | 84-86 ms |
  | iGPU AMD | D | 163-164 ms | 13-14 ms | 137-158 ms | 135-137 ms |
  | iGPU AMD | U | 168-187 ms | 13 ms | 137-158 ms | 131-135 ms |

  Sous charge, c'est surtout la page de banc elle-même qui attend son GPU,
  avant la capture. Le stream n'ajoute que 13 ms (AMD) à 38-52 ms (Arc). U n'y
  change rien de net. Mais la fenêtre de charge était sur l'écran virtuel
  capturé, qui devient l'écran principal pendant le stream (corrigé dans
  `beeabde4`).
- **Cases refaites** (04/10, 17:04-17:25, build `ded56fc6`, fenêtre de charge
  sur un écran à part, deux passes par mode). L'écran virtuel de l'UM790Pro
  était entre-temps passé de 120 à 240 Hz, sans le banc : ce n'est pas tout à
  fait la même case que le matin.

  | GPU chargé (charge) | Mode | Âge montré | E2E hôte → dessin | Avant la capture | Clic → drapeau |
  |---|---|---|---|---|---|
  | Arc (30 i/s) | D | 192 ms | 48 ms | ~105 ms | 99 ms |
  | Arc (30 i/s) | U | 189 ms | 50 ms | ~105 ms | 96 ms |
  | iGPU AMD (47-52 i/s) | D | 107 ms | 23 ms | 70-75 ms | 93 ms |
  | iGPU AMD (47-52 i/s) | U | 118 ms | 28 ms | 70-75 ms | 97 ms |

  Le verdict ne change pas. Sous charge, la page de banc attend son GPU, U et
  D font jeu égal, et le stream reste la plus petite part de l'âge. Au même
  niveau, la charge tourne à 30 i/s sur l'Arc au lieu de 45 le matin : sa
  fenêtre, posée sur un écran physique, ne pèse plus pareil.

**Reste avant la porte U0** : les TV (U0.3 quater), la borne Steam sur l'iGPU et MoonlightWeb au même outil (U0.4) et le
rapport U0.5.

### 6.5 U0.4 — la borne Steam, hôte RTX (04/10/2026, 22:10-22:40)

Steam Remote Play (bêta « Steam Beta Update »), hôte DualRTX, l'écran
principal étant celui de la RTX (NVENC pour le HEVC). Client l'UM790Pro sous
Windows en 1 GbE (780M, décodage matériel, 1920×1080, débit automatique,
modificateur de qualité au milieu, 4:4:4 coupé). Mesure sans caméra
(`scripts/bench/photon/`, `2e0a012b`, `5de1e2af`) : l'hôte affiche une
fenêtre qui passe du noir au blanc à chaque clic, streamée comme jeu non-Steam.
Le client clique dans la fenêtre du stream et relit ce pixel sur son propre
bureau composé (GDI), jusqu'au changement. Le clic → photon compte donc tout,
de la montée du clic à la composition du client, sauf le balayage de l'écran
lui-même. 60 clics par passe, aucun manqué ; résultats dans
`bench-out/photon/steam-*.json`.

| Passe | Codec | Low Latency Networking | Médiane | p90 | Min - max |
|---|---|---|---|---|---|
| 1 | HEVC | non | 49,9 ms | 58,9 ms | 32,6 - 67,2 |
| 2 | PyroWave | non | 42,7 ms | 59,0 ms | 32,7 - 66,6 |
| 3 | HEVC | non | 57,9 ms | 66,6 ms | 40,6 - 91,7 |
| 4 | PyroWave | non | 42,6 ms | 58,3 ms | 32,4 - 83,9 |
| 5 | HEVC | oui | 58,6 ms | 83,2 ms | 41,3 - 375,8 |
| 6 | PyroWave | oui | 49,7 ms | 59,1 ms | 32,9 - 92,3 |

- ⚠️ **Correction (05/10, 00:10)** : de 22:40 à 23:00, un Chrome de banc de la
  session Wi-Fi tournait sur DualRTX, avec la RTX et DISPLAY5, l'écran
  streamé ; il était lancé là par un défaut de son outil, corrigé par `09df1c3c`.
  La passe 1 (HEVC, finie à 22:56) en est entachée. Toutes les autres passes
  ont tourné après 23:00.
- **PyroWave natif bat le HEVC de Steam de 15 ms en médiane sur la RTX, en ne
  gardant que les passes propres** : 42,6-42,7 ms contre 57,9 ms (58,6 avec Low
  Latency Networking). Avec la passe 1 entachée, l'écart était de 7 à 15 ms. Il est très stable d'une passe à l'autre,
  là où le HEVC varie de 8 ms. Les p90 sont proches (58-59 contre 59-67 ms).
  **La porte de U0.4 (au moins 2 ms sur NVIDIA) est franchie** : le POC ne se
  resserre pas sur les hôtes à iGPU.
- « Low Latency Networking » de Steam dégrade les deux codecs (+7 ms en
  médiane pour PyroWave ; pour le HEVC, une queue à 83 ms au p90 et une pointe
  à 376 ms).
- **MoonlightWeb, même couple, même outil** (23:22, `--dev` du build
  `ded56fc6`, écran de la RTX streamé en « Auto », 120 i/s, tearing, Chrome
  sur l'UM790Pro) : médiane 58,2 ms, p90 75,0 ms (42 à 108), 60 clics sur 60.
  C'est au niveau du HEVC de Steam (49,9-57,9), 15 ms derrière son PyroWave.
  Une 2e passe est invalide (58 clics manqués sur 60, 23:26:40-23:27:41). Le
  Chrome de banc de la session Wi-Fi a redémarré sur DISPLAY5 à 23:27, en plein
  écran sur l'écran streamé : c'est la cause la plus probable. Le drapeau de
  latence de la `--dev` au point du clic est l'autre suspect.
- **Les deux mesures de MoonlightWeb ne disent pas la même chose.** Son propre
  clic → drapeau donnait 33-43 ms sur ce couple (U0.3), parce qu'il lit le
  drapeau dans le canevas, au moment du dessin. `click-photon.ps1` lit le
  bureau composé du client : il compte en plus la composition de Chrome et
  celle du DWM. L'écart, environ 15 à 25 ms, est ce que coûte le chemin de
  présentation du navigateur, qu'un client natif comme Steam n'a pas. Pour
  Ultra, c'est un poste que le codec ne touche pas (U3, présentateurs au
  photon).
- **Passes du 05/10 au matin** (07:10-07:55, même couple, même outil,
  `bench-out/photon/*-3.json` et `steam-amd-*.json`) :

  | Hôte (encodeur) | Client | Passes, médiane (p90) |
  |---|---|---|
  | RTX (NVENC) | Steam HEVC | 58,5 ms (75,6) |
  | RTX | Steam PyroWave | 49,2 ms (58,0) |
  | RTX | MoonlightWeb HEVC, lecture à 200 px du clic | 57,3 ms (74,1) |
  | iGPU AMD (écran de l'AMD en principal) | Steam HEVC | 57,9 (66,7), puis 49,4 ms (59,0) |
  | iGPU AMD | Steam PyroWave | 41,7 (58,2), puis 49,6 ms (58,6) |

- **Bilan de la borne Steam, passes propres seulement** :
  - RTX : PyroWave 42,6, 42,7 et 49,2 ms contre 57,9 et 58,5 ms pour le
    HEVC, soit un gain de 9 à 16 ms ;
  - iGPU AMD : PyroWave 41,7 et 49,6 ms contre 49,4 et 57,9 ms, soit environ
    8 ms en moyenne.
  - MoonlightWeb (57,3 et 58,2 ms) est au niveau du HEVC de Steam, sur les
    deux passes.
- **Les médianes tombent sur des marches d'environ 8,3 ms** (41,7, 49,4-49,6,
  57,3-58,5), la période d'un écran à 120 Hz (le M27Q de l'hôte et le client à
  120 Hz). Les écarts entre codecs valent donc une ou deux images
  d'affichage, et une même configuration peut tomber d'une marche à l'autre
  d'une passe à l'autre. C'est une lecture, pas encore une mesure.

### 6.6 U0.5 — rapport de la phase U0 (version finale du 05/10/2026)

Le brouillon du 04/10 (`3c8ea54b`) avait servi à la porte U0, franchie le soir
même. Cette version le complète avec ce que la phase U1 a appris depuis sur le
banc. Seules les TV (U0.3 quater) manquent encore, et elles ne bloquent aucune
porte.

**0. Une correction qui vaut pour tout ce qui suit.** Le « 1 GbE » entre
DualRTX et l'UM790Pro passe par un saut Wi-Fi 7 entre deux répéteurs (§6.10) :
2,6 ms d'aller-retour, TCP à 74-90 Mbit/s, UDP propre jusqu'à ~150 Mbit/s, et un
lien partagé avec la maison. Les chiffres « Ethernet » de U0 sont donc ceux d'un
bon Wi-Fi. Le poste réseau du tableau (6-10 ms) en est gonflé d'environ 2 ms, et
le reste est inchangé. Ils seront refaits sur câble.

**1. Budget par étape, mesuré.** Les deux couples les mieux couverts, en ms :

| Étape | UM790Pro en Ethernet, hôte RTX/Arc/AMD (U0.3, flagpath) | iPhone / iPad en Wi-Fi (U0.3, détail de latence) |
|---|---|---|
| Montée du clic | 1,5-3 | — |
| Hôte : capture → remis au relais | 2-6 (encodage NVENC 1,4-2, Arc 3,5-4,5, AMF 4-4,3) | 4-6 |
| Réseau, dont l'attente dans usrsctp | 6-10, dont 3-5 | 2-20 (pointes des coupures Wi-Fi) |
| Décodage | 0,4-5 | iPhone 5-10 ; iPad 16-22 à 50-62 i/s, 8 à 26 i/s |
| Attente et dessin | 0,2-6 | 7-11 (file de rendu) |
| Âge de l'hôte au dessin (journal par image) | 8-14 médiane | iPhone 31-48 ; iPad 31-82 |

En Wi-Fi, le réseau domine quand la cadence monte. La fenêtre de congestion
d'usrsctp retient la vidéo : 186-232 ms au p90 sur le N95 à 120 i/s, 50-79 ms
sur le Mac (plan Wi-Fi, `docs/design/network-latency-findings.md`). Sur un
client mobile, c'est le décodeur. Sur le chemin de l'UM790Pro (« Ethernet »),
aucun poste ne dépasse 6 ms, sauf le réseau : celui-ci compte le saut Wi-Fi 7.

Deux postes que ce tableau ne montre pas, trouvés depuis :
- **La composition chez le client** : 15-25 ms entre le dessin dans le canevas
  et le bureau composé (U0.4). Le présentateur et le plein écran n'y changent
  rien (U3, §6.9).
- **La retenue de Chrome sur une piste vidéo RTP** : 7,8 ms en moyenne, le
  métronome à 64 Hz de Blink (§6.11). Elle ne touche pas le DataChannel de
  U0, mais tout transport RTP de la suite. La route audio la ramène à 0,2 ms.

**2. Le modèle du §3, recalé.**
- Le segment codec du HEVC est bien de 3 à 6 ms sur la RTX et l'Arc, en
  Ethernet, comme prévu.
- La cadence pèse plus que le codec, comme prévu. La détection de l'« Auto »
  la règle déjà au mieux : 240 i/s sur l'UM790Pro, 120 sur le Mac, la cadence
  de l'écran sur le N95 et l'iPad. La barre « HEVC réglé Ultra » du §2 ne
  bat donc jamais l'« Auto » détecté (§6.4).
- Ce que le modèle n'avait pas : **le chemin de présentation du navigateur.**
  Le même clic, mesuré sur le bureau composé du client (`click-photon.ps1`),
  coûte 58 ms dans Chrome contre 33-43 ms lu dans le canevas : 15 à 25 ms
  passent dans la composition de Chrome et du DWM. Steam, client natif, n'a
  pas ce poste.
- Ce que le modèle sous-estimait : **le transport.** Le §3 comptait 2,8 ms de
  sérialisation pour PyroWave à 170 Mbit/s. Le DataChannel plafonne à
  95-107 Mbit/s sur ce chemin, et fait attendre la vidéo derrière toute
  charge lourde de la même association (U1.2, §6.8). Le gain d'Ultra
  dépend donc d'abord de la piste qui le porte (U1.4 : la route audio).
- Segment codec, mesuré contre prédit : RTX 1,4-2 ms d'encodage (prédit 3,4
  avec le décodage), Arc 3,5-4,5, AMF 4-4,3 en Ethernet. L'iGPU AMD encode
  donc plus vite que prévu. Avec le décodage (0,4-5 ms), son segment HEVC
  fait 5-9 ms, pas 10,5-18,5. Contre ~3,5-4 ms pour PyroWave au seuil, la
  marge d'Ultra sur cet hôte tombe à 1-6 ms, au lieu de 4-12.

**3. La borne Steam (U0.4, RTX puis iGPU AMD, §6.5).** Sur les passes propres,
PyroWave natif bat le HEVC de Steam de 9 à 16 ms sur la RTX (42,6-49,2 contre
57,9-58,5 ms), et d'environ 8 ms sur l'iGPU AMD. MoonlightWeb, au même outil,
est au niveau du HEVC de Steam (57,3-58,2 ms). La porte de U0.4 (≥ 2 ms sur
NVIDIA) est franchie.
- Les médianes tombent sur des marches d'environ 8,3 ms, la période de l'écran
  à 120 Hz : l'écart vaut une à deux images d'affichage.
- Ce gain dépasse ce que le codec peut gagner seul : le modèle du §3 prédisait
  plutôt une légère perte sur NVIDIA, et l'encodage plus le décodage du HEVC ne
  coûtent ici que 3 à 7 ms.
- Steam semble donc mettre sur sa voie HEVC une attente que sa voie PyroWave
  n'a pas : une file de décodage ou un rythme de présentation. Ce n'est pas
  vérifié : il faudrait le relevé de performance de Steam, ou une caméra.
- Lecture prudente : PyroWave dans un navigateur ne gagnera sur notre HEVC que
  ce que le codec fait vraiment gagner, soit 3 à 7 ms en Ethernet sur ces
  hôtes. Et seulement si le décodeur et le présentateur ne reprennent pas ce
  gain (U3).

**4. Le verdict de l'essai « cadence de l'hôte »** (plan 1 `framerate-hote`,
U0.3 bis et ter). C'est la détection côté client (« Auto » avec détection, UA)
qui est sur `main` et sert de référence. `host-guarded` n'a pas battu
l'« Auto » d'aujourd'hui (UA.3).

**La barre « HEVC réglé Ultra » qui en découle** : l'« Auto » détecté lui-même
(240 i/s sur l'UM790Pro, la cadence de l'écran ailleurs), HEVC, P1, tearing.
Forcer 120 i/s avec un écran virtuel à 240 Hz ne fait jamais mieux, et fait
décrocher le lien du N95 en Wi-Fi (§6.4).

| Client, lien | « Auto » détecté : âge montré / clic → drapeau |
|---|---|
| UM790Pro, « Ethernet » | 24-27 / 35-43 ms |
| Mac M1, Wi-Fi | 29-37 / 61-75 ms |
| N95, Wi-Fi | 45-58 / 83-94 ms |
| iPhone, Wi-Fi (« Mesurée ») | 41-48 ms (120 i/s forcé : 31-40) |

L'iPhone est le seul client où forcer 120 i/s gagne (5-10 ms).

**5. La porte U0, tranchée** (Bruno, 04/10, environ 23:45 : « Ok, go ») :
- U1 lancé, avec trois corrections au plan :
  1. La référence de U3 et U5 devient l'« Auto » détecté.
  2. Ultra reste réservé à l'Ethernet, avec PyroWave au cœur.
  3. Le chemin de présentation (U3) passe avant le décodeur.
- Depuis, U3 a montré que le présentateur ne rend pas ces 15-25 ms (§6.9).
  Le levier qui reste est le transport. Le DataChannel ne porte pas Ultra
  (U1.2). La route audio le porte sur ce chemin, sans retenue de Chrome et
  sans faire attendre la vidéo (U1.4 ter et quater, §6.11).

**6. Ce qui reste ouvert de U0** :
- **Les TV** (U0.3 quater) : budget par étape sur la Mi TV et la Freebox Player
  POP, au créneau que Bruno fixera.
- **Le câble** : les passes de U0, U1.2 et U1.4 sont à refaire sur un vrai
  1 GbE, pour retirer le saut Wi-Fi 7 du poste réseau.
- **MoonlightWeb sur l'iGPU AMD au même outil que Steam** : seule la RTX a sa
  passe MoonlightWeb au photon.

### 6.7 Après la porte U0 : U1 lancé, U3 préparé (05/10/2026, nuit)

**Porte U0 franchie** (Bruno, 04/10, environ 23:45 : « Ok, go »).
- La référence de la suite est l'« Auto » détecté.
- Ultra ne vaut qu'en Ethernet, avec PyroWave au cœur.
- Le chemin de présentation passe avant le décodeur.

**U1.1 fait.**
- `e8f02ce0` côté hôte : la clé `ultra=synthetic:<Kio>` envoie, après chaque
  image vidéo, une charge incompressible de cette taille. Elle part sur le
  canal négocié id 5 (l'id 4 est celui du HID), au format des chunks vidéo et
  avec le tampon de l'image, par un `FrameSender` à part. `ultrachannel=`
  choisit la fiabilité du canal.
- `5ee304da` côté client : `mw_ultra_sink` (`UltraSink.js`) compte par seconde
  les images, les Mbit/s, les pertes, l'étalement et le retard en plus.
- `3178043e` : chaque passe de banc garde ces chiffres.
- Essai local sur DualRTX (04/10, 23:44) : 273 images de 200 Kio, aucune
  perdue, étalement de 3 à 9 ms. Les réglages de la table U1.1 que le plan
  Wi-Fi a déjà rendus clés de banc (`sctpburst`, `sctpbuf`, `sctpcc`,
  `retrcut`, `relaylog`) sont repris, pas refaits.
- **U1.2 est prêt**, à passer quand l'UM790Pro sera libre : 6 tailles de 40 à
  2000 Kio × 60 et 120 i/s, au transport du produit. Puis, à 350 et 1000 Kio
  et 120 i/s : un tampon d'envoi de 4 Mio, le canal non ordonné, et
  l'ancien burst maximal.

**U3, le chemin de présentation, à mesurer avant le décodeur.** Mesuré
au `click-photon.ps1`, le clic coûte 58 ms dans Chrome, contre 33-43 ms lu
dans le canevas : 15 à 25 ms pour la composition de Chrome et du DWM (§6.5).
Le client a déjà quatre présentateurs : Canvas2D désynchronisé (le défaut en
tearing), WebGL2, WebGPU, et `<video>` nourri au décodage (menu de débogage).
Une première matrice, sans code neuf :
- ces quatre présentateurs × en fenêtre / en plein écran, le plein écran
  pouvant laisser le DWM passer en « independent flip » ;
- sur l'UM790Pro en Ethernet (écran à 240 Hz), hôte RTX ;
- `click-photon.ps1 -ReadDx 200` lit un point écarté du clic, pour que le
  drapeau de banc (`latency_flag_enabled`) ne le masque plus (`0bceb41e`) ;
- 60 clics par case, en alternance, et le même couple refait sous Steam en
  référence native.

Ce qui en sortira : le présentateur et le mode d'écran qui rendent le plus de
ces 15-25 ms. Le décodeur d'Ultra (U3.4) se branchera sur ce présentateur-là.

### 6.8 U1.2 — ce que le DataChannel porte à haut débit (05/10/2026, 06:56-08:46)

> **Corrigé le 10/10/2026 : le plafond venait du chemin, pas du DataChannel.**
> - Entre DualRTX et l'UM790Pro, il y avait un saut Wi-Fi 7 (§6.10).
> - Sur un vrai câble, la même association SCTP porte ~530 Mbit/s (§6.16), avec
>   les mêmes messages de 16 Kio et le même MTU, trois fois les 170 Mbit/s de
>   PyroWave.
> - Ni la taille des messages ni le MTU de 1 500 o de la table U1.1 n'ont été
>   essayés depuis. Qu'ils pèsent sous ce plafond reste une hypothèse.
> - Deux passes restent inexpliquées : 200 Kio à 60 i/s ici (19 Mbit/s), et
>   350 Kio sur le câble au §6.16 (85 Mbit/s).

Hôte DualRTX : la `--dev` du build `66f71c56`, avec les défauts Windows
`sctpburst=0` et `retrcut=3`. L'écran streamé est un écran physique, DISPLAY1
(celui de l'Arc, à 120 Hz), choisi par Bruno pour ne faire aucune bascule
d'écran virtuel ; la page de banc défile dessus. Client l'UM790Pro sous
Windows, Chrome, en 1 GbE. Chaque passe ajoute `ultra=synthetic:<Kio>`
(§6.7), `relaylog=1`, 30 s d'âge du contenu et 30 clics. Médianes sur les
30 dernières secondes, lues par le client (`mw_ultra_sink`) ; le retard est
le p95 au-dessus du plus petit de la session.

| Kio / image | Débit demandé à 60 / 120 i/s | Reçu à 60 i/s | Reçu à 120 i/s | Retard p95 (60 / 120) | Vidéo à 60 / 120 i/s |
|---|---|---|---|---|---|
| 40 | 20 / 39 Mbit/s | 19,4 | 37,7 | 77 / 16 ms | 60 / 111 i/s |
| 200 | 98 / 197 | 19,2 | 98,9 | 1 427 / 397 ms | 10 / 59 i/s |
| 350 | 172 / 344 | 62,4 | 103,3 | 818 / 717 ms | 12 / 34 i/s |
| 500 | 246 / 492 | 105,7 | 95,1 | 605 / 1 320 ms | 25 / 21 i/s |
| 1 000 | 492 / 983 | 105,2 | 95,7 | 1 508 / 2 685 ms | 11 / 13 i/s |
| 2 000 | 983 / 1 966 | 106,7 | 97,0 | 3 121 / 3 961 ms | 6 / 5 i/s |

Variantes, à 120 i/s (reçu, retard p95, images Ultra perdues) :

| Variante | 350 Kio | 1 000 Kio |
|---|---|---|
| canal Ultra non ordonné, sans retransmission (`ultrachannel=unordered`) | 90,7 Mbit/s, 820 ms, 3 | 63,2, 2 809 ms, 15 |
| l'ancien burst maximal (`sctpburst=10`) | 97,0, 810 ms, 0 | 15,4, 3 421 ms, 22 |
| un tampon d'envoi de 1 Mio (`sctpbuf=1024`, le plus que la clé accepte) | 64,5, 765 ms, 11 ; **la vidéo ne s'affiche plus** | 49,5, 4 357 ms, 22 ; idem |

- **Le DataChannel plafonne à 95-107 Mbit/s sur ce lien 1 GbE**, quelle que
  soit la taille des images, à 60 comme à 120 i/s. PyroWave en demande 170 à
  son seuil subjectif (§3) : **avec le transport d'aujourd'hui, Ultra ne tient
  pas**. Au-delà du plafond, l'attente monte à des secondes, et la vidéo tombe
  avec elle.
- **Un expéditeur à part ne protège pas la vidéo.** Ultra et la vidéo
  partagent la même association SCTP, donc sa fenêtre de congestion et ses
  tampons : une charge Ultra au plafond fait tomber la vidéo à 5-25 i/s. La
  promesse de U1.1 (« jamais la vidéo ») ne vaut que pour la file de
  l'expéditeur, pas pour le lien.
- Aucune variante ne relève le plafond.
  - Le canal non ordonné perd des images.
  - L'ancien burst maximal s'effondre à 1 000 Kio : le défaut `sctpburst=0`
    est le bon.
  - Le tampon de 1 Mio fait pire, et casse la vidéo : `sctpbuf` fixe aussi la
    taille du plus grand message.
- **Un piège de banc** : une valeur hors bornes (`sctpbuf=4096`) fait ignorer
  **toute** la chaîne `MW_NATIVE_TUNING`, Ultra compris. Deux passes perdues,
  mises de côté dans `bench-out/content-age/u1-void/`.
- À 200 Kio et 60 i/s, le débit reçu (19 Mbit/s) est bien plus bas qu'à 350
  Kio (62). Ce n'est pas expliqué, ni encore refait.

**Ce que U1.3 doit trancher** : d'où vient ce plafond. L'hôte (le fil d'envoi
et libjuice) ? Le client (la réception SCTP de Chrome, la boucle de messages) ?
Ou usrsctp lui-même, dont le plan Punktfunk relevait déjà le plafond sous
pertes (A0, §8r.1) ? La table U1.1 prévoit encore le MTU de 1 500 et la
réception dans un worker. Si rien ne passe nettement 170 Mbit/s, la porte U1
ouvre la sonde du plan B (le flux Ultra dans une piste RTP, par Encoded
Transform).

### 6.9 U3 d'abord — le chemin de présentation de Chrome (05/10/2026, 08:47-09:00)

Hôte DualRTX (`--dev` `66f71c56`), qui streame l'écran de la RTX (DISPLAY5,
sans écran virtuel), le drapeau de `click-photon` par-dessus. Client
l'UM790Pro sous Windows, Chrome, en 1 GbE, « Auto » avec détection, en
tearing. Pour chaque case : 60 clics, le pixel lu à 200 px du clic
(`u3_series.py`, `pass.py --setting/--fullscreen`, `885070b2`) ;
`bench-out/photon/u3-*.json`.

| Présentateur | Fenêtre agrandie, médiane (p90) | Plein écran, médiane (p90) |
|---|---|---|
| Canvas2D (le défaut) | 58,8 ms (68,1) | 65,4 ms (74,3) |
| `<video>` nourri au décodage | 65,3 ms (73,6) | 59,4 ms (74,3) |
| WebGL2 (`gl-fsr1`) | 65,0 ms (82,2) | 65,5 ms (81,9), 2 clics manqués |
| WebGPU (`fsr1`) | 65,3 ms (74,2) | 65,7 ms (74,5) |

Repères, même couple, même outil (§6.5) : Steam PyroWave 42,6-49,2 ms,
Steam HEVC 57,9-58,5 ms, MoonlightWeb HEVC 57,3-58,2 ms.

- **Aucun présentateur ni le plein écran ne rend les 15-25 ms.** Toutes les
  cases tombent entre 58,8 et 65,7 ms : deux paliers à 6,5 ms d'écart,
  sans tendance par présentateur ni par mode. Le plein écran, qui pouvait
  laisser le DWM passer en « independent flip », ne gagne rien ici.
- Le meilleur reste le défaut (Canvas2D en fenêtre, 58,8 ms), au niveau du
  HEVC de Steam. L'écart avec PyroWave natif (10 à 16 ms) n'est donc pas une
  affaire de présentateur dans Chrome.
- **Non vérifié** : que chaque case a bien tourné avec le présentateur
  demandé. Le réglage passe par `video_enhancement_algo`, mais aucune trace ne
  le confirme après coup. Le pilote coupait la passe `pass.py` avant qu'elle
  écrive son JSON (overlay compris), et la page ne nomme son présentateur qu'à
  un endroit : la sonde de latence, et seulement sur un clic manqué
  (`LatencyProbe.js`, `via`). Une reprise doit relever le présentateur en
  direct par CDP, pendant la case. Les paliers de 6,5 ms
  laissent aussi penser que la cadence de présentation du client découpe les
  mesures, plus que le présentateur lui-même. À refaire en relevant le
  présentateur et la cadence d'affichage du client.
- Conséquence pour la suite (U3) : le gain ne viendra pas d'un autre
  présentateur, mais d'abord du transport (§6.8, plafond à 100 Mbit/s) et de
  la cadence de bout en bout.

### 6.10 Le chemin du banc, et U1.4 : la vidéo sur une piste RTP (05/10/2026, 10:00-11:50)

**Le « 1 GbE » de §6.1-6.9 n'en est pas un.** DualRTX et l'UM790Pro sont
chacun câblés à 1 Gbit/s sur un répéteur Freebox, mais les deux répéteurs
(rez-de-chaussée, 2e étage) sont reliés entre eux en Wi-Fi 7. Mesuré par un
outil socket (TCP, UDP rythmé, écho UDP ; `network-latency-findings.md`,
`4aebfc9a`) :
- aller-retour à vide de 2,6 ms en médiane, là où un câble donne ~0,3 ms ;
- TCP à 74-90 Mbit/s ;
- UDP sans perte jusqu'à 150 Mbit/s, puis ~155-175 Mbit/s reçus avec des
  pertes.

Le plafond de 95-107 Mbit/s de §6.8 vient donc d'abord du chemin. Toutes les
passes « Ethernet » entre ces deux machines (U0, U1.2, U3, Steam U0.4) l'ont
emprunté. Bruno rapproche les deux PC sur un câble le soir du 05/10 : U1.2 et
U1.4 y seront refaites.

**U1.4 (décision de Bruno, 05/10)** : la vidéo sur une piste RTP plutôt que sur
le DataChannel, pour les quatre codecs, avec un interrupteur par codec et par
type d'hôte. Mesurée exprès sur ce chemin Wi-Fi, pour ses pertes.
- Hôte (`e399d604`) : `rtp_video` dans settings.json, ou `MW_RTP_VIDEO` au
  banc, au format `native:h264+hevc+av1+ultra;other:h264+hevc+av1`. Vide par
  défaut, donc SCTP partout.
- Client (`b2f6d264`) : `RTCRtpScriptTransform` dans un worker, avant le
  décodeur de Chrome, puis le même `onVideo` que le DataChannel.
- Chrome 154 négocie H.264, H265 et AV1 en RTP.

Banc :
- **Natif** : client l'UM790Pro, Chrome, l'écran de l'Arc (DISPLAY1) streamé, la
  page de banc qui défile, 30 s d'âge du contenu, 30 clics, deux manches
  alternées (`u14_series.py`).
- **Autre hôte** : le Sunshine de DualRTX (l'écran DISPLAY5), relayé par la
  `--dev`, une manche.

| Hôte natif, médiane (manches 1 / 2) | Âge du contenu | Hôte → affiché | i/s affichées |
|---|---|---|---|
| H.264, RTP | 109 / 100 ms | 23,8 / 26,4 ms | 53 |
| H.264, SCTP | 86 / 89 ms | 15,3 / 16,0 ms | 52 |
| HEVC, RTP | 55 / 54 ms | 18,3 / 18,7 ms | 60 |
| HEVC, SCTP | 42 / 44 ms | 8,8 / 11,6 ms | 60 |
| AV1, RTP | 112 / 123 ms | 37,5 / 38,9 ms | 42 |
| AV1, SCTP | 115 / 117 ms | 32,9 / 32,9 ms | 40 |

| Sunshine (une manche) | Âge du contenu | Clic → drapeau, médiane (30 clics sur 30) |
|---|---|---|
| H.264, RTP / SCTP | 53,0 / 46,7 ms | 47,1 / 47,2 ms |
| HEVC, RTP / SCTP | 44,3 / 38,6 ms | 47,8 / 46,7 ms |
| AV1, RTP / SCTP | 52,2 / 46,5 ms | 59,9 / 48,1 ms |

Sur l'hôte natif, les clics ne passent qu'à 2-7 sur 30, en RTP comme en SCTP
(le drapeau sur DISPLAY1 n'est pas lu) : on ne s'y fie pas. L'H.264 et l'AV1 de
l'Arc encodent lentement (11 ms par image en H.264), ce qui gonfle l'âge des
deux transports de la même façon.

**Ultra (PyroWave simulé, `ultra=synthetic:250`, ~122 Mbit/s à 60 i/s), la
vidéo HEVC à côté**, deux manches :

| Transport d'Ultra et de la vidéo | Ultra reçu | Attente en plus d'Ultra, p50 / p95 | Vidéo hôte → affiché | i/s vidéo |
|---|---|---|---|---|
| RTP | 119-122 Mbit/s, 0 perte | 15 / 25-30 ms | 19,7 / 21,0 ms | 57-60 |
| SCTP | 122 Mbit/s, 0 perte | 35-52 / 62-90 ms | 42,5 / 64,8 ms | 59-60 |

- **Sans charge, RTP coûte 5 à 10 ms**, sur les trois codecs et sur les deux
  types d'hôte. Ce retard est fixe, quelle que soit la taille de l'image, et ce
  n'est pas l'hôte : il envoie l'image 4 ms après la capture dans les deux cas,
  et `sendFrame` prend 0,37 ms. Le saut du worker vers la page prend 0,2 ms.
  **C'est Chrome qui retient l'image** entre l'arrivée de son dernier paquet
  (`receiveTime` de ses métadonnées) et sa remise au transform : 7,5 ms en
  médiane, p90 14 ms, au plus 17 ms, réparties uniformément sur une période de
  60 Hz. On dirait une cadence interne de Chrome. `jitterBufferTarget = 0`
  n'y change rien, ni `--disable-features=WebRtcMetronome`. La cause exacte
  reste à trouver.
- **Avec une charge Ultra, RTP gagne nettement.** En SCTP, la vidéo attend
  derrière Ultra dans la même association (42-65 ms de l'hôte à l'affichage).
  En RTP, chaque piste a son propre chemin : la vidéo reste à 20 ms et Ultra
  attend trois fois moins.
- **Aucune perte n'a été vue sur ce chemin Wi-Fi**, en RTP comme en SCTP : les
  cas de pertes restent à mesurer, avec pertes injectées.
- Recommandation provisoire : SCTP reste le défaut pour H.264, HEVC et AV1 tant
  que la retenue de Chrome n'est pas levée. Pour Ultra, RTP est le bon
  transport : il porte PyroWave sans faire attendre la vidéo.

À refaire sur câble ce soir : le même jeu de passes, plus une passe de base en
`sctpburst=0` avec l'UM790Pro pour client (demande de la session Wi-Fi).

### 6.11 U1.4 ter : le métronome de Chrome, et la route audio (05/10/2026, 12:40-14:25)

Demande de Bruno : « il y a un gain énorme à faire sur ce point ». Même chemin,
mêmes machines, HEVC sur l'hôte natif sauf mention contraire.
`network-latency-findings.md`, `b01d9454`. Code : `36f1a492`, `c83acc30`.

**La cause.** Chrome remet les images vidéo reçues au transform sur une grille
fixe de 15,625 ms, soit 64 fois par seconde :
- **La preuve par la mesure.** Les heures de remise se calent sur cette période
  (cohérence de phase 0,98). Les heures d'arrivée des paquets ne s'y calent pas
  (0,01-0,07). Les images du DataChannel, sur le même socket, non plus.
- **La source.** `VideoMetronomeWorker`, dans
  `rtc_encoded_video_stream_transformer.cc`, met chaque image en file jusqu'au
  prochain tick d'un métronome à 64 Hz. Il est actif par défaut depuis 2024,
  pour réveiller moins souvent le JavaScript des grosses visios. Son
  interrupteur (`RTCAlignReceivedEncodedVideoTransforms`) a été retiré en
  octobre 2025.
- **Ce qui n'y change rien :**
  - l'ancienne API `createEncodedStreams`, sur le thread de la page ;
  - les réglages de minuteur de Chrome.
- **`VSyncDecoding` empire les choses :** 50 ms de retenue.
- **Les images audio ne sont pas retenues.**

**Le contournement : la « route audio ».** L'item `aroad` de `rtp_video` (banc
seulement) découpe chaque image en paquets Opus sur une piste audio, avec 8
octets d'en-tête. Le worker du transform les réassemble. La retenue de Chrome
tombe de 7,8 ms à 0,2 ms.

| Hôte → affiché, médiane | Piste vidéo RTP | Route audio | SCTP |
|---|---|---|---|
| HEVC, sans charge | 17,3 / 18,1 ms | 9,1 / 9,3 / 10,1 ms | 9,6 / 9,8 ms |
| H.264, sans charge | 23,7 ms | 14,6 ms | 14,8 ms |
| AV1, sans charge (40 i/s seulement, à refaire) | 30,9 ms | 31,1 ms | 21,1 ms |
| HEVC sous Ultra 250 (~122 Mbit/s) | 21,4 ms | 11,1-11,6 ms | 123-129 ms |

Sous la charge Ultra, l'attente en plus d'Ultra, en médiane / p95, est de :
- 16,8 / 41,4 ms sur une piste vidéo ;
- 7,2 / 22,3 ms sur la route audio ;
- 141-155 / 185-200 ms en SCTP.

- **La route audio efface toute la pénalité du RTP** : sans charge, elle fait
  jeu égal avec SCTP. Sous charge, elle garde la vidéo presque aussi fraîche
  que sans charge, et elle rend Ultra deux fois plus réactif que la piste
  vidéo.
- **Il lui manque la retransmission.** Chrome n'envoie pas de NACK pour une
  piste audio dont il ne joue rien. Sous 122 Mbit/s, il y a eu 10 demandes
  d'image clé en 2 minutes. Prochaine étape (U1.4 quater) : un NACK à nous,
  qui envoie au hôte les morceaux manquants par le canal d'entrée.
- **Le Wi-Fi 7 du banc est partagé avec la maison.** De 13h27 à 13h55, un
  iPhone mal capté streamait de la vidéo par rafales :
  - Ultra en RTP est tombé de 122 à 78-109 Mbit/s, avec des pertes ;
  - la sonde UDP cadencée restait propre jusqu'à ~200 Mbit/s ;
  - une fois l'iPhone arrêté, les chiffres du matin sont revenus.

  Les passes sous charge de 14h10-14h25 ont été faites sans lui.

**U1.4 quater, le NACK de la route audio (14h35-15h10, `a95e95d9` ; constats
`f4ad0dfd`).** Préférence de Bruno : la latence passe avant la qualité, et une
image partiellement dégradée quelques secondes est acceptable.

Le fonctionnement :
- Le worker redemande un morceau manquant dès qu'il voit le trou, par le canal
  d'entrée.
- L'hôte le renvoie depuis un historique des 60 dernières images.
- Une image vidéo complète attend au plus 15 ms une image plus ancienne, puis
  part marquée perdue.
- `MW_AROAD_DROP` (pour mille) jette des morceaux au premier envoi, au banc
  seulement.

| HEVC à vide, hôte → affiché | p50 | p90 | i/s affichées | Images abandonnées |
|---|---|---|---|---|
| Route audio, sans perte | 9,1-9,3 ms | 13,2 ms | 59,4-59,7 | — |
| Route audio, 1 % de morceaux jetés | 9,7 ms | 14,5 ms | 59,9 | 0 |
| Route audio, 5 % de morceaux jetés | 11,8 ms | 16,3 ms | 59,4 | 0 |
| SCTP, `loss=50` (5 % des messages jetés avant SCTP) | 8,4 ms | 11,2 ms | 38 | — |

- La route audio répare 5 % de pertes pour 2,5 ms de plus, sans perdre une
  seule image.
- Le DataChannel, lui, n'affiche plus qu'une image sur deux ou trois et
  demande 153 reprises en 2 minutes. Ses 8,4 ms ne comptent que les images
  arrivées intactes.
- Sous Ultra 250, avec la vidéo à côté, un morceau vidéo renvoyé attend
  derrière les rafales d'Ultra et dépasse les 15 ms : il reste 11 demandes
  d'image clé en 2 minutes. Dans le produit, Ultra remplace la vidéo au lieu de
  rouler à côté, ce cas n'existe donc qu'au banc.
- **AV1 refait** (deux manches), hôte → affiché en médiane :
  - route audio : 30,0 / 18,0 ms ;
  - SCTP : 32,9 / 31,7 ms ;
  - piste vidéo : 39,9 / 38,8 ms.

  Le 21 ms de SCTP mesuré plus tôt était du bruit.

**Recommandation provisoire, à confirmer sur câble :**
- la route audio, plutôt que la piste vidéo, pour la case RTP de l'interrupteur
  U1.4, pour les trois codecs et pour Ultra ;
- SCTP reste le défaut tant que Bruno n'a pas basculé les cases.

**À corriger avant de passer la route audio au produit hors Windows**
(constat de la session audio + DSCP, 05/10). libdatachannel marque chaque
paquet d'une piste « audio » en EF, DSCP 46 (`track.cpp:215-220`), et les
autres pistes en AF42. Sur un hôte Linux ou macOS, toute la vidéo de la route
audio partirait donc en EF. Un point d'accès qui suit la RFC 8325 la range dans
la file voix du Wi-Fi, qui n'agrège pas les trames : son débit s'effondre, et
la vraie voix de la maison en pâtit. Sous Windows, libjuice ne marque rien, et
les bancs sur DualRTX ne sont pas touchés. Il faut que la route audio porte une
marque vidéo (AF4x) et que seul le vrai son garde EF. Le plan DSCP choisira les
marques.

Nuance mesurée le 06/10 sur la Freebox du banc (D0, constats réseau
`559ed14e`, §3). La Freebox ne suit pas la RFC 8325 :
- quand le câble de l'hôte et le Wi-Fi du client sont sur le même appareil,
  tout part en BE, quel que soit le DSCP ;
- derrière un répéteur lointain (celui du N95, par exemple), elle applique
  l'ancienne règle, file = DSCP >> 3. La route audio en EF part donc en VI,
  comme la piste vidéo RTP en AF42, et non en VO. Mais le SCTP en AF11 part
  en BK, la file de fond.

Sur un hôte Linux ou macOS, une comparaison des routes vers un client
lointain mêle donc la différence de file à celle du transport : SCTP en BK
contre la route audio en VI. La correction reste due pour un point d'accès
qui suit la RFC 8325.

### 6.12 U2.4 : le PyroWave de référence, l'oracle des portages (05/10/2026, soir)

Priorité donnée par Bruno, le 05/10 au soir : « compléter l'implémentation de
PyroWave, je veux savoir les résultats ». L'ordre retenu avec le coordinateur :
l'oracle, puis le décodeur WebGPU (U3.4), puis l'encodeur HLSL (U2.5), puis
l'intégration U4.

**L'outil** (`e6d2a275`). `mw-pyrowave-ref` compile le PyroWave de l'amont
(Hans-Kristian Arntzen, MIT, commit `509e4f88`), en Vulkan, depuis l'arbre
vendorisé par punktfunk. Cet arbre ajoute des correctifs qui ne changent pas le
flux. L'outil encode et décode des Y4M 4:2:0, et mesure le PSNR. Il n'entre ni
dans le build du produit, ni dans l'installeur.
- `scripts/bench/ultra/make_corpus.py` : un lot d'images de test en 1080p,
  60 images par clip, rangé hors du dépôt. Il comprend du texte qui défile,
  des dégradés, un damier fin avec des lignes de 1 px, du bruit, et 60 images
  du clip de jeu du banc (`cod.webm`). Rien n'est capturé à l'écran.
- `scripts/bench/ultra/oracle.py` : passe tout le lot par GPU et par débit, et
  garde les flux et les images décodées pour les portages.

**Ce que l'oracle dit déjà** (temps GPU de la bibliothèque, par image,
1080p, en ms) :

| Clip | Débit | PSNR-Y (RTX / iGPU AMD) | Encodage RTX / AMD | Décodage RTX / AMD |
|---|---|---|---|---|
| Jeu | 170 Mbit/s | 51,9 / 50,3 dB | 0,14 / 1,9 | 0,10 / 1,7 |
| Texte | 170 Mbit/s | 33,4 / 33,3 dB | 0,15 / 2,2 | 0,10 / 1,2 |
| Texte | 250 Mbit/s | 44,5 / 43,9 dB | 0,16 / 2,2 | 0,10 / 1,4 |
| Dégradés | ≤ 97 Mbit/s (n'a pas besoin de plus) | 67,0 / 54,3 dB | 0,10 / 1,3 | 0,10 / 1,3 |
| Damier, bruit | 170 Mbit/s | 16-17 dB (incompressibles) | 0,21-0,23 / 2,9-3,6 | 0,10 / 1,4 |

- **Sur la RTX, le codec est pratiquement gratuit** : 0,1-0,24 ms d'encodage,
  0,1 ms de décodage, contre 1,4-2 ms pour NVENC (§6.6).
- **Sur l'iGPU AMD, il coûte 1,3-3,7 ms à l'encodage**, contre 4-4,3 ms pour
  AMF. Le gain sur cet hôte est donc de 1 à 3 ms, au bas de la marge du rapport
  U0.5. Les temps viennent d'images isolées sur un GPU au repos : ils seront
  repris en continu.
- **Le texte est le cas dur.** 33 dB à 170 Mbit/s, 44,5 dB à 250 : un texte
  fin demande plus que le seuil subjectif publié. Le jugement à l'œil de Bruno
  (U2) devra porter sur du texte.
- **L'encodeur Vulkan de l'amont est faux sur l'Arc A380** : la moitié droite
  de l'image sort grise (PSNR-Y 10 dB). Le décodeur de l'Arc lit bien le flux
  de la RTX, c'est donc l'encodeur qui est en cause, sans doute ses tailles de
  sous-groupe. L'oracle tourne donc sur la RTX. Le portage HLSL (U2.5) devra
  être vérifié à part sur l'Arc.
- **Un piège de la machine** : une couche Vulkan implicite (de capture)
  plante la création du périphérique. L'outil coupe les couches implicites.

### 6.13 U3.4 : le décodeur PyroWave dans Chrome, en WebGPU (05/10/2026, soir)

**Le décodeur** (`96383922`, `95c71a44`) : `frontend/js/stream/ultra/PyroWaveDecoder.js`,
un portage en WGSL du décodeur de l'amont (MIT, en-tête et provenance dans le
fichier), sans `subgroups`.
- Les sommes cumulées passent par la mémoire du groupe de travail. Le même code
  vaut donc aussi pour Safari et les mobiles.
- Les coefficients sont dans un seul tampon f32.
- La transformée inverse travaille par tuiles de 32×32 en mémoire partagée, en
  une passe par niveau.
- Le bord : l'échantillonneur miroir de l'amont, avec ses décalages, revient à
  l'extension symétrique de JPEG 2000 sur le signal entrelacé. Le portage la
  calcule directement.

Le labo est `scripts/bench/ultra/decoder-lab.html`, avec son pilote
`decoder_lab.py`. Il tourne dans un Chrome headless à lui, sans aucune fenêtre,
et vise un GPU par `--use-adapter-luid`.

| 1080p, 170 Mbit/s | Écart à l'oracle | Décodage GPU dans Chrome (p50) | Amont en Vulkan (dequant + iDWT) |
|---|---|---|---|
| RTX 5060 Ti | ≤ 1 code, 5 clips × 3 débits | 0,17 ms | 0,10 ms |
| Arc A380 | ≤ 1 code | 1,16 ms | 0,43 ms |
| iGPU AMD (2 CU) | ≤ 1 code | 5,1 ms | 1,3-1,7 ms |

- **Le jalon est tenu** (cible : ±2 codes). La justesse est la même sur les trois
  GPU.
- **La vitesse est bonne sur une carte dédiée, à reprendre sur un petit GPU.**
  Sur l'iGPU AMD, la déquantification égale l'amont (1,1 ms contre 0,6-1,0).
  La transformée inverse, elle, coûte 4 ms contre 0,7. Ni les barrières entre
  passes, ni les fréquences de repos, ni les contrôles de bornes de Chrome n'en
  sont la cause. C'est le nombre d'opérations par échantillon : l'amont lit
  4 texels d'un coup par `textureGather`, laisse le matériel faire le miroir,
  et calcule en FP16. Ce sera la piste à suivre, à mesurer d'abord sur le 780M
  de l'UM790Pro (12 CU), un vrai client Ultra.
- **Sans les contrôles de bornes de Chrome** (`disable_robustness`), la version
  committée reste juste, sur la RTX comme sur l'AMD. Elle ne fait donc aucun
  accès hors limites. Une version intermédiaire, sans tuiles, sortait faux
  ainsi.
- **La conversion en 8 bits** (0,6 ms sur l'AMD) ne sert qu'au labo. Le
  produit dessinera directement depuis les plans f32.
- **Sur le 780M de l'UM790Pro** (05/10, 21:10), Chrome headless dans la
  session minis, sans fenêtre : au plus 1 code d'écart avec l'oracle, et 2,0 ms
  par image 1080p. Ce temps se répartit en 1,44 ms d'iDWT, environ 0,35 ms de
  déquantification et 0,21 ms de conversion, qui ne sert qu'au labo. Dans le
  produit, le décodage prendrait donc environ 1,8 ms : il tient dans le budget
  de ce client.

### 6.14 U2.5 : l'encodeur PyroWave en HLSL sur D3D12 (05/10/2026, soir)

**L'encodeur** (`9c4ddb39`) :
`backend/native-host/tools/pyrowave-d3d12/src/PyroWaveEncoder12.{h,cpp,hlsl}`.
C'est la classe que reprendra l'encodeur Ultra du moteur (U4). Elle porte les
six passes de l'amont : DWT, quantification, analyse et résolution du débit,
assemblage. Le découpage en paquets se fait côté CPU, comme dans l'amont.
- **SM 5.0, sans instruction de vague.** Les opérations de sous-groupe de
  l'amont deviennent un thread par bloc 8×8 (la quantification) ou par bloc
  32×32 (l'analyse et l'assemblage), qui travaille en série. Rien ne dépend de
  la taille des vagues, alors que l'encodeur Vulkan de l'amont est faux sur
  l'Arc. Le tout compile avec le `d3dcompiler` que le moteur utilise déjà.
- **Ce qui diffère de l'amont.** Les coefficients sont en f32, et chaque
  sous-bloc 4×2 a un emplacement fixe de 16 octets, sans allocation atomique.
  Le flux n'est donc pas identique octet pour octet, mais sa qualité est la
  même.
- **Un outil autonome**, avec son propre dossier de build. Il ne touche pas à
  `build/`, dont se servent les `--dev` des autres sessions.

**Vérifié sur WARP** (le D3D12 logiciel), à 170 Mbit/s, sur 10 images :

| Clip | PSNR-Y contre la source | Amont (oracle) | Débit obtenu |
|---|---|---|---|
| Texte | 33,39 dB | 33,38 dB | 169,97 Mbit/s |
| Jeu | 51,64 dB | 51,63 dB | 169,98 Mbit/s |

- Les 20 920 blocs ont exactement la taille qu'ils annoncent, et le décompte de
  l'en-tête de trame est juste. Le décodeur WebGPU (§6.13) les lit.
- La couche de débogage, avec la validation côté GPU, ne signale rien, à 170
  comme à 60 Mbit/s.

**Un incident, et ce qu'il a appris.** Le premier essai sur la RTX a provoqué
sept réinitialisations du pilote NVIDIA (TDR, 20:27-20:28). La RTX pilote
l'écran principal de Bruno. La prod et la `--dev` de la session Wi-Fi ont
survécu. Deux défauts en étaient la cause :
- **Une lecture hors du tampon.** Un descripteur racine n'a pas de taille, donc
  rien ne borne un accès, et le compilateur peut exécuter les deux côtés d'une
  branche. Les niveaux de la DWT qui lisent un plan LL calculaient quand même
  une adresse dans la source 8 bits, hors de celle-ci. Chaque adresse est
  désormais bornée dans son tampon.
- **Un décalage variable de 24 bits qui donnait 0** (d3dcompiler + WARP). Les
  octets se placent désormais par des décalages constants. Un auto-test
  (`SelfTestCS`, `--selftest`) garde le cas.

Depuis, l'outil tourne sur WARP sauf si `--vendor` désigne un GPU. Le passage
sur un vrai GPU attend le feu vert du coordinateur.

**Les temps GPU** (20:41-20:42, au feu vert du coordinateur, un GPU à la fois,
`--repeat 50` sur 10 images 1080p à 170 Mbit/s). Aucune erreur D3D12, et
aucun événement de pilote dans le journal Système après les passes.

| GPU | Encodage p50 (p99) | Dont DWT / quantification / assemblage | Amont en Vulkan | Encodeur matériel du produit |
|---|---|---|---|---|
| RTX 5060 Ti | 0,63-0,68 ms (0,66-0,73) | 0,15 / 0,11-0,14 / 0,24-0,32 | 0,14 ms | NVENC 1,4-2 ms |
| iGPU AMD (2 CU) | 8,0-8,2 ms (8,5-8,7) | 4,5 / 1,2-1,6 / 1,2-1,4 | ~2-3 ms | AMF 4-4,3 ms |
| Arc A380 | 4,4-4,5 ms (4,5-7,8) | 0,7 / 2,8-6,1 / 0,5-0,6 | faux (§6.12) | VE 3,5-4,5 ms |

- **Sur la RTX, c'est déjà moins que NVENC**, mais quatre fois l'amont : les
  passes en série (un thread par bloc) et l'assemblage octet par octet coûtent.
- **Sur l'Arc (22:19, au feu vert), c'est à peu près la vitesse de VE.** La
  quantification en prend l'essentiel : un thread par bloc 8×8, qui boucle en
  série, convient mal aux GPU Intel.
- **Sur le petit iGPU AMD, c'est plus lent qu'AMF.** Comme pour le décodeur
  (§6.13), ce GPU à 2 CU est limité par le calcul, et la DWT en prend la
  moitié. Paralléliser les passes en série et alléger la DWT est la suite côté
  hôte, à mesurer aussi sur le 780M.

**Reste** : l'intégration (U4) : l'encodeur dans le moteur, la
route audio comme transport, et le décodeur de §6.13 dans le client.

### 6.15 U4 : PyroWave dans MoonlightWeb, derrière des clés cachées (05/10/2026, nuit)

Écrit sans banc ni GPU réel. La première mesure de bout en bout attend le feu
vert du coordinateur.

**L'hôte** (`3c1e908a`, `e28183a2`, `e27da614`).
- `PyroWaveEncoder12` est désormais un encodeur du moteur
  (`src/encode/windows/d3d12/`). L'outil de labo compile le même fichier.
- `UltraEncoder12` est un troisième `IVideoEncoder12` de la route D3D12, à côté
  de VE, NVENC et AMF.
  - Il attend la conversion sur le GPU, puis copie les deux plans de l'image
    NV12 dans un tampon, à leurs empreintes de copie.
  - Il encode sur une file COMPUTE à lui, avec la priorité que le moteur donne
    à ses files.
  - Il rend l'image comme une image clé : tous ses paquets PyroWave bout à
    bout. Une demande d'image clé ne lui coûte rien, et une image perdue ne
    demande aucune réparation.
- La DWT lit la chroma entrelacée du NV12 (`kind` 2). Sur WARP, le flux est
  identique octet pour octet à celui de la source en plans séparés, et la
  validation côté GPU ne signale rien.
- Les clés de banc : `pipeline=d3d12,enc12=pyrowave`, et `ultrambps=<Mbit/s>`
  (170 par défaut). La route D3D12 l'accepte sur ses trois GPU : il n'y a ni
  codec à négocier, ni vague de rafraîchissement à obtenir. Les tests natifs de
  choix de route le couvrent.
- Le transport ne change pas : c'est la route audio, avec le numéro d'image
  (`native:hevc+aroad`).
- **Un écart avec le plan du transport (noté le 10/10/2026).** U4 envoie
  l'image entière, coupée en octets comme une image HEVC, et non en messages de
  blocs entiers comme le prévoyait le plan : des morceaux de 1 100 o, réparés
  par NACK. La page attend donc l'image complète, et abandonne une image restée
  incomplète. Les paquets décodables seuls de
  l'amont (`packetize`, 8 Kio) n'ont pas encore été essayés.

**La page** (`82ba6d5f`, `216c690f`).
- `PyroWaveDecoder.present()` dessine l'image en RGB (BT.709, plage limitée)
  dans un canevas WebGPU. Le chemin complet décodage → OffscreenCanvas →
  `VideoFrame` → Canvas2D reste à 3 niveaux près de la conversion faite en
  JavaScript sur l'image de l'oracle.
- `UltraPlayer` garde une seule image en vol, et la plus fraîche en attente
  gagne. Il rend une `VideoFrame`.
- Avec `localStorage mw_ultra=pyrowave`, StreamView dimensionne le lecteur
  d'après le premier en-tête de début de trame. Il fait ensuite passer ses
  images par `onDecodedFrame` : la cadence, le présentateur Canvas2D, le
  journal par image et la sonde de latence marchent sans changement.

**La première passe proposée** : la `--dev` de `build/` (seul exe à avoir sa
règle de pare-feu), avec `MW_NATIVE_TUNING=pipeline=d3d12,enc12=pyrowave` et
`MW_RTP_VIDEO=native:hevc+aroad`, l'hôte RTX, et Chrome sur l'UM790Pro avec
`mw_ultra=pyrowave`. Puis la même passe en HEVC, sur le même chemin, pour
comparer.

### 6.16 Sur un vrai câble : NetProbe, U1.2, U1.4 et UA.4 rejoués (06/10/2026, 14:34-15:30)

DualRTX hôte, l'UM790Pro client sous Windows, en câble seul sur le commutateur
de DualRTX (`Ethernet 2`, 1 Gbit/s). `build/` de 12:10. Tout passe sur l'écran
virtuel du produit à 120 Hz (240 Hz pour U1.2 et le mode U), et non sur l'écran
physique de l'Arc comme au §6.10. Il y a eu 23 passes, toutes enregistrées
(`scratchpad/cable_run.sh`, noms `u1c-*`, `u14c-*`, `u03-umcab-*`).

**Le chemin (NetProbe).** Aller-retour UDP à vide p50 0,38 ms (2,6 ms par le
Wi-Fi 7), TCP 948 Mbit/s sur un ou quatre flux (74-90). L'UDP cadencé passe
sans perte jusqu'à 500 Mbit/s, avec un délai en plus p99 ≤ 2,8 ms ; à
800 Mbit/s, 0,4 % de pertes. Le commutateur virtuel Hyper-V de DualRTX ne
compte pas. Le plafond de 150 Mbit/s du §6.10 venait bien du saut Wi-Fi.

**U1.2, le DataChannel à haut débit** (source synthétique, 120 i/s) :

| Taille | Demandé | Porté | Âge de la vidéo à côté |
|---|---|---|---|
| 350 Kio | 344 Mbit/s | 85 Mbit/s | 697 ms, RTT 151 ms |
| 1 000 Kio | 983 Mbit/s | 528 Mbit/s | 257 ms |
| 2 000 Kio | 1,97 Gbit/s | 534 Mbit/s, 9 perdues | — |
| 1 000 Kio, canal non ordonné | | 524 Mbit/s | délai p95 365 ms (527) |
| 1 000 Kio, `sctpburst=10` | | 513 Mbit/s | délai p95 376 ms |

- L'association SCTP porte jusqu'à ~530 Mbit/s, trois fois les 170 de
  PyroWave. Au-delà de ce qu'elle porte, la vidéo qui partage l'association
  attend des centaines de millisecondes.
- La passe à 350 Kio s'est effondrée à 85 Mbit/s : elle demandait pourtant
  moins que ce que 1 000 Kio a porté. C'est la seule passe de ce genre ;
  elle est à refaire, avec 200 Kio, avant d'en tirer quoi que ce soit.
- Ultra 250 Kio à 60 i/s (123 Mbit/s, ci-dessous) tient sans gêner la vidéo.

**U1.4, les trois transports** (médianes ; âge = âge du contenu montré,
hôte → dessin = journal par image ; deux manches, sauf la route audio sous
Ultra) :

| | SCTP | Piste vidéo RTP | Route audio |
|---|---|---|---|
| HEVC seul, âge | 23,1 / 23,2 ms | 32,9 / 31,6 ms | 23,3 / 23,1 ms |
| HEVC seul, hôte → dessin | 8,8 / 8,7 ms | 19,2 / 18,7 ms | 8,7 / 9,1 ms |
| HEVC seul, clic | 47,3 / 47,1 ms | 55,2 / 59,6 ms | 48,7 / 42,2 ms |
| Ultra 123 Mbit/s à côté, âge | 23,3 / 22,1 / 22,7 ms | 31,3 / 31,6 ms | 22,5 ms |
| Ultra, délai en plus du flux Ultra p50 | 7,0 / 5,9 / 8,0 ms | 13,4 / 16,3 ms | 6,6 ms |

- Sur le câble, SCTP ne pénalise plus la vidéo quand Ultra passe à côté (en
  Wi-Fi : 125 ms, §6.11). La route audio fait jeu égal avec lui.
- La piste vidéo garde ses 9-10 ms de retenue (le métronome de Chrome, §6.11),
  et Chrome y envoie toujours ses PLI en boucle.

**5 % de pertes injectées (HEVC)** :

| | Âge | Images dessinées par seconde | Clics vus | Perdus |
|---|---|---|---|---|
| Route audio (`MW_AROAD_DROP=50`) | 24,5 ms | 58,7 | 29/30 | 0 % |
| SCTP (`loss=50`) | 22,6 ms | 31,9 | 20/30 | 6,8 % |

La réparation de la route audio coûte +1,3 ms et ne perd rien. SCTP en perd
une sur deux, comme en Wi-Fi.

**UA.4, l'Auto détecté sur le câble** (cellule Arc, deux manches) :

| | Mode | Âge montré | p99 |
|---|---|---|---|
| D | Auto détecté | 14,4 / 14,6 ms | 21,7 / 22,0 ms |
| U | 120 i/s forcés, écran virtuel à 240 Hz | 19,4 / 18,5 ms | 32,1 / 25,2 ms |

L'Auto monte à 120 i/s et bat l'Ultra forcé de 4-5 ms.

**Ce que ça tranche.**
- Pour U1 : sur 1 GbE, le DataChannel a la place de PyroWave. Le plafond du
  §6.8 venait du chemin, pas de SCTP.
- La recommandation du §6.11 tient. La route audio devient la case RTP de
  l'interrupteur. Elle égale SCTP sur un lien propre, ne perd rien sous
  pertes, et ne retient rien comme la piste vidéo.
- Reste à comprendre la passe à 350 Kio.

### 6.17 PyroWave de bout en bout, contre HEVC, sur le câble (06/10/2026, 18:30-19:15)

Même banc qu'au §6.16 : l'hôte DualRTX encode sur la RTX, l'écran virtuel du
produit est en 2560×1440 à 120 Hz, le flux à 60 i/s, et le client est
l'UM790Pro sur le câble. PyroWave passe par les clés du §6.15 :
`pipeline=d3d12,enc12=pyrowave` côté hôte, `mw_ultra=pyrowave` côté page. Il
envoie des images de ~354 Ko, soit ~155 Mbit/s. La série compte deux manches
alternées et 30 clics par passe (`scratchpad/pw_run.sh`, noms `u14p-*`).

| Médianes | HEVC, route audio | PyroWave, route audio | HEVC, SCTP | PyroWave, SCTP |
|---|---|---|---|---|
| Hôte → dessin | 7,2 / 7,2 ms | 15,9 / 15,7 ms | 7,0 / 7,1 ms | 17,6 / 15,9 ms |
| Clic | 42,9 / 41,2 ms | 48,2 / 48,3 ms | 42,7 / 42,0 ms | 54,8 / 51,2 ms |
| Âge montré | 25,6 / 25,7 ms | 35,3 / 26,1 ms | 25,4 / 8,9 ms | 35,9 / 26,3 ms |
| Images dessinées par seconde | 59,9 | 60 / 59,9 | 60 | 60 |

- PyroWave marche sur les deux routes, sans perte et à 60 i/s. Il coûte
  pourtant ~9 ms de plus que HEVC de l'hôte au dessin, et 6-10 ms de plus au
  clic.
- L'âge montré dépend de la phase de l'écran : il varie de 9 à 26 ms d'une
  manche à l'autre pour le même HEVC. Il ne départage pas les deux codecs ici.
- **La piste vidéo RTP ne porte pas PyroWave** (deux passes, aucune
  enregistrée). L'hôte y emballe l'image comme du HEVC : il la découpe aux
  codes de début de NAL. Le flux d'ondelettes contient de faux codes de début,
  et l'hôte les coupe (`[HEVC-PATCH] NAL[0..19]` sur un même paquet). Il
  faudrait un emballage propre à PyroWave. La route audio fait déjà mieux que
  cette piste en HEVC (piste vidéo : 22 ms de l'hôte au dessin).
- Piège du banc : `mw_ultra=pyrowave` reste dans le profil du Chrome de banc
  d'une passe à l'autre. Une passe HEVC qui suit doit poser `mw_ultra=off`,
  sinon la page lit du HEVC comme du PyroWave et ne dessine rien.

Reste à expliquer les ~9 ms : le temps d'encodage sur la RTX, le décodage
WebGPU sur la 780M, la taille des images, ou le chemin de présentation de
`UltraPlayer`.

### 6.18 D'où viennent les ~9 ms de PyroWave (06/10/2026, 19:20-19:35)

**Le découpage par image** se lit dans les relevés du §6.17, sans nouvelle
passe (`scratchpad/pw_split.py`). Les postes sont :
- l'encodage : de la capture à la remise au relais, sur l'horloge de l'hôte ;
- le trajet : de cette remise à l'arrivée de l'image entière dans la page. Il
  comprend l'envoi, le câble et Chrome ;
- le décodage : de l'arrivée à l'image décodée ;
- le dessin.

| Médianes (p90) | HEVC | PyroWave | Écart |
|---|---|---|---|
| Encodage (RTX) | 3,8 ms (4,5) | 2,5 ms (3,5) | −1,3 ms |
| Trajet | 1,7 ms (3,7) | 5,3-7,0 ms (10) | +3,5 à +5 ms |
| Décodage (780M) | 0,7 ms (1,1) | 6,9 ms (8,5) | **+6,2 ms** |
| File et dessin | 0,3 ms | 0,2 ms | 0 |

- **L'encodeur n'est pas en cause** : il est plus rapide que NVENC.
- **Le trajet tient à la taille.** Une image de 354 Ko demande 2,8 ms de
  sérialisation sur 1 GbE, quel que soit le transport. L'image HEVC est 5 à
  10 fois plus petite. Comme l'image part entière puis se décode entière, ce
  temps s'ajoute aux autres au lieu de se recouvrir avec eux.
- **Le décodage est le premier poste.**

**Le décodage, poste par poste.** `UltraPlayer` note maintenant chaque étape.
Le GPU est mesuré par timestamp-query, une image sur huit. Le relevé est dans
`globalThis.__mwUltraPlayer.summary()`, que `pass.py` range sous
`ultraPlayer`. Deux passes (`u14q-*`), médianes :

| | Route audio | SCTP |
|---|---|---|
| Lecture des paquets | 0,1 ms | 0,1 ms |
| Préparation et envoi du travail | 0,2 ms | 0,2 ms |
| Envoi → `onSubmittedWorkDone` | 6,1 ms | 6,4 ms |
| dont GPU, décodage (déquantification + iDWT) | 3,15 ms | 3,15 ms |
| dont GPU, affichage en RGB | 0,59 ms | 0,59 ms |
| VideoFrame depuis le canevas | 0,1 ms | 0,1 ms |
| Décodage complet, vu du journal | 6,5 ms | 6,7 ms |

- **La passe « pack » est retirée.** La conversion en 8 bits ne sert qu'au labo
  (§6.13), puisque l'affichage lit les plans f32. Le décodage gagne environ
  0,4 ms (6,9 → 6,5-6,7 ms).
- **Le GPU travaille 3,7 ms sur les 6,1 à 6,4 ms d'attente.** À 1440p, 3,15 ms
  de décodage correspondent bien aux 1,8 ms du labo à 1080p (§6.13).
- **Rendre l'image dès l'envoi ne gagne rien à l'écran.** Interrupteur de banc
  `mw_ultra_early=1` : la VideoFrame part à la page dès la soumission, sans
  attendre `onSubmittedWorkDone`.
  - L'hôte → dessin tombe à 8,1 ms (route audio) et 9,4 ms (SCTP).
  - Le clic, lui, ne bouge pas : 45,4 contre 41,6 ms, et 48,0 contre 48,2 ms.
  - Le gain n'était qu'une heure notée plus tôt : le dessin attend de toute
    façon la fin du GPU. Les ~2,4 ms hors timestamps ne sont donc pas seulement
    un rappel tardif. L'interrupteur reste éteint par défaut.
  - Piège du banc : `mw_ultra_early` reste lui aussi dans le profil du Chrome
    de banc. Les passes suivantes posent `mw_ultra_early=0`.
- **Le clic départage moins que l'hôte → dessin.** Sur ces quatre passes, il
  vaut 41,6 à 48,2 ms, et la route audio égale déjà le HEVC (41-43 ms). À
  30 clics par passe, un écart de ±4 ms reste du bruit.

**Ce que disent les sources** (recherche du 06/10 : le blog de l'auteur, les
dépôts `pyrowave` et `pyrofling`, la PR #355 de Nova, Hacker News, la doc
NVENC et GeForce NOW). Notre résultat est cohérent, pas anormal.
- **Les chiffres de l'auteur sont ceux d'un gros GPU en natif** : sur une
  RX 9070 XT, 0,13 ms d'encodage à 1080p et moins de 0,1 ms de décodage. Il
  ne publie aucune mesure de bout en bout. Son client `pyrofling` est un client
  natif Vulkan, qui prend la dernière image prête à chaque cycle, sans
  décodage par tranches.
- **Le gain promis se fait surtout côté client**, contre un décodeur matériel
  qui retient des images. Sur une Retroid Pocket 6 en 1080p60, Nova mesure 1,9
  à 2,7 ms en PyroWave contre 11 ms en HEVC. S'y ajoutent l'absence de file et
  de lookahead dans l'encodeur, un temps fixe, et des pertes qui ne coûtent
  qu'un bloc flou.
- **Notre HEVC est déjà dans le cas favorable** : 3,8 ms de NVENC et 0,7 ms de
  WebCodecs sans retenue. Il n'y a presque plus rien à gagner, alors que
  PyroWave paie 2,8 ms de sérialisation et un décodage WebGPU sur un iGPU.
- **GeForce NOW joue surtout sur la cadence** : 240 et 360 i/s, Reflex côté
  serveur, Adaptive Sync. NVENC sait aussi rendre la main par tranche
  (`enableSubFrameWrite`).

**Pistes, par gain attendu :**
1. **Envoyer et décoder par tranches.** Le format s'y prête (blocs 32×32
   indépendants). La sérialisation se recouvrirait alors avec l'encodage et le
   décodage : 2 à 4 ms attendues.
2. **Accélérer l'iDWT en WGSL** : lecture de 4 texels à la fois, FP16
   (§6.13). Sur un petit GPU, c'est 4 fois l'amont : de l'ordre de 1,5 ms à
   gagner.
3. **Passer à 120 i/s à débit égal**, avec des images deux fois plus petites.
   Ou baisser le débit à ~100 Mbit/s.
4. Même tout cela fait, PyroWave rejoindrait le HEVC sur ce client (7 à 9 ms)
   sans le battre nettement. Son terrain reste les clients dont le décodeur
   retient des images (TV, mobiles, certains Mac) et les liens avec pertes.

### 6.19 PyroWave à 120 images/s : il passe devant le HEVC (06/10/2026, 19:40)

> **Corrigé le 10/10/2026 : ce verdict est faux contre le HEVC du produit.**
> - Le HEVC de cette passe et du §6.20 passait par la piste vidéo RTP
>   (`U14_SMOKE_SCTP=0`). Il payait donc la retenue de Chrome sur cette piste
>   (§6.11, ~8 ms par image), que le HEVC du produit, par SCTP, ne paie pas.
> - La sonde du clic d'avant `652fc726` comptait aussi ~4,5 ms d'elle-même par
>   clic. Les valeurs absolues ne valent plus, seuls les écarts à cadence égale.
> - Rejoué contre SCTP au §6.26 : PyroWave perd ~7,5 ms par image de la
>   capture au dessin (10,7 contre 3,5 ms), et ~8,5 ms au clic.
> - Avec la relance et `early`, à l'écran du client : 24,3 ms du clic à
>   l'écran, contre 22,5 pour le HEVC (§6.38-6.39). PyroWave reste derrière,
>   de ~2 ms.

Même banc qu'au §6.18 (RTX → câble → UM790Pro, écran virtuel à 120 Hz), mais
le flux à 120 images/s, et 60 clics par passe au lieu de 30. PyroWave par la
route audio, HEVC par la piste vidéo RTP.

| Médianes, ms         | HEVC 120 | PyroWave 120 |
| -------------------- | -------: | -----------: |
| Clic → drapeau       |     41,4 |     **32,6** |
| Clic p90             |     59,0 |         43,7 |
| Montré (âge à l'œil) |     37,2 |         24,9 |
| Hôte → dessin (e2e)  |     13,0 |         10,6 |
| Décodage GPU (pyro)  |        — |         1,77 |
| Présentation GPU     |        — |         0,33 |
| Attente du GPU       |        — |          4,6 |

- À 120 images/s, chaque image PyroWave fait la moitié des octets : la
  sérialisation tombe vers 1,4 ms.
- Le GPU de la 780M décode en 1,77 ms au lieu de 3,15 ms à 60 images/s : la
  charge continue garde ses horloges hautes.
- Le HEVC à 120 images/s se dessine tard (« drawn » 33 ms, 3 200 images
  répétées par minute) : le décodeur ou le dessin de la page ne suit pas la
  cadence sur ce client. PyroWave n'en répète que 800.
- L'écart au clic (−8,8 ms sur 60 clics) dépasse le bruit (±4 ms à 30 clics).

**Verdict.** La promesse de PyroWave tient à haute cadence : c'est là que le
HEVC ralentit et que les images PyroWave deviennent assez petites. La suite :
confirmer en répétant les passes, puis 120 images/s comme cadence d'Ultra.

### 6.20 Trois passes de plus : l'avance se réduit, mais reste (06/10/2026, 20:10)

> **Corrigé le 10/10/2026** : comme au §6.19, le HEVC de ces passes était celui
> de la piste vidéo RTP. L'avance de PyroWave (−2,4 ms en médiane, ~2 ms de
> l'hôte au dessin, 4× moins d'images répétées) se mesure contre cette piste,
> pas contre le HEVC du produit. Contre SCTP, PyroWave est derrière (§6.26,
> §6.38-6.39). Ce qui reste : 120 images/s comme cadence d'Ultra, et la
> conclusion sur la Freebox.

Même banc qu'au §6.19, trois passes de plus de chaque, en ordre alterné
(PyroWave, HEVC, HEVC, PyroWave, PyroWave, HEVC), 60 clics par passe.

| Clic → drapeau, médiane, ms | Passe 0 | Passe 1 | Passe 2 | Passe 3 | Les 4 réunies |
| --------------------------- | ------: | ------: | ------: | ------: | ------------: |
| HEVC 120                    |    41,4 |    42,3 |    33,8 |    30,9 |          35,2 |
| PyroWave 120                |    32,5 |    38,3 |    29,8 |    31,2 |      **32,8** |

| Les 4 passes réunies, ms    | HEVC 120 (227 clics) | PyroWave 120 (229 clics) |
| --------------------------- | -------------------: | -----------------------: |
| Clic, médiane               |                 35,2 |                 **32,8** |
| Clic, moyenne               |                 38,6 |                     34,4 |
| Clic p90                    |                 49,0 |                     43,6 |
| Hôte → dessin (e2e), passes |            12,7-13,2 |                10,2-11,2 |
| Images répétées / min       |          3 230-3 410 |                  620-1 030 |

- L'écart de −8,8 ms du §6.19 était en partie un tirage : le HEVC varie d'une
  passe à l'autre (deux passes vers 42 ms, deux vers 31-34 ms). Réunies,
  l'écart est de −2,4 ms en médiane, −4,2 ms en moyenne, −5,4 ms au p90.
- Ce qui ne bouge pas d'une passe à l'autre : PyroWave dessine ~2 ms plus tôt
  (e2e), répète quatre fois moins d'images, et sa queue est plus courte. Le
  décodage GPU reste à 1,77 ms à chaque passe.
- « Montré » varie trop entre passes (HEVC de 18,6 à 41,4 ms) pour servir de
  juge ; le clic reste la mesure.

**Verdict.** À 120 images/s, PyroWave est devant le HEVC sur ce client, de
peu en médiane (~2-3 ms) et nettement sur la queue et la régularité. Ce n'est
pas un écart de 9 ms ; c'est assez pour faire de 120 images/s la cadence
d'Ultra. Le gros du reste est côté page (~20 ms de clic hors e2e, communs aux
deux codecs).

**Freebox Player POP (sondée le 06/10 au soir).** TV Bro (WebView Chrome 153,
Android 10, Mali-G31) expose `navigator.gpu` mais `requestAdapter()` rend
`null` : pas de WebGPU (Android le réserve à 12+). WebGL2 y est, avec
`EXT_color_buffer_float`. PyroWave sur une TV passe donc par le repli WebGL2.

### 6.21 120 images/s par défaut, et le repli WebGL2 du décodeur (06/10/2026, 20:45)

**La cadence.** Il n'y a rien à changer : l'« Auto » du produit prend déjà
la fréquence de l'écran du client, plafonnée à 120 (`AUTO_FPS_MAX`). Une passe
PyroWave avec la cadence et l'écran virtuel laissés au produit (UM790Pro,
dalle à 240 Hz) donne 1920×1080 à 120 i/s, écran virtuel créé à 240 Hz
(`u14f-…-pw-ar-auto`). Un client à 60 Hz reste à 60 : « si l'écran le
permet ». Seul le banc forçait 60 (`U14_FPS`, défaut 60 dans `u14_series.py`).

**Le repli WebGL2** (`a53d7eed`, `PyroWaveDecoderGL.js`). Sans calcul ni
écriture dispersée, chaque étape devient une passe où chaque fragment calcule
une sortie : décalages des blocs 8×8 et des signes par bloc 32×32 ;
magnitudes des 128 fils d'un bloc (MRT : 8 valeurs et leur compte de
non-nuls) ; comptes par groupe de 16 fils ; signes, à la somme exclusive des
non-nuls qui précèdent (le scan que l'amont fait en mémoire partagée). Puis,
par niveau et composante, une passe en lignes qui lit les coefficients
directement dans la texture des fils, et une en colonnes. Chaque échantillon
déroule les quatre pas CDF 9/7 sur sa propre fenêtre de 9, miroir aux bords.
Le parseur de paquets passe dans `PyroWaveFrame.js`, commun aux deux.

| Banc du décodeur (`decoder_lab.py`), 5 clips 1080p, 60 images | WebGPU | WebGL2 |
| ------------------------------------------------------------- | -----: | -----: |
| Écart max avec la référence (Y, C)                            |   1, 1 |   1, 1 |
| Affichage : écart RGB max                                     |      3 |      3 |
| Décodage GPU, RTX 5060 Ti, p50                                | 0,17 ms | 0,83 ms |

SwiftShader d'abord (la règle « WARP d'abord ») : juste, 270 ms par image.

| De bout en bout, UM790Pro (780M), 120 i/s, 30 clics | WebGPU (§6.20) | WebGL2, minuteries | WebGL2, messages |
| --------------------------------------------------- | -------------: | -----------------: | ---------------: |
| Images dessinées / s                                |        113-118 |                 96 |              113 |
| Remplacées avant décodage                           |         79-112 |              1 167 |              150 |
| Soumis → GPU fini, p50                              |    4,3-4,9 ms |             9,2 ms |           5,7 ms |
| Clic, médiane                                       |   29,8-38,3 ms |            42,3 ms |          41,9 ms |

- La première version attendait la barrière GPU par `setTimeout(0)` en
  chaîne, bridé à 4 ms par le navigateur : le décodage suivant attendait, un
  dixième des images était remplacé. Par `MessageChannel`, le repli tient les
  120 i/s.
- Le clic à ~42 ms sur une passe de 30 clics est au-dessus des passes WebGPU,
  mais une passe seule ne départage rien (§6.20). Le repli ne sert de toute
  façon que là où WebGPU manque.
- Clé de banc : `mw_ultra_api=webgl2` force le repli. Elle reste dans le
  profil du Chrome de banc ; une passe WebGPU qui suit doit poser
  `mw_ultra_api=webgpu`.

**Freebox.** Pas encore mesurée : la box a quitté le réseau dans la soirée
(plus d'adresse MAC, le paquet magique ne la réveille pas), sans doute mise en
veille profonde par la TV (HDMI-CEC). Prochaine étape dès qu'elle est
rallumée : le repli sur la Mali-G31.

### 6.22 Le repli WebGL2 sur la Freebox : faux, et 400 ms par image (06/10/2026, 22:15)

Le labo du décodeur (`decoder_lab.py --api webgl2 --remote-cdp`) a tourné dans
TV Bro, sur la Mali-G31 de la Freebox Player POP (WebGL2, `EXT_color_buffer_float`,
pas de minuteur GPU). La box est sur son port Ethernet.

- **Compilation.** Le compilateur GLSL du Mali refusait la passe des signes
  (« no default precision defined for variable 'float[8]' » : un constructeur
  de tableau, malgré `precision highp float`). Elle remplit désormais son
  tableau élément par élément (`0753ba4a`). Le résultat est inchangé sur la
  RTX et en SwiftShader (écart 1 avec la référence, 3 au présent).
- **Exactitude.** Sur la Mali, l'image est fausse sur tous les clips essayés,
  même le dégradé : écart jusqu'à 255, PSNR ~10 dB contre la référence. La
  cause n'est pas cherchée (les limites lues sont suffisantes : 4 cibles,
  textures de 4096, flottants et entiers 32 bits).
- **Vitesse.** Décodage + présentation + lecture d'un pixel : **~405 ms par
  image 1080p** (p50 sur trois clips), soit 2,5 i/s. C'est cinquante fois trop
  lent pour 120 i/s, et vingt-cinq fois pour 60.

Verdict : **PyroWave ne vaut pas pour la Freebox**, même avec un repli juste.
Son GPU n'a ni WebGPU ni la puissance de calcul ; HEVC, décodé par le circuit
de la box, reste sa voie. Le repli WebGL2 garde son intérêt pour un client
au GPU de PC sans WebGPU (il tient 120 i/s sur le 780M, §6.21). Le mode Ultra
étant à activer à la main, l'image fausse sur Mali ne touche aucun
utilisateur.

**La Mi TV (22:45), même GPU.** Le même labo dans son TV Bro (MT5867, une
autre Mali-G31, pas d'adaptateur WebGPU non plus) donne la même image fausse
(PSNR 9-10 dB sur le dégradé, le jeu et le texte) et **~627 ms par image
1080p** (p50), soit 1,6 i/s. Le verdict vaut donc pour les deux TV du banc :
PyroWave reste un codec de PC, et les TV gardent le HEVC.

**L'iPhone 13 Pro (23:15), lui, décode juste et vite.** Safari d'iOS 26.5 a
un adaptateur WebGPU (« apple », minuteur GPU compris), et le décodeur
principal y tourne sans repli : écart 1 avec la référence (PSNR ≥ 65 dB),
**4,8 ms de GPU par image 1080p** (p50, 7,7 au p99 ; clip `game10-1080p`,
170 Mbit/s, GPU froid). C'est l'ordre du 780M au même régime (3,15 ms à
60 i/s, §6.20), sous les 8,3 ms d'une image à 120 i/s. La page était servie
par `tailscale serve` (HTTPS du réseau privé ; elle affiche désormais aussi
ses erreurs, faute de DevTools sur un téléphone). Le décodage n'est donc pas
l'obstacle sur un iPhone ; le débit l'est, PyroWave restant réservé à
l'Ethernet. Le repli WebGL2 y est juste lui aussi (écart 1), en **19 ms
d'horloge par image** (p50, 23 au p99 : décodage + présentation + lecture
d'un pixel, sans minuteur GPU) : assez pour 30 à 50 i/s, pas pour 60 ; sur
iPhone, la voie est WebGPU.

### 6.23 Décoder par tranches, pendant que l'image arrive (06/10/2026, 23:20)

Première piste du §6.18. L'hôte envoie déjà les blocs dans l'ordre de leur
index, du niveau le plus grossier au plus fin : la page n'a donc pas besoin de
l'image entière pour commencer (`2e3c70a3`).

- **Le worker de la route audio** passe à la page le début d'une image encore
  en route, par morceaux contigus d'au moins 48 Kio (clés de banc
  `mw_ultra_slices=1`, `mw_ultra_slice_kb`). Le dernier morceau ne part jamais
  seul : l'image entière suit comme avant.
- **`UltraPlayer`** soumet chaque morceau au GPU dès qu'il arrive. Il
  déquantifie les blocs déjà là, puis inverse l'ondelette de chaque niveau
  devenu complet. À l'arrivée de l'image, il ne reste que le dernier morceau,
  le niveau le plus fin et l'affichage. S'il manque un morceau, ou si une
  image par tranches attend encore le GPU, l'image suivante se décode
  entière, comme avant.
- **Le labo** (`decoder_lab.py --slices N`) coupe chaque image en N morceaux
  n'importe où, au milieu des blocs. Il vérifie le décodage et chronomètre ce
  que laisse le dernier morceau.

| GPU, 1080p, `game10` / `text10` | Image entière | 4 morceaux | 8 | 16 |
|---|---|---|---|---|
| Écart à la référence | 1 | 1 | 1 | 1 |
| Reste à la dernière tranche, p50 | 1,98 / 1,97 ms | 1,46 / 0,82 ms | 0,82 / 0,78 ms | 0,78 / 0,77 ms |

- Le résultat est exact, sur SwiftShader comme sur la 780M.
- À 8 morceaux, il ne reste que **0,8 ms au lieu de 2,0 ms**. Le plancher est
  l'ondelette inverse du niveau le plus fin, qui attend sa dernière bande.
- Pour descendre sous ce plancher, l'hôte devrait entrelacer les rangées de
  blocs des trois bandes du niveau fin. La page pourrait alors inverser ce
  niveau par bandes horizontales.

**Sur le câble (23:32-23:49).** Même banc qu'au §6.20 : la RTX encode,
l'écran virtuel est en 2560×1440 à 120 Hz, le flux à 120 i/s, la route audio
le transporte, et l'UM790Pro est le client. Six passes alternées, par
tranches (`sl1`) contre image entière (`sl0`), 60 clics chacune
(`scratchpad/pwslice_run.sh`, `pwslice_report.py`).

| Médianes | Image entière (3 passes) | Par tranches (3 passes) |
|---|---|---|
| Images décodées par tranches | 0 % | 98 % (~3 morceaux par image) |
| GPU restant à l'arrivée de l'image | 1,77 ms | 1,44 ms |
| Envoi → fin du GPU | 4,1-4,5 ms | 3,7-4,4 ms |
| Hôte → dessin (p90) | 10,3-10,8 ms (12,7-13,0) | 9,3-10,0 ms (11,6-12,5) |
| Clic, ~173 clics réunis (p90) | 31,9 ms (41,1) | 34,3 ms (41,8) |
| Images répétées par minute | 504-586 | 474-678 |

- **Le gain est réel mais petit** : −0,7 ms de l'hôte au dessin, à chaque
  passe. À 120 i/s, une image ne fait que ~180 Ko, soit trois morceaux de
  48 Kio. Et le dernier, avec le niveau le plus fin, reste à faire à
  l'arrivée.
- **Le clic ne le voit pas.** Il est même 2,4 ms plus haut en médiane, mais
  son p90 est égal. C'est dans le bruit relevé au §6.20 (31 à 42 ms d'une
  passe à l'autre) : rien n'explique un recul de 2 ms quand l'hôte → dessin
  gagne 0,7 ms.
- **La clé reste éteinte par défaut.** Une seconde moitié rendrait le gain
  visible : que l'hôte entrelace les rangées du niveau fin, et que la page
  inverse ce niveau par bandes horizontales.

### 6.24 Le niveau fin entrelacé par rangées, inversé par bandes (07/10/2026, 06:40)

La seconde moitié du §6.23 (`5e9ef99c`) :

- **L'hôte envoie les blocs dans l'ordre des index, sauf le niveau fin.**
  Ses trois bandes (luma seule, la moitié des octets) partent entrelacées
  par rangées de blocs : rangée 0 de HL, de LH, de HH, puis rangée 1, etc.
  Chaque bloc porte son index, et un décodeur ne dépend pas de l'ordre.
- **Aucun bit libre ne signale cet ordre** dans l'en-tête de PyroWave. La
  page le suppose donc. Un bloc qui arrive derrière la frontière (un hôte
  qui envoie un autre ordre) annule les tranches de l'image, qui est alors
  décodée entière. Le résultat reste juste, il n'y a que le gain de perdu.
- **La page déquantifie dans l'ordre d'envoi**, par une table position →
  index. Elle inverse le niveau fin par bandes de tuiles de 32 lignes, dès
  que les rangées de blocs qu'elles lisent sont réglées (16 lignes de bande
  plus 2 de marge de chaque côté).

Le labo (`decoder_lab.py --slices N`, le corpus de référence remis dans
l'ordre d'envoi) est exact, comme l'image entière : écart max 1 sur
SwiftShader et sur le 780M. GPU restant au dernier morceau, 780M, 1080p :

| Morceaux | `game10` §6.23 → §6.24 | `text10` §6.23 → §6.24 |
|---|---|---|
| Image entière | 1,98 ms | 1,97 ms |
| 4 | 1,46 → 1,46 ms | 0,82 → 0,58 ms |
| 8 | 0,82 → 0,48 ms | 0,78 → 0,33 ms |
| 16 | 0,78 → 0,34 ms | 0,77 → 0,18 ms |

- **Le plancher de ~0,8 ms est tombé.** Le dernier morceau ne porte plus
  que la fin de l'inverse du niveau fin, pas l'inverse entier.
- **À 4 morceaux, `game10` ne gagne rien** : son dernier quart commence
  avant le niveau fin, et tout le niveau fin reste à faire à l'arrivée.
- Sur le câble à 120 i/s, une image fait ~3 morceaux de 48 Kio. Le
  morceau plus petit (`mw_ultra_slice_kb`) est donc à essayer avec cette
  passe.

### 6.25 U3.7 B1 : Chrome voit trop tard que le GPU a fini (09/10/2026, soir)

> ⚠️ Les verdicts des §6.19-6.20 ne tiennent plus. Leur HEVC passait par la
> piste vidéo RTP (métronome de Chrome, ~8 ms), et la sonde du clic se
> mesurait elle-même jusqu'à `652fc726` (`click-waits.md` §6). La remesure à
> 120 i/s contre le HEVC du produit (SCTP) est au §6.26.

Le point de départ est B0 (`click-waits.md` §3.4), sur le 780M, en plein flux :
5,1 ms de la soumission à la fin pour 2,2 ms de travail GPU, et 3,6 ms pour une
soumission vide. B1 reprend la question hors flux, avec
`scripts/bench/ultra/gpuwait-lab.html` et `gpuwait_lab.py` :

- les mêmes soumissions et la même trace que `UltraPlayer`, que
  `gpuwait.py --dir bench-out/ultra-lab` lit telle quelle ;
- un Chrome 155 headless à part, un GPU de DualRTX à la fois
  (`--use-adapter-luid`) ;
- la page isolée (COOP/COEP), donc une horloge à la µs et non à 0,1 ms ;
- 1 500 soumissions par cas, deux tours ABBA. Médianes en ms.

| Cas | RTX 5060 Ti | Arc A380 | iGPU AMD (2 CU) |
|---|---|---|---|
| commande vide, sans passe | 0,10 | 0,11 | 0,10 |
| passe vide horodatée | 2,9-3,2 | 3,0-3,2 | 3,0-3,1 |
| dont après la fin du GPU | 2,6-2,8 | 2,7-2,8 | 1,8-2,3 |
| image 1080p décodée, 120 i/s | 3,7 | 4,2 | 8,0-8,3 |
| dont travail du GPU | 0,19 | — * | 5,4 |
| dont après la fin du GPU | 2,8 | — * | 1,3-1,5 |

\* En décodage, l'Arc rend des horodatages faux.

- **Ce n'est pas le 780M.** Les trois GPU, de trois marques, paient les mêmes
  ~3 ms pour une passe vide, que le GPU exécute en quelques µs. La montée en
  fréquence du GPU (hypothèse 2 de B1) n'en est donc pas la cause.
- **C'est le processus GPU de Chrome, qui ne relève ses barrières que de
  temps en temps (hypothèse 1).**
  - Une commande vide, qui n'attend rien, revient en 0,1 ms : l'aller-retour
    lui-même ne coûte rien.
  - Le temps passe entre la fin du travail et le moment où Chrome s'en
    aperçoit : 2 à 3 ms. Les fins ne tombent sur aucune grille fixe : le
    délai court depuis la soumission.
  - Lecture probable : un relevé différé d'~2 ms après la dernière commande
    reçue, puis toutes les ~2 ms. Ce n'est pas vérifié dans les sources de
    Chromium.
- Rien d'autre ne change ce délai : attendre par `mapAsync` au lieu
  d'`onSubmittedWorkDone`, décoder dans un worker plutôt que sur le fil
  principal, soumettre à 120 i/s ou d'affilée.
- Sur l'iGPU AMD, le décodage (5,4 ms) plus l'attente dépassent l'intervalle
  de 8,33 ms : 109 à 117 images par seconde au lieu de 120.

**Le levier : une commande vide pendant l'attente.** `queue.submit([])` toutes
les 0,25 ms (ou toutes les 1 ms), par une boucle de messages, tant que l'image
attend :

| Décodage à 120 i/s | Soumission → fin | Après la fin du GPU | Début du GPU | Fil principal par image | Images/s |
|---|---|---|---|---|---|
| RTX, sans | 3,7 | 2,8 | 0,5-0,6 | 0,6-0,8 | 120 |
| RTX, toutes les 0,25 ms | 1,0-1,2 | 0,34-0,37 | 0,5-0,6 | 1,9-2,3 | 120 |
| RTX, toutes les 1 ms | 1,3 | 0,63 | 0,6-0,7 | 2,5 | 120 |
| iGPU AMD, sans | 8,1-8,2 | 1,3-1,4 | 1,3 | 0,6-0,8 | 109-115 |
| iGPU AMD, toutes les 0,25 ms | 6,6-6,7 | 0,36-0,43 | 0,75-0,8 | 7,2 | 120 |
| iGPU AMD, toutes les 1 ms | 7,2 | 1,0 | 0,7-0,8 | 7,5 | 120 |

- **N'importe quelle commande réveille Chrome.** Une soumission vide,
  4 octets par `writeBuffer` ou un `onSubmittedWorkDone` de plus ramènent tous
  le délai à ~0,35 ms. Sur l'iGPU AMD, le GPU commence aussi plus tôt (1,3 →
  0,75 ms), et le décodage retrouve ses 120 images par seconde.
- **Le prix : le fil principal tourne pendant toute l'attente.** La boucle de
  messages coûte 1,1 à 1,6 ms par image sur la RTX, et ~6,5 sur l'iGPU AMD.
  Des minuteries ne marchent pas à la place : dans ce Chrome, un
  `setTimeout(1)` part 1,5 à 3 ms plus tard, et le délai ne bouge pas.
- **Ce qu'on peut en attendre sur le 780M**, d'après les chiffres de B0 : la
  fin vue 1,2 à 1,8 ms plus tôt, et le GPU qui commence ~0,5 ms plus tôt. Soit
  1,5 à 2 ms par image, sur le clic comme sur l'hôte → dessin, à condition que
  l'affichage ne repaie pas ce délai plus loin.
- Le HEVC ne passe pas par ce chemin : WebCodecs remet ses images autrement.
  C'est un handicap propre à PyroWave dans la page.

**À vérifier avant d'en tirer un gain :**

- **Le 780M dans le même labo**, et une fenêtre affichée (hypothèse 3 : la
  file partagée avec le compositeur).
- **Ce qui arrive à l'écran.** La sonde corrigée date le dessin, pas
  l'affichage. Une image remise plus tôt à la page (`mw_ultra_early=1`, ou B2.1
  par le canevas WebGPU) avance cette heure sans prouver que l'écran avance.
  Un tel levier se juge donc en bout de chaîne, sur l'écran du client
  (`scripts/bench/photon/`).
- **La relance dans `UltraPlayer`**, derrière une clé de banc, en ne tournant
  qu'autour de la fin attendue (les horodatages GPU la donnent) pour épargner
  le fil principal. Puis des passes ABBA sur le câble, jugées au clic et à
  l'hôte → dessin.

### 6.26 Le HEVC du produit contre PyroWave, à 120 i/s sur le câble (09/10/2026, 19:29-19:49)

Le banc de référence de « Capture et Attente » (`click-waits.md` §6.1) :

- **Hôte :** DualRTX, une `--dev` avec `build\` à `98276780`. L'écran virtuel du
  produit est à 240 Hz, rendu par la RTX. `mw-click-target` est en fenêtre
  (tearing).
- **Flux :** « Auto », 1080p à 120 i/s (la fréquence de l'UM790Pro), détection
  coupée.
- **Client :** l'UM790Pro sous Windows (780M), en câble, Chrome 154.
- **Sonde :** corrigée (`652fc726`, `5c96775d`), 60 clics par passe, deux passes
  par bras en ABBA, soit 120 clics par bras.
- **Une passe écartée.** La première passe HEVC SCTP a pris le chemin par la
  Freebox : paire « prflx 82.67.150.202 ← prflx 192.168.1.254 », RTT de la
  synchro 4,7 ms au lieu de 0,5, +2 ms par image. Elle a été refaite (r3).
  Constat transmis au réseau (`network-latency-findings.md`).
- **Outils :** `clicksplit.py` (le clic coupé à la capture, sur l'horloge du
  client) et `gpuwait.py`. Le lanceur et les rapports sont dans le scratchpad
  (`pw120c/` : `run.sh`, `report.py`, `pwlegs.py`, `paths.py`).

« Hors sonde » : la capture → dessin des images hors des fenêtres de la sonde
(du clic à 30 ms après le drapeau vu), le « capture → écran » du §6.1. Médiane
/ moyenne en ms.

| Bras | Clic | Clic → capture | Capture → dessin (clic) | Hors sonde |
|---|---|---|---|---|
| HEVC, SCTP (le produit) | 10,5 / 10,1 | 6,5 / 6,4 | 3,6 / 3,7 | 3,5 / 3,6 |
| HEVC, route audio | 11,7 / 11,4 | 8,1 / 7,7 | 3,6 / 3,7 | 3,5 / 3,6 |
| PyroWave, route audio | 18,7 / 19,2 | 5,7 / 6,0 | 12,3 / 13,1 | 10,8 / 11,4 |
| PyroWave, SCTP | 20,2 / 19,5 | 7,2 / 6,8 | 12,6 / 12,6 | 11,0 / 11,2 |

- **PyroWave perd ~8,5 ms au clic, et ~7,5 ms par image.** Le clic → capture
  ne dépend pas du codec : il va de 6 à 8 ms selon la phase tirée au hasard,
  à ±1 ms d'un bras à l'autre. Tout l'écart est de la capture au dessin. Le
  HEVC du produit retrouve les chiffres du §6.1 (3,5 ms).
- **Le gain des §6.19-6.20 venait de la piste vidéo RTP du HEVC**, pas de
  PyroWave.
- **La route audio ne sert à rien au HEVC** : ses images font ~180 octets sur
  cette scène presque fixe. Pour PyroWave, elle fait arriver l'image 0,7 ms
  plus tôt que SCTP. Le clic n'en montre rien.

Les étapes d'une image, d'après le journal des images, le relais de l'hôte et
la trace d'`UltraPlayer` (passes r2, ~6 200 images chacune). Médianes en ms :

| Étape | HEVC (4 passes) | PyroWave, route audio | PyroWave, SCTP |
|---|---|---|---|
| hôte : capture → relais | 1,7-2,1 | 1,7 | 1,8 |
| relais → arrivée dans la page | ~1,0 | 3,4 | 4,1 |
| arrivée → soumission | — | 0,3 | 0,3 |
| décodage (HEVC) ; soumission → fin (PyroWave) | 0,4 | 4,7 | 4,8 |
| dont travail du GPU | — | 2,2 | 2,2 |
| image remise → dessinée | 0,2 | 0,3 | 0,3 |
| capture → dessin | 3,3-3,7 | 10,7 | 11,4 |

- **Le trajet : +2,5 à 3 ms.** PyroWave envoie l'image entière à chaque fois :
  177 Ko, soit 1,45 ms rien que sur le fil à 1 Gbit/s. Le HEVC n'envoie que
  la différence, ~180 octets ici.
- **Le décodage : +4,3 ms.** Le décodeur matériel du 780M rend le HEVC en
  0,4 ms. PyroWave demande 2,2 ms de GPU (iDWT 1,77, présentation 0,31), puis
  attend Chrome.
- **B1 sur le 780M, en plein flux** (`gpuwait.py`, les deux passes r2) :
  - de la soumission à la fin : 4,6 ms, pour 2,2 ms de GPU ;
  - après la fin du GPU : 1,1 à 1,7 ms (selon la borne) ;
  - avant que le GPU commence : 0,4 à 0,9 ms ;
  - une soumission vide horodatée : 3,6-3,8 ms.

  C'est le délai du §6.25, au même ordre que sur les GPU de DualRTX.

**Ce que les leviers connus peuvent rendre, au mieux :**

| Levier | Gain par image | Source |
|---|---|---|
| la relance (§6.25) | 1,5 à 2 ms | §6.25 |
| le décodage par tranches | ~0,7 ms | §6.23 |
| la route audio plutôt que SCTP | ~0,7 ms | ci-dessus |

Ces leviers laissent PyroWave vers 8 ms de la capture au dessin, contre 3,5 ms
pour le HEVC. Même plancher sans aucune attente, PyroWave ne passe pas sous
~5,8 ms :

| Étape | ms |
|---|---|
| hôte | 1,8 |
| fil (177 Ko à 1 Gbit/s) | 1,45 |
| GPU | 2,2 |
| dessin | 0,3 |

Un jeu en mouvement grossirait les images du HEVC, pas celles de PyroWave.
Mais sous RE9, qui bouge et sature le GPU de l'hôte, le HEVC du produit reste
à 4,7 ms de la capture à l'écran (`click-waits.md` §7, en REALTIME) : l'écart
ne se refermerait pas.

**Verdict.** Dans le navigateur, sur ce client et ce lien, PyroWave n'a pas de
gain à offrir contre le HEVC du produit par SCTP. Il perd ~7,5 ms par image et
~8,5 ms au clic, et ses leviers connus n'en rendent pas la moitié. Ce qui
reste de U3.7 (la relance dans `UltraPlayer`, B2) réduirait l'écart sans
l'inverser. La suite du POC est une décision de Bruno.

### 6.27 U3.7 B2.0 : la relance dans `UltraPlayer` (09/10/2026, 20:12-20:22)

Bruno a choisi de continuer U3.7. La relance du §6.25 entre dans le lecteur,
derrière la clé de banc `mw_ultra_nudge` (`92e79bd7`, `GpuNudge.js`) :
pendant qu'une image attend la fin de son travail, une soumission vide toutes
les 0,25 ms, par une boucle de messages.

- **`all`** : la boucle tourne dès la soumission, comme au labo du §6.25.
- **`1` (`auto`)** : la boucle ne démarre que 1 ms avant la fin attendue, soit
  la plus courte soumission → fin des 32 dernières images (10ᵉ centile).
  - Quand ce départ est à plus de 2,5 ms, une minuterie couvre l'attente
    d'avant. Plus près, la boucle part dès la soumission.
  - Les horodatages GPU ne servent pas : avec la relance, la fin vue suit
    celle du GPU à ~0,35 ms près, et Safari ou Firefox n'ont pas toujours
    `timestamp-query`.

Le labo du §6.25 (`gpuwait_lab.py`, `nudge=player`, `a5086ee6`) fait tourner
`GpuNudge` lui-même : 1080p à 120 i/s, deux tours ABBA. Médianes en ms.

| Cas | Soumission → fin | Début du GPU | Après la fin du GPU | Boucle par image | Fil principal par image | Images/s |
|---|---|---|---|---|---|---|
| RTX, sans | 3,7-3,8 | 0,5-0,6 | 2,8-2,9 | — | 0,85 | 120 |
| RTX, `all` | 1,0-1,25 | 0,55-0,7 | 0,32-0,37 | 1,0-1,2 | 2,0-2,5 | 120 |
| RTX, `auto` | 1,05-1,4 | 0,57-0,75 | 0,37-0,39 | 1,0-1,3 | 2,3-2,55 | 120 |
| iGPU AMD, sans | 8,3-8,4 | 1,35 | 1,45-1,55 | — | 0,75-0,9 | 107-109 |
| iGPU AMD, `all` | 6,65-6,7 | 0,72-0,74 | 0,37 | 6,6 | 7,1 | 120 |
| iGPU AMD, `auto` | 6,9 | 0,9 | 0,44-0,48 | 2,9-3,0 | 3,3-3,4 | 120 |

- **`GpuNudge` rend ce que rendait la relance du labo** : Chrome voit la fin
  ~0,35 ms après celle du GPU, au lieu de 1,5 à 2,9 ms.
- **Sur la RTX, `auto` revient à `all`** : la fin attendue (~1 ms) est trop
  proche pour une minuterie.
- **Sur l'iGPU AMD, `auto` coupe la boucle de moitié** (2,9-3,0 ms par image
  contre 6,6), pour +0,25 ms :
  - la minuterie arrive ~1 ms en retard ;
  - le GPU commence moins tôt (0,9 contre 0,73 ms), faute de relance juste
    après la soumission.
- **Un défaut trouvé au premier essai, corrigé avant le commit.** 10 % des
  images en `auto` lançaient leur boucle dès la soumission : un message de
  l'attente précédente, encore en route, démarrait la suivante. Chaque message
  porte maintenant le numéro de son attente.
- **Sur le 780M**, le GPU travaille 2,2 ms et la fin se voit vers 3 ms avec la
  relance. Le départ y tombe donc à moins de 2,5 ms : `auto` y vaut `all`.
  - Attendu : 1,5 à 2 ms de moins par image (§6.25).
  - Coût : ~3 ms de fil principal par image, soit ~36 % d'un cœur à 120 i/s.
  - La boucle rend la main entre deux messages : les autres tâches de la page
    attendent au plus un tour de boucle. À vérifier en plein flux.

**Reste à mesurer :**

- des passes ABBA sur le câble (l'UM790Pro), PyroWave avec et sans
  `mw_ultra_nudge=1`, jugées au clic et à l'hôte → dessin ;
- puis le bout de chaîne, sur l'écran du client.

### 6.28 La relance sur le câble, à 120 i/s (09/10/2026, 20:27-20:40)

Le banc du §6.26, avec trois bras en ABCCBA (deux passes de 60 clics par bras) :

- le HEVC du produit par SCTP, en témoin ;
- PyroWave par SCTP sans relance (`mw_ultra_nudge=0`) ;
- PyroWave par SCTP avec relance (`mw_ultra_nudge=1`, donc `auto`).

Les six passes ont pris le LAN direct (paires IPv6 internes, RTT de la synchro
0,5-0,7 ms). Le lanceur et les rapports sont dans le scratchpad (`pw120c/` :
`run-nudge.sh`, `report.py`, `player.py`, `pwlegs.py`).

Au clic et hors sonde, comme au §6.26. Médiane / moyenne en ms, 120 clics par
bras, ~11 500 images hors sonde.

| Bras | Clic | Clic → capture | Capture → dessin (clic) | Hors sonde |
|---|---|---|---|---|
| HEVC, SCTP (le produit) | 10,5 / 10,3 | 6,7 / 6,4 | 3,9 / 3,9 | 3,7 / 3,8 |
| PyroWave, SCTP, sans relance | 18,6 / 18,9 | 5,7 / 6,1 | 12,9 / 12,8 | 11,1 / 11,2 |
| PyroWave, SCTP, avec relance | 16,0 / 17,5 | 4,4 / 5,9 | 11,1 / 11,5 | 10,1 / 10,6 |

Les étapes de chaque passe, sur toutes ses images (journal des images de la
page, horloge du client). Médianes en ms.

| Passe | Capture → arrivée | Arrivée → décodée | Décodée → dessinée | Capture → dessin | Octets par image |
|---|---|---|---|---|---|
| HEVC r1 / r2 | 3,2 / 2,95 | 0,4 / 0,4 | 0,2 / 0,2 | 3,9 / 3,6 | ~170 |
| PyroWave sans relance r1 / r2 | 6,05 / 5,2 | 5,1 / 5,0 | 0,2 / 0,2 | 11,7 / 10,7 | 177 000 |
| PyroWave avec relance r1 / r2 | 5,8 / 6,05 | 4,1 / 3,6 | 0,2 / 0,2 | 10,1 / 10,3 | 177 000 |

- **La relance tient sur le câble : −1,2 ms de décodage par image** (5,0-5,1 →
  3,6-4,1 ; soumission → fin du lecteur 4,7-4,8 → 3,3-3,8). Le §6.27
  attendait 1,5 à 2 ms. Par image, de la capture au dessin, le gain se lit
  −1,0 ms en médiane et −0,7 en moyenne : l'arrivée varie de ±0,4 ms d'une
  passe à l'autre, avec l'hôte.
- **Le clic suit** : −1,3 ms de la capture au dessin en moyenne. Le clic
  entier gagne 1,4 ms en moyenne, mais son clic → capture tient au hasard de
  la phase.
- **Le coût est celui prévu** : la boucle tourne 3,3-3,6 ms par image, ~40 %
  d'un cœur du client à 120 i/s. `auto` y vaut presque `all` : 13 soumissions
  vides par image, une minuterie pour 7 à 9 % des images seulement. Aucun clic
  perdu, aucune image incomplète ni en erreur. Avec ou sans relance, 1 à 8
  images par passe ont été remplacées par une plus récente avant leur
  décodage.
- **PyroWave reste ~6,4 ms derrière le HEVC du produit** (10,1 contre 3,7
  hors sonde). L'écart a deux moitiés :
  - **L'arrivée, +2,7 ms.** Chaque image PyroWave fait 177 Ko, soit ~1,4 ms de
    fil à 1 Gbit/s plus SCTP. Le HEVC envoie ~170 octets sur cette scène presque
    fixe ; en jeu, ses images grossiraient et l'écart baisserait un peu.
  - **Le décodage, +3,2-3,7 ms** sur le 780M, contre 0,4 ms au décodeur matériel.
- **Le dessin ne pèse que 0,2 ms** dans la page, avec ou sans relance. Ce que
  B2.1 (présenter par le canevas WebGPU) peut gagner se trouve donc après la
  page, dans le compositeur : seul le bout de chaîne le montrera.

**Verdict.** La relance est un vrai gain, mais elle ne retourne pas celui du
§6.26. Les deux leviers qui restent sont plus gros que B2.1 :

- les tranches, pour décoder pendant que l'image arrive ;
- un iDWT plus rapide, ou des images plus petites.

### 6.29 Les tranches avec la relance, sur le câble à 120 i/s (09/10/2026, 22:27-22:47)

Le banc du §6.28 (`build\` de 20:41, avec `7315bfcb`), avec quatre bras en
ABCDDCBA (deux passes de 60 clics par bras), la relance allumée sur tous les
bras PyroWave :

- le HEVC du produit par SCTP, en témoin ;
- PyroWave par la route audio, en images entières ;
- PyroWave par la route audio, par tranches de 48 Kio (le défaut) ;
- PyroWave par la route audio, par tranches de 16 Kio (`mw_ultra_slice_kb=16`).

Les tranches n'existent que sur la route audio : `RtpVideo.js` ne donne les
morceaux qu'au worker de la piste `vaudio`. Les trois bras PyroWave la
prennent donc, et le bras en images entières sert de témoin. Les huit passes
ont pris le LAN direct (paires IPv6 internes). Le lanceur et les rapports sont
dans le scratchpad (`pw120c/` : `run-slices.sh`, `report.py`, `player.py`,
`pwlegs.py`, `relaydrawn.py`), avec `gpuwait.py`.

Au clic et hors sonde, comme au §6.26. Médiane / moyenne en ms, 120 clics par
bras, ~11 500 images hors sonde.

| Bras | Clic | Clic → capture | Capture → dessin (clic) | Hors sonde |
|---|---|---|---|---|
| HEVC, SCTP (le produit) | 10,1 / 10,3 | 6,6 / 6,6 | 3,6 / 3,7 | 3,4 / 3,5 |
| PyroWave, route audio, images entières | 15,4 / 16,9 | 5,4 / 6,2 | 9,7 / 10,7 | 8,7 / 9,4 |
| PyroWave, route audio, tranches de 48 Kio | 15,4 / 16,5 | 5,3 / 6,5 | 9,5 / 9,9 | 8,2 / 8,9 |
| PyroWave, route audio, tranches de 16 Kio | 15,9 / 16,5 | 5,0 / 5,7 | 10,0 / 10,9 | 8,5 / 9,4 |

La part de l'hôte, de la capture à la remise au relais, varie de 1,3 à 2,1 ms
d'une passe à l'autre, dans un même bras. Elle ne dépend pas des tranches, qui
ne se règlent que dans la page. Le tableau suivant la retire image par image
(`relaydrawn.py` : le relais de l'hôte joint au journal des images par
l'horodatage de capture). Il reprend aussi les passes du §6.28, dont le HEVC
témoin donne les mêmes chiffres. Médianes (moyennes) en ms, les deux passes
de chaque bras.

| Bras | Hôte : capture → relais | Relais → arrivée | Relais → dessin |
|---|---|---|---|
| HEVC, SCTP (§6.28 et ici) | 1,7-2,2 | 0,86-0,90 | 1,5-1,6 (1,7) |
| PyroWave, SCTP, sans relance (§6.28) | 1,4-2,2 | 3,8-3,9 | 9,2-9,5 (9,5-9,6) |
| PyroWave, SCTP, avec relance (§6.28) | 1,8-2,1 | 3,9-4,0 | 8,1-8,3 (8,6-8,9) |
| PyroWave, route audio, images entières | 1,35-1,7 | 3,0-3,1 | 7,0-7,4 (7,9-8,1) |
| PyroWave, route audio, tranches de 48 Kio | 1,3-1,5 | 3,1-3,2 | 6,45-7,2 (7,3-7,8) |
| PyroWave, route audio, tranches de 16 Kio | 1,9-2,1 | 3,1-3,2 | 6,5 (7,5) |

La page, après le dernier octet (`gpuwait.py`, passes r1 ; les r2 de 16 Kio
donnent les mêmes chiffres). Les deux horloges, du GPU et de la page, ne se
recalent qu'entre deux bornes : le départ et la fin du GPU sont donnés par
cet intervalle. Médianes en ms.

| Étape | Images entières | Tranches de 48 Kio | Tranches de 16 Kio |
|---|---|---|---|
| morceaux par image | — | 3 | 10 |
| soumission → fin | 3,9 | 2,7 | 2,9 |
| dont soumission → départ du GPU | 0,55-1,0 | 1,4-1,8 | 1,55-2,0 |
| dont travail du GPU (décodage + présentation) | 2,2 (1,77 + 0,31) | 0,58 (0,13 + 0,31) | 0,53 (0,08 + 0,31) |
| dont fin du GPU → rappel | 0,27-0,74 | 0,24-0,73 | 0,24-0,67 |
| boucle de relance, par image | 3,2-3,7 | 2,6-3,5 | 2,6-2,7 |

- **Les tranches prennent** : 98,5 à 99,4 % des images, ~3 morceaux de 48 Kio
  ou ~10 de 16 Kio par image. L'ordre d'envoi de l'hôte est donc bien celui
  qu'attend la page. Aucune image incomplète ni en erreur ; 28 à 37 images par
  passe ont été remplacées avant leur décodage, dans les trois bras PyroWave
  (1 à 8 par SCTP au §6.28).
- **Le GPU n'a plus que 0,55 ms de travail après le dernier octet**, au lieu
  de 2,2 : l'iDWT se fait pendant que l'image arrive.
- **Mais la dernière soumission démarre plus tard** : 1,4 à 2,0 ms avant que
  le GPU s'y mette, au lieu de 0,55-1,0 en images entières. Deux causes
  possibles : le GPU travaille encore aux morceaux d'avant, ou le processus
  GPU de Chrome digère encore leurs soumissions. Dix morceaux attendent un peu
  plus que trois alors que chacun porte moins de travail, ce qui penche pour
  les soumissions. Seuls des horodatages GPU sur les morceaux eux-mêmes
  trancheront.
- **Au net, −0,8 à −1 ms de soumission à la fin, et −0,5 à −0,75 ms du relais
  au dessin** (16 Kio : 7,25 → 6,5 en médiane, 8,0 → 7,5 en moyenne). Les
  tranches de 48 Kio tombent entre les deux, avec une passe moins bonne (r2 :
  soumission → fin 3,6 ms au lieu de 2,7). Hors sonde, −0,2 à −0,5 ms en
  médiane ; au clic, rien de lisible derrière les ±1 ms de la phase.
- **L'arrivée bouge à peine** : +0,1-0,2 ms du relais à l'arrivée avec les
  tranches, le temps que le worker poste les morceaux au fil principal.
- **La boucle de relance tourne moins** : 2,6-2,7 ms par image avec les
  tranches de 16 Kio, ~32 % d'un cœur au lieu de ~40 %.
- **La route audio vaut ~1 ms de mieux que SCTP pour PyroWave**, relance
  allumée : du relais à l'arrivée, 3,9-4,0 → 3,0-3,1 ms ; du relais au dessin,
  8,1-8,3 → 7,0-7,4 ms. Le §6.26 voyait 0,7 ms à l'arrivée.

**Bilan depuis le §6.26.** Du relais au dessin, PyroWave est passé de 9,2-9,5
ms (SCTP, sans relance, §6.28) à 6,5 ms : relance, route audio et tranches de
16 Kio, soit −2,9 ms. Il reste ~4,9 ms derrière le HEVC du produit (1,55 ms) :

- **l'arrivée, +2,2-2,3 ms** (3,1-3,2 contre 0,9) : 177 Ko par image, dont
  ~1,4 ms de fil à 1 Gbit/s ;
- **la page après le dernier octet, ~3,3 ms contre ~0,7** : 0,55 ms de GPU,
  et autour de lui 1,4-2,0 ms avant le départ, 0,25-0,7 ms avant le rappel et
  0,2 ms de dessin.

**Verdict.** Les tranches sont un gain, plus régulier à 16 Kio, mais petit.
Ce n'est plus le travail du GPU qui pèse dans la page : c'est le temps de
Chrome autour de la soumission. Les leviers qui restent :

- savoir où attend la dernière soumission (horodatages GPU sur les morceaux),
  pour choisir entre moins de soumissions et un iDWT plus rapide ;
- des images plus petites, pour l'arrivée ;
- le bout de chaîne, puis B2.1, après la page.

### 6.30 Où attend la dernière soumission : les morceaux horodatés (09/10/2026, 23:01-23:06)

Le banc du §6.29, même `build\`, avec le seul bras des tranches de 16 Kio
(relance allumée, route audio), deux passes de plus (r3 et r4). En trace
(`mw_ultra_trace=1`), la passe GPU de chaque morceau porte désormais ses
propres horodatages, relus avec ceux de l'image (`70326160`). Chaque morceau
ne démarre qu'après sa propre soumission. `gpuwait.py` en tire une borne de
plus pour recaler les deux horloges : elles se recalent à 0,11-0,12 ms près,
au lieu de 0,38-0,40. Les deux passes ont pris le LAN direct (la même paire
IPv6 interne qu'au §6.29). Les rapports sont dans le scratchpad (`pw120c/` :
`pieces.py`, `backlog.py`).

La dernière soumission d'une image (médianes en ms, r3 / r4) :

| Étape | r3 | r4 |
|---|---|---|
| relais → dessin (r1-r2 du §6.29 : 6,5) | 6,60 | 7,31 |
| soumission → fin (r1 du §6.29 : 2,9) | 2,6 | 3,3 |
| dernière soumission → début de sa passe | 1,69-1,80 | 2,35-2,47 |
| dont le GPU encore sur les morceaux | 1,35-1,47 | 1,89-2,00 |
| dont le GPU libre, avant la passe finale | 0,32 | 0,33 |
| images où le GPU travaille encore aux morceaux à la dernière soumission | 99 % | 99 % |

Les morceaux sur le GPU (horloge du GPU, médianes en ms) :

| Étape | r3 | r4 |
|---|---|---|
| passe d'un morceau | 0,054 (p90 1,02) | 0,054 (p90 1,07) |
| le morceau lourd, 7e sur 10, arrivé à +1,5-1,6 ms | 1,24 | 1,18 |
| chacun des neuf autres | 0,02-0,19 | 0,02-0,18 |
| les passes des morceaux, par image | 1,82 | 1,75 |
| début du premier → fin du dernier | 3,0 | 3,7 |
| GPU à vide entre les morceaux, par image | 1,2 | 1,7 |

De la dernière soumission à la fin du dernier morceau (horloges recalées au
milieu de leurs bornes, médianes en ms) :

| Part | r3 | r4 |
|---|---|---|
| l'intervalle | 1,41 | 1,94 |
| le morceau lourd | 1,13 | 1,11 |
| les autres morceaux | 0,09 | 0,12 |
| le GPU à vide, un morceau pas encore arrivé | 0,14 | 0,41 |

- **La dernière soumission attend le GPU, pas Chrome.** Dans 99 % des images,
  le GPU travaille encore aux morceaux quand elle part. De ses 1,7-2,4 ms
  d'attente, ~1,1 ms est le morceau lourd qui tourne encore, ~0,1 ms les
  autres morceaux, et 0,15-0,4 ms le GPU à vide, faute d'un morceau que Chrome
  ne lui a pas encore passé. Une fois le GPU libre, la passe finale démarre en
  0,32 ms. Le coût de Chrome lui-même, entre une soumission et le GPU, n'est
  donc qu'une petite part de l'attente.
- **Un seul morceau porte les deux tiers du décodage.** Le 7e sur 10 prend à
  lui seul 1,18-1,24 ms de GPU, sur les 1,75-1,82 ms des morceaux (1,77 pour
  l'image entière au §6.29). D'après l'ordre d'envoi, c'est là que le niveau 1
  se complète et que se débloque l'iDWT des deux niveaux les plus fins. Les
  niveaux grossiers occupent donc ~70 % des octets de ces images. Avant lui,
  les six premiers morceaux ne portent que ~0,5 ms de travail en 1,5 ms, et le
  GPU attend surtout.
- **Les morceaux s'empilent dans Chrome.** Une soumission atteint le GPU en
  0,28 ms quand le morceau d'avant y est fini (médiane ; 0,5 en moyenne), et
  en 0,87 ms sinon, ce qui arrive pour trois morceaux sur quatre. Le GPU les
  enchaîne ensuite, 0,12-0,15 ms entre deux départs contre 0,2 entre deux
  soumissions. Il reste malgré tout à vide 1,2 à 1,7 ms par image entre les
  morceaux.
- **Les références vides ne mesurent plus Chrome seul en tranches.** Soumises
  à la fin d'une image, elles attendent derrière les morceaux de la suivante
  (1,9-2,0 ms avant le départ du GPU). C'est ce que le §6.29 voyait.
- **Les horodatages des morceaux coûtent peu.** La r3 tombe à 0,1 ms des r1-r2
  du §6.29, du relais au dessin, et décode plus vite (2,6 ms de soumission à la
  fin, contre 2,9). La r4 perd 0,7 ms : l'hôte rend son image plus tard (2,15
  ms de la capture au relais), et ses morceaux mettent plus longtemps à
  atteindre le GPU, comme la r2 de 48 Kio au §6.29.

**Verdict.** Il y a moins à gagner à soumettre moins souvent : ~0,2-0,4 ms
au mieux, le GPU à vide avant le dernier morceau. Le levier est l'iDWT : plus
rapide, ou lancé plus tôt.

- **Plus rapide** : le morceau lourd est l'iDWT des niveaux fins sur la 780M.
  Le gain le plus proche est ~0,5-1,1 ms, et il vaut aussi pour les images
  entières.
- **Plus tôt** : l'iDWT du niveau 1 attend que tout le niveau soit là, vers 70
  % des octets. L'hôte pourrait l'entrelacer par lignes comme le niveau le
  plus fin, mais l'iDWT irait alors par bandes sur deux niveaux, et ça demande
  l'hôte et la page.

Même au mieux, ce levier n'enlève qu'~1 ms des ~4,9 ms de retard sur le HEVC
du produit. Le plus gros poste reste l'arrivée : +2,2-2,3 ms pour 177 Ko par
image.

### 6.31 Un iDWT deux fois plus rapide sur les iGPU AMD, au labo (09/10/2026, 23:31-23:36)

Le levier du §6.30, d'abord au labo, sans stream. Il tourne dans le Chrome
headless de `decoder_lab.py`, sur les quatre GPU l'un après l'autre (banc donné
par 59). Le labo a quatre options de plus :
- `--split` chronomètre chaque étape seule, dans sa propre passe : la
  déquantification, chaque niveau de l'iDWT, la conversion (`decodeSplit()`).
- `--idwt 1|2` choisit le shader de l'iDWT.
- `--cmp` compare les plans f32 à ceux de l'ancien shader, image par image.
- `--bw` mesure la bande passante mémoire du GPU, en copiant 64 Mio.

**Le nouveau shader** (`IDWT2_WGSL` dans `PyroWaveDecoder.js`, désormais le
défaut) reprend la structure de `idwt.comp` de l'amont :
- 64 threads par tuile de 32×32, au lieu de 256.
- Chaque thread fait les quatre étapes de levage dans ses registres. Il les
  fait sur une suite de 8 sorties (16 échantillons avec leur marge) de deux
  lignes à la fois, puis de deux colonnes.
- La mémoire du groupe ne garde la tuile qu'entre les étapes. Il y a trois
  barrières au lieu de neuf, et environ six fois moins d'accès à la mémoire
  partagée.
- Il fait les mêmes opérations, dans le même ordre. Ses plans f32 sont donc
  identiques, au bit près, à ceux de l'ancien. C'est vrai sur SwiftShader
  (essayé d'abord) et sur les quatre GPU, pour une image entière comme par
  tranches. L'écart à l'oracle reste de 1 code.
- L'ancien shader reste pour l'A/B du banc : `mw_ultra_idwt=1` dans la page,
  `--idwt 1` au labo. Le résumé du lecteur dit lequel a tourné (`idwt`).

Chaque niveau de l'iDWT seul, ancien → nouveau shader (1080p, `game10` à 170
Mbit/s, p50 en ms) :

| GPU | Niveau 4 | 3 | 2 | 1 | 0 | iDWT | Image entière |
|---|---|---|---|---|---|---|---|
| 780M (UM790Pro) | 0,024 → 0,019 | 0,050 → 0,030 | 0,147 → 0,073 | 0,524 → 0,265 | 0,692 → 0,344 | 1,44 → 0,73 | 1,98 → 1,28 |
| iGPU AMD (2 CU) | 0,039 → 0,018 | 0,118 → 0,051 | 0,395 → 0,173 | 1,50 → 0,70 | 1,98 → 0,90 | 4,03 → 1,84 | 5,06 → 2,90 |
| Arc A380 | 0,022 → 0,013 | 0,022 → 0,016 | 0,064 → 0,052 | 0,217 → 0,179 | 0,301 → 0,272 | 0,63 → 0,53 | 1,15 → 1,06 |
| RTX 5060 Ti | 0,005 → 0,004 | 0,007 → 0,005 | 0,014 → 0,008 | 0,042 → 0,024 | 0,054 → 0,031 | 0,121 → 0,072 | 0,170 → 0,122 |

- « Image entière » est le décodage du labo en une passe : déquantification,
  iDWT et conversion en 8 bits. Le produit fait l'affichage à la place de cette
  conversion.
- Les autres étapes ne changent pas. Sur la 780M, la déquantification prend
  0,34 ms et la conversion 0,21 ms. Sur l'iGPU AMD : 0,66 et 0,28 ms. Sur
  l'Arc : 0,28 et 0,24 ms. Sur la RTX : 0,037 et 0,016 ms.
- En 16 morceaux, sur la 780M, il reste au dernier morceau 0,207 ms au lieu de
  0,335 pour `game10`, et 0,112 au lieu de 0,185 pour `text10`. En 8 morceaux,
  0,29 et 0,21 ms (0,48 et 0,33 avec l'ancien shader au §6.24).

Ce que le labo dit :
- **Le coût d'un niveau suit son nombre d'échantillons.** Sur la 780M, avec
  l'ancien shader, les niveaux 1 et 0 coûtent tous deux 0,33 ns par
  échantillon. Le niveau 1 n'a donc rien d'anormal.
- **Les barrières entre dispatches ne pèsent pas.** Les étapes, chacune dans
  sa passe, font 1,98 ms de bout en bout, autant que le décodage en une seule
  passe. Leurs cinq niveaux font 1,44 ms, comme l'iDWT d'un seul tenant au
  §6.13.
- **Le morceau lourd du §6.30 reste à expliquer.** Au labo, le niveau 1 seul
  prend 0,52 ms. Le morceau lourd du flux (1,2 ms) porte donc plus que lui, ou
  tourne plus lentement. Le banc sur le câble le dira, avec les mêmes
  horodatages.
- **Sur la 780M, le nouveau shader approche la limite de la mémoire.** La copie
  y va à 75 Go/s (iGPU AMD 64, Arc 145, RTX 401). En f32, le niveau 0 lit
  8,4 Mo de bandes et écrit 8,4 Mo, soit au moins 0,22 ms. Le nouveau shader y
  met 0,34 ms.
- **La suite du gain demande moins d'octets.** L'amont garde ses deux niveaux
  fins en FP16 (`PYROWAVE_PRECISION=1` : R16F, calculs en f32). Faire de même
  diviserait par deux les octets des deux niveaux fins : déquantification, iDWT
  et affichage. Sur la 780M, j'estime le gain de plus à 0,3-0,5 ms.

**Ce que le flux peut en attendre** (à mesurer sur le câble) :
- Sur la 780M, une image entière devrait se décoder ~0,7 ms plus vite.
- Par tranches, le gain tombe là où la dernière soumission attendait le GPU,
  encore sur les morceaux : 1,35-2,0 ms au §6.30. Les niveaux 1 et 0 y
  coûtent moitié moins. La passe finale devrait donc partir jusqu'à ~0,5-0,7
  ms plus tôt.
- Même ainsi, ce n'est qu'une part des ~4,9 ms de retard sur le HEVC.

### 6.32 Le nouvel iDWT sur le câble : −0,6 ms par tranches, −1 ms en images entières (10/10/2026, 02:28-03:12)

Le banc des §6.29-6.30, avec le shader pour seul facteur :
- l'UM790Pro sous Windows en client ;
- la `--dev` de DualRTX sur l'écran virtuel du produit à 240 Hz, rendu par la
  RTX ;
- 120 i/s, relance allumée, PyroWave sur la route audio.

`build\` a été recompilé à 02:27 sur `e2e46af4`. Il avait deux commits de
retard, dont `7315bfcb` pour les candidats ICE.

Cinq bras, en 19 passes de 60 clics (`scratchpad/pw120c/run-idwt.sh`) :
- `h9` : le HEVC du produit par SCTP, au début, au milieu et à la fin de la
  série ;
- `ko` / `kn` : PyroWave par tranches de 16 Kio, ancien / nouveau shader
  (`mw_ultra_idwt=1` / `2`) ;
- `wo` / `wn` : PyroWave en images entières, ancien / nouveau shader.

Chaque mode a deux ABBA, le second en BAAB. Les 16 passes PyroWave ont pris
le shader demandé, comme l'indique le résumé du lecteur. Toutes les passes ont
pris le LAN direct (la paire IPv6 interne). Aucun TDR (événement 4101) sur les
deux PC.

Les rapports sont dans le scratchpad (`pw120c/` : `relaydrawn.py`,
`report.py`, `backlog.py`, `pieces.py`, `slicesum.py`), plus `gpuwait.py`.

Du relais au dessin (en ms : moyenne des médianes de chaque passe ; entre
crochets, la plus basse et la plus haute) :

| Bras | relais → arrivée | relais → dessin, p50 | moyenne | p90 |
|---|---|---|---|---|
| HEVC (SCTP) | 0,86 | 1,52 [1,51-1,52] | 1,67 | 2,04 |
| tranches, ancien | 3,32 | 6,79 [6,56-7,01] | 7,69 | 10,77 |
| tranches, nouveau | 3,33 | 6,21 [6,09-6,43] | 6,99 | 9,37 |
| entières, ancien | 3,37 | 7,77 [7,27-8,22] | 8,42 | 11,16 |
| entières, nouveau | 3,38 | 6,78 [6,28-7,17] | 7,33 | 9,41 |

Le décodage sur le GPU de la 780M (horloge du GPU, médianes en ms) :

| Étape | Ancien | Nouveau |
|---|---|---|
| image entière : passe de décodage | 1,77 | 1,07 |
| image entière : passe d'affichage | 0,31 | 0,31 |
| image entière : soumission → fin (horloge de la page), p50 / p90 | 3,3-3,9 / 4,5-5,1 | 2,6 / 3,5-3,7 |
| tranches : le morceau lourd, 7e sur 10 | 1,24-1,26 | 0,79 |
| tranches : les morceaux et la passe finale, par image | 1,84-1,89 | 1,28 |
| tranches : dernière soumission → fin du dernier morceau | 1,41-1,96 | 1,03-1,50 |
| dont le morceau lourd encore en cours | 1,09-1,15 | 0,73-0,78 |

Les clics (`report.py`, en ms ; 180 clics pour le HEVC, 240 par bras
PyroWave) :

| Bras | Clic p50 | Moyenne | p90 | Capture → dessin hors des clics, moyenne |
|---|---|---|---|---|
| HEVC (SCTP) | 10,4 | 9,9 | 13,9 | 3,7 |
| tranches, ancien | 15,8 | 17,3 | 23,2 | 9,3 |
| tranches, nouveau | 15,5 | 16,3 | 21,9 | 8,6 |
| entières, ancien | 17,3 | 17,5 | 22,7 | 10,0 |
| entières, nouveau | 15,8 | 16,8 | 22,8 | 9,1 |

Ce que la série dit :
- **Le gain du labo se retrouve en entier.** En images entières, la passe de
  décodage tombe de 1,77 à 1,07 ms. C'est exactement le labo sans sa
  conversion en 8 bits (1,98 − 0,21 et 1,28 − 0,21, §6.31). Dans le flux, la
  780M décode donc à la vitesse du labo. Du relais au dessin, l'image gagne
  1,0 ms en médiane, 1,1 en moyenne et 1,75 au p90. C'est plus que les 0,7 ms
  du GPU, car la queue de l'attente raccourcit aussi.
- **Par tranches, −0,6 ms en médiane, −0,7 en moyenne, −1,4 au p90.** Le
  morceau lourd passe de 1,24 à 0,79 ms, et la dernière soumission attend
  d'autant moins. Les deux bras ne se recouvrent pas : la passe la plus lente
  du nouveau shader (6,43 ms) bat la plus rapide de l'ancien (6,56). C'est
  pareil en images entières (7,17 contre 7,27).
- **L'arrivée ne bouge pas** : 3,3-3,4 ms du relais à l'arrivée, dans les quatre
  bras. Tout le gain vient du décodage.
- **Le clic suit, en plus petit et plus bruité.** Il gagne 1,0 ms en moyenne
  par tranches et 0,7 en images entières. Hors des clics, la capture → dessin
  gagne 0,7-0,9 ms en moyenne.
- **Le morceau lourd du §6.30 porte plus que le niveau 1.** Dans le flux, le
  GPU ne va pas plus lentement qu'au labo : les images entières le montrent.
  Le nouveau shader gagne 0,45 ms sur ce morceau. Comme il divise l'iDWT par
  deux, le morceau porte ~0,9 ms d'iDWT avec l'ancien shader : tout le niveau
  1 (0,52 ms au labo) et à peu près la moitié du niveau 0. Il reste ~0,34 ms
  qui ne dépendent pas du shader, autant que la déquantification d'une image
  entière au labo.
  - Hypothèse, non vérifiée : quand le niveau 1 se complète, la frontière
    règle d'un coup les rangées du niveau 0 encore sans bloc. Leurs blocs sont
    alors déquantifiés et transformés avec lui.
- **Le découpage coûte plus au nouveau shader.** Par tranches, les morceaux et
  la passe finale font 1,28 ms de GPU par image, contre 1,07 pour l'image
  entière (+0,21). L'ancien shader ne perdait que 0,08 ms (1,84-1,89 contre
  1,77). Je n'ai pas cherché la cause. Une piste : les bandes du niveau 0 sont
  de petits dispatches, que les groupes de 64 threads remplissent moins que
  les 256 de l'ancien.
- **Les tranches gagnent moins qu'avant.** Avec le nouveau shader, elles battent
  l'image entière de 0,57 ms (6,21 contre 6,78), contre 0,98 avec l'ancien.
  L'image entière du nouveau shader vaut les tranches de l'ancien.
- **La relance tourne moins** : 8,0-9,9 commandes vides par attente, au lieu de
  9,8-13,4.

**Verdict.** Le nouveau shader reste le défaut. Du relais au dessin, PyroWave
reste à ~4,7 ms du HEVC du produit (6,21 contre 1,52 en médiane), et à 6,4 ms
au clic moyen. Cet écart se compose de :
- l'arrivée : +2,5 ms (3,33 contre 0,86) ;
- la page après l'arrivée : +2,2 ms (2,88 contre 0,66).

Les leviers suivants :
- **le FP16 des deux niveaux fins**, comme l'amont : −0,3 à −0,5 ms estimées
  sur la 780M ;
- **le surcoût du découpage** avec le nouveau shader : 0,21 ms de GPU par
  image ;
- **l'arrivée** : 177 Ko par image. Des images plus petites sont une décision
  de Bruno.

### 6.33 Les deux niveaux fins en FP16, au labo : −0,17 ms de décodage sur la 780M, l'iDWT d'AMD ne bouge pas (10/10/2026, 05:47-05:54)

Le premier levier du §6.32, au labo d'abord, sans stream. Il tourne dans le
Chrome headless de `decoder_lab.py` : sur SwiftShader d'abord, puis sur les
quatre GPU l'un après l'autre (banc donné par 59). Aucun TDR.

**Le changement** (`1c4d4626`) reprend le défaut de l'amont sur ordinateur
(`PYROWAVE_PRECISION=1`) :
- Les bandes des niveaux 0 et 1 et les plans de sortie sont stockés en FP16.
  Les niveaux plus grossiers restent en f32, et tous les calculs se font en
  f32.
- Le stockage reste un tampon. Deux échantillons voisins d'une ligne tiennent
  dans un mot de 32 bits (`pack2x16float`). Il n'y faut pas la fonction
  `shader-f16` de WebGPU : le chemin marche partout.
- La déquantification écrit quatre mots par sous-bloc dans un plan FP16.
  L'iDWT existe en trois variantes, selon le stockage des bandes d'un niveau
  et de sa sortie. L'affichage et la conversion du labo lisent l'un ou
  l'autre.
- `mw_ultra_fp16=0` (⚠️ collante) garde tout en f32, pour l'A/B. Le résumé du
  lecteur dit lequel a tourné (`fp16`). L'ancien shader d'iDWT reste en f32
  seulement.
- Le labo gagne de quoi mesurer :
  - `--fp16 0|1` choisit le stockage ;
  - `--stages dequant+idwt` chronomètre la passe de décodage du lecteur ;
  - par tranches, chaque morceau est chronométré, et leur somme donnée ;
  - `--present` chronomètre aussi l'affichage ;
  - `--cmp` compare aussi l'image 8 bits au chemin f32.

**L'image reste juste.**
- Sur les quatre GPU, elle reste à 1 code de l'oracle, comme en f32.
- Le PSNR contre la source perd 0,03 dB sur `game10` (51,63 → 51,60) et
  0,01 dB sur `text10`.
- Contre le chemin f32, 1,6 % (`game10`) et 2,0 % (`text10`) des octets
  bougent d'un code, jamais plus. Sur SwiftShader, ce n'est que 0,4-0,6 %.
- Les quatre GPU, tous sous D3D12, donnent exactement la même image. Leur
  conversion en FP16 arrondit sans doute vers zéro, celle de SwiftShader au
  plus proche.
- Par tranches (7, 11 et 16 morceaux), les écarts sont les mêmes qu'en image
  entière.

f32 → FP16, 1080p, `game10` à 170 Mbit/s, p50 en ms. `text10` donne les mêmes
écarts.

| GPU | Déquantification | iDWT niveau 1 | niveau 0 | iDWT, 5 niveaux | Passe de décodage du lecteur | Affichage |
|---|---|---|---|---|---|---|
| 780M (UM790Pro) | 0,337 → 0,164 | 0,264 → 0,262 | 0,344 → 0,345 | 0,730 → 0,732 | 1,068 → 0,894 | 0,307 → 0,230 |
| iGPU AMD (2 CU) | 0,62-1,0 dans les deux cas | 0,695 → 0,665 | 0,895 → 0,889 | 1,83 → 1,80 | 2,50 → 2,27 | 0,583 → 0,550 |
| Arc A380 | 0,284 → 0,180 | 0,178 → 0,119 | 0,272 → 0,158 | 0,531 → 0,351 | 0,813 → 0,531 | 0,230 → 0,175 (`text10`) |
| RTX 5060 Ti | 0,038 → 0,021 | 0,024 → 0,019 | 0,031 → 0,024 | 0,072 → 0,059 | 0,105 → 0,078 | 0,020 → 0,020 |

- Chaque étape est mesurée seule, dans sa propre passe (`--split`, deux fois,
  dans les deux ordres : mêmes chiffres). La passe de décodage du lecteur est
  la déquantification et l'iDWT d'un seul tenant.
- La conversion en 8 bits du labo passe de 0,209 à 0,128 ms sur la 780M (Arc
  0,242 → 0,117).
- Par tranches, sur la 780M, il reste au dernier morceau 0,184 ms au lieu de
  0,238 en 11 morceaux, et 0,160 au lieu de 0,207 en 16.

Ce qui retient l'iDWT sur AMD, avec le FP16. Chaque variante de mesure (jamais
committée) retire une part du travail. Elle passe sur SwiftShader d'abord.
Niveau 1 / niveau 0, en ms :

| Variante | iGPU AMD (2 CU) | 780M |
|---|---|---|
| le shader tel quel | 0,665 / 0,889 | 0,262 / 0,345 |
| sans les contrôles de bornes de Dawn | 0,642 / 0,858 | — |
| sans lire les bandes (mêmes calculs d'adresse) | 0,561 / 0,747 | 0,222 / 0,293 |
| sans le levage | 0,560 / 0,745 | 0,232 / 0,307 |
| sans le miroir des bords | 0,620 / 0,829 | 0,246 / 0,323 |

Ce que le labo dit :
- **Le FP16 gagne partout, mais pas là où je l'attendais sur AMD.**
  - Sur la 780M, la déquantification va deux fois plus vite : elle était
    limitée par ses écritures. L'affichage et la conversion gagnent aussi.
  - L'iDWT ne bouge pas, ni sur la 780M ni sur l'iGPU AMD : sur AMD, ce ne
    sont pas les octets qui le limitent.
  - Sur l'Arc, l'iDWT tombe d'un tiers (0,531 → 0,351) : là, c'étaient les
    octets.
- **Mon estimation du §6.31 était fausse pour AMD.** J'y voyais le niveau 0
  limité par la mémoire. Les 0,3-0,5 ms attendues sur la 780M se réduisent à
  0,17 ms sur la passe de décodage et 0,08 sur l'affichage.
- **Ce qui retient l'iDWT sur AMD reste à trouver.** Aucune part n'y domine :
  - ne plus lire les bandes ne gagne que 15 % ;
  - sans le levage, 11-16 % ;
  - sans le miroir des bords, 6-7 % ;
  - sans les contrôles de bornes, 3-4 %.
  Le reste tient à la latence, à l'occupation des unités ou à la mémoire
  partagée. Il faudrait un profileur (RGP) pour trancher.
- **Le surcoût du découpage se retrouve au labo.** Sur la 780M, les passes de
  11 morceaux font 1,286 ms en f32 et 1,069 en FP16. La passe de décodage d'un
  seul tenant fait 1,068 et 0,894 : le surcoût est de +0,22 et +0,175 ms (le
  câble disait +0,21, §6.32).
  - Chaque morceau coûte environ 15 µs de GPU fixes sur la 780M et l'Arc, 4
    sur la RTX.
  - Hypothèse, non vérifiée : c'est la latence de petits dispatches, que leur
    travail ne remplit pas.
  - Ce surcoût ne pèse que là où le GPU est en retard sur les octets : autour
    du morceau lourd, puis à la fin.

**Ce que le flux peut en attendre** (à mesurer sur le câble) :
- Sur la 780M, en image entière : environ −0,25 ms (décodage −0,17,
  affichage −0,08).
- Par tranches, un peu moins à la fin : le dernier morceau gagne ~0,05 ms,
  l'affichage 0,08, et le morceau lourd une part de sa déquantification.
- Il resterait ~4,5 ms de retard sur le HEVC du produit.

**Verdict.** Le FP16 devient le défaut : il est plus rapide sur les quatre
GPU, et l'image reste à 1 code de l'oracle. Les leviers suivants :
- **l'iDWT sur AMD** : trouver ce qui le retient, au profileur, ou essayer
  d'autres structures (bandes chargées par vecteurs, `textureGather` comme
  l'amont) ;
- **le surcoût des morceaux** : moins de dispatches près de la fin ;
- **l'arrivée** : 177 Ko par image. Des images plus petites sont une décision
  de Bruno.

### 6.34 Le FP16 sur le câble : −0,5 ms en images entières, rien par tranches (10/10/2026, 06:09-06:53)

Le banc du §6.32, avec le stockage pour seul facteur (`mw_ultra_fp16=0` /
`1`) :
- l'UM790Pro sous Windows en client ;
- la `--dev` de DualRTX sur l'écran virtuel du produit à 240 Hz, rendu par la
  RTX ;
- 120 i/s, relance allumée, nouveau shader d'iDWT ;
- PyroWave sur la route audio.

`build\` date de 02:27 (`e2e46af4`) : depuis, seuls le JS du lecteur et la doc
ont changé, et le JS est servi en direct. Accord de Bruno pour l'écran
virtuel ; banc donné par 59.

Cinq bras, en 19 passes de 60 clics (`scratchpad/pw120c/run-fp16.sh`) :
- `h10` : le HEVC du produit par SCTP, au début, au milieu et à la fin ;
- `kf` / `kh` : PyroWave par tranches de 16 Kio, f32 / FP16 ;
- `wf` / `wh` : PyroWave en images entières, f32 / FP16.

Chaque mode a deux ABBA, le second en BAAB. Les 16 passes PyroWave ont pris le
stockage demandé, comme l'indique le résumé du lecteur. Aucun TDR (événement
4101) sur les deux PC.

Du relais au dessin (en ms : moyenne des médianes de chaque passe ; entre
crochets, la plus basse et la plus haute) :

| Bras | relais → arrivée | relais → dessin, p50 | moyenne | p90 |
|---|---|---|---|---|
| HEVC (SCTP) | 0,88 | 1,54 [1,52-1,56] | 1,71 | 2,10 |
| tranches, f32 | 3,33 | 6,01 [5,66-6,33] | 6,84 | 9,33 |
| tranches, FP16 | 3,21 | 6,02 [5,88-6,24] | 6,64 | 8,79 |
| entières, f32 | 3,04 | 6,41 [6,15-6,71] | 7,07 | 9,16 |
| entières, FP16 | 2,98 | 5,92 [5,68-6,22] | 6,53 | 8,28 |

Le décodage sur le GPU de la 780M (horloge du GPU, médianes en ms ; « puis »
sépare le premier ABBA du second) :

| Étape | f32 | FP16 |
|---|---|---|
| image entière : passe de décodage | 1,07 | 0,88 |
| image entière : passe d'affichage | 0,31 | 0,24 |
| image entière : du début du GPU à sa fin | 1,52 | 1,25 |
| image entière : soumission → fin (horloge de la page), p50 / p90 | 2,5-3,0 / 3,5-4,0 | 2,3 / 2,9-3,4 |
| tranches : le morceau lourd, 7e sur 10 | 0,79 | 0,65 |
| tranches : les morceaux et la passe finale, par image | 1,285 | 1,067 |
| tranches : dernière soumission → fin du dernier morceau | 1,05-1,10 puis 0,93-0,95 | 0,61-0,68 puis 1,34-1,50 |
| dont le GPU inactif, à attendre un morceau | 0,21-0,23 puis 0,15-0,16 | 0,14-0,15 puis 0,38-0,52 |

Les clics (`report.py`, en ms ; 180 clics pour le HEVC, 240 par bras
PyroWave) :

| Bras | Clic p50 | Moyenne | p90 | Capture → dessin hors des clics, moyenne |
|---|---|---|---|---|
| HEVC (SCTP) | 11,2 | 10,6 | 13,5 | 3,5 |
| tranches, f32 | 15,3 | 16,3 | 22,0 | 8,6 |
| tranches, FP16 | 15,1 | 15,9 | 21,4 | 8,4 |
| entières, f32 | 15,1 | 15,8 | 22,0 | 8,7 |
| entières, FP16 | 14,8 | 15,4 | 20,3 | 8,2 |

Ce que la série dit :
- **Le GPU fait au câble ce qu'il faisait au labo.** La passe de décodage
  tombe de 1,07 à 0,88 ms (labo : 1,068 → 0,894), l'affichage de 0,31 à 0,24.
  Par tranches, les morceaux et la passe finale passent de 1,285 à 1,067 ms
  (labo, 11 morceaux : 1,286 → 1,069). Le surcoût du découpage reste à +0,21
  et +0,18 ms.
- **En images entières, l'image gagne plus que le GPU.** Du relais au dessin :
  −0,49 ms en médiane, −0,54 en moyenne, −0,88 au p90, pour 0,27 ms de GPU.
  - Comme au §6.32, la queue de l'attente raccourcit aussi : soumission → fin
    −0,4 ms en médiane, −0,6 au p90.
  - La relance tourne moins : 8,5 commandes vides par attente au lieu de 9,9.
  - Le FP16 gagne dans les deux ABBA. Les bras se touchent pourtant : la
    passe FP16 la plus lente (6,22) est derrière la passe f32 la plus rapide
    (6,15).
- **Par tranches, la médiane ne bouge pas** (6,01 contre 6,02). La moyenne
  gagne 0,2 ms, le p90 0,55.
  - Le signe s'inverse d'un ABBA à l'autre : dans chacun, la paire du milieu
    gagne.
  - Le GPU gagne pourtant 0,22 ms par image, dont 0,14 sur le morceau lourd.
  - Au second ABBA, les deux passes FP16 ont attendu 1,3-1,5 ms entre la
    dernière soumission et la fin du dernier morceau, au lieu de 0,6-0,7 au
    premier. Leur GPU y restait inactif 0,4-0,5 ms, à attendre un morceau que
    Chrome n'avait pas encore apporté. Leur travail sur le GPU, lui, n'a pas
    bougé. Les passes f32 du même ABBA n'ont pas eu ce retard.
  - Je n'en ai pas trouvé la cause. Elle tient à la façon dont Chrome apporte
    les morceaux au GPU (§6.25), pas au GPU. Au second ABBA, les octets d'une
    image arrivaient aussi plus serrés : le dernier morceau 1,6 ms après le
    premier, au lieu de 1,9-2,0.
- **Les tranches ne gagnent plus rien.** En FP16, l'image entière les égale en
  médiane (5,92 contre 6,02) et les bat en moyenne (6,53 contre 6,64) et au
  p90 (8,28 contre 8,79). Au §6.32, les tranches gagnaient encore 0,57 ms.
  - L'image entière arrive aussi 0,2-0,3 ms plus tôt, avec les mêmes octets
    (2,98-3,04 contre 3,21-3,33 ms).
  - Hypothèse, non vérifiée : par tranches, le fil principal soumet chaque
    morceau pendant que l'image arrive, et il relève les messages suivants
    plus tard.
- **Le clic suit, en plus petit.** L'image entière en FP16 est le meilleur bras
  PyroWave : 15,4 ms en moyenne et 20,3 au p90, contre 15,8 et 22,0 en f32.
- **L'arrivée a été plus rapide qu'au §6.32** en image entière : 3,0 ms contre
  3,4. Il ne faut comparer les bras qu'à l'intérieur d'une même série.

**Verdict.** Le FP16 reste le défaut, et l'image entière, qui est le défaut du
lecteur, en profite pleinement. Du relais au dessin, PyroWave reste à ~4,4 ms
du HEVC du produit (5,92 contre 1,54 en médiane), et à 4,8 ms au clic moyen.
Cet écart se compose de :
- l'arrivée : +2,1 ms (2,98 contre 0,88) ;
- la page après l'arrivée : +2,3 ms (2,95 contre 0,66). Sur ces 2,95 ms, le
  GPU ne travaille que 1,25 ms.

Les leviers suivants :
- **l'iDWT sur AMD** : 0,73 ms sur la 780M, que le FP16 n'a pas touchées ;
- **la page après l'arrivée** : B2.1, jugé en bout de chaîne. La soumission →
  fin prend 2,3 ms pour 1,25 ms de GPU ;
- **le surcoût des morceaux** passe après : les tranches ne gagnent plus rien,
  tant que Chrome tarde à leur apporter les morceaux ;
- **l'arrivée** : 177 Ko par image. Des images plus petites sont une décision
  de Bruno.

### 6.35 L'iDWT sur AMD : les tuiles intérieures lues sans miroir, −40 % (10/10/2026, 07:20-07:38)

Le premier levier du §6.34, au labo, sans stream ni écran virtuel. Il tourne
dans le Chrome headless de `decoder_lab.py` : sur SwiftShader d'abord, puis sur
les quatre GPU l'un après l'autre (banc donné par 59). Aucun TDR.

**Ce qui retenait l'iDWT sur AMD.** Le §6.33 n'y trouvait aucune part
dominante. J'ai compté les instructions du shader 2, à la main et sans
profileur. La lecture de la fenêtre de 40×40 en fait environ les deux tiers :
- chaque échantillon y coûte trois miroirs, le choix de sa bande et, en FP16,
  le choix d'une moitié de mot ;
- chaque mot FP16 y est lu deux fois ;
- le levage, lui, n'en fait qu'un sixième.

Les variantes du §6.33 le montraient déjà : sans lire les bandes, mais avec les
mêmes calculs d'adresse, le shader ne gagnait que 15 %.

**Le changement** (`ec7d7ea5`) :
- Une tuile dont la fenêtre tient dans le niveau n'a rien à refléter. Elle lit
  ses bandes un mot à la fois : deux échantillons d'une ligne en FP16, un en
  f32. Les valeurs vont directement dans les registres du passage des lignes,
  sans calcul de miroir, sans passer par la mémoire du groupe, avec une
  barrière de moins.
- Les tuiles du bord gardent l'ancienne lecture. En 1080p, 91 % des tuiles du
  niveau 0 sont intérieures, et 82 % de celles du niveau 1.
- C'est le shader 3, désormais le défaut. Il donne les mêmes plans que le
  shader 2, au bit près : sur SwiftShader et les quatre GPU, en image entière et
  par tranches, en FP16 comme tout en f32.
- `mw_ultra_idwt=2` (⚠️ collante) garde le shader 2 pour l'A/B, `=1` le
  premier. Au labo, `--idwt 3` est le défaut ; `--cmp-idwt` et `--cmp-fp16`
  choisissent ce à quoi `--cmp` compare.

Shader 2 → 3, 1080p, `game10` et `text10` à 170 Mbit/s, p50 en ms. Chaque étape
est mesurée seule, dans sa passe (`--split`), dans les deux ordres.

| GPU | Niveau 1 | Niveau 0 | Niveaux 2-4 | iDWT, 5 niveaux | Passe de décodage du lecteur | Dernier de 11 morceaux |
|---|---|---|---|---|---|---|
| 780M (UM790Pro) | 0,262 → 0,160 | 0,345 → 0,181 | 0,124 → 0,105 | 0,732 → 0,446 | 0,88-0,90 → 0,60-0,62 | 0,184 → 0,116 |
| iGPU AMD (2 CU) | 0,667 → 0,400 | 0,891 → 0,491 | 0,240 → 0,182 | 1,80 → 1,07 | −0,4 à −0,7 | 0,49 → 0,29 |
| Arc A380 | 0,118 → 0,097 | 0,158 → 0,126 | 0,074 → 0,064 | 0,350 → 0,288 | 0,52-0,53 → 0,46-0,47 | 0,103 → 0,087 |
| RTX 5060 Ti | 0,019 → 0,015 | 0,024 → 0,018 | 0,017 → 0,017 | 0,057 → 0,050 | 0,077 → 0,068 | 0,017 → 0,016 |

- Le dernier morceau est celui de `game10`. Celui de `text10` passe de 0,116 à
  0,078 ms sur la 780M.
- Sur l'iGPU AMD, la passe de décodage n'est comparable qu'à l'intérieur d'une
  même série. Sa déquantification varie en effet de 0,42 à 0,93 ms d'une passe
  à l'autre. La première série donne 2,38-2,41 → 1,67-1,75 ms, la seconde
  2,24-2,28 → 1,83-1,93.
- En 11 morceaux sur la 780M, la somme des passes tombe de 1,07-1,09 à 0,80-0,83
  ms.

Deux autres variantes, essayées dans la même série, ne sont pas gardées :
- **La même lecture par mots, mais vers la mémoire du groupe**, comme les
  tuiles du bord. Elle gagne moins que la lecture vers les registres : 0,523 ms
  sur la 780M, au lieu de 0,465 ms pour la lecture vers les registres des seuls
  niveaux FP16. Sur l'Arc, elle ne gagne rien.
- **La tuile en FP16 dans la mémoire du groupe**, comme l'amont
  (`PYROWAVE_PRECISION=1`). Elle gagne encore jusqu'à 0,07 ms, mais sous D3D12
  l'image s'écarte de 2 codes de l'oracle (49,5-57,7 dB au plus bas, contre
  62,6-63,7) et perd 0,1-0,7 dB contre la source. Les quatre GPU donnent la
  même image : c'est la conversion en FP16 de D3D12, qui arrondit sans doute
  vers zéro (§6.33). Sur SwiftShader, elle reste à 1 code (−0,02 dB).

Ce que le labo dit :
- **Sur AMD, l'iDWT payait le calcul des adresses**, pas les octets ni le
  levage. Sans ce calcul, il gagne 40 % sur les deux GPU AMD, 18 % sur l'Arc et
  12 % sur la RTX.
- **Les tuiles du bord sont la plus grosse part qui reste à la lecture.** Par
  tuile, elles coûtent environ deux fois une tuile intérieure. J'estime, sur la
  780M, qu'elles font 17 % du niveau 0 et 29 % du niveau 1, soit ~0,08 ms. Les
  lire comme les autres en gagnerait ~0,04.
- **Les petits niveaux tiennent à la latence** de leurs dispatches : niveaux 3
  et 4 ensemble, ~0,045 ms sur la 780M, dont le shader 3 n'ôte que 0,005.
- **Sur l'iGPU AMD, l'amont reste devant.** Il fait son iDWT en 0,7 ms en
  Vulkan (§6.13), contre 1,07 pour nous.

**Ce que le flux peut en attendre** (à mesurer sur le câble) :
- Sur la 780M, en image entière : −0,27-0,29 ms sur la passe de décodage, donc
  environ −0,3 ms du relais au dessin. Au §6.34, le gain du GPU s'est retrouvé
  au moins tout entier dans le flux.
- Par tranches : −0,07 ms au dernier morceau, un peu plus au morceau lourd.
- Il resterait ~4,1 ms de retard sur le HEVC du produit.

**Verdict.** Le shader 3 devient le défaut : il est plus rapide sur les quatre
GPU, et ses plans sont identiques à ceux du shader 2. Sur la 780M, l'iDWT ne
prend plus que 0,45 ms, et la passe de décodage ~0,6 ms, des ~2,95 ms que la
page met après l'arrivée (§6.34). Les leviers suivants :
- **la page après l'arrivée** : B2.1, jugé en bout de chaîne. C'est le plus
  gros poste qui reste dans la page ;
- **les tuiles du bord** (~0,04 ms) et le surcoût des morceaux passent après ;
- **l'arrivée** : 177 Ko par image. Des images plus petites sont une décision
  de Bruno.

### 6.36 Le shader 3 sur le câble : −0,5 ms par tranches, presque rien en images entières (10/10/2026, 07:50-08:34)

Le banc du §6.34, avec le shader de l'iDWT pour seul facteur
(`mw_ultra_idwt=2` / `3`) :
- l'UM790Pro sous Windows en client ;
- la `--dev` de DualRTX sur l'écran virtuel du produit à 240 Hz, rendu par la
  RTX ;
- 120 i/s, relance allumée, FP16 sur toutes les passes ;
- PyroWave sur la route audio.

`build\` date toujours de 02:27 : depuis, seuls le JS du lecteur et la doc ont
changé. Accord de Bruno pour l'écran virtuel (« Go ») ; banc donné par 59.

Cinq bras, en 19 passes de 60 clics (`scratchpad/pw120c/run-idwt3.sh`) :
- `h11` : le HEVC du produit par SCTP, au début, au milieu et à la fin ;
- `k2` / `k3` : PyroWave par tranches de 16 Kio, shader 2 / shader 3 ;
- `w2` / `w3` : PyroWave en images entières, shader 2 / shader 3.

Chaque mode a deux ABBA, le second en BAAB. Les 16 passes PyroWave ont pris le
shader demandé, comme l'indique le résumé du lecteur. La dernière passe
PyroWave laisse `mw_ultra_idwt=3`, le défaut, dans le Chrome de banc. Aucun TDR
(événement 4101) sur les deux PC.

Du relais au dessin (en ms : moyenne des médianes de chaque passe ; entre
crochets, la plus basse et la plus haute) :

| Bras | relais → arrivée | relais → dessin, p50 | moyenne | p90 |
|---|---|---|---|---|
| HEVC (SCTP) | 0,87 | 1,54 [1,52-1,55] | 1,69 | 2,07 |
| tranches, shader 2 | 3,17 | 5,82 [5,34-6,17] | 6,48 | 8,85 |
| tranches, shader 3 | 3,17 | 5,32 [5,05-5,75] | 6,04 | 7,99 |
| entières, shader 2 | 2,96 | 5,80 [5,70-5,92] | 6,43 | 8,24 |
| entières, shader 3 | 2,92 | 5,73 [5,49-5,98] | 6,26 | 7,88 |

Le décodage sur le GPU de la 780M (horloge du GPU, médianes en ms) :

| Étape | Shader 2 | Shader 3 |
|---|---|---|
| image entière : passe de décodage | 0,88 | 0,60 |
| image entière : du début du GPU à sa fin | 1,25 | 0,97 |
| image entière : soumission → fin (horloge de la page), p50 | 2,3 | 2,0-2,4 |
| tranches : le morceau lourd | 0,65 | 0,41 |
| tranches : les morceaux et la passe finale, par image | 1,07 | 0,79 |
| tranches : dernière soumission → fin du dernier morceau, moyenne | 0,86-1,22 | 0,59-0,90 |

Les clics (`report.py`, en ms ; 180 clics pour le HEVC, 240 par bras
PyroWave) :

| Bras | Clic p50 | Moyenne | p90 | Capture → dessin hors des clics, moyenne |
|---|---|---|---|---|
| HEVC (SCTP) | 10,5 | 9,9 | 13,8 | 3,4 |
| tranches, shader 2 | 14,8 | 15,5 | 20,7 | 8,1 |
| tranches, shader 3 | 14,6 | 15,2 | 20,8 | 7,8 |
| entières, shader 2 | 14,5 | 15,3 | 20,3 | 8,4 |
| entières, shader 3 | 14,6 | 15,1 | 20,1 | 7,9 |

Ce que la série dit :
- **Le GPU fait au câble ce qu'il faisait au labo.** La passe de décodage
  tombe de 0,88 à 0,60 ms (labo : 0,89 → 0,61). Par tranches, les morceaux et
  la passe finale passent de 1,07 à 0,79 ms, et le morceau lourd de 0,65 à
  0,41.
- **Par tranches, l'image gagne 0,5 ms**, plus que le GPU (0,28 ms) : −0,50 en
  médiane dans chacun des deux ABBA, −0,44 en moyenne, −0,86 au p90.
  - Après la dernière soumission, le GPU a moins de retard à rattraper sur le
    morceau lourd. L'attente jusqu'à la fin du dernier morceau tombe de 1,0 à
    0,7 ms en moyenne.
  - Dans chaque ABBA, les deux passes du shader 3 sont devant les deux du
    shader 2. D'un ABBA à l'autre, les deux bras se touchent : le second ABBA
    est plus rapide pour les deux (`k2` 5,34 contre `k3` 5,41-5,75 au premier).
- **En images entières, l'image ne gagne presque rien** : −0,07 ms en médiane,
  −0,17 en moyenne, −0,35 au p90. Le premier ABBA gagne 0,20 ms, le second en
  perd 0,06.
  - La soumission → fin tombe de 2,3 à 2,0 ms la plupart du temps. Mais dans
    deux des quatre passes du shader 3, elle remonte à 2,5-2,8 ms pendant 20 à
    40 s.
  - Pendant ces phases, le travail du GPU ne bouge pas (0,60 ms) : c'est
    l'attente avant que le GPU démarre et après qu'il a fini qui s'allonge.
  - Les passes `k2` en montrent aussi, en tranches avec le shader 2. Ces phases
    ne viennent donc pas du shader 3. Je n'en ai pas trouvé la cause : elle est
    du côté de Chrome ou du pilote, pas du shader.
  - Les deux passes du shader 3 sans ces phases (5,49 et 5,58 ms) sont devant
    les quatre du shader 2 (5,70-5,92).
- **Les tranches repassent devant**, avec le shader 3 : 5,32 contre 5,73 ms en
  médiane, 6,04 contre 6,26 en moyenne. Au p90, l'image entière reste un peu
  meilleure (7,88 contre 7,99). Au §6.34, avec le shader 2, les deux se
  valaient.
- **Le clic bouge peu** : −0,2 à −0,3 ms en moyenne dans les deux modes, moins
  que le bruit d'une passe à l'autre.

**Verdict.** Le shader 3 reste le défaut. Il gagne dans les deux modes, et
surtout par tranches. Du relais au dessin, PyroWave reste derrière le HEVC du
produit :
- par tranches, à ~3,8 ms en médiane (5,32 contre 1,54), le meilleur écart
  mesuré jusqu'ici ;
- en images entières, qui sont le défaut du lecteur, à ~4,2 ms ;
- à ~5,2 ms au clic moyen (15,1-15,2 contre 9,9). Le HEVC a cliqué plus vite
  qu'au §6.34 (9,9 contre 10,6 ms), PyroWave à peine.

Par tranches, l'écart se compose de :
- l'arrivée : +2,3 ms (3,17 contre 0,87) ;
- la page après l'arrivée : +1,5 ms (2,15 contre 0,67).

Les leviers suivants :
- **le bout de chaîne** : juger à l'écran du client, et y départager les
  tranches et l'image entière. Les tranches (`mw_ultra_slices=1`) restent
  éteintes par défaut jusque-là ;
- **B2.1**, jugé au même endroit ;
- **les phases lentes de la soumission → fin**, à surveiller : elles mangent le
  gain du GPU en image entière ;
- **les tuiles du bord** (~0,04 ms) et le surcoût des morceaux passent après ;
- **l'arrivée** : 177 Ko par image. Des images plus petites sont une décision
  de Bruno.

### 6.37 Le bout de chaîne, à l'écran du client : la sonde du clic retient l'image qu'elle mesure (10/10/2026, 09:27-10:13)

Le banc du §6.36, avec trois changements :
- **le flux à 119 i/s**, contre l'écran à 120 Hz du client (le M27Q de
  l'UM790Pro, DISPLAY1). Le décalage entre l'arrivée des images et les
  compositions du DWM fait alors un tour par seconde. Une moyenne sur les clics
  n'est donc pas liée à une seule phase : à 120 i/s contre 120 Hz, cette phase
  ne bouge presque pas pendant une passe, et chaque passe tomberait sur sa
  propre marche de 8,3 ms (§6.5) ;
- **un guetteur sur le client** : `mw-click-sound --tick` dans la session
  console, sans clic à lui. Il trouve le drapeau de l'hôte dans la fenêtre par
  ses couleurs et date chaque apparition sur le bureau composé, sur QPC ;
- **l'origine de la page sur l'horloge de Chrome** (`MW_BENCH_TICKS_ORIGIN=1`,
  `39089fe9`) : des repères `performance.mark` lus dans une courte trace CDP.
  Sous Windows, cette horloge est QPC en µs. Vérifié sur DualRTX : l'heure de
  la page ainsi recalée tombe entre deux lectures de QPC qui l'encadrent.
  L'écart entre les cinq repères est de 15 à 112 µs, soit l'arrondi de
  `performance.now()`.

Accord de Bruno pour l'écran virtuel (« Go. Je te donne mon accord. ») ; banc
donné par 59. Deux passes d'essai, puis 19 passes de 60 clics de la sonde
(`scratchpad/pw120c/run-endchain.sh`, rapports `endqpc-report.txt`,
`endsplit-report.txt`, `relaydrawn-endchain.txt`). Aucun TDR (événement 4101)
sur les deux PC. Les bras :
- `h12` : le HEVC du produit par SCTP, au début, au milieu et à la fin ;
- `xw` : PyroWave en images entières, shader 3, FP16, relance allumée ;
- `xk` : par tranches de 16 Kio ;
- `xe` : `xw` avec `mw_ultra_early=1`, l'image remise à la soumission ;
- `xn` : `xw` sans la relance.

**Deux essais écartés.**
- PresentMon (celui du pilote AMD) ne voit aucune présentation de Chrome :
  Chrome compose par DirectComposition, sans `Present`. PresentMon ne voit que
  les compositions du DWM.
- Une lecture GDI d'un pixel de l'écran (`GetPixel`) attend la composition
  suivante : 8,3 ms par lecture à 120 Hz, mesuré sur l'UM790Pro.
  `click-photon.ps1` ne lit donc qu'une fois par composition, pas toutes les
  ~1 ms comme le dit son README. Pour le guetteur, c'est un avantage : chaque
  apparition est datée à la composition près (±0,5-0,8 ms autour de leur
  grille), sans charger le GPU.

Chaque passe a ses 60 clics vus à l'écran, 60 sur 60. Les phases des dessins
se répartissent sur la période : entre 3 et 35 clics par quart de période.

Du relais au dessin, sur toutes les images (médiane, en ms ; entre crochets,
la plus basse et la plus haute des passes) :

| Bras | relais → dessin |
|---|---|
| HEVC (SCTP) | 1,61 [1,57-1,65] |
| entières | 5,72 [5,49-6,19] |
| tranches | 5,48 [5,17-5,72] |
| entières, `early` | 3,61 [3,49-3,96] |
| entières, sans relance | 7,05 [7,00-7,09] |

Du clic à l'écran (en ms ; 180 clics pour le HEVC, 240 par bras PyroWave) :
- **clic → écran** : de l'horodatage du clic par la sonde, mis sur QPC, à
  l'apparition du drapeau sur le bureau composé ;
- **sonde** : son clic → dessin habituel ;
- **relecture** : la durée du dessin de l'image du drapeau (`drawn − drawStart`
  du journal des images), où la sonde relit le canevas.

| Bras | Clic → écran, moyenne (erreur type) | Médiane | Passes | Sonde | Relecture | Début du dessin → écran |
|---|---|---|---|---|---|---|
| HEVC (SCTP) | 23,9 (0,5) | 23,9 | 24,0 / 23,1 / 24,5 | 10,7 | 4,1 | 13,2 |
| entières | 28,2 (0,4) | 27,5 | 28,4 / 27,6 / 28,0 / 29,0 | 15,8 | 3,5 | 12,4 |
| tranches | 28,3 (0,4) | 27,3 | 27,8 / 28,3 / 28,0 / 29,2 | 15,2 | 3,7 | 13,1 |
| `early` | 28,3 (0,4) | 27,4 | 27,2 / 29,4 / 28,3 / 28,3 | 13,4 | 4,8 | 14,9 |
| sans relance | 28,5 (0,4) | 28,3 | 28,1 / 29,3 / 28,5 / 28,3 | 16,2 | 3,8 | 12,3 |

Ce que la série dit :
- **La sonde du clic retient l'image qu'elle mesure.** Pour lire le drapeau,
  elle dessine le canevas de sortie dans un petit canevas lu par le CPU
  (`getImageData`). Cette relecture prend 3,5 à 4,8 ms sur la 780M, sur
  l'image même dont on veut l'heure d'écran.
  - Les images du drapeau mettent 4,0 à 4,6 ms de plus du relais au dessin que
    les autres images de la même minute (HEVC 5,7 contre 1,7 ms ; images
    entières 10,8 contre 6,3).
  - Le clic de la sonde s'arrête avant cette relecture (`652fc726`). Le reste
    de la chaîne, lui, la paie.
- **À l'écran, ce que la sonde gagnait au dessin disparaît**, dans ces
  conditions :
  - `early` dessine 2,4 ms plus tôt au clic de la sonde (2,1 ms sur toutes les
    images), et arrive à l'écran au même moment que `xw` (28,3 contre 28,2 ms).
    Sa relecture est plus longue (4,8 contre 3,5 ms) : elle attend la fin du
    travail du GPU que la page n'attend plus.
  - La relance avance le dessin de 1,3 ms sur toutes les images, et de 0,4 ms
    au clic de la sonde. À l'écran, l'écart est de 0,3 ms (28,2 contre 28,5),
    moins que l'erreur type.
  - Les tranches et l'image entière se valent à l'écran (28,3 contre 28,2).
- **PyroWave est à ~4,3 ms du HEVC à l'écran** (28,2 contre 23,9), contre
  ~5,1 ms au clic de la sonde (15,8 contre 10,7). La relecture du HEVC est plus
  longue (4,1 contre 3,5 ms), ce qui réduit l'écart à l'écran. Ce chiffre
  n'est donc pas encore celui d'un usage réel.
- **De la fin de la relecture à l'écran, il faut ~9 ms en moyenne** dans tous
  les bras (8,8-10,3) : une demi-période (4,2 ms), plus ~5 ms avant que l'image
  puisse être composée. Ces ~5 ms sont le chemin de Chrome jusqu'au DWM, plus
  le délai de composition du DWM. Le codec n'y change rien.

**Verdict.** Cette série ne départage pas `early`, la relance et les tranches.
La sonde est sur le chemin de l'image mesurée, et sa relecture attend le GPU :
elle efface ce que ces leviers gagnent, ou une partie. Rien ne change dans le
lecteur : les tranches, `early` et la relance restent des clés de banc, et le
défaut reste l'image entière.

**La mesure propre** : les clics donnés par le client lui-même (`SendInput`,
`mw-click-sound --clicks`), la sonde de la page au repos, donc sans relecture,
le même guetteur à l'écran et le flux à 119 i/s. Le journal des images de la
page reste enregistré, pour le relais → dessin de toutes les images. Il faut
pour cela que `pass.py` enregistre ce journal en mode `--hold`, et qu'il tienne
le flux le temps des clics. Puis B2.1 au même endroit.

### 6.38 Le bout de chaîne propre : les clics du client lui-même, la relance gagne 2,5 ms à l'écran (10/10/2026, 10:42-11:41)

Le banc du §6.37, mais les clics viennent du client lui-même, et la sonde de la
page reste au repos : aucune relecture du canevas sur l'image du drapeau.
- **Les clics** : `mw-click-sound --clicks 70 --interval 803 --warmup 3
  --window MoonlightWeb --center` dans la session console de l'UM790Pro
  (`6b158f86`). Il pose le curseur au milieu de la fenêtre du flux, clique par
  `SendInput` et date chaque clic sur QPC. Il date aussi l'apparition de son
  drapeau sur le bureau composé, comme le guetteur du §6.37. 803 ms et non
  800 : 800 ms font tout juste 96 périodes de 120 Hz, et la phase du clic
  resterait la même d'un clic à l'autre.
- **La page** : `pass.py --hold 75` enregistre toutes les images du maintien
  (`<tag>.frames.csv`) avec l'origine de la page (`032205ce`). Il écrit un
  repère au début du maintien (`MW_BENCH_HOLD_MARK`), et le lanceur part de ce
  repère pour lancer les clics.
- **Le découpage** (`scratchpad/pw120c/endclean.py`) : tout est mis sur le QPC
  du client. Les heures de l'hôte passent sur l'horloge de la page par
  l'estimation du journal des images (`hostMs − captureMs`), puis sur QPC par
  l'origine. L'image du drapeau est la première présentation du bureau après
  celle de `mw-click-target`, puis la première image que le flux emporte.
  Testé d'abord sur les fichiers du §6.37 : il retrouve son clic → écran
  (`xk` 27,81 ms, comme `endqpc.py`).

Accord de Bruno pour l'écran virtuel (« Oui, vas-y ») ; banc donné par 59. Deux
passes d'essai, puis 19 passes ABBA de 70 clics (`scratchpad/pw120c/run-clean.sh`,
rapport `endclean-report.txt`). Aucun TDR (événement 4101) sur les deux PC. Les
bras sont ceux du §6.37 : `ch` (HEVC par SCTP), `cw` (images entières, shader 3,
FP16, relance allumée), `ck` (par tranches de 16 Kio), `ce` (`cw` avec
`mw_ultra_early=1`) et `cn` (`cw` sans la relance).

**La première passe d'essai a gelé côté client.** Le flux s'est arrêté 8 s
après le début du maintien, au 6ᵉ clic, et n'est pas reparti.
- Le lecteur n'a plus reçu d'image, et le débit lu sur la page était de
  0 Mbit/s. L'hôte, lui, a continué d'envoyer.
- Le temps d'aller-retour SCTP vu par l'hôte est passé de ~2 à 80-117 ms : le
  client répondait encore, mais lentement.
- Cause non trouvée. Le gel ne s'est pas reproduit dans les 20 passes
  suivantes, avec un second client CDP sur la page (console et état toutes les
  5 s, `cdpspy.py`). S'il revient, ce sera un bug du produit, pas du banc.

Toutes les passes de la série ont leurs 73 appuis reçus par l'hôte et 8 880 à
8 924 images dans le journal. Il manque 0 à 2 clics par passe : ceux dont
l'image n'a pas été retrouvée. Rien n'a été joué par l'outil. Deux clics de la
même passe (`ck-r2`, vers 11:11) ont pourtant entendu un son dans la sortie de
l'UM790Pro, de source inconnue (DualRTX dans le flux, ou une notification du
client).

Du clic à l'écran, sur le QPC du client seul (en ms ; 210 clics pour le HEVC,
277 à 280 par bras PyroWave) :

| Bras | Clic → écran, moyenne (erreur type) | Médiane | Passes | Relais → dessin, toutes les images | Dessin → écran |
|---|---|---|---|---|---|
| HEVC (SCTP) | 22,5 (0,3) | 22,4 | 22,2 / 22,5 / 22,7 | 1,12 | 10,3 |
| entières | 25,8 (0,4) | 24,8 | 25,3 / 25,6 / 26,5 / 25,7 | 5,89 | 8,6 |
| tranches | 25,8 (0,4) | 25,1 | 26,2 / 26,2 / 24,8 / 26,1 | 5,66 | 8,7 |
| `early` | 24,8 (0,3) | 23,7 | 25,3 / 24,7 / 24,6 / 24,6 | 3,82 | 9,8 |
| sans relance | 28,3 (0,3) | 27,2 | 28,8 / 27,5 / 29,0 / 27,8 | 7,40 | 9,4 |

Le clic découpé (moyennes, en ms) :

| Bras | Montée | Appli | Bureau | Emport | Encodage | Descente | Page | Écran |
|---|---|---|---|---|---|---|---|---|
| HEVC (SCTP) | 3,0 | 1,8 | 2,1 | 2,4 | 1,5 | 0,6 | 0,8 | 10,3 |
| entières | 3,1 | 1,5 | 2,3 | 2,5 | 1,4 | 3,2 | 3,2 | 8,6 |
| tranches | 3,4 | 1,6 | 2,2 | 2,4 | 1,4 | 3,4 | 2,7 | 8,7 |
| `early` | 3,2 | 1,6 | 2,3 | 2,6 | 1,4 | 3,1 | 0,7 | 9,8 |
| sans relance | 3,3 | 1,7 | 2,3 | 2,5 | 1,4 | 3,1 | 4,7 | 9,4 |

- **Montée** : du `SendInput` du client à celui de l'hôte. Cela comprend
  l'entrée dans Chrome, le canal de données et le relais.
- **Appli** : de là à la présentation du drapeau par `mw-click-target`.
- **Bureau** : jusqu'à la présentation du bureau qui le porte (écran virtuel à
  240 Hz : une demi-période en moyenne).
- **Emport** : jusqu'à l'image que le flux à 119 i/s emporte.
- **Encodage** : jusqu'au relais.
- **Descente** : du relais à l'arrivée dans la page (`arrivedMs`).
- **Page** : de l'arrivée au dessin.
- **Écran** : du dessin à l'apparition sur le bureau composé du client.

La montée et la descente passent d'une horloge à l'autre. L'estimation
hôte ↔ page varie de ±0,4 ms d'une passe à l'autre : elle déplace un peu de
temps entre ces deux jambes, sans changer le clic → écran.

Ce que la série dit :
- **La sonde ne pèse plus sur l'image mesurée.** Les images du drapeau ne sont
  plus que 0 à 0,5 ms plus lentes du relais au dessin que les autres, contre
  4,0 à 4,6 ms avec la sonde (§6.37). La cause de ce reste n'est pas
  cherchée : les images PyroWave ont toutes la même taille (~178 Ko), drapeau
  ou non.
- **La relance gagne 2,5 ms à l'écran** (28,3 → 25,8 ms, cinq erreurs types ;
  les quatre passes sans relance sont au-dessus des quatre passes avec). Elle
  gagne 1,5 ms au dessin, et ~0,7 de plus du dessin à l'écran. C'est le plus
  gros levier mesuré à l'écran. Au §6.37, la relecture de la sonde attendait
  le GPU à sa place et cachait ce gain.
- **`early` gagne 1,0 ms à l'écran** (25,8 → 24,8 ms, deux erreurs types),
  soit la moitié de ce qu'il gagne au dessin (2,1 ms). Le reste du travail du
  GPU se retrouve après le dessin (9,8 contre 8,6 ms jusqu'à l'écran).
  `early` et la relance s'ajoutent : le bras `ce` a les deux.
- **Les tranches ne gagnent rien à l'écran** (25,8 contre 25,8). Au dessin,
  elles gagnent 0,2 ms sur toutes les images.
- **PyroWave est à 3,3 ms du HEVC à l'écran** en images entières avec la
  relance, et à 2,3 ms avec `early` en plus. L'écart vient de la descente
  (+2,7 ms : 178 Ko par image sur le câble) et de la page (+2,4 ms : le
  décodage). PyroWave en reprend 1,6 du dessin à l'écran : son dessin a déjà
  attendu le GPU, celui du HEVC non.
- **Avant l'encodage, la chaîne coûte ~9,5 ms** dans tous les bras (9,4 à
  9,8) : la montée, l'appli, le bureau et l'emport. Le codec n'y change rien.
  Dont ~4,6 ms pour les deux grilles de l'hôte (le bureau à 240 Hz, puis le
  flux à 119 i/s).

**Verdict.** Mesurés à l'écran du client, sans la sonde, la relance (−2,5 ms)
et `early` (−1,0 ms) gagnent vraiment, et ensemble. Les tranches ne gagnent
rien. Le lecteur ne change pas encore : ce sont toujours des clés de banc,
mesurées sur un seul client (la 780M de l'UM790Pro, sous Windows), et la boucle
de relance tient le fil principal pendant qu'elle tourne. Proposé : B2.1
(présenter par le canevas WebGPU, sans VideoFrame ni `onSubmittedWorkDone`),
jugé sur ce banc contre `ce`, le meilleur PyroWave mesuré, et contre le HEVC.
Puis décider des valeurs par défaut du lecteur : la relance et `early`, ou ce
que B2.1 rend inutile.

Le README de `scripts/bench/photon/` est corrigé (`28f5ab57`) : une lecture
`GetPixel` attend la composition suivante (8,3 ms à 120 Hz), pas ~1 ms.

### 6.39 B2.1, présenter par un canevas WebGPU : une période d'écran de plus (10/10/2026, 12:16-12:46)

B2.1 retire au lecteur la VideoFrame et le dessin Canvas2D : la passe de
présentation du décodeur dessine elle-même dans un canevas que la page montre
(`68592009`, clé de banc `mw_ultra_present`).
- `dom` : le contexte WebGPU d'un canevas qui prend la place de celui de la
  page.
- `offscreen` : un OffscreenCanvas auquel ce canevas a passé son contrôle.
  Chrome envoie ses images au compositeur lui-même, à part de celles de la
  page.
- Dans les deux cas, la page reçoit à la soumission un objet de remplacement,
  pour sa comptabilité (journal des images, cadence). Plus rien n'attend la
  fin du travail, sauf les images suivantes : deux au plus sur la file du GPU,
  puis la plus fraîche attend. La relance n'a plus rien à hâter : elle reste
  coupée.

Au labo d'abord (`84d182cc`, `present-lab.html`, Chrome headless sur
SwiftShader) : `dom` et `offscreen` laissent à l'écran exactement l'image du
chemin VideoFrame (captures identiques au pixel). Un contexte WebGPU demandé
`desynchronized`, comme le Canvas2D de la page, laisse un canevas noir : écarté.

Puis le banc du §6.38 (`scratchpad/pw120c/run-b21.sh`, rapport
`b21-report.txt`). Accord de Bruno pour l'écran virtuel (« Je t'autorise pour
l'écran virtuel ») ; banc donné par 59.
- **Muet à coup sûr**, la condition de 59 après les deux sons du §6.38 : la
  sortie de l'UM790Pro coupée pendant toute la série (`audio-mute.ps1`, l'état
  d'avant gardé puis remis à la fin). Aucun clic n'a entendu de son.
- Deux passes d'essai, puis 8 passes ABBA de 70 clics. L'essai `offscreen` a
  été refait : en fin de passe, le lecteur d'overlay du banc demandait un
  contexte WebGL2 au canevas transféré, ce qui lève une erreur (corrigé,
  `3cf04fe7`).
- Aucun TDR sur les deux PC, et pas de gel du client.
- Les bras : `bh` (HEVC par SCTP), `be` (le `ce` du §6.38 : images entières,
  relance, `early`), `bd` (`dom`) et `bo` (`offscreen`).

Du clic à l'écran, sur le QPC du client seul (en ms ; 139 à 140 clics par bras,
sans les essais) :

| Bras | Clic → écran, moyenne (erreur type) | Médiane | Passes | Relais → dessin, toutes les images | Dessin → écran |
|---|---|---|---|---|---|
| HEVC (SCTP) | 22,5 (0,4) | 22,5 | 22,8 / 22,2 | 1,14 | 9,9 |
| relance + `early` | 24,3 (0,5) | 23,6 | 25,1 / 23,5 | 3,68 | 9,5 |
| `dom` | 32,2 (0,5) | 31,4 | 31,5 / 32,8 (essai 32,0) | 3,38 | 18,4 |
| `offscreen` | 32,4 (0,5) | 31,9 | 31,8 / 32,9 (essai 33,3) | 3,42 | 18,4 |

Ce que la série dit :
- **B2.1 remet l'image plus tôt, mais elle arrive une période plus tard à
  l'écran.** De l'arrivée à la remise, 0,3 ms au lieu de 0,7. Du dessin à
  l'écran, 18,4 ms au lieu de 9,5 : +8,9 ms, soit une période d'écran à
  120 Hz (8,3 ms). Au clic → écran, +7,9 ms (`dom`) et +8,1 ms (`offscreen`),
  plus de dix erreurs types. Chaque passe B2.1, essais compris, est au-dessus
  des deux passes de `be`.
- **Lecture** : le Canvas2D désynchronisé part vers le compositeur à la fin de
  la tâche qui l'a dessiné. Un canevas WebGPU, celui de la page comme celui
  d'un OffscreenCanvas, part avec l'image suivante de Chrome. Ce n'est pas
  vérifié dans le code de Chromium.
- **Ce que B2.1 gagne ailleurs ne se voit pas à l'écran.**
  - Plus de boucle de relance sur le fil principal (2,5 ms par image dans
    `be`).
  - Moins d'images remplacées : 6 à 16 par passe, contre 40 à 43.
  - Une soumission → fin de 3,3-3,4 ms, comme sans relance au §6.38 (3,5).
- **Le banc n'a pas bougé depuis le matin** : `be` et le HEVC redonnent les
  chiffres du §6.38 (24,3 contre 24,8 ; 22,5 contre 22,5).

**Verdict.** B2.1 est clos, en négatif. Dans Chrome sous Windows, le chemin le
plus court vers l'écran reste la VideoFrame dessinée sur le Canvas2D
désynchronisé. Ce qu'elle coûte (~0,4 ms jusqu'à la remise) est bien moins que
la période qu'attend un canevas WebGPU. Le lecteur garde donc la VideoFrame ;
`mw_ultra_present` reste une clé de banc, pour un autre navigateur ou un
Chrome plus récent. Restent les deux leviers mesurés au §6.38 : la relance
(−2,5 ms à l'écran) et `early` (−1,0 ms). Proposé : en faire les valeurs par
défaut du lecteur PyroWave, avec des clés pour les couper.

### 6.40 Les phases lentes, relues dans les traces : presque rien avec les défauts (10/10/2026)

Sans banc : les traces des passes en images entières des §6.36, §6.38 et §6.39
(`mw_ultra_trace=1`), relues par `scratchpad/pw120c/phases.py`. Pour chaque
image : la soumission → fin (horloge de la page) et le travail du GPU, du début
de sa passe de décodage à la fin de sa présentation (horloge du GPU). Une
seconde est lente quand sa médiane de soumission → fin dépasse de 0,35 ms celle
de sa passe.

| Bras | Passes | Soumission → fin, p50 | GPU, début → fin | Secondes lentes |
|---|---|---|---|---|
| shader 3, relance (§6.36 `w3`) | 4 | 2,0-2,4 | 0,95-0,98 | 1 / 8 / 16 / 17 sur 64 |
| shader 2, relance (§6.36 `w2`) | 4 | 2,3 | 1,23-1,26 | 0 / 0 / 0 / 2 sur 64 |
| relance (§6.38 `cw`) | 4 | 2,0 | 0,97-0,99 | 2 à 7 sur 79 |
| sans relance (§6.38 `cn`) | 4 | 3,5 | 0,96-0,97 | 0 à 2 sur 79 |
| relance + `early` (§6.38 `ce`, §6.39 `be`) | 6 | 2,9-3,2 | 1,01-1,03 | 0 ou 1 sur 79 |

- **Le GPU ne ralentit jamais.** Dans les secondes lentes, son travail reste le
  même à 0,03 ms près. Les images arrivent au même rythme (8,3 ms) et la
  relance tourne comme ailleurs. C'est l'attente de Chrome, avant ou après le
  GPU, qui s'allonge, de 0,5 à 0,7 ms.
- **Les longues phases ne viennent que de deux passes du §6.36** (`w3-r2`
  et `w3-r4`, 08:08 et 08:22) : 16 et 17 secondes lentes, par blocs de 7 à 13
  secondes. Les huit passes des §6.38-6.39 avec la relance n'en ont plus que
  quelques secondes isolées.
- **Avec les défauts du lecteur** (relance + `early`), 2 secondes lentes sur
  ~470, en six passes. Il n'y a plus rien à gagner là.
- **Après l'arrivée, PyroWave est déjà au niveau du HEVC.** Au §6.38, de
  l'arrivée à l'écran du client : 10,5 ms pour `early` (page 0,7 + écran 9,8),
  contre 11,1 pour le HEVC (0,8 + 10,3). Tout l'écart restant à l'écran
  (+1,8 à +2,3 ms, §6.38-6.39) est dans la descente : 3,1 ms contre 0,6.
- **La descente est surtout le câble.** DualRTX est relié à 1 Gbit/s (sa
  carte Realtek 5GbE négocie 1 Gbps). À ce débit, une image de ~178 Ko met déjà
  ~1,45 ms à passer sur le fil, contre ~0,15 ms pour une image HEVC. Sur les
  +2,5 ms de descente, ~1,3 ms est donc le fil lui-même. Le reste, ~1,2 ms, est
  la pile : Chrome, la route audio et le découpage en paquets.

**Verdict.** Les phases lentes, le surcoût des morceaux et les tuiles du bord
sont clos sans banc. Les phases lentes ont presque disparu avec les défauts. Les
tranches ne gagnent rien à l'écran (§6.38) : leur surcoût ne compte plus. Les
tuiles du bord valent ~0,04 ms. Le seul levier encore grand est la descente :
- **des images plus petites**, décision de Bruno ;
- **un lien plus rapide** : à 2,5 Gbit/s, le fil seul prendrait ~0,6 ms au
  lieu de ~1,45. C'est du matériel (le commutateur ou la box), pas du code ;
- **la pile** (~1,2 ms) : la taille des messages et le MTU, jamais essayés
  (§6.8).

### 6.41 P-B, la taille des paquets de la route audio : l'hôte envoie au rythme du fil, −0,4 ms par image (10/10/2026, 13:20-14:26)

Sur la route audio, l'image PyroWave (~178 Ko) part en 163 paquets de
1 100 octets. La taille d'un message, pour elle, c'est donc celle d'un paquet.

**L'hôte envoie moins vite que le fil.** Son propre journal (`RTP video:
sendFrame`, passes des §6.38-6.39) le montre : la boucle qui passe les 163
paquets à libdatachannel prend 1,5 à 2,5 ms en médiane, 3 à 5 ms au p99. Le
fil, lui, ne demande que 1,6 ms, trames Ethernet comprises. Pour trouver ce
qui coûte, sans banc d'abord (`scratchpad/pw120c/`) :
- **L'envoi brut de Windows** (`udpburst.py`, feu vert de 59, 12 s par essai,
  vers l'UM790Pro). La socket est réglée comme libjuice la règle : IPv6 double
  pile (la paire du flux), tampons de 1 Mio, non bloquante. Elle passe par le
  commutateur Hyper-V (`vEthernet (LAN)`). Il faut 5,1 µs par datagramme :
  l'image part en 0,84 ms en paquets de 1 100 octets, en 0,67 ms en paquets de
  1 400. Aucun envoi n'est refusé. Le noyau prend donc l'image plus vite que
  le fil.
- **Le fil qui dort entre deux images** (`burstshape.py`, `ecotail.py`,
  `pintail.py`, en boucle locale). C'est ce que fait celui de l'hôte. Sur un
  cœur qui tourne sans arrêt, la rafale prend 0,46 ms au p90. Si le fil dort
  entre deux rafales, elle prend 1,7 à 2 ms au p90 et 2 à 5 ms au p99. Tous
  les paquets de la rafale ralentissent, pas seulement les premiers. Rien
  n'y change :
  - MMCSS « Games » ;
  - la sortie d'EcoQoS et la haute priorité, comme l'hôte (`StreamPriority.cpp`) ;
  - un cœur fixé, quel qu'il soit ;
  - tourner à vide 0,5 à 2 ms avant la rafale.

  Lecture : l'état d'énergie des cœurs, en mode « Utilisation normale ».
  C'est un réglage du système, pas du produit.
- **libdatachannel** propose AES-GCM en premier. La session prend
  `SRTP_AEAD_AES_256_GCM`. Par paquet, le chemin fait trois copies, un
  `srtp_protect` et un `sendto`.

Les nouvelles mesures (`3ed245ea`, `f90e87ea`) :
- Le journal du relais (`relaylog=1`) date désormais aussi, sur la route
  audio, le premier et le dernier paquet passés à libdatachannel.
- La trace de la page (`mw_ultra_trace=1`, `<tag>.rtptrace.json`) note pour
  chaque image :
  - quand Chrome a reçu le premier et le dernier paquet (`receiveTime`) ;
  - quand le worker a lu le premier morceau et posté l'image ;
  - quand la page l'a eue.

  L'horloge de Chrome est arrondie à 0,1 ms.
- `scratchpad/pw120c/down.py` coupe la descente en morceaux, rapport
  `down-report.txt`.
- Une clé de banc règle la taille des paquets : `aroadchunk=<octets>`, de 256
  à 1 400.

**La série** (`scratchpad/pw120c/run-pb.sh`) : le banc du §6.38, avec les clics
du client lui-même. Les deux bras ont les défauts du lecteur (images entières,
relance, `early`) :
- `p1` : des paquets de 1 100 octets ;
- `p4` : des paquets de 1 400 octets (`aroadchunk=1400`). Avec l'en-tête de la
  route, RTP, l'étiquette SRTP, UDP et IPv6, le paquet fait 1 488 octets. Le
  MTU de 1 500 le tient.

Le déroulé :
- Une passe d'essai `p4`, puis 8 passes ABBA de 70 clics.
- Feu vert de 59 ; accord permanent de Bruno pour l'écran virtuel.
- UM790Pro muet toute la série ; 0 TDR, aucun gel.
- 69 ou 70 clics sur 70 retrouvés par passe ; 19 morceaux redemandés sur la
  passe d'essai.

Par image, en ms (p50 / p90 ; 35 000 à 44 000 images par bras) :

| | `p1` (1 100 o) | `p4` (1 400 o) |
|---|---|---|
| boucle d'envoi de l'hôte, premier → dernier paquet | 2,02 / 3,54 | 1,59 / 2,96 |
| Chrome, premier → dernier paquet reçu | 1,90 / 3,40 | 1,60 / 2,80 |
| dernier paquet reçu → image postée par le worker | 0,3 / 0,9 | 0,3 / 0,6 |
| worker → page | 0,1 / 0,3 | 0,1 / 0,2 |
| descente (relais → arrivée dans la page) | 2,86 / 4,23 | 2,45 / 3,75 |
| relais → dessin, toutes les images | 3,75 | 3,29 |
| clic → écran, moyenne (erreur type) | 24,42 (0,32), 278 clics | 23,56 (0,30), 348 clics |

Ce que la série dit :
- **Chrome suit l'hôte.** Il reçoit les paquets d'une image sur le même
  intervalle que l'hôte met à les envoyer (écart médian −0,09 à −0,02 ms).
  La descente, c'est donc :
  - la boucle d'envoi de l'hôte ;
  - ~0,3 ms dans le worker ;
  - ~0,1 ms jusqu'à la page ;
  - 0 à 0,7 ms entre l'hôte et Chrome, d'une passe à l'autre. Ce morceau
    passe d'une horloge à l'autre, sur une estimation.
- **L'hôte dépense 12,4 µs par paquet, quelle que soit sa taille** (`p1` comme
  `p4`). Moins de paquets vont donc d'autant plus vite. Avec `p4` :
  - −0,43 ms sur la boucle d'envoi ;
  - −0,41 ms sur la descente ;
  - −0,46 ms du relais au dessin.

  Chaque passe `p4` est sous chaque passe `p1`.
- **À 1 400 octets, l'hôte suit le rythme du fil en médiane** : 1,58 ms
  contre 1,56 pour 128 paquets de 1 488 octets à 1 Gbit/s. Il reste la
  traîne. 47 % des images partent un peu moins vite que le fil, de 0,22 ms en
  moyenne (p90 2,96 ms).
- **Au clic → écran, −0,86 ms** (24,42 → 23,56, deux erreurs types). Le bras
  `p1` retrouve le `be` du §6.39 (24,3). Le HEVC n'a pas été repassé : à
  22,5 ms aux §6.38-6.39, PyroWave reste ~1 ms derrière lui à l'écran.
- **1 400 octets, c'est le plafond.** En IPv6, un MTU de 1 500 tient au plus
  1 412 octets d'image. Au-delà, il faudrait des trames géantes, réglées sur
  les deux cartes et sur le commutateur.
- **Passe de vérification** (`p1-rv`), avec le nouveau défaut et sans la clé :
  1 400 octets dans le journal, envoi en 1,54 ms, Chrome en 1,60 ms.

**Verdict.** Les paquets de 1 400 octets deviennent le défaut de la route
audio pour PyroWave (`enc12=pyrowave`). `aroadchunk=1100` y revient. Les autres
usages de la route audio (le HEVC des bancs, le plan Wi-Fi) gardent 1 100.

Sur le câble à 1 Gbit/s, la descente est maintenant le fil et l'hôte, au même
rythme. Ce qui reste :
- **Un lien plus rapide, seul, ne changerait rien en médiane** : l'hôte
  mettrait toujours ~1,6 ms à envoyer l'image. Il faut aussi un hôte qui
  dépense moins par paquet. Le levier connu est l'envoi groupé (UDP
  segmentation offload : `WSASendMsg` avec `UDP_SEND_MSG_SIZE`, un appel par
  64 Ko de paquets égaux). Il demande de modifier libjuice et libdatachannel.
  À 1 Gbit/s, il ne rendrait que la traîne (~0,2 ms en moyenne). Avec un lien
  à 2,5 Gbit/s en plus, ~1 ms.
- **Des images plus petites** raccourcissent les deux à la fois.
- **La taille des messages SCTP et le MTU de SCTP** (le P-B du plan) : pas
  passés. PyroWave prend la route audio, déjà 0,7 ms devant SCTP (§6.26).

### 6.42 Des images plus petites : −0,4 ms par 45 Ko, au prix du texte (10/10/2026, 15:13-15:53)

Bruno a choisi cette piste parmi les trois du §6.41. PyroWave code chaque image
entière, au débit de la clé `ultrambps` (170 Mbit/s par défaut) : à 119 i/s,
178 Ko par image, et l'encodeur remplit ce budget, même sur une scène fixe.
Une image plus petite passe plus vite sur le fil et dans la boucle d'envoi de
l'hôte, mais elle perd du détail. Les deux se mesurent ici.

**La qualité, hors ligne** (`scratchpad/size/quality.py`, feu vert de 59).
- L'encodeur du produit (`mw-pyrowave-d3d12`) tourne sur WARP. Le décodeur de
  référence de l'amont tourne sur la RTX. Le PSNR de la luminance est calculé
  sur le corpus 1080p.
- Les clips du corpus sont à 60 i/s : le débit est réduit dans le même
  rapport, pour retrouver la taille par image du flux à 119 i/s.
- Des morceaux de 480 × 270 pixels d'une même image, décodés à chaque taille,
  servent à juger à l'œil.

| taille par image (débit à 119 i/s) | texte | jeu (`game`) | dégradé |
|---|---|---|---|
| 354 Ko (le seuil de l'amont en 1080p60) | 33,4 | 51,9 | 67,6 |
| **178 Ko (170 Mbit/s, le défaut)** | **28,1** | **46,9** | **66,1** |
| 134 Ko (128 Mbit/s) | 24,1 | 44,7 | 64,6 |
| 89 Ko (85 Mbit/s) | 22,1 | 42,8 | 64,1 |
| 45 Ko (43 Mbit/s) | 21,2 | 40,1 | 62,2 |

PSNR Y en dB. Le clip `game10` donne les mêmes valeurs que `game` à 0,3 dB
près.
- Le texte couvre tout l'écran : c'est le cas le plus dur. Il est déjà faible
  au défaut. À 134 Ko, il devient plus mou, avec un peu de bavure autour des
  lettres. À 89 Ko, il reste lisible, mais flou.
- L'extrait de jeu vient d'une vidéo déjà compressée, donc plus douce qu'un
  vrai rendu de jeu. À 89 Ko, il se distingue à peine de la source.

**La série** (`scratchpad/pw120c/run-sz.sh`). C'est le banc du §6.41 : clics du
client lui-même, défauts du lecteur, paquets de 1 400 octets. Une passe d'essai
`z89`, puis 12 passes alternées de 70 clics :
- `zh` : le HEVC du produit, par SCTP ;
- `z178` : PyroWave au défaut ;
- `z134` : `ultrambps=128` ;
- `z89` : `ultrambps=85`.

Le déroulé :
- Feu vert de 59 ; accord permanent de Bruno pour l'écran virtuel.
- UM790Pro muet toute la série, puis remis ; 0 TDR, aucun gel.
- 70 clics sur 70 dans chaque passe, sauf un (69).
- La taille dans le journal du relais est la taille attendue : 178,5, 134,4
  et 89,2 Ko.

Par image, p50 / p90 en ms (~26 500 images par bras), et par clic :

| | `zh` (HEVC) | `z178` | `z134` | `z89` |
|---|---|---|---|---|
| boucle d'envoi de l'hôte | | 1,12 / 1,76 | 0,84 / 1,29 | 0,56 / 1,37 |
| Chrome, premier → dernier paquet reçu | | 1,50 / 1,70 | 1,10 / 1,30 | 0,80 / 1,20 |
| descente (relais → arrivée dans la page) | 0,41 / 1,01 | 2,21 / 3,05 | 1,84 / 2,54 | 1,43 / 2,15 |
| relais → dessin | 1,11 / 1,70 | 2,75 / 3,63 | 2,33 / 3,09 | 1,92 / 2,64 |
| clic → écran, moyenne (erreur type), 210 clics | 22,24 (0,38) | 23,79 (0,36) | 22,18 (0,38) | 22,46 (0,33) |

Ce que la série dit :
- **Le gain suit le fil.** Du relais au dessin, −0,42 ms à 134 Ko, −0,83 ms à
  89 Ko, en médiane. Ça fait ~9,3 µs par Ko, le prix du fil à 1 Gbit/s
  (~8,5 µs par Ko, trames Ethernet comprises). Chaque passe `z89` est sous
  chaque passe `z134`, elle-même sous chaque passe `z178`.
- **Cette fois, l'hôte envoie plus vite que le fil, à toutes les tailles.** Sa
  boucle prend 1,12 ms pour 178 Ko, contre 1,59 au §6.41. Chrome reçoit au
  rythme du fil : 1,50 ms pour 128 paquets, 1,56 attendus. La vitesse de la
  boucle varie donc d'une série à l'autre. Le fil, lui, ne bouge pas.
- **Après l'arrivée, PyroWave est un peu devant le HEVC** : 0,49 à 0,54 ms
  jusqu'au dessin, contre 0,70. C'est `early` (§6.38). Tout l'écart reste
  dans la descente.
- **Même à 89 Ko, PyroWave reste ~0,8 ms derrière le HEVC**, du relais au
  dessin (1,92 contre 1,11). Sur une scène fixe, une image HEVC pèse ~100
  octets : c'est son cas le plus favorable.
- **Le clic → écran va dans le même sens, mais il est bruité.** `z178` est
  +1,55 ms derrière le HEVC, au-delà du bruit. `z134` et `z89` sont à égalité
  avec lui, à moins d'une erreur type. Les étapes de l'hôte avant l'encodage
  (montée, appli, bureau, capture) varient de ±0,5 ms d'un bras à l'autre :
  elles ne dépendent pas du codec. Sans elles, de l'encodage à l'écran, il
  reste +1,8 ms à `z178`, +0,8 à `z134` et +0,9 à `z89`, à ±0,3 ms près.

**Verdict.** Une image deux fois plus petite fait gagner 0,8 ms par image à
l'écran. Le prix est surtout sur le texte, déjà faible au défaut : 28 dB à
178 Ko, 24 à 134, 22 à 89. Même à 89 Ko, l'écart avec le HEVC ne se ferme pas
sur une scène fixe. **Le défaut ne change pas** (170 Mbit/s) : la taille est un
choix de qualité, qui revient à Bruno. Une page montre les morceaux décodés
à chaque taille, avec les chiffres :
https://claude.ai/artifact/BuLgNR6TzGGHe6WNTLYqFK.

Ce qui reste pour la descente :
- **un lien plus rapide**, qui raccourcit le fil sans toucher à la qualité.
  Dans cette série, l'hôte suit déjà le fil en médiane, à toutes les tailles
  (pas au p90). L'envoi groupé (USO, §6.41) ne servirait donc qu'à la traîne,
  aux séries où la boucle de l'hôte est lente, ou avec un lien à 2,5 Gbit/s ;
- **une taille qui suit le contenu**, par exemple plus petite quand l'image
  bouge vite et que l'œil voit moins le détail. Rien n'est écrit pour ça.

Suite (10/10) : Bruno a choisi 134 Ko par défaut (`ultrambps` 128 au lieu de
170, `5060bb35`).

### 6.43 Clôture de PyroWave (10/10/2026)

Bruno clôt le chantier PyroWave le 10/10, sur ce bilan (DualRTX NVENC →
UM790Pro, Chrome, câble à 1 Gbit/s) :
- **PyroWave ne bat pas le HEVC du produit.** Après l'arrivée du dernier
  octet, il est à égalité ou un peu devant (`early`, §6.38-6.40). Tout
  l'écart est dans la descente : le fil et la boucle d'envoi de l'hôte
  (§6.41). Au défaut de 134 Ko, il reste ~1,2 ms derrière le HEVC du relais au
  dessin sur une scène fixe, ~0,8 ms à 89 Ko (§6.42).
- **La qualité du texte est faible** : 24 dB au défaut, 22 à 89 Ko.
- **Ce qui est gardé, dans le code** : l'encodeur D3D12 et le décodeur
  WebGPU, la route audio et ses paquets de 1 400 octets, la relance et
  `early`. Tout reste derrière les clés de banc (`pipeline=d3d12`,
  `enc12=pyrowave`) et n'est jamais choisi seul.
- **Ce qui n'a pas été fait** : un lien à 2,5 Gbit/s (bloqué sur le matériel :
  les deux cartes savent le faire, l'équipement entre elles bride à 1 Gbit/s),
  l'envoi groupé (USO), une taille qui suit le contenu, P-B sur SCTP, et la
  matrice U5 (autres hôtes, dont les GPU intégrés Intel et AMD où le plan
  attendait le gain, Steam, les TV).

## 7. Concrètement, pour l'utilisateur

Pendant le POC, rien ne change : Ultra est caché derrière deux clés de banc et
n'est jamais choisi seul. S'il gagne, il deviendra plus tard, dans un autre plan,
un mode « Ultra (LAN) » pour un appareil relié en Ethernet. L'image arriverait
plus tôt, surtout quand l'hôte a un GPU Intel ou AMD intégré, et sans image clé :
une perte ne ferait plus sauter l'image, elle flouterait quelques blocs pendant
une image. En échange, le débit passerait de 20-40 à 150-200 Mbit/s. Sur un PC
NVIDIA, il faut s'attendre à une égalité, voire une légère perte. S'il perd, on
saura pourquoi et de combien, et les gains trouvés en chemin hors codec (cadence
à 120 ou 240 Hz, présentateur, réglages du transport) iront à tout le monde. Les
TV passent au banc elles aussi : si le décodeur Ultra tourne assez vite sur leur
petit GPU, elles en profiteront ; sinon elles gardent leur décodeur matériel.

Déjà visible (U0.2) : le détail de la latence, dans les statistiques du stream,
gagne une ligne « Mesurée (hôte → dessin) ». C'est l'âge réel de l'image à
l'écran, pris d'un bout à l'autre. Si elle lit nettement plus que le total
au-dessus d'elle, une partie du trajet n'est chronométrée par personne. En Wi-Fi,
elle peut lire un peu faux, de la moitié de l'écart entre la montée et la
descente.

Déjà visible (U0.3, N95) : sur un portable modeste en Wi-Fi, forcer 120 i/s
dégrade tout. L'image a 130 à 320 ms de retard au lieu de 45 à 58, avec des
coupures de plusieurs secondes, et plus d'un clic sur deux reste sans réponse visible.
L'« Auto » d'aujourd'hui fait le bon choix sur cet appareil : il essaie de
monter, voit que ça ne tient pas, et reste à la cadence de l'écran. En Ethernet, sur un mini-PC (U0.3, UM790Pro), le même « Auto » monte à
240 i/s : l'image a 24 à 27 ms de retard et le clic s'affiche en 35 à 43 ms,
aussi bien que le réglage Ultra forcé.

Déjà visible (U0.4, la borne Steam) : sur un PC à carte NVIDIA relié en
Ethernet, le codec PyroWave de Steam affiche un clic 7 à 15 ms plus tôt que
son HEVC. C'est ce qui justifie de poursuivre le POC. Une partie de ce gain
vient sans doute de la façon dont Steam présente l'image, et pas seulement du
codec : dans le navigateur, le gain à attendre est plus petit, et le chemin
d'affichage de Chrome (15 à 25 ms) devient le prochain poste à travailler.

Pas encore visible (U1.4, la vidéo sur une piste RTP) : un interrupteur caché
peut faire passer la vidéo par RTP au lieu du DataChannel, codec par codec,
séparément pour l'hôte natif et pour Sunshine ou Apollo. Il est coupé par défaut
et rien ne change pour l'utilisateur. Aujourd'hui, pour un stream seul, RTP fait
attendre l'image 5 à 10 ms de plus, parce que Chrome la retient avant de nous la
rendre. En revanche, quand un flux très lourd comme Ultra passe à côté de la
vidéo, RTP évite que la vidéo attende derrière lui : elle reste aussi fraîche que
sans charge. Si la retenue de Chrome se lève, RTP pourra servir à tout le monde.
Sinon, il restera le transport d'Ultra seulement.

Clos (10/10, §6.43) : PyroWave ne rejoint pas le produit. Sur un PC NVIDIA
relié en Ethernet à 1 Gbit/s, son image arrive environ 1 ms plus tard que celle
du HEVC d'aujourd'hui, avec un texte moins net. Rien ne change pour
l'utilisateur : il reste caché derrière ses clés de banc. Ce qu'il a appris sur
le chemin de l'image dans Chrome reste écrit ici pour la suite.

Pas encore visible (U1.4 ter, la route audio) : la retenue de Chrome vient d'une
horloge interne que la page ne peut pas couper. Mais elle ne touche que les
pistes vidéo. En faisant voyager les images de la vidéo par une piste audio,
qu'on ne joue jamais, le RTP rattrape le DataChannel pour un stream seul. Quand
un flux lourd passe à côté, la vidéo reste fraîche, à 11 ms de l'hôte à l'écran
au lieu de 21 ms sur une piste vidéo et de 125 ms sur le DataChannel. Elle sait
maintenant redemander un morceau perdu. Même quand 5 % des paquets se perdent,
chaque image arrive, avec 2 à 3 ms de retard en plus. Le DataChannel, lui, en
perd une sur trois. Rien de tout cela n'est encore activé pour les joueurs :
la mesure sur câble vient d'abord, puis Bruno choisit, codec par codec.

Mesuré sur câble (06/10) : sur un vrai câble Ethernet, le DataChannel d'aujourd'hui
suffit déjà. L'image a le même âge par la route audio et par le DataChannel,
même quand un flux Ultra de 120 Mbit/s passe à côté. La route audio ne se
distingue que quand des paquets se perdent : elle garde toutes les images, et
le DataChannel en perd une sur deux. Sur ce câble, l'« Auto » fait mieux que le
120 i/s forcé, de 4 à 5 ms.
