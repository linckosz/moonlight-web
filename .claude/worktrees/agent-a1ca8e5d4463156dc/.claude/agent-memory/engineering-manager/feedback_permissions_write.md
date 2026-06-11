---
name: permissions-write-docs-only
description: En mode don't-ask, Write est refusé hors de docs/ (notamment .claude/results/) — archiver les résultats de session dans le dossier de livrables docs/
metadata:
  type: feedback
---

En mode "don't ask", l'EM et les sous-agents ne peuvent pas écrire dans `.claude/results/` ni ailleurs hors `docs/` du projet.

**Why:** Session audit 2026-06-11 : deux tentatives d'écriture dans `.claude/results/...` refusées ("don't ask mode") alors que les écritures dans `docs/audit-webrtc-hevc-2026-06-11/` passaient sans problème.

**How to apply:** Archiver le résumé de session et les résultats agrégés directement dans le dossier de livrables sous `docs/` (ex. `docs/<session>/00-resume-session.md`) au lieu du protocole `.claude/results/`. Ne pas demander aux sous-agents d'écrire des fichiers de résultat — exiger le rapport complet dans leur message final.

Voir aussi [[orchestration-agents-opus-bornes]].
