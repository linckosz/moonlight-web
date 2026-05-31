---
name: pin-auto-regenerate-admin-ui
description: AdminView affiche PIN stale apres autoRegeneratePin — fix via polling auth status
metadata:
  type: project
---

Le PIN dans Moonlight-Web est automatiquement regenere apres chaque validation reussie (`AuthManager::autoRegeneratePin()` appele dans le handler `POST /api/auth/validate`). L'AdminView ne reflete pas ce changement car la PIN affichee est figee au moment du render initial ou du clic "Generate".

**Fix:** Dans le timer de pooling des sessions (`_startSessionsPolling()`, intervalle 5s), on appelle desormais `_loadAuthStatus()` et on met a jour le contenu texte de l'element `#admin-pin-display` s'il differe du `this._pin` courant. Ainsi, quand un PIN est consomme et auto-regenere, l'affichage se met a jour dans les 5 secondes suivantes.

Comportement:
- Si le PIN est utilise par un client distant, `autoRegeneratePin()` genere un nouveau PIN
- Le prochain cycle de pooling detecte la difference et met a jour l'affichage
- Si le PIN est "--------" (cleared), l'affichage reflete aussi l'etat vide
- Un log console signale les auto-regenerations detectees

**Fichier modifie:** `frontend/js/ui/AdminView.js` — bloc `_startSessionsPolling()` (lignes 131-159)
