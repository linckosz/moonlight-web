---
name: internet-access-ui-refactor
description: "Refonte UI Internet Access (nport tunnel): checkbox + URL + etats dans AdminView"
metadata:
  type: project
---

# Internet Access UI Refactor (2026-05-14)

## Changements effectues

### Backend
- **NportClient.cpp/h** : ajout de `lastError()` et `m_LastError` pour tracker les erreurs.
- **main.cpp** : generation auto du sous-domaine `moonlightweb-XXXXXXXX` (8 hex chars via QRandomGenerator) au premier demarrage, persiste dans AppSettings. Endpoint `/api/tunnel/disable` ne clear plus le subdomain (uncheck = stop only).

### Frontend
- **AdminView.js** : section tunnel remplacee par checkbox + URL + texte info. 6 etats: idle, starting, active, error, unavailable. Anti-race via seq guard dans `_pollTunnelActive()`. Plus de champs de saisie subdomain ni de boutons Enable/Disable.
- **style.css** : nouveaux styles pour tunnel-checkbox-row, tunnel-url-link/disabled, tunnel-info-success/error/pending/warning/neutral, tunnel-spinner.

## Decisions d'architecture
- Le sous-domaine est auto-genere UNE fois au demarrage du backend, pas cote frontend
- Le disable ne fait que `nportClient.stop()` sans effacer le subdomain persiste
- Le polling utilise un compteur de sequence pour annuler les polls obsoletes si l'utilisateur toggle rapidement

## Fichiers modifies
- `backend/src/network/NportClient.h`
- `backend/src/network/NportClient.cpp`
- `backend/src/main.cpp`
- `frontend/js/ui/AdminView.js`
- `frontend/css/style.css`
