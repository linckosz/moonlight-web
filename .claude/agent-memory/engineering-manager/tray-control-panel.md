---
name: tray-control-panel
description: Ajout d'une action "Control Panel" dans le menu du System Tray, pointant vers la page web du frontend
metadata:
  type: project
---

Ajout de l'action "Control Panel" dans le menu contextuel du System Tray (TrayManager).

**Fichier modifié :** `backend/src/TrayManager.cpp`
- Ligne 50 : ajout de `QAction* controlPanelAction = m_Menu->addAction(tr("&Control Panel"));`
- Ligne 53 : ajout d'un séparateur entre "Control Panel" et "Restart"
- Ligne 57 : `connect(controlPanelAction, &QAction::triggered, this, &TrayManager::onOpen);`

L'action ouvre la même URL que "Open" (`https://localhost:<port>`) via `QDesktopServices::openUrl()`.

Pas de modification nécessaire dans `TrayManager.h` — le slot `onOpen()` existant est réutilisé.
