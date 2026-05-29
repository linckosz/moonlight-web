# Mission: Référence moonlight-qt — gestion HEVC

## Contexte
Moonlight-Web implémente un streaming HEVC dans le navigateur via WebCodecs. On a un bug d'image verte qui semble lié à un mauvais parsing/forwarding des NAL units HEVC, potentiellement le VPS (Video Parameter Set) qui n'est pas inclus dans la description hvcC du VideoDecoder.

## Questions

### 1. Comment moonlight-qt construit-il la configuration du décodeur vidéo HEVC ?
- Regarde dans `D:\Code\moonlight-qt\app\streaming\video` ou `D:\Code\moonlight-qt\app\streaming`
- Comment les NAL units (VPS, SPS, PPS) sont transmis au décodeur ?
- Est-ce que le VPS est obligatoire ou optionnel ?

### 2. Comment moonlight-qt sélectionne-t-il le codec vidéo ?
- Regarde `D:\Code\moonlight-qt\app\streaming\Session.cpp` ou équivalent
- Comment la négociation codec se fait avec Sunshine ?
- Comment le fallback H.264 → HEVC → AV1 est géré ?
- Quelles sont les constantes de codec utilisées ?

### 3. Y a-t-il des spécificités HEVC pour le VPS ?
- Est-ce que le VPS est toujours présent dans le stream HEVC de Sunshine ?
- Est-ce qu'il est utilisé pour la configuration du décodeur (couleurs, résolution, etc.) ?

## Output
- Explique comment moonlight-qt gère HEVC du début à la fin
- Donne les patterns de code pertinents (fichiers, fonctions, structures de données)
- Résumé dans `.claude/results/expert-moonlight-qt/2026-05-29-hevc-green-fix/Resume-2026-05-29.md`

En fin de travail, écris ton résumé dans `.claude/results/expert-moonlight-qt/2026-05-29-hevc-green-fix/Resume-2026-05-29.md`. Inclus uniquement tes résultats/conclusions (pas la réflexion intermédiaire). Format : tâche accomplie, décisions prises, points bloquants.
