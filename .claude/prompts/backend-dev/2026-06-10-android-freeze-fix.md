# Session: 2026-06-10-android-freeze-fix — backend-dev

## Tache : Modifier kMaxPayloadSize dans DataChannelRelay.h

### Contexte

Le stream Android WebRTC DC freeze apres ~1 seconde a cause d'une combinaison de :
1. kMaxPayloadSize=14000 fragmentant trop les frames en chunks SCTP
2. Le dirty mode frontend qui bloque les delta frames
3. Les keyframes piegees par le stale check

### Fichier a modifier

**`d:\Code\moonlight-web-deepseek\backend\src\streaming\DataChannelRelay.h`**

### Changement

Ligne 97 : modifier `kMaxPayloadSize` de `14000` a `32000` :

```cpp
// Avant:
static constexpr int kMaxPayloadSize = 14000;

// Apres:
static constexpr int kMaxPayloadSize = 32000;
```

### Justification

- 32000 est un bon compromis entre 14000 (trop de chunks, reordering SCTP eleve) et 64000 (trop gros, risque de fragmentation SCTP sous-jacente)
- Les DataChannels SCTP supportent des messages jusqu'a 256KB, 32000 est bien en dessous
- Une keyframe de 120KB passe de 9 chunks a 4 chunks
- Moins de chunks = moins de reordering unordered SCTP = moins de gaps frameId

### Instructions

1. Lis le fichier avec Read
2. Fais la modification avec Edit
3. Ecris ton resume dans `.claude/results/backend-dev/2026-06-10-android-freeze-fix/Resume-2026-06-10.md`
