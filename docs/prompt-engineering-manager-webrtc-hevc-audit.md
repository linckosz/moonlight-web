# Prompt Engineering Manager — Audit Pipeline WebRTC DataChannel UDP HEVC

> **Session :** Audit complet du pipeline vidéo HEVC sur WebRTC DataChannel (UDP)
> **Date :** 2026-06-11
> **Contexte :** Le stream fonctionne, mais des problèmes de latence et de pixélisation subsistent sur Android.
> **Cible :** Latence optimale + image propre, en acceptant du drop plutôt que de l'accumulation.

---

## 1. État actuel du projet

- **Phase 1–6** : ✅ Terminées (HTTP, découverte, pairing, apps, RTSP, vidéo, audio)
- **Phase 7 (Input)** : ⏳ En cours
- **Phase 8 (Polish)** : ⏳
- **Transport vidéo** : WebRTC DataChannel (SCTP over UDP) — canal `video` pour les NAL units HEVC/H.264
- **Encodage** : Sunshine encode en HEVC (ou H.264 en fallback), envoie les NAL units via WebRTC DataChannel
- **Backend** : C++17, Qt 6.11, libdatachannel (libdatachannel.dll)
- **Frontend** : Vanilla JS, WebCodecs (VideoDecoder), Canvas 2D, AudioWorklet
- **Plateformes testées** : PC (Windows Chrome), Mac (Chrome/Safari), iOS (Safari), Android (Chrome)
- **Fonctionnement** : Le stream fonctionne sur **toutes les plateformes**

### Comportement observé par plateforme

| Plateforme | Status | Observations |
|---|---|---|
| **PC (Windows)** | ✅ Fonctionnel | Pas de problème particulier détecté en test |
| **Mac (Chrome)** | ✅ Fonctionnel | Pas de problème particulier détecté en test |
| **iOS (Safari)** | ✅ Fonctionnel | Pas de problème particulier détecté en test |
| **Android (Chrome)** | ⚠️ Dégradé | **Deux problèmes identifiés** (voir section 2) |

---

## 2. Problèmes identifiés sur Android

### Problème A — Accumulation de latence (buffer bloat visuel)

**Symptôme :** À certains moments, on perçoit une augmentation de la latence, comme si l'affichage était plus lent que le nombre d'images reçues, et que le pipeline se forçait à traiter **toutes** les images dans la limite d'un buffer, accumulant du retard.

**Comportement attendu :** Si une frame arrive et qu'on n'a pas assez de temps pour la traiter (rendu de la frame précédente toujours en cours, decodeur saturé, etc.), on doit **dropper** cette frame plutôt que de l'accumuler dans une file d'attente qui ne fait qu'augmenter la latence.

**Hypothèses possibles :**
- Buffer/queue quelque part dans le pipeline qui accumule sans backpressure
- Le VideoDecoder ou le canvas n'impose pas de signal "occupé" en amont
- Absence de mécanisme de détection de retard et de drop proactif
- Différence de performance CPU/GPU Android vs desktop qui expose le problème

### Problème B — Pixélisation blocky (type blocs de compression)

**Symptôme :** À certains moments, pixélisation sous forme de blocs de compression vidéo (macro-blocks), comme si l'image de référence (keyframe / I-frame) est perdue ou corrompue. Les blocs persistent jusqu'à ce qu'un changement fort de couleur dans la zone force un reset des pixels (ex: ouverture/fermeture d'une application).

**Comportement attendu :** L'image doit rester propre en permanence. Si une image de référence est perdue, le décodeur doit récupérer rapidement (soit via un IDR automatique, soit via une détection + demande d'IDR).

**Hypothèses possibles :**
- Perte de paquets sur le DataChannel SCTP qui corrompt des NAL units
- Le décodeur HEVC Android gère mal certaines corruptions sans le signaler
- Pas de détection de corruption suivie d'une demande d'I-frame
- Problème spécifique au décodeur HEVC hardware Android

### ⚠️ Tests réseau instable — non faits

**Les tests actuels ont été réalisés en conditions réseau favorables (LAN ou WiFi stable).** Aucun test poussé n'a encore été fait avec un réseau instable, à faible bande passante, ou avec une latence élevée.

Il est **nécessaire d'optimiser le pipeline pour les réseaux peu performants**, en anticipant les scénarios suivants :

