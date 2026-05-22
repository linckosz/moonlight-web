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
1. Verifier que UPnP discover trouve l'IGD :
   - Log : `[UPNP] IGD found: LAN addr=192.168.x.x`
2. Verifier que le port mapping est ajoute :
   - Log : `[UPNP] Port mapping added successfully: 48010 UDP`
3. Verifier l'IP publique :
   - Log : `[UPNP] External IP address: <PUBLIC_IP>`
4. Depuis l'exterieur, ouvrir l'URL publique
5. Lancer un stream
6. Verifier les logs :
   - `[DataChannelRelay] Rewrote host candidate: <LAN_IP> -> <PUBLIC_IP>:48010`
   - `[SignalingServer] UPnP ICE: port range fixed to 48010-48010`
7. Verifier que le stream fonctionne (video + audio + input)

---

## 5. Tests de robustesse

| Scenario | Verification |
|---|---|
| Routeur non-UPnP | discover echoue → fallback STUN-only, pas de crash |
| Routeur UPnP desactive | idem |
| Port 48010 occupe | addPortMapping echoue sur 48010, essai 48011...48014 |
| Mapping expire | Timer de renouvellement toutes les 30 min |
| Double demarrage | 2eme session → UPnP deja setup, skip |
| Arret brutal du serveur | cleanupUPnP() appelle removePortMapping |
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
