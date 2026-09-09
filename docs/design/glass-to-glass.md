# Latence « glass-to-glass » — protocole de mesure

> Ce que le joueur sent, c'est le temps entre un pixel qui change sur l'écran de
> l'hôte et le même pixel qui change sur l'écran du client. Aucune estampille
> logicielle ne le mesure : les stats de session s'arrêtent au dernier octet
> envoyé côté hôte et au `draw()` côté client, avant le compositeur, le scan-out
> et la dalle. Il faut une caméra.

## 1. Ce qu'on mesure, et ce que les stats mesurent déjà

| Segment | Mesuré par | Où le lire |
|---|---|---|
| Présent hôte → dernier octet envoyé | six étapes hôte (`StageStats`) | ligne `host stages over N sent frames` dans le log du worker ; `stages` dans les stats ; overlay « Hôte · … » |
| Réseau | `browserRtt / 2` (ping/pong sur le canal d'entrée) | overlay « Réseau » |
| Dernier fragment → `draw()` terminé | remise, décodage, file, rendu (`[perf]` console avec `mw_perf_diag`) | overlay et console |
| **`draw()` → pixels visibles sur le client** | **rien** : compositeur, file de présentation, vsync, temps de réponse de la dalle | **caméra** |
| **Scan-out hôte → présent DXGI** | rien de plus que `LastPresentTime` | caméra |

La caméra mesure donc le total, et la différence entre ce total et la somme des
segments instrumentés est la **part invisible** (présentation client + dalle).
C'est elle que la chaîne « Allow tearing » (plan v2 §2.4) doit réduire, et
c'est la seule façon de prouver qu'elle le fait.

## 2. Méthode A — téléphone à 240 i/s (recommandée)

Matériel : un téléphone qui filme en ralenti 240 i/s (iPhone : Réglages →
Appareil photo → Ralenti 1080p à 240 i/s). Résolution temporelle : **4,17 ms par
image**, incertitude ±1 image.

Mise en place :

1. Les deux écrans dans le champ, côte à côte, sans reflet. Client et hôte
   peuvent être la même machine (§4, harnais local) ou deux machines.
2. Un **événement visible et instantané** sur l'hôte, de deux sortes :
   - **clic** : une page qui passe du noir au blanc au `mousedown` (voir §5),
     déclenchée **depuis le client** (le stream renvoie l'input) — mesure
     input → hôte → écran client, le chiffre complet ;
   - **horloge** : la même page affiche `performance.now()` en gros chiffres à
     chaque `requestAnimationFrame` — mesure hôte → client seulement, et
     donne un point de mesure par image filmée.
3. Filmer 10 s ; répéter la manœuvre (clic) au moins **10 fois**.

Dépouillement (n'importe quel lecteur qui avance image par image) :

- **clic** : image où le blanc apparaît sur l'hôte → image où il apparaît sur le
  client ; différence × 4,17 ms. Dix valeurs : **médiane et p90**.
- **horloge** : sur une même image filmée, lire le nombre affiché par l'hôte et
  celui affiché par le client ; la différence est la latence à cet instant. Une
  lecture toutes les 24 images (0,1 s) sur 10 s donne 100 points : **médiane,
  p90, max**.

Pièges : l'exposition auto du téléphone (fixer l'exposition) ; le PWM de
rétroéclairage qui fait clignoter l'image (baisser le ralenti à 120 si
nécessaire) ; le client en HDR qui écrase les blancs.

## 3. Méthode B — photodiode (optionnelle, ±0,1 ms)

Deux photodiodes (ou deux phototransistors) collées sur les écrans, un
microcontrôleur qui date les deux fronts. Précision au dixième de milliseconde,
mais pas de contexte visuel : à réserver aux A/B fins (tearing on/off, cadence)
une fois la méthode A stabilisée.

## 4. Harnais local (isoler le réseau)

Le navigateur tourne **sur l'hôte lui-même** et streame son propre écran, en
fenêtre sur un second écran (ou en moitié d'écran). Réseau ≈ 0 : ce qui reste
est la chaîne capture → encode → décode → présentation, et c'est le chiffre à
comparer avec la somme des étapes instrumentées pour isoler la part invisible.

Sur bench-desk : streamer « Display 1 » (LINDY, RTX 5060 Ti, 1080p) et afficher le
client sur « Display 3 » (M27Q). Filmer les deux.

## 5. Page de mesure

Une page HTML autonome suffit — à servir depuis n'importe où sur l'hôte, en
plein écran :

```html
<!doctype html><meta charset=utf-8><title>g2g</title>
<style>html,body{margin:0;height:100%;background:#000;color:#fff;font:120px monospace;
display:flex;align-items:center;justify-content:center}body.on{background:#fff;color:#000}</style>
<div id=t>0</div>
<script>
const t=document.getElementById('t');
addEventListener('mousedown',()=>document.body.classList.add('on'));
addEventListener('mouseup',()=>document.body.classList.remove('on'));
(function f(){t.textContent=Math.round(performance.now());requestAnimationFrame(f)})();
</script>
```

## 5 bis. Méthode C — sonde intégrée clic → drapeau (build debug, hôte Windows)

Sans caméra, mais **clic → image** seulement (pas horloge), et sur un hôte
Windows en build debug (`QT_DEBUG`, comme le sélecteur de profil manette).

Principe :

1. **Hôte** (`backend/src/LatencyFlag.cpp`) : un hook souris bas niveau
   (`WH_MOUSE_LL`, clics *injectés* seulement) sur son propre thread ; à chaque
   clic gauche, une fenêtre Win32 topmost, click-through, en haut au centre de
   **chaque** écran (44 %–56 % de la largeur × 0–5 % de la hauteur) affiche
   trois bandes pleines bleu / blanc / rouge pendant 100 ms. C'est une fenêtre
   OS, pas un ajout dans la texture capturée : rien n'est ajouté au pipeline
   capture → encodage. En haut, parce qu'avec le tearing autorisé ce sont les
   premières lignes scannées — les plus fraîches.
2. **Client** (`frontend/js/stream/LatencyProbe.js`) : envoie un clic par le
   chemin normal, horodate (`performance.now()`), peint un cercle gris d'une
   frame à la position du pointeur (pour une caméra), puis échantillonne trois
   pixels (un par bande) de la surface affichée — canvas ou `<video>` — à chaque
   frame présentée et à chaque rAF, via un `drawImage` 100×20 en plus proche
   voisin. Premier échantillon « bleu, blanc, rouge » = drapeau affiché. Rien
   après 200 ms = échantillon écarté (perte réseau probable).

Activation : `"latency_flag_enabled": true` dans `settings.json`, puis
redémarrage du serveur. Il n'y a **ni case dans les Réglages, ni route
d'écriture** : c'est un instrument de mesure, pas une préférence — allumé, il
pose une bande tricolore sur l'écran de l'hôte à chaque clic, ce qu'un joueur ne
veut jamais. `GET /api/settings/streaming` le rend en lecture seule
(`latency_flag_enabled` = l'overlay tourne vraiment, `latency_flag_supported` +
`latency_flag_reason` = ce que cette machine peut en faire). La réponse de
lancement porte `latency_flag: true`, et seulement alors le StreamView installe
la sonde.

Usage, console du navigateur pendant un stream :

```js
await mwLatency.run();          // 3 clics espacés de 2 s, résumé médiane/p90
await mwLatency.run(10, 1500);  // 10 clics, 1,5 s
mwLatencyResults;               // toutes les entrées : {ts (µs epoch), latencyMs, fromMarkMs, ok, reason}
```

Une entrée **écartée** porte en plus `saw` et `via` : les trois pixels réellement
lus (`blue?140,140,140 white?…`) et la surface qui les a rendus
(`renderer · canvas2d 1920x1080`). Sans eux, un `timeout` ne distingue pas un
drapeau absent de l'image, une image qui n'est pas celle qu'on croit
échantillonner, et une surface qu'on relit sans qu'elle ait été dessinée — les
trois se lisent « timeout ». Si les couleurs ne sont pas celles de l'écran de
l'hôte au moment du clic, le drapeau n'est **pas** dans cette image, et c'est là
qu'il faut chercher, pas dans le pipeline.

### Ce que la compression fait aux trois bandes

Le détecteur ne compare pas des couleurs, il **classe** : bleu = `b ≥ 120` et
`b − r ≥ 60` et `b − g ≥ 60`, blanc = les trois `≥ 170`, rouge symétrique. Les
seuils sont vérifiés par un aller-retour Y′CbCr calculé dans les tests
(`LatencyProbe.test.js`) plutôt que par des valeurs supposées :

| Cas | bleu | blanc | rouge |
|---|---|---|---|
| Aller-retour BT.709 limité | 1,0,255 | 255,255,255 | 255,1,0 |
| Encodé BT.709, décodé BT.601 | 3,0,245 | 255,255,255 | 233,0,2 |
| Encodé BT.601, décodé BT.709 | 0,15,255 | 255,255,255 | 255,24,0 |
| Limité lu en pleine plage | 16,16,240 | 235,235,235 | 239,15,15 |
| + quantification pas 80 | 0,0,218 | 255,237,255 | 255,25,0 |

Trois primaires saturées en aplat : une erreur de matrice ou de plage déplace la
teinte de quelques pour cent sans jamais rendre le bleu moins bleu que le rouge,
et un aplat est ce qu'un codec conserve le mieux (le bruit de quantification
frappe les contours, pas l'intérieur d'une bande).

⚠️ **La marge la plus mince n'est pas la compression, c'est la géométrie.** Sur
la frontière entre deux bandes, le 4:2:0 moyenne la chroma des deux et le bleu
tombe à `b = 137` contre un seuil de 120. Ce qui protège, c'est de lire au
**centre** de chaque bande : une bande fait 4 % de la largeur, le point de
lecture est à 1,5 % de la largeur de la frontière la plus proche — 28 px en
1920, 9,6 px même en 640 de large — contre les ~2 px que décale le
sous-échantillonnage de chroma. Ne pas déplacer les points d'échantillonnage
vers les bords des bandes en croyant gagner en robustesse.

⚠️ **Ce qui n'est pas couvert** : c'est un modèle (matrice, plage,
quantification), pas une mesure du vrai encodeur — les pixels du drapeau chez
l'hôte n'ont jamais été comparés aux pixels décodés d'un flux réel. Et le mode
`tonemap` (ACES) est le seul où ces seuils sont douteux : l'ACES désature
délibérément les couleurs très saturées et abaisse le blanc. Sans conséquence
aujourd'hui, ce mode n'étant atteignable qu'en forçant `mw_hdr_request=1`
(§16.5 du design).

### ⚠️ Un drapeau par écran (07/09/2026)

Le drapeau n'était créé que sur l'écran **principal** (`SM_CXSCREEN`), alors que
la session streame l'écran que le spectateur a choisi. Sur tout autre écran il
était donc simplement **absent de l'image**, et un drapeau absent se lit
exactement comme un pipeline qui n'a rien livré : la sonde expirait à chaque
clic sans rien dire de plus. C'est ce qui a fait passer pour un problème Intel,
puis pour un problème client, une géométrie côté hôte. Les machines à écran
virtuel (hôte headless, VDD, banc Intel qui en a deux) en font le cas normal.

Mesuré sur bench-desk (harnais de scratchpad : `LatencyFlag` réel + Desktop
Duplication sur chaque sortie, clic injecté par `SendInput`) :

| | DISPLAY1 (principal, 2560×1440) | DISPLAY11 (1920×1080) |
|---|---|---|
| Avant | bleu/blanc/rouge | `rgb(1,64,108) rgb(2,66,112) rgb(1,67,115)` — rien |
| Après | bleu/blanc/rouge | bleu/blanc/rouge |

Le même harnais tranche la question restée ouverte du 07/09 (« le drapeau
est-il seulement peint ? ») : **oui, et la Desktop Duplication le capture**. La
capture d'écran de contrôle ne prouvait rien parce que `BitBlt` ne voit pas une
fenêtre *layered* ; le chemin réel de la capture, lui, la voit. Piège du
harnais, pas du produit : une trame `AcquireNextFrame` dont
`LastPresentTime == 0` est une mise à jour de **pointeur seul** et ne porte pas
d'image de bureau à jour — l'échantillonner rendait une couleur plate et faisait
croire à l'absence du drapeau sur un écran qui l'avait.

`latencyMs` compte depuis l'envoi du clic, `fromMarkMs` depuis la frame où le
cercle gris a été peint (ce qu'une caméra sur l'écran client verrait). Les
clics partent à la position courante du pointeur sur l'hôte : placer la souris
sur une zone inerte avant de lancer.

Ce qu'elle ne couvre pas : la présentation client après `draw()` (compositeur,
scan-out, dalle) — la sonde s'arrête au dernier échantillon lisible sur la
surface, c'est-à-dire au plus tard au rAF qui suit la présentation. Pour cette
part, il reste la caméra (§2), qui peut apparier le cercle gris et le drapeau.

## 6. Conditions à consigner

| Champ | Exemple |
|---|---|
| Build | `git rev-parse --short HEAD` |
| Hôte | GPU, encodeur, codec, résolution, fps réglé, débit |
| Client | machine, navigateur, rafraîchissement, tearing on/off, pacer on/off |
| Réseau | LAN Ethernet / Wi-Fi / 4G via rendez-vous ; RTT (`Réseau` de l'overlay) |
| Contenu | bureau fixe / défilement / jeu |
| Stats | `host stages` p99 du log ; `[perf]` p99 client |

## 7. Relevés

| Date | Build | Conditions | n | Médiane | p90 | Part invisible (médiane − somme instrumentée) |
|---|---|---|---|---|---|---|
| — | — | à faire par Bruno : bench-desk, harnais local (§4), 1080p60 HEVC 20 Mbps, tearing on | — | — | — | — |
| — | — | idem en LAN depuis une autre machine | — | — | — | — |
| — | — | idem par Internet (4G, rendez-vous) | — | — | — | — |

Le premier relevé fixe la référence ; chaque chantier des phases C, E et F qui
prétend gagner de la latence doit se relire ici.