| Scénario | Impact potentiel |
|---|---|
| **Bande passante réduite** (< 10 Mbps) | Congestion, paquets drops, frames incomplètes |
| **Latence variable** (jitter > 50ms) | Arrivée désordonnée ou tardive des NAL units, accumulation |
| **Perte de paquets** (1-5%) | Corruption de NAL units, perte d'I-frame, pixélisation blocky |
| **Réseau mobile** (4G/5G instable) | Combinaison de tous les facteurs ci-dessus, particulièrement pertinent pour Android |
| **Congestion temporaire** | Rafales de pertes suivies de rattrapage → risque de buffer bloat |

**L'audit doit donc considérer la résilience du pipeline face à ces conditions dégradées**, et pas seulement le fonctionnement en conditions idéales. Les mécanismes de drop, de backpressure, et de récupération après corruption sont d'autant plus critiques.

---

## 3. Mission — Audit complet du pipeline

### 3.1 Cartographier le pipeline complet d'une frame

Je veux comprendre **chaque étape**, de la réception réseau jusqu'à l'affichage canvas, pour le chemin **WebRTC DataChannel UDP HEVC**. Pour chaque étape, explique :

1. **Ce qui se passe exactement** (transformation, copie, décision)
2. **Le fichier source** et la fonction/classe responsable
3. **Le thread** sur lequel l'opération s'exécute
4. **Les buffers/queues** impliqués et leur taille
5. **Le coût potentiel** (copie mémoire, attente, conversion)

Le pipeline doit couvrir :

```
Sunshine (encode HEVC)
  ↓ RTP/UDP → Internet → WebRTC DataChannel (SCTP)
  ↓
[Backend C++/libdatachannel]
  - Réception paquet SCTP → assemblage message DataChannel "video"
  - Décapsulation / vérification
  - Transmission au frontend (WebSocket signaling ou DataChannel direct ?)
  ↓
[Frontend JS]
  - Réception du message (DataChannel.onmessage)
  - Extraction NAL units
  - Décodage (VideoDecoder WebCodecs)
  - Rendu (Canvas 2D / createImageBitmap / drawImage)
  ↓
[Affichage final]
```

### 3.2 Fichiers à auditer en priorité

**Backend C++ :**
- `backend/src/Stream/` — StreamSession, StreamRelay, WebRtcDataChannel
- `backend/src/WebRTC/` — WebRtcManager, DataChannel handling
- `backend/src/Video/` — tout ce qui touche au relay vidéo
- Fichiers de configuration libdatachannel (buffer sizes, SCTP params)

**Frontend JS :**
- `frontend/js/stream/StreamView.js` — gestion du stream, réception DataChannel
- `frontend/js/stream/VideoDecoder.js` ou équivalent — interface WebCodecs VideoDecoder
- `frontend/js/stream/VideoRenderer.js` — rendu canvas
- `frontend/js/stream/WebRtcDataChannel.js` ou équivalent — gestion DataChannel
- `frontend/js/stream/NalUnit.js` ou équivalent — parsing NAL units
- `frontend/js/stream/` — tout autre fichier du pipeline vidéo

### 3.3 Axes d'audit

Pour chaque composant du pipeline, évalue :

| Axe | Question |
|---|---|
| **Bottlenecks** | Où est-ce que le débit est limité ? Thread unique ? Attente synchrone ? |
| **Copies inutiles** | Y a-t-il des copies mémoire CPU↔CPU ou CPU↔GPU évitables ? |
| **Buffers/Queues** | Taille des buffers ? Comportement quand plein (block ou drop) ? |
| **Backpressure** | Le pipeline a-t-il un mécanisme qui propage la congestion vers l'amont ? |
| **Drops** | Y a-t-il un mécanisme de drop ? Est-il assez agressif ? Se déclenche-t-il au bon endroit ? |
| **Synchronisation** | Y a-t-il des attentes inutiles (locks, barriers) entre étapes ? |
| **Paramètres** | Les paramètres SCTP, WebRTC, décodeur, canvas sont-ils optimaux ? |
| **Gestion d'erreur** | Que se passe-t-il si une NAL unit est corrompue ? Le décodeur récupère-t-il ? |
| **Résilience réseau** | Comment le pipeline se comporte-t-il en cas de perte de paquets, jitter élevé, bande passante réduite ? Les mécanismes de récupération sont-ils suffisants ? |
| **Plateforme** | Y a-t-il des différences de comportement par plateforme (Android spécifiquement) ? |

