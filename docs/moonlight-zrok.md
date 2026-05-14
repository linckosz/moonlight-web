# Architecture cible — Moonlight-Web + zrok + ENET UDP P2P

## Objectif

Construire une architecture de streaming gaming Web :

- sans VPN utilisateur
- sans compte utilisateur final
- sans VPS personnel
- avec découverte automatique via URL HTTPS
- avec URL fixe persistante générée par Moonlight-Web
- avec streaming UDP peer-to-peer faible latence
- Continuer à utiliser moonlight-common-c
- avec accès direct aux frames via ENET
- sans pipeline vidéo WebRTC

---

# Vue globale

```
┌─────────────────────────────────────────────┐
│ HOST PC (chez utilisateur)                  │
│                                             │
│ Moonlight-Web Host                          │
│ ├── HTTP UI Server                          │
│ ├── zrok embedded client                    │
│ ├── ICE/STUN signaling                      │
│ ├── ENET UDP server (moonlight-common-c)    │
│ ├── Video stream UDP                        │
│ ├── Audio UDP                               │
│ └── Input UDP                               │
└─────────────────────────────────────────────┘
                    │
                    │ HTTPS reverse tunnel
                    ▼
┌─────────────────────────────────────────────┐
│ zrok public infrastructure                  │
│ (OpenZiti hosted service)                   │
│                                             │
│ ONLY USED FOR:                              │
│ - discovery                                 │
│ - HTTPS UI exposure                         │
│ - signaling bootstrap                       │
└─────────────────────────────────────────────┘
                    │
                    ▼
┌─────────────────────────────────────────────┐
│ CLIENT Browser                              │
│                                             │
│ Web UI                                      │
│ ├── signaling websocket                     │
│ ├── ICE candidate exchange                  │
│ ├── UDP connectivity establishment          │
│ ├── ENET client                             │
│ ├── audio playback                          │
│ └── mouse/keyboard/gamepad input            │
└─────────────────────────────────────────────┘
```

---

# IMPORTANT

## zrok ne transporte PAS le stream

Le trafic vidéo/audio/input ne doit jamais passer dans zrok.

zrok sert uniquement à :

- exposer l’interface web
- bootstrapper la connexion
- permettre l’échange ICE/STUN

Puis :

```
ICE/STUN
↓
UDP hole punching
↓
ENET direct P2P
```

---

# URL publique persistante

## Objectif

Moonlight-Web doit générer une URL publique fixe et persistante par machine host.

Format :

```
https://moonlightweb-{UniqueID}.share.zrok.io
```

Exemple :

```
https://moonlightweb-k82x4pe8.share.zrok.io
```

---

## Règles importantes

- URL identique entre redémarrages
- URL identique après reboot machine
- générée automatiquement
- aucun input utilisateur requis
- bookmarkable
- utilisable en QR code

---

# Génération du UniqueID

## Premier lancement

1. Génèrer un ID machine unique alphanumérique de 8 caractères en minuscules uniquement (a–z, 0–9), basé sur un mélange de timestamp et de suffixe aléatoire. L’objectif est d’obtenir un identifiant court, unique à forte probabilité de non-collision, adapté pour des usages comme URLs, bases de données ou tracking.
2. Construire un nom zrok :

```
moonlightweb-{UniqueID}
```

Exemple :

```
moonlightweb-k82x4pe8
```

3. Réserver le share :

```bash
zrok reserve public http://localhost:<UI_PORT> --unique-name moonlightweb-k82x4pe8
```

4. Stocker localement :

```json
{
  "zrokReservedName": "moonlightweb-k82x4pe8"
}
```

---

## Lancements suivants

```bash
zrok share reserved moonlightweb-k82x4pe8
```

---

# Workflow complet

## Phase 1 — Host startup

### 1. Démarrage Moonlight-Web

```
HTTP UI:
localhost:<UI_PORT>

Signaling WS:
localhost:<WS_PORT>

ENET:
UDP 47998 (video) / 47999 (audio) / 48000 (inputs)
```

⚠️ Le port UI est dynamique et doit être injecté dans zrok au runtime.

---

### 2. Lancement zrok

```bash
zrok share reserved moonlightweb-k82x4pe8
```

Doit pointer vers :

```
http://localhost:<UI_PORT>
```

---

### 3. URL utilisateur

```
https://moonlightweb-k82x4pe8.share.zrok.io
```

---

## Phase 2 — Client bootstrap

### 4. Ouverture URL

```
https://moonlightweb-k82x4pe8.share.zrok.io
```

---

### 5. Signaling

```
wss://moonlightweb-k82x4pe8.share.zrok.io/ws
```

Utilisé uniquement pour :
- ICE candidates
- IP publiques
- ports UDP
- session ENET

---

## Phase 3 — NAT traversal

### 6. STUN

```
stun.l.google.com:19302
```

---

### 7. ICE exchange

```json
{
  "ip": "82.x.x.x",
  "port": 49000,
  "protocol": "udp"
}
```

---

### 8. Hole punching

```
CLIENT UDP ↔ HOST UDP
```

---

## Phase 4 — ENET streaming

### 9. Connexion ENET

```
ENET connect()
```

---

### 10. Flux

| Flux | Type |
|------|------|
| Video | UDP |
| Audio | UDP |
| Input | UDP |
| Control | UDP |

---

## Phase 5 — Client rendering

Ne change rien de l'actuel qui fonctionne déjà,

# Architecture réseau

```
                ┌──────────────┐
                │ zrok servers │
                └──────┬───────┘
                       │
      ┌────────────────┴──────────────┐
      ▼                               ▼
┌────────────┐                 ┌────────────┐
│ Host PC    │                 │ Browser    │
│ zrok agent │                 │ Client     │
│ signaling  │                 │ JS app     │
└─────┬──────┘                 └─────┬──────┘
      │                                │
      └──────── UDP P2P ENET ──────────┘
```

---

# Contraintes

- NAT traversal pas garanti partout
- fallback nécessaire
- UDP parfois bloqué

---

# Conclusion

- zrok = bootstrap uniquement
- ENET = transport principal
- UDP = peer-to-peer direct
- URL fixe = moonlightweb-{UniqueID}
- zéro compte utilisateur final