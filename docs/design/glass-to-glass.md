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

Sur DualRTX : streamer « Display 1 » (LINDY, RTX 5060 Ti, 1080p) et afficher le
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
| — | — | à faire par Bruno : DualRTX, harnais local (§4), 1080p60 HEVC 20 Mbps, tearing on | — | — | — | — |
| — | — | idem en LAN depuis une autre machine | — | — | — | — |
| — | — | idem par Internet (4G, rendez-vous) | — | — | — | — |

Le premier relevé fixe la référence ; chaque chantier des phases C, E et F qui
prétend gagner de la latence doit se relire ici.
