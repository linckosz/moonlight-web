# Backend — Gaming Mode (souris)

## Tâche

Ajouter le support du mode Gaming (on/off) dans le backend :
- AppSettings : `gamingMode()` / `setGamingMode()`
- Routes settings streaming : GET/POST `/api/settings/streaming` inclut `gaming_mode`
- Route `/start` : transmet `gamingMode` au `StreamSession` et l'inclut dans la reponse JSON

## Lecture prealable obligatoire

Lis d'abord ces fichiers pour comprendre le pattern existant :

1. **backend/src/appsettings.h** — voir comment les getters/setters sont declares (ex: `bool preferredCodec() const;` / `void setPreferredCodec(Codec);`)
2. **backend/src/appsettings.cpp** — voir l'implementation, la cle JSON (ex: `"preferred_codec"`), les valeurs par defaut
3. **backend/src/routes/settings_streaming.cpp** — voir le GET et POST actuels
4. **backend/src/streamsession.h** — voir le constructeur, les membres
5. **backend/src/streamsession.cpp** — voir le constructeur, `onShimConnectionStarted()`
6. **backend/src/routes/start_streaming.cpp** (ou le fichier qui contient la route `/start`) — voir comment `StreamSession` est cree et comment la reponse JSON est construite.

## Modifications a faire

### A. AppSettings (`appsettings.h` + `appsettings.cpp`)

Ajouter :
```cpp
// appsettings.h
bool gamingMode() const;
void setGamingMode(bool enabled);
```

```cpp
// appsettings.cpp
bool AppSettings::gamingMode() const {
    return m_settings.value("gaming_mode", true).toBool();
}

void AppSettings::setGamingMode(bool enabled) {
    m_settings.setValue("gaming_mode", enabled);
    emit settingChanged("gaming_mode");
}
```

`m_settings` est un `QSettings` — utilise le meme pattern que `preferredCodec`.

### B. Routes settings streaming

**GET `/api/settings/streaming`** : ajouter dans le JSON retourne :
```json
{"preferred_codec": "h264|hevc|av1", "gaming_mode": true|false}
```

**POST `/api/settings/streaming`** : accepter un champ optionnel `"gaming_mode"` (booleen). Si present, appeler `appSettings.setGamingMode(...)`. Meme pattern que `"preferred_codec"`.

### C. StreamSession — constructeur

Ajouter un parametre `bool gamingMode` au constructeur de `StreamSession`.
Stocker dans un membre `m_gamingMode`.

### D. Route `/start` — creation du StreamSession

Passer `settings.gamingMode()` au constructeur de `StreamSession`.

### E. Route `/start` — reponse JSON

Dans `onShimConnectionStarted()` (ou equivalent), ajouter `"gamingMode"` au JSON de reponse :
```cpp
QJsonObject response;
response["status"] = "streaming";
response["wsUrl"] = wsUrl;
response["gamingMode"] = m_gamingMode;
// ... autres champs existants
```

## Pattern a respecter

- Meme style que les autres settings (codec, etc.)
- Cle JSON snake_case `"gaming_mode"` dans les fichiers de settings
- Cle JSON camelCase `"gamingMode"` dans la reponse de `/start` (pour le frontend)
- Valeur par defaut : `true` (mode Gaming actif par defaut, comme avant)

## Fichiers modifies

- `backend/src/appsettings.h`
- `backend/src/appsettings.cpp`
- `backend/src/routes/settings_streaming.cpp`
- `backend/src/streamsession.h`
- `backend/src/streamsession.cpp`
- `backend/src/routes/start_streaming.cpp` (ou fichier equivalent contenant le handler de `/start`)

## Resultat attendu

Ecris ton resume dans `.claude/results/backend-dev/{session}/Resume-2026-05-13.md`.
