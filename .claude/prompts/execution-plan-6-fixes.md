# Execution Plan — 6 Fixes (2026-05-30-6-fixes)

## Repartition

### Backend-dev (issues 1, 3, 4, 5, 6)
1. **Issue 1** — `backend/src/main.cpp`: Remplacer QHostInfo::localHostName() par GetComputerNameW/gethostname
2. **Issue 5** — `backend/src/server/AuthManager.cpp` + `backend/src/main.cpp`: Clean IPv4-mapped IPv6 (::ffff:x.x.x.x → x.x.x.x)
3. **Issue 6** — `backend/src/main.cpp`: Detecter IP privees (192.168.x, 10.x, 172.16-31.x, 127.x) → "Local"
4. **Issue 3** — `backend/src/server/AuthManager.cpp/.h`: PIN par defaut "--------"
5. **Issue 4** — `backend/src/main.cpp`: Endpoint /api/admin/pin/clear si necessaire

### Frontend-dev (issues 2, 3, 4)
1. **Issue 2** — `frontend/js/ui/LoginView.js`: Styliser l'input hostname (bordure, fond, curseur, hover, focus)
2. **Issue 3** — `frontend/js/ui/AdminView.js`: Afficher "--------" si PIN par defaut
3. **Issue 4** — `frontend/js/ui/AdminView.js`: Bouton "Clear" a cote de "Generate PIN"
