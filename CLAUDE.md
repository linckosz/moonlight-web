# CLAUDE.md — Moonlight-Web (MW)

## Présentation du projet

- **Nom** : Moonlight-Web (MW)
- **Stack backend** : C++ avec Qt (QML pour composants UI si nécessaire)
- **Stack frontend** : Application web (HTML/CSS/JS, framework léger)
- **Objectif** : Client de streaming vidéo H.264 communiquant avec Sunshine via API, avec interface multi-hôtes et affichage canvas-based
- **Référence principale** : [moonlight-qt](D:\Code\moonlight-qt\app) — dossier applicatif de Moonlight-Qt
- **Documentation architecture** : [docs/moonlight-qt-architecture.md](docs/moonlight-qt-architecture.md)
- **Documentation API Sunshine** : [docs/sunshine-api.md](docs/sunshine-api.md)

## Standards de code

- Code propre, modulaire, prêt pour la production
- Technologies simples, robustes et performantes — éviter la sur-ingénierie
- Commentaires dans le code : **toujours en anglais**, très concis (1-2 lignes max)
- Toujours expliquer brièvement les décisions d'architecture avant d'implémenter

## Règles de communication

- Répondre **toujours en français**
- Commentaires dans le code **toujours en anglais**

## Règles de comportement

- Consulter automatiquement `@docs/moonlight-qt-architecture.md` dès que le contexte touche au streaming vidéo/audio, aux codecs, aux renderers, ou à tout choix d'architecture qui pourrait avoir un équivalent dans moonlight-qt. Ne pas attendre que l'utilisateur le demande.
- Consulter automatiquement `@docs/sunshine-api.md` dès que le contexte concerne l'API Sunshine.

## Commandes utiles

À compléter au fur et à mesure :

```bash
# Build backend
cd backend && cmake -B build && cmake --build build

# Lancer le serveur
./backend/build/mw-server

# Servir le frontend (développement)
npx serve frontend/
```
