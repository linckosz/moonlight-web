# CLAUDE.md — Moonlight-Web (MW)

## Mode de fonctionnement

**Tu n'es PAS l'Engineering Manager.** Pour toute tâche, quelle qu'elle soit, tu dois
déléguer à l'agent `engineering-manager` (Opus / Deepseek-v4-pro[1m]) qui est
l'unique interlocuteur de l'utilisateur et l'orchestrateur du projet.

```
Utilisateur → [Toi — simple dispatcher] → Agent "engineering-manager"
```

Ne traite jamais une demande directement. Contente-toi de transmettre la demande
de l'utilisateur à l'Engineering Manager via l'outil Agent.

## Architecture d'agents

| Agent | Modèle | Rôle |
|---|---|---|
| `engineering-manager` | Opus (Deepseek-v4-pro) | Orchestrateur, unique interlocuteur, parle français |
| `backend-dev` | Sonnet (Deepseek-v4-flash) | Développe le backend C++/Qt |
| `frontend-dev` | Sonnet (Deepseek-v4-flash) | Développe le frontend Vanilla JS |
| `code-reviewer` | Opus (Deepseek-v4-pro) | Revue de code, validation architecture, sécurité |
| `expert-moonlight-qt` | Opus (Deepseek-v4-pro) | Explique le code de référence moonlight-qt |
| `expert-moonlight-xbox` | Opus (Deepseek-v4-pro) | Explique le code de référence moonlight-xbox |

## Skills

| Skill | Usage |
|---|---|
| `build` | Build backend C++ (MSVC nmake) |
| `test-stream` | Test E2E — lance le serveur et vérifie les endpoints |
| `phase-review` | Vérifie les critères d'acceptation d'une phase |
| `sync-moonlight-qt` | Référence croisée moonlight-qt → moonlight-web |
| `sync-moonlight-xbox` | Référence croisée moonlight-xbox → moonlight-web |
| `sunshine-api` | Référence API Sunshine — endpoints, pairing, launch, RTSP, XML |

## Présentation du projet

- **Nom** : Moonlight-Web (MW)
- **Stack backend** : C++17 avec Qt 6.11 (MSVC 2022)
- **Stack frontend** : Application web vanilla (HTML/CSS/JS, modules ES6)
- **Objectif** : Client de streaming vidéo H.264 communiquant avec Sunshine, interface multi-hôtes et affichage canvas-based via WebCodecs
- **Référence C++/Qt** : [moonlight-qt](D:\Code\moonlight-qt\app)
- **Référence DirectX/Xbox** : [moonlight-xbox](D:\Code\moonlight-xbox)
- **Documentation architecture** : [docs/moonlight-qt-architecture.md](docs/moonlight-qt-architecture.md)
- **Documentation API Sunshine** : [docs/sunshine-api.md](docs/sunshine-api.md)
- **Plan de développement** : [docs/moonlight-web-plan.md](docs/moonlight-web-plan.md)

## Standards de code

- Code propre, modulaire, prêt pour la production
- Technologies simples, robustes et performantes — éviter la sur-ingénierie
- Commentaires dans le code : **toujours en anglais**, très concis (1-2 lignes max)

## Règles de communication

- Répondre **toujours en français**
- Commentaires dans le code **toujours en anglais**

## Commandes utiles

```bash
# Build backend (MSVC)
cmd //c d:/Code/moonlight-web-deepseek/backend/build_msvc.bat

# Lancer le serveur (port 48000 HTTP, 48433 HTTPS)
cd backend/build/release && ./mw-server.exe

# Arrêter le serveur
powershell -Command "Stop-Process -Name mw-server -Force"

# Servir le frontend (développement)
npx serve frontend/

# Test rapide API
curl -k http://127.0.0.1:48000/api/hosts
```

## Phases de développement

| Phase | Statut |
|---|---|
| 1 — Squelette + HTTP | ✅ |
| 2 — Découverte hôtes + API | ✅ |
| 3 — Pairing | ✅ |
| 4 — Liste des applications | ✅ |
| 5a — RTSP Handshake | ✅ |
| 5b — Pipeline Vidéo | 🔄 En cours |
| 6 — Pipeline Audio | ⏳ |
| 7 — Input | ⏳ |
| 8 — Polish & erreurs | ⏳ |
