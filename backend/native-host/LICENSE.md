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
| libva | MIT |
| libpipewire-0.3 | MIT |
| ViGEmClient | BSD-3-Clause |
| SDK Windows / Apple | Licence du SDK correspondant |

## Dépendances interdites

`moonlight-common-c` (GPL-3.0), FFmpeg / libavcodec (LGPL-2.1+, GPL avec
x264/x265), x264 (GPL-2.0), x265 (GPL-2.0), et tout en-tête Qt.
