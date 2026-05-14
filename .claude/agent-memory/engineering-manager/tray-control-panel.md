---
name: tray-control-panel
description: Actions "Open" et "Server Settings" dans le System Tray, ouvre le frontend dans le navigateur
metadata:
  type: project
---

Le System Tray (TrayManager) propose deux actions qui ouvrent le frontend :

1. **Open** (`onOpen()`) : ouvre `https://localhost:<port>/` (page d'accueil / liste des hosts)
2. **Server Settings** (`onOpenSettings()`) : ouvre `https://localhost:<port>/admin` (page admin)

**Fichier modifié :** `backend/src/TrayManager.cpp`
- Ligne 50 : `QAction* controlPanelAction = m_Menu->addAction(tr("&Server Settings"));`
- Ligne 57 : `connect(controlPanelAction, &QAction::triggered, this, &TrayManager::onOpenSettings);`
- `onOpenSettings()` utilise un path `/admin` (pas de hash `#admin`) depuis la migration vers History API (2026-05-14).

**Pourquoi pas de hash :** Le frontend utilise desormais le History API routing (`window.location.pathname`), pas le hash-based routing. Voir [[history-api-routing]].
