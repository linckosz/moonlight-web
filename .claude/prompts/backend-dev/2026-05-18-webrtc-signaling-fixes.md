# Tâche : 3 correctifs SignalingServer WebRTC

Session ID : `2026-05-18-webrtc-signaling-fixes`

## Contexte

Fichiers concernés :
- `d:\Code\moonlight-web-deepseek\backend\src\streaming\SignalingServer.cpp`
- `d:\Code\moonlight-web-deepseek\backend\src\streaming\SignalingServer.h`

## Correctif 1 — `isPrivateAddress()` ne détecte pas `::ffff:127.0.0.1`

**Problème :** `isPrivateAddress()` teste les adresses IPv4-mapped IPv6 (`::ffff:x.x.x.x`) pour les plages privées (10.x, 172.16-31.x, 192.168.x, 169.254.x) mais **pas** 127.x.x.x (loopback). Donc `::ffff:127.0.0.1` est classifié comme Internet.

**Solution recommandée :** Dans `onNewWsConnection()`, normaliser les adresses IPv4-mapped IPv6 en IPv4 pures **avant** d'appeler `isPrivateAddress()`. Comme ça `isPrivateAddress()` reçoit `"127.0.0.1"` au lieu de `"::ffff:127.0.0.1"`.

Où :
1. Dans `onNewWsConnection()` (ou `onWsConnected()`), localiser où `m_remoteAddr` est initialisé.
2. Après avoir récupéré l'adresse distante du WebSocket, ajouter un helper qui convertit `::ffff:x.x.x.x` → `x.x.x.x` si c'est le cas.
3. Laisser `isPrivateAddress()` inchangée (elle fonctionne déjà avec les IPv4 pures).

Détail d'implémentation :
```cpp
// Helper lambda ou méthode privée :
static std::string normalizePeerAddress(const std::string &addr) {
    // Si commence par "::ffff:" et a une IPv4 valide derrière, extraire l'IPv4
    static const std::string prefix = "::ffff:";
    if (addr.compare(0, prefix.size(), prefix) == 0) {
        std::string ipv4 = addr.substr(prefix.size());
        // Vérifier rapidement que c'est bien une IPv4 (contient 3 points)
        if (std::count(ipv4.begin(), ipv4.end(), '.') == 3) {
            return ipv4;
        }
    }
    return addr;
}
```

Puis dans `onNewWsConnection()` :
```cpp
m_remoteAddr = normalizePeerAddress(ws->peerAddress().toString().toStdString());
```

## Correctif 2 — Race condition double déclenchement fallback WS

**Problème :** Quand le browser ET le backend timeout tous les deux ICE (~30s), le message `fallback-ws-request` peut arriver APRÈS que `startWsFallback()` ait déjà été déclenché côté backend → `"Unknown message type"`.

**Solution :** Ajouter un guard dans le handler `fallback-ws-request` du `onWsTextMessage()` :

```cpp
if (method == "fallback-ws-request") {
    if (m_WsFallbackActive) {
        logInfo("Fallback WS already active, ignoring duplicate request");
        return;
    }
    startWsFallback();
}
```

Vérifier que `m_WsFallbackActive` est bien un `std::atomic<bool>` ou protégé par mutex, car `startWsFallback()` peut être appelé depuis différents threads.

## Correctif 3 — Robustesse locale (forcer LAN pour 127.0.0.1)

**Problème :** Si le peer est 127.0.0.1 / ::1 / ::ffff:127.0.0.1, il faut forcer le mode LAN (pas de STUN, pas d'ICE-TCP) plutôt que de compter sur la classification.

**Solution :** Dans `onNewWsConnection()`, après la normalisation du correctif 1, ajouter un flag `m_forceLanMode` (ou réutiliser un flag existant). Puis, dans la partie ICE configuration, forcer le mode LAN si ce flag est true.

Étapes :
1. Ajouter `bool m_forceLanMode = false;` dans le `.h`
2. Dans `onNewWsConnection()` :
   ```cpp
   m_forceLanMode = (m_remoteAddr == "127.0.0.1" || m_remoteAddr == "::1");
   ```
3. Dans la création de la configuration ICE (là où les STUN/TCP sont configurés), si `m_forceLanMode` est true :
   - Supprimer les STUN servers
   - Désactiver ICE-TCP
   - (Éventuellement) forcer `iceTransportPolicy: relay`... non, plutôt `iceTransportPolicy: all` mais sans STUN ni TURN.

## Instructions

1. **Lire** d'abord `SignalingServer.h` et `SignalingServer.cpp` en entier
2. **Analyser** le code existant pour trouver exactement où placer chaque modification
3. **Implémenter** les 3 correctifs
4. **Compiler** avec `cmd //c d:/Code/moonlight-web-deepseek/backend/build_msvc.bat`
5. **Corriger** les éventuelles erreurs de compilation

## Règles
- Code propre, minimal, sans sur-ingénierie
- Commentaires en anglais, concis (1 ligne max)
- Thread-safe

## Résultat attendu

En fin de travail, écris ton résumé dans :
`.claude/results/backend-dev/2026-05-18-webrtc-signaling-fixes/Resume-2026-05-18.md`

Inclus : fichiers modifiés, résumé de chaque correctif, statut compilation.
