# UPNP NAT Traversal — Plan de Test (Phase 7)

## 1. Tests unitaires (backend)

### 1a. Fallback sans miniupnpc

| Test | Methode | Critere |
|---|---|---|
| Construction | `UPNPClient c;` | `!c.isAvailable()`, `c.gatewayAddress().isNull()` |
| Destructeur | `{ UPNPClient c; }` | Pas de crash |
| discover() sans IGD | `c.discover(500)` | retourne `false`, `!c.isAvailable()`, error signal emis |
| addPortMapping() sans IGD | `c.addPortMapping(48010, 48010)` | retourne `false`, error signal emis |
| removePortMapping() sans IGD | `c.removePortMapping(48010)` | retourne `false` |
| getExternalIPAddress() sans IGD | `c.getExternalIPAddress()` | retourne `""` |
| Double discover() | `c.discover(); c.discover();` | Pas de crash (cleanup interne OK) |

**Execution** : `cd backend/tests && run_upnp_tests.bat`

### 1b. E2E avec miniupnpc (necessite routeur UPnP sur le LAN)

| Test | Methode | Critere |
|---|---|---|
| discover() avec IGD | `c.discover(2000)` | retourne `true`, `c.isAvailable()`, `!c.gatewayAddress().isNull()` |
| getExternalIPAddress() | `c.getExternalIPAddress()` | retourne IP publique non-vide |
| addPortMapping(48010) | `c.addPortMapping(48010, 48010, 3600, "test")` | retourne `true`, mapping visible dans l'admin du routeur |
| removePortMapping(48010) | `c.removePortMapping(48010)` | retourne `true`, mapping disparait du routeur |
| Port fallback | Essayer port occupe, verifier port+1 | addPortMapping reussi sur le port suivant |

**Execution** : `cd backend/tests && run_upnp_tests.bat --upnp`

---

## 2. Tests API REST

### 2a. Settings streaming (upnp_enabled)

**GET /api/settings/streaming**
```bash
curl -k https://localhost/api/settings/streaming
```
Attendu : `{"upnp_enabled": true, "video_codec": "auto", "gaming_mode": true}`

**POST /api/settings/streaming (disable UPnP)**
```bash
curl -k -X POST https://localhost/api/settings/streaming -d '{"upnp_enabled": false}'
```
Attendu : `{"upnp_enabled": false, "status": "saved"}`

**Verifier persistance** : Refaire GET, verifier `upnp_enabled: false`

**POST /api/settings/streaming (re-enable)**
```bash
curl -k -X POST https://localhost/api/settings/streaming -d '{"upnp_enabled": true}'
```
Attendu : `{"upnp_enabled": true, "status": "saved"}`

### 2b. Reponse /start

**POST /api/hosts/:id/start** (lancer un stream)
```bash
curl -k -X POST https://localhost/api/hosts/<uuid>/start -d '{"appId": <id>}'
```
Attendu : La reponse JSON contient les champs suivants selon le contexte:

| Champ | Valeur UPnP OK | Valeur sans UPnP |
|---|---|---|
| `upnpAvailable` | `true` | `false` |
| `upnpPublicIP` | `"1.2.3.4"` | absent (`undefined`) |
| `upnpPort` | `48010` | absent (`undefined`) |

**Note** : Sans miniupnpc compile, `upnpAvailable` sera toujours `false`.

### 2c. Frontend verification

Ouvrir https://localhost dans un navigateur :
1. Aller dans Settings → checkbox UPnP NAT Traversal presente
2. Decocher/recocher → verifier le Toast "Saved"
3. Relancer la page → le setting est persiste
4. Lancer un stream → verifier le Toast UPnP dans la console :
   - `Toast.success('UPnP active — port mapped')` si UPnP disponible
   - `Toast.warning('UPnP not available...')` si pas d'UPnP

---

## 3. Test de regression LAN

1. Lancer mw-server normalement
2. Ouvrir https://localhost dans un navigateur sur le meme LAN
3. Lancer un stream
4. Verifier :
   - Pas de candidat UPnP dans les logs backend : `[DataChannelRelay] Rewrote host candidate` NE doit PAS apparaitre
   - La connexion WebRTC s'etablit correctement (candidats host directs)
   - La video s'affiche
   - L'audio fonctionne
   - Les entrees clavier/souris fonctionnent
5. Verifier les logs backend :
   - `[SignalingServer] LAN ICE: no STUN, no UPnP` (pas de STUN ni UPnP pour LAN)
   - `[Signaling] UPnP: no IGD found` ou success (selon le reseau)

---

## 4. Test E2E streaming depuis l'exterieur

**Pre-requis** :
- Serveur MW lance sur le reseau domestique
- Routeur avec UPnP actif
- Client sur un reseau 4G/5G ou reseau ami
- tunnel public actif (ou exposition directe)

