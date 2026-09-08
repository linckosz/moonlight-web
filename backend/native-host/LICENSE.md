# mw-native-host — licence

Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>

Ce module (`backend/native-host/`) est la **capture et l'encodage natifs** de
MoonlightWeb. Il est distribué aujourd'hui sous **GPL-3.0-or-later**, comme le
reste de MoonlightWeb, mais il est délibérément maintenu **séparable** :

- il ne lie **aucune dépendance GPL** — en particulier jamais `moonlight-common-c` ;
- il n'inclut **aucun en-tête** de `backend/src/` ni de Qt ;
- toutes ses dépendances sont sous licence permissive (BSD / MIT / Apache-2.0) ou
  sont des SDK de constructeur dont l'usage commercial est autorisé.

Bruno Martin étant seul détenteur du copyright sur ce module, il peut le
relicencier — y compris sous une licence propriétaire — sans le consentement
d'un tiers. C'est la raison d'être de la frontière décrite ci-dessus, et
`tests/boundary_test.cmake` la fait respecter mécaniquement au moment du build.

Toute contribution externe à ce répertoire nécessite une cession de droits
explicite, faute de quoi la propriété ci-dessus est perdue.

## Dépendances autorisées dans ce module

| Dépendance | Licence |
|---|---|
| libopus | BSD-3-Clause |
| OpenH264 | BSD-2-Clause |
| NVIDIA Video Codec SDK (en-têtes) | Licence SDK NVIDIA, usage commercial autorisé |
| AMD AMF (en-têtes) | MIT |
| Intel oneVPL / libvpl | MIT |
| libva, libva-drm | MIT (Expat) |
| libdrm | MIT |
| libpipewire-0.3 | MIT (Expat) — voir la note ci-dessous |
| ViGEmClient | BSD-3-Clause |
| SDK Windows / Apple | Licence du SDK correspondant |

`libdrm.so.2`, `libva.so.2`, `libva-drm.so.2` et `libpipewire-0.3.so.0` sont les
**seules** bibliothèques système que l'hôte Linux lie — la liste est celle que
`readelf -d` donne sur le binaire installé, pas celle qu'on espère — et
**aucune n'est embarquée** dans le
`.deb` / `.rpm` : ce sont des dépendances déclarées, résolues par le système.
Le job Linux de `.github/workflows/release.yml` le vérifie : une bibliothèque de
cette liste retrouvée dans l'AppDir y fait échouer la build (« must stay the
system's »).

### ⚠️ PipeWire : ce que « Expat and LGPL-2.1+ » veut dire

Le fichier de copyright Debian de `libpipewire-0.3-0` annonce « Expat **and**
LGPL-2.1+ », ce qui se lit mal au premier regard. Le partage est par fichier, et
il tombe du bon côté (vérifié le 08/09/2026 sur le banc) :

| Fichiers | Licence | Lié par ce module ? |
|---|---|---|
| `Files: *`, dont `src/pipewire/*` — la bibliothèque **cliente** | Expat (MIT) | ✅ c'est ce qu'on lie |
| `spa/plugins/bluez5/*` | LGPL-2.1+ | ❌ plugin du **démon** |
| `src/modules/module-client-node/v0/*` | LGPL-2+ | ❌ module de compat du **démon** |
| `spa/plugins/alsa/90-pipewire-alsa.rules` | LGPL-2.1+ | ❌ fichier de règles udev |

Les parties copyleft sont côté **démon** — un logiciel séparé, déjà présent sur
la machine, que ce module ne lie ni ne distribue. C'est `libpulse` qui est
LGPL-2.1+ en entier, et c'est pour ça qu'elle est refusée (§19.7 du design).

Le refus des dépendances copyleft ici ne vient pas d'un risque de contamination :
une LGPL est compatible avec la GPL-3 sous laquelle MoonlightWeb est distribué,
et son copyleft ne porte que sur ses propres fichiers. Il vient de la
**séparabilité** décrite en tête de ce document — une dépendance LGPL mettrait une
condition durable sur le relicenciement du module, y compris propriétaire.

## Dépendances interdites

`moonlight-common-c` (GPL-3.0), FFmpeg / libavcodec (LGPL-2.1+, GPL avec
x264/x265), x264 (GPL-2.0), x265 (GPL-2.0), **libpulse (LGPL-2.1+)** — écartée
au profit de libpipewire pour le son Linux, voir la note ci-dessus — et tout
en-tête Qt.
