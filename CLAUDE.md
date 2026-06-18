# CLAUDE.md — Moonlight-Web (MW)

## Mode de fonctionnement

**Traite chaque demande directement, en une seule passe.** Tu n'invoques un
subagent que si l'utilisateur le demande explicitement par son nom. L'agent
`engineering-manager` n'est plus le point d'entrée par défaut — il ne sert que
pour les chantiers multi-domaines lourds, sur demande explicite.

## Optimisation de coût et de contexte — RÈGLES OBLIGATOIRES

- **Subagents = exception.** Jamais de subagent pour ce qu'un Read/Edit/Grep
  direct résout. Jamais de chaîne manager → dev → reviewer automatique.
  Maximum 1 subagent par tâche, sauf demande explicite de l'utilisateur.
- **Une seule passe** : Grep ciblé → lire le minimum de fichiers → éditer →
  builder → conclure. Pas d'exploration multi-étapes spéculative.
- **Pas de relecture inutile** : ne jamais recharger un fichier déjà lu dans la
  session ; pour les gros fichiers, lire uniquement les sections utiles
  (offset/limit). Ne jamais recopier le contenu d'un fichier dans la réponse.
- **Limiter les chaînes d'outils** : regrouper les appels indépendants en un
  seul bloc parallèle ; pas d'allers-retours outil par outil.
- **Réponses directes et diffs** : conclusion d'abord, pas de rapport long,
  pas de duplication d'information déjà dans le contexte.
- **Sessions courtes et spécialisées** : une session = un domaine (backend OU
  frontend OU config). `/compact` dès ~80k tokens de contexte, `/clear` à
  chaque changement de sujet. Pas de session longue multi-domaines.
- **Modèles** : `engineering-manager` = fable ; les autres subagents = opus par
  défaut, sonnet acceptable pour une tâche légère bien cadrée.

## Agents (usage exceptionnel, sur demande explicite)

| Agent | Modèle | Rôle |
|---|---|---|
| `engineering-manager` | fable | Orchestration de chantiers multi-domaines lourds uniquement |
| `backend-dev` | opus (sonnet si léger) | Backend C++/Qt |
| `frontend-dev` | opus (sonnet si léger) | Frontend Vanilla JS |
| `code-reviewer` | opus (sonnet si léger) | Revue de code — uniquement sur demande |
| `expert-moonlight-refs` | opus (sonnet si léger) | Référence moonlight-qt / xbox / web-stream ⚠️ autorisation requise |

⚠️ **Codebases externes** (`moonlight-qt`, `moonlight-xbox`, `moonlight-web-stream`,
sources Sunshine) : ne jamais les scanner sans autorisation explicite de
l'utilisateur. Pour l'API Sunshine, le skill `sunshine-api` suffit.

## Skills projet

| Skill | Usage |
|---|---|
| `build` | Build backend C++ (MSVC qmake+nmake), rapport d'erreurs |
| `test-stream` | Test E2E — lance le serveur et vérifie les endpoints |
| `sunshine-api` | Référence API Sunshine — endpoints, pairing, launch, RTSP, XML |

## Projet

- **Stack** : backend C++17 / Qt 6.11 (MSVC 2022) ; frontend Vanilla JS (modules ES6), HTML/CSS
- **Objectif** : client de streaming Sunshine dans le navigateur — WebCodecs + canvas, transport WebRTC (DataChannels + media tracks) avec fallback WSS, multi-hôtes, pairing, audio AudioWorklet, input clavier/souris/touch
- **État** : phases 1–6 terminées (HTTP, découverte, pairing, apps, RTSP, vidéo, audio) ; input avancé et polish en cours
- **Docs** : [plan](docs/moonlight-web-plan.md) · [API Sunshine](docs/sunshine-api.md) · [architecture moonlight-qt](docs/moonlight-qt-architecture.md) · [i18n](docs/i18n.md)

## Standards

- Code propre, simple, robuste — éviter la sur-ingénierie
- Commentaires dans le code : **toujours en anglais**, concis (1-2 lignes max)
- Réponses à l'utilisateur : **toujours en français**

## Commandes utiles

```bash
# Build backend (MSVC)
cmd //c d:/Code/moonlight-web-deepseek/backend/build_msvc.bat

# Lancer / arrêter le serveur (HTTP 48000, HTTPS 443)
cd backend/build/release && ./mw-server.exe
powershell -Command "Stop-Process -Name mw-server -Force"

# Test rapide API
curl -k http://127.0.0.1:48000/api/hosts
```

## graphify (optionnel — pas de chaîne obligatoire)

Graphe de connaissance dans `graphify-out/`. Utiliser `graphify query "<question>"`
**seulement** pour les questions d'architecture larges où un Grep ciblé ne suffit
pas. Après une série de modifications significatives (pas après chaque édition),
lancer `graphify update .` (AST-only, sans coût API).