**Procedure** :
1. Verifier que l'allocateur (processus principal, thread `mw-upnp`) trouve l'IGD :
   - Log : `[UPNP] Gateway found, public <PUBLIC_IP>, this host 192.168.x.x`
2. Verifier le trou du tunnel a la montee du rendez-vous :
   - Log : `[UPNP] tunnel: claimed 3478 (UDP+TCP, public <PUBLIC_IP>)`
   - Log : `[Tunnel] Router hole 3478 ready (public <PUBLIC_IP>), 1 of 4 — more are claimed as browsers arrive`
3. Depuis l'exterieur, ouvrir l'URL publique
4. Lancer un stream : le trou media est reclame AVANT le spawn du worker
   - Log : `[UPNP] media slot 0: claimed 48010 (UDP+TCP, public <PUBLIC_IP>)`
   - Log worker : `[SignalingServer] Router forwards <PUBLIC_IP> : 48010 to media port 48010`
5. Verifier les logs :
   - `[DataChannelRelay] Rewrote host candidate: <LAN_IP> -> <PUBLIC_IP>:48010`
6. Verifier que le stream fonctionne (video + audio + input)
7. Un deuxieme navigateur sur le tunnel : `[UPNP] tunnel: claimed 3479 …` apres que le
   premier a ete servi, jamais en le bloquant
8. `settings.json` porte `router_ports` ; un redemarrage reutilise les memes numeros
   (`[UPNP] tunnel: claimed 3478 …` sans `after skipping`)

### 4b. Deux hotes MoonlightWeb sur le meme LAN

1. Demarrer l'hote A, puis l'hote B (Internet Access + UPnP actifs sur les deux)
2. Sur B : `[UPNP] tunnel: claimed 3479 … after skipping 3478 (<IP de A>)` et, au premier
   stream, `[UPNP] media slot 0: claimed external 46100 -> 48010 after skipping 48010 (<IP de A>)`
3. Verifier la table du routeur en SOAP (`GetSpecificPortMappingEntry`, la table COM ment) :
   `NewInternalClient` de 3478 = A, de 3479 = B, de 48010 = A, de 46100 = B
4. Depuis un reseau d'entreprise, le tunnel de A ET celui de B repondent
5. Redemarrer B : il reprend 3479 (memorise), A n'est pas touche
6. Trempage > 1 h : aucun `[UPNP] … now forwards to …` sur aucun des deux hotes

---

## 5. Tests de robustesse

| Scenario | Verification |
|---|---|
| Routeur non-UPnP | discover echoue → fallback STUN-only, pas de crash |
| Routeur UPnP desactive | idem |
| 48010 tenu par un voisin du LAN | l'externe marche vers le pool 46100-46199, le bind local reste 48010 (`RouterPortCore`, TNR `test_router_port_core.cpp`) |
| 3478-3481 et 5349-5352 tenus | le tunnel prend 46000-46031, un port a la fois ; tout tenu → port ephemere, candidat reflexif seul |
| Routeur qui reecrit en silence (Livebox) | relecture apres chaque ecriture et a chaque renouvellement → `dropped and forgotten`, re-reclamation |
| Mapping expire | renouvellement toutes les 30 min sur le thread `mw-upnp` ; 2 echecs → mappings permanents |
| Deuxieme stream sur le meme slot | trou deja tenu → reponse immediate, pas de SOAP |
| Arret du serveur | `RouterPortAllocator::~RouterPortAllocator` retire tous les mappings, les numeros restent dans `router_ports` |
| CGNAT detecte | getExternalIPAddress retourne IP du CGNAT, pas de solution → message utilisateur |

---

## 6. Checklist finale

- [ ] `UPNPClient::discover()` detecte l'IGD sur reseau domestique
- [ ] `UPNPClient::addPortMapping()` cree le mapping UDP
- [ ] `UPNPClient::getExternalIPAddress()` retourne IP publique correcte
- [ ] `UPNPClient::removePortMapping()` nettoie
- [ ] Build mw-server avec `MW_HAVE_MINIUPNPC`
- [ ] Build mw-server SANS miniupnpc (fallback preserve)
- [ ] GET/POST /api/settings/streaming avec upnp_enabled
- [ ] Reponse /start contient upnpAvailable/upnpPublicIP/upnpPort
- [ ] SettingsView checkbox UPnP fonctionne
- [ ] StreamView Toast UPnP s'affiche
- [ ] Candidat host reecrit avec IP publique (log visible)
- [ ] Streaming LAN sans regression
- [ ] Port mapping cleanup a l'arret
- [ ] Deux hotes sur le meme LAN : chacun son trou, table du routeur a l'appui (§4b)