### 3.4 Identifier les optimisations

Classer les optimisations par catégorie :

1. **Optimisations de code** — changements dans la logique (ex: détection de retard, drop proactif)
2. **Optimisations de paramètres** — tuning de constantes (ex: buffer size, timeout, watermark SCTP)
3. **Optimisations de workflow** — réduction de copies, changement d'architecture de données (ex: éviter une copie CPU→GPU, transfert zero-copy, ring buffer, etc.)

Pour chaque optimisation, précise :
- Le gain estimé (latence, fluidité, qualité)
- Le risque (régression, complexité)
- La priorité (P0 = critique, P1 = important, P2 = nice-to-have)

### 3.5 Identifier les bugs

Pour chaque bug suspecté :
- Localisation précise (fichier, fonction)
- Condition de déclenchement
- Impact (latence, corruption visuelle, crash)
- Lien avec les problèmes observés sur Android

---

## 4. Règles et contraintes

### Ce que j'accepte

- ✅ **Drop d'image** s'il y a un bottleneck (plus de frames reçues que traitées) — je préfère un drop qu'une accumulation de latence
- ✅ **Léger artefact visuel** durant une frame ou très peu de frames (< 300ms) si ça permet de garder une latence optimale
- ✅ **Modifications agressives** du pipeline si nécessaire (changer l'architecture de buffer, ajouter un mécanisme de drop, etc.)

### Ce que je n'accepte pas

- ❌ Augmentation de la latence par accumulation (buffer bloat)
- ❌ Pixélisation persistante qui nécessite une action utilisateur pour se résorber
- ❌ Régression sur les plateformes qui fonctionnent déjà bien (PC, Mac, iOS)

### Règles de développement

- Pas de sur-ingénierie — solutions simples, robustes, performantes
- Commentaires en anglais, concis
- Testable sur toutes les plateformes (PC, Mac, iOS, Android)
- Le code doit rester lisible et maintenable

---

## 5. Livrables attendus

### Livrable 1 — Cartographie du pipeline

Document markdown détaillant le pipeline complet frame-par-frame, avec pour chaque étape :
- Fichier, classe, fonction
- Thread d'exécution
- Buffers/queues utilisés
- Transformation effectuée
- Coût estimé

### Livrable 2 — Rapport d'audit

Pour chaque composant audité :
- Bottlenecks identifiés
- Copies inutiles
- Absence de backpressure
- Absence de mécanisme de drop
- Bugs suspectés

### Livrable 3 — Plan d'optimisation

Liste ordonnée et priorisée des optimisations à implémenter :
- Description technique détaillée
- Fichiers et fonctions à modifier
- Approche d'implémentation
- Tests de validation
- Estimation de l'impact

### Livrable 4 — Plan d'implémentation

Roadmap séquencée avec dépendances entre les optimisations, regroupées en étapes logiques pour éviter les conflits et permettre des tests incrémentaux.

---

## 6. Modèle

**Utiliser exclusivement le modèle `fable` (Fable 5)** pour cette session. C'est le modèle le plus capable disponible — je veux la meilleure qualité d'analyse possible pour cet audit.

- Engineering Manager : `fable`
- Agents backend-dev / frontend-dev : `fable` également — pour la phase d'audit, la précision prime sur la rapidité
- Agents experts (moonlight-qt, moonlight-xbox, moonlight-web-stream) : `fable`
- Code-reviewer : `fable`

Pas de contrainte de tokens — allez en profondeur.

---

## 7. Instructions pour l'Engineering Manager

1. **Commence par l'audit** — lis et comprends TOUS les fichiers du pipeline avant de proposer des changements
2. **Utilise les agents experts** (`expert-moonlight-qt`, `expert-moonlight-xbox`, `expert-moonlight-web-stream`) pour comprendre comment les implémentations de référence gèrent ces problèmes
3. **Utilise `backend-dev` et `frontend-dev`** pour les analyses détaillées de code
4. **Utilise `code-reviewer`** pour valider chaque proposition avant implémentation
5. **Présente tes conclusions en français**, de façon structurée
6. **Sois exhaustif** — je veux un audit complet, pas un survol
7. **Si tu as besoin de clarifications**, pose des questions avant de conclure
8. **Ne code pas** dans un premier temps — on veut l'audit et le plan d'abord
