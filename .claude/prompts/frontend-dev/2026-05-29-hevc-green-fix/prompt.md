# Mission: Analyser le bug image verte HEVC (Frontend)

## Contexte
Le streaming HEVC produit une image verte après le decode. Le premier NAL reçu est `0x40` qui est un **VPS** (Video Parameter Set) en HEVC, mais `NalParser.js` ne rapporte que SPS et PPS. L'hypothèse est que le VPS n'est pas inclus dans la description hvcC, ce qui fait que le décodeur VideoDecoder de Chrome mal interprète les couleurs.

## Logs clés
```
First 16 bytes: 00 00 00 01 40 01 0c 01 ff ff 01 60 00 00 03 00
SPS first byte (NAL type): 0x42 type=2
PPS first byte (NAL type): 0x44 type=4
NalParser.feed() returned: ready=true sps=52 pps=7
Configuring VideoDecoder: codec=hvc1.1.0.L0.B0 descLen=120
```

Le premier NAL `0x40` est un VPS HEVC (type 32 = (0x40 >> 1) & 0x3F), mais il n'est pas loggé/acquis.

## Tâches

### 1. Lis et analyse `d:\Code\moonlight-web-deepseek\frontend\js\pipeline\NalParser.js`
- Cherche comment les NAL types sont parsés (HEVC vs AVC)
- Vérifie si le VPS (type 32) est extrait et stocké
- Vérifie le format `nalu` (field `type`) — est-ce que HEVC utilise le bon masque de bits ?
- Regarde `NAL_TYPE_HEVC_*` et la fonction `_extractNALInfoHEVC` si elle existe

### 2. Lis et analyse `d:\Code\moonlight-web-deepseek\frontend\js\ui\StreamView.js`
- Cherche `toAvcc()` — comment les start codes 0x00000001 sont convertis en AVCC (length-prefixed)
- Cherche la construction du `description` pour `VideoDecoder.configure()`
- Vérifie si VPS est inclus dans les arrays de NALs pour HEVC
- Vérifie le codec string (`hvc1.1.0.L0.B0`)

### 3. Regarde aussi `d:\Code\moonlight-web-deepseek\frontend\js\pipeline\VideoPipeline.js`
- Comment le flux HEVC est démarré
- Comment le codec est négocié/détecté côté frontend

## Output
- Décris le problème exact que tu trouves dans le parsing HEVC
- Si tu trouves le bug, écris la correction directement
- Écris un résumé dans `.claude/results/frontend-dev/2026-05-29-hevc-green-fix/Resume-2026-05-29.md`

En fin de travail, écris ton résumé dans `.claude/results/frontend-dev/2026-05-29-hevc-green-fix/Resume-2026-05-29.md`. Inclus uniquement tes résultats/conclusions (pas la réflexion intermédiaire). Format : tâche accomplie, fichiers modifiés, décisions prises, points bloquants.
