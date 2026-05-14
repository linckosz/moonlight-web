Analyse le TrayManager de Moonlight-Web pour comprendre comment il construit l'URL pour onOpen().

Le projet est à d:\Code\moonlight-web-deepseek.
Le backend est dans d:\Code\moonlight-web-deepseek\backend\src.

Objectif : Comprendre quel URL exact est ouvert quand l'utilisateur clique sur "Server Settings" (anciennement "Control Panel") dans le menu system tray.

Lis les fichiers suivants :
1. d:\Code\moonlight-web-deepseek\backend\src\TrayManager.cpp
2. d:\Code\moonlight-web-deepseek\backend\src\TrayManager.h

Cherche spécifiquement :
1. Comment onOpen() construit l'URL
2. Quel port est utilisé (http port, https port)
3. Où est défini le titre "Server Settings" (ou "Control Panel")
4. Comment l'URL est ouverte (QDesktopServices::openUrl, QProcess, etc.)

En fin de travail, écris ton résumé dans
`.claude/results/backend-dev/2026-05-14-tray-settings/Resume-YYYY-MM-DD.md`.
Inclus uniquement tes résultats/conclusions (pas la réflexion intermédiaire).
Format : tâche accomplie, fichiers modifiés, décisions prises, points bloquants.
