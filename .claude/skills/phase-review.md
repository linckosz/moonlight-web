---
name: phase-review
description: Vérifie les critères d'acceptation d'une phase du plan contre l'état actuel du code
---

# Skill — Phase Review

Vérifie la complétude d'une phase de développement par rapport au plan d'architecture.

## Procédure

### 1. Charger le plan
Lire `docs/moonlight-web-plan.md` et localiser la phase demandée.

### 2. Extraire les critères d'acceptation
Pour la phase X, lister chaque critère d'acceptation un par un.

### 3. Vérifier chaque critère

Pour chaque critère :
- **Fichiers créés** : Vérifier que tous les fichiers listés existent et ne sont pas vides
- **Fonctionnalité** : Vérifier que le code implémente ce qui est décrit
- **Intégration** : Vérifier que les routes REST sont enregistrées dans `main.cpp`, que le `.pro` est à jour, etc.

### 4. Vérifier les points d'attention
Si la phase list des "Points d'attention", vérifier chacun.

### 5. Produire le rapport

## Format de rapport

```
## Phase Review — Phase [X] : [Nom]

### Critères d'acceptation

| # | Critère | Statut | Notes |
|---|---------|--------|-------|
| 1 | [description] | ✅/❌/⚠️ | [détails si nécessaire] |

### Fichiers attendus

| Fichier | Existe | Taille | Statut |
|---------|--------|--------|--------|
| [chemin] | ✅/❌ | N lignes | OK / Manquant / Vide |

### Points d'attention

| Point | Traité ? | Notes |
|-------|----------|-------|
| [description] | ✅/❌/⚠️ | [détails] |

### Verdict
- Critères remplis : X/Y
- Fichiers présents : X/Y
- Prêt pour la phase suivante : Oui / Non

### Bloquants (si applicable)
- [description de ce qui empêche de passer à la suite]
```

## Notes

- Les phases précédentes sont supposées complètes — ne pas les re-vérifier sauf si demandé
- Se concentrer sur la fonctionnalité, pas le style de code (c'est le rôle du code-reviewer)
- Si un fichier est listé dans le plan mais manque, c'est un bloquant
