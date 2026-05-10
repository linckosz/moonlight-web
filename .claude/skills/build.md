---
name: build
description: Build le backend C++/Qt (MSVC nmake), collecte et formate les erreurs de compilation
---

# Skill — Build Backend

Compile le backend Moonlight-Web et rapporte les résultats.

## Procédure

1. Vérifier que le script `d:/Code/moonlight-web-deepseek/backend/build_msvc.bat` existe
2. Exécuter le build :
   ```bash
   cmd //c d:/Code/moonlight-web-deepseek/backend/build_msvc.bat 2>&1
   ```
3. Analyser la sortie :
   - Si `error` apparaît → extraire les lignes d'erreur avec contexte (fichier, ligne, message)
   - Si `warning` apparaît → les lister séparément
   - Si le build réussit → confirmer + donner le chemin de l'exécutable
4. Formater le rapport proprement

## Format de rapport

```
## Build — [SUCCESS | FAILED]

### Exécutable
- [chemin]/mw-server.exe

### Warnings (s'il y en a)
- [fichier:ligne] — [message]

### Erreurs (s'il y en a)
- [fichier:ligne] — [message]
  → Suggestion de fix : [si évident]

### Temps de build
[X] secondes
```

## Notes

- Le script MSVC configure l'environnement via vcvars64.bat automatiquement
- Les DLLs Qt et OpenSSL doivent être dans le dossier de sortie pour l'exécution
- Si le build échoue avec "qmake not found", vérifier que Qt 6.11 est installé dans `C:/Qt/6.11.0/`
