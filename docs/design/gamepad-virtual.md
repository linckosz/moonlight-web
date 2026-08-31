# Manette virtuelle — architecture

> Révision du plan initial, corrigée après vérification des dépendances
> disponibles en septembre 2026. Les corrections de fond sont signalées ⚠️.

## 1. Objectif

Transmettre une manette physique connectée au navigateur vers le host, sur les
trois plateformes, sans dépendance payante.

**Contrainte fondamentale, inchangée** : 100 % gratuit et open source. Aucune
licence à acheter, aucun abonnement, aucun SDK commercial, aucun compte
fournisseur. Toute dépendance doit être redistribuable et compatible avec la
relicence du module natif (voir `native-capture-encoder.md` §12).

---

## 2. ⚠️ Ce que l'état de l'art permet réellement

C'est la correction la plus importante du plan initial, qui demandait
« d'étudier en priorité les solutions open source » pour du XInput virtuel.
**L'étude est faite. Il n'y a qu'une réponse.**

| Solution | Licence | XInput ? | HID générique ? | Verdict |
|---|---|---|---|---|
| **ViGEmBus + ViGEmClient** | BSD-3 (pilote) + MIT (client) | ✅ X360 natif | DS4 seulement | **Le seul praticable** |
| libvirtualhid (LizardByte) | lib MIT, **pilote payant** | ❌ (pas de XUSB) | ✅ | **Exclu** : licence par utilisateur |
| vJoy | **GPL** | ❌ (DirectInput) | ✅ | **Exclu** : licence + pas de XInput |
| Pilote UMDF2 maison (VHF) | à nous | ❌ | ✅ | **Coût réel** : signature EV + attestation Microsoft, plusieurs centaines d'euros/an |

### 2.1 ViGEmBus est archivé, et ce n'est pas un problème technique

Retiré le **2 novembre 2023** pour un **conflit de marque** avec ViGEM GmbH —
pas pour un défaut, pas pour une incompatibilité. Le pilote fonctionne sous
Windows 11, il est signé, et DS4Windows comme DualSenseX en dépendent toujours.
Un successeur (*VirtualPad*) est annoncé mais n'est pas sorti.

**Ne pas le remplacer par réflexe.** L'abstraction de §5 contient le changement
à un seul fichier le jour où VirtualPad existe.

### 2.2 ⚠️ Le HID virtuel sous Windows n'est pas gratuit

Le plan initial disait « privilégier un périphérique HID virtuel standard ». Cela
suppose qu'il en existe un accessible en user-mode. **Il n'y en a pas.** Il faut
VHF, donc un pilote UMDF2, donc une signature EV et une attestation Microsoft.
C'est exactement ce que libvirtualhid fait payer.

**Conséquence directe** : les profils DualSense, Switch Pro et Generic HID sont
**hors de portée sans écrire et signer un pilote**. Ils restent dans
l'architecture (§6) mais pas dans le périmètre livrable.

### 2.3 ⚠️ Volants et palonniers : hors périmètre, assumé

Aucune solution gratuite n'expose le nombre d'axes qu'un volant demande. Un vrai
passthrough exige un descripteur HID sur mesure — donc un pilote, donc §2.2.
L'API Gamepad du navigateur les présente de toute façon comme des manettes
génériques à axes nombreux, que ViGEm ne peut pas restituer.

À documenter comme limite, pas à poursuivre.

---

## 3. Principe architectural

Le protocole ne dépend jamais d'un type de manette ni d'un OS.

```text
Manette physique → API Gamepad navigateur → GamepadState
        → DataChannel WebRTC (existant)
        → GamepadManager → ControllerProfile → VirtualGamepadBackend
                                                 ├── Windows (ViGEm)
                                                 ├── Linux   (uinput)
                                                 └── macOS   (voir §9)
```

Cette séparation est obligatoire, et c'est elle qui rend §2 supportable : le
jour où un pilote libre apparaît, seul le backend change.

---

## 4. `GamepadState`

Structure plate, indépendante de tout OS :

```text
controller_id
buttons (bitfield)   dpad
left_stick_x/y       right_stick_x/y
left_trigger         right_trigger
```

Extensible plus tard (touchpad, gyro, accéléromètre, batterie, gâchettes
adaptatives) **sans rien ajouter maintenant** : le protocole initial reste
compact, il est émis très fréquemment.

---

## 5. Trois concepts séparés

| Concept | Répond à |
|---|---|
| `GamepadState` | ce que l'utilisateur fait physiquement |
| `ControllerProfile` | sous quelle forme le host présente la manette à l'OS |
| `VirtualGamepadBackend` | comment le périphérique virtuel est créé et alimenté |

Interface commune :

```text
VirtualGamepad { create(profile) · destroy() · update_state(GamepadState) · send_output_report(...) }
```

Le code MoonlightWeb ne connaît ni les API Windows, ni uinput, ni IOHID, ni les
descripteurs HID.

> **Déjà en place.** `IInputSink` et `VigemGamepad` (commit 9a62329)
> implémentent cette séparation pour le chemin XInput. Ce plan la généralise, il
> ne la crée pas.

---

## 6. Profils

```text
XInput ✅ livrable  ·  DualShock4 ✅ livrable (ViGEm sait le faire)
DualSense ⚠️ pilote  ·  SwitchPro ⚠️ pilote  ·  GenericHID ⚠️ pilote
```

Chaque profil définit : identité, capacités, mapping boutons/axes, format des
reports, descripteur HID si nécessaire, capacités de sortie.

⚠️ **Generic HID n'est pas un repli utilisable** (contrairement au plan
initial) : il exige justement le pilote qu'on n'a pas. Le vrai repli est
**XInput**, qui couvre l'écrasante majorité des jeux Windows.

Une manette inconnue ne doit jamais bloquer la connexion : elle est présentée en
X360, avec ses boutons supplémentaires perdus. C'est une dégradation, pas un
refus.

### 6.1 Ce que XInput ne peut pas porter

Paddles, clic de touchpad, bouton Share/Capture, gyro, gâchettes adaptatives.
Un pad X360 n'a pas ces boutons. Les bits correspondants sont **jetés
sciemment**, comme le fait tout host basé sur XInput.

---

## 7. Détection

`navigator.getGamepads()` : `id`, `buttons`, `axes`, `mapping`, `connected`.

Ne jamais dépendre du seul `gamepad.id` — il varie selon navigateur, OS, pilote,
USB/Bluetooth et version du système. Détection à plusieurs niveaux :
`mapping` → heuristiques sur `id` → capacités observées.

### 7.1 Sélection manuelle — debug uniquement

L'auto-détection est le comportement de production. Un sélecteur manuel de
profil existe **mais n'est visible qu'en mode debug** (`debug_build`, déjà
exposé par `/api/settings`).

Raison : en production le bon comportement est de deviner juste, et un réglage
visible transforme un défaut de détection en question posée à l'utilisateur.
En debug il est indispensable pour isoler « la détection s'est trompée » de
« le profil est mal implémenté ».

---

## 8. Protocole

Le profil est annoncé à la création, jamais répété :

```text
GAMEPAD_CREATE  { controller_id, profile }
GAMEPAD_STATE   { controller_id, buttons, dpad, lx, ly, rx, ry, lt, rt }
GAMEPAD_DESTROY { controller_id }
GAMEPAD_RUMBLE  { controller_id, low_frequency, high_frequency, duration }
```

Versionné (`GAMEPAD_PROTOCOL_VERSION`) pour ajouter gyro/touchpad/gâchettes
adaptatives sans casser les anciens clients.

### 8.1 ⚠️ Encodage binaire : évaluer l'existant d'abord

`streaming/InputMessageCodec.h` et `InputEncoder.cpp` **existent déjà dans
l'arbre** mais sont exclus du build. Ils donnent un encodage binaire compact à
la place du JSON.

Les évaluer et les activer avant d'en écrire un nouveau. Et **mesurer** : le
JSON actuel n'a jamais été montré comme un coût réel sur ce chemin.

### 8.2 Transport

Le DataChannel WebRTC existant. Aucune connexion supplémentaire, aucun serveur
supplémentaire. Le serveur de rendez-vous établit la connexion et ne voit jamais
une donnée de manette.

---

## 9. Backends par plateforme

### Windows — prioritaire

ViGEmBus (§2). Installé silencieusement par l'installeur (déjà fait, commit
9a62329). Son absence dégrade proprement : clavier et souris intacts, pas de
manette, une ligne dans le log.

#### 9.1 Rattraper une installation manquée — l'encart d'installation

L'installeur pose ViGEmBus, mais il peut avoir échoué (réseau coupé pendant le
téléchargement), avoir été contourné (build lancé depuis les sources), ou le
pilote peut avoir été désinstallé depuis. Dans ces cas la manette ne marche pas
et **rien ne le dit** : le log le sait, l'utilisateur non.

D'où un encart — un message discret dans la page, pas une fenêtre modale —
proposant de poser le pilote en un clic, sur la **page principale** et la **page
admin**.

**La garde, qui est le vrai sujet.** Le pilote s'installerait sur la machine du
**service**, pas sur celle du navigateur. Afficher ce bouton à quelqu'un qui
regarde depuis un autre PC, c'est lui proposer de modifier une machine qui n'est
pas devant lui — et il n'aurait aucune raison de le deviner.

La condition est donc **strictement `NetClassify::Kind::Loopback`**
(`backend/src/server/NetClassify.h`), et surtout **pas** `isPrivateOrLoopback()`
ni le drapeau LAN : un autre PC du même réseau est classé `Private` et n'est pas
la bonne machine. Un accès par le rendez-vous ressort en `Tunnel`/`Public`, donc
masqué lui aussi, sans règle supplémentaire à écrire.

Deux corollaires :

- le verdict est **calculé côté serveur** et envoyé comme un booléen déjà
  tranché. Le navigateur ne le déduit pas de son URL — `localhost` dans la barre
  d'adresse ne prouve rien, un tunnel SSH suffit à le fabriquer ;
- la route d'installation **revérifie la même condition** au lieu de faire
  confiance au fait que le bouton n'était pas censé s'afficher.

**Sonder la présence.** La seule réponse fiable est celle du pilote lui-même :
`vigem_connect()` renvoie `VIGEM_ERROR_BUS_NOT_FOUND` quand le bus est absent.
Pas de lecture de registre, pas de recherche de fichier — l'un et l'autre
mentent après une désinstallation partielle. La sonde tourne une fois au
démarrage, et à nouveau après une tentative d'installation.

**Élévation.** En service (SYSTEM), l'installation silencieuse passe directement.
Hors service — instance `--dev`, exécution depuis les sources — le processus
n'est pas élevé : dans ce cas l'encart ne prétend pas installer, il donne le lien
amont. Une invite UAC qui échoue sans explication serait pire que pas de bouton.

**Ce que l'encart n'est pas.** Ni bloquant, ni répété : une manette absente reste
une dégradation propre (§9), pas une erreur. Il se ferme, et il disparaît de
lui-même dès que la sonde voit le bus.

Chaînes en i18n (`frontend/locales/{en,fr,zh}.json`), `data-i18n` ⇒
`textContent`, donc `&` brut jamais `&amp;`.

### Linux

`uinput`, standard et libre. Règle udev à l'installation pour éviter root en
permanence. **C'est la plateforme la plus simple des trois** : uinput fait
nativement ce que Windows demande un pilote signé pour faire, y compris des
descripteurs HID arbitraires — donc les volants (§2.3) y seraient possibles
alors qu'ils ne le sont pas sous Windows.

### macOS

⚠️ **À vérifier avant de s'engager.** macOS a fermé les extensions noyau au
profit de DriverKit, qui exige un *entitlement* accordé par Apple et un compte
développeur payant. Le statut exact d'un HID virtuel gratuit sous macOS 15+ n'a
pas été vérifié pour ce document.

Tant que ce n'est pas tranché : macOS **client** fonctionne (c'est un
navigateur, il envoie des états), macOS **host** peut n'avoir aucune manette.
Documenter la limite plutôt qu'introduire une dépendance propriétaire.

---

## 10. Rumble

Chemin inverse, déjà livré pour XInput/ViGEm (commit 9a62329) :

```text
Jeu → manette virtuelle → host → GAMEPAD_RUMBLE → DataChannel → navigateur
    → GamepadHapticActuator, ou ignoré proprement si absent
```

Attention à l'échelle : le protocole porte des amplitudes 16 bits, XUSB en
rapporte 8. Le facteur est **257** et non 256, pour que 0xFF donne 0xFFFF.

---

## 11. Multi-manettes

Quatre au maximum sous Windows : XInput n'expose que quatre emplacements. Chaque
manette a `controller_id`, `profile`, `state`, `virtual_device`. Le protocole ne
suppose jamais une manette unique.

---

## 12. Cycle de vie

```text
GAMEPAD_CREATE → périphérique créé → GAMEPAD_STATE × N → GAMEPAD_DESTROY
DataChannel perdu → détruire TOUS les périphériques virtuels
```

Aucun périphérique orphelin ne doit survivre à une session : il resterait dans
la liste des manettes de Windows et le jeu suivant verrait un contrôleur que
personne ne tient. À la reconnexion, tout est recréé — aucun état HID obsolète
n'est conservé entre deux sessions.

---

## 13. Sécurité

Valider `controller_id`, `profile`, longueur du message, bitfields, plages des
axes et des gâchettes. Hors limites → borné ou rejeté.

Un client distant ne doit jamais pouvoir provoquer une allocation illimitée de
contrôleurs, une consommation mémoire illimitée, une boucle CPU, ou des reports
HID de taille arbitraire. Le plafond de §11 est une garde de sécurité autant
qu'une limite d'API.

---

## 14. Performance

Chemin critique : pas de JSON si mesuré coûteux (§8.1), pas d'allocation, pas de
verrou par événement. Messages binaires de taille fixe, structures
préallouées, bitfields.

⚠️ **Le vrai gain de latence est ailleurs, et il est identifié** : le relais
marshale chaque message d'entrée vers le thread Qt
(`DataChannelRelay.cpp`, `onMessage` → `Qt::QueuedConnection`) parce que le même
handler pilote presse-papier, politique et statistiques. C'est **un tour de
boucle d'événements** par événement d'entrée. Le sink natif est déjà sûr pour
s'en passer ; le relais ne l'est pas.

Sortir clavier/souris/manette du handler partagé vaut plus que tout
micro-optimisation d'encodage.

---

## 15. Ordre d'implémentation ⚠️ (inversé)

Le plan initial commençait par Generic HID — le plus difficile, celui qui exige
un pilote signé — et mettait XInput en phase 2. C'est l'inverse.

| Phase | Contenu | État |
|---|---|---|
| **1** | `GamepadState`, protocole, `GamepadManager`, **XInput** de bout en bout | ⚡ **partiellement fait** (9a62329) |
| **2** | Multi-manettes, cycle de vie, validation, tests, **encart ViGEmBus (§9.1)** | à faire |
| **3** | DualShock 4 (ViGEm sait le faire) | à faire |
| **4** | Linux / uinput | à faire |
| **5** | macOS — après la vérification de §9 | bloqué |
| **6** | DualSense, Switch Pro, Generic HID | **bloqué sur un pilote signé** |
| **7** | Gyro, touchpad, gâchettes adaptatives | bloqué par phase 6 |

Les phases 6 et 7 ne sont pas un travail à planifier : ce sont des décisions
d'investissement (signer un pilote) à prendre séparément.

---

## 16. Tests

**Sans matériel** : protocole (messages valides, invalides, tronqués, versions
inconnues, `controller_id` hors bornes, valeurs hors plage), mapping par profil
(tous les boutons, D-pad, sticks, gâchettes, min/max/centre, transitions),
cycle de vie.

**Avec matériel** : Windows → détection par une application XInput standard.
Linux → périphérique visible dans le sous-système input. Multi-manettes → 1, 2,
4, en vérifiant que les événements restent associés au bon `controller_id`.

---

## 17. Licences

Documenter dans `docs/gamepad-dependencies.md` : dépendance, version, licence,
usage, dépendance runtime, redistribution autorisée, dépendance commerciale.

État actuel :

| Dépendance | Version | Licence | Commercial | Redistribué |
|---|---|---|---|---|
| ViGEmClient | 1.16.18.0 | **MIT** | non | oui, vendoré |
| ViGEmBus (pilote) | 1.22.0 | **BSD-3** | non | non — installé depuis l'amont |

⚠️ Le plan initial et le document d'architecture notaient ViGEmClient en BSD-3.
**C'est MIT** — vérifié dans le `LICENSE` de l'amont. MIT est strictement plus
simple pour une relicence commerciale.

---

## 18. Règle absolue (inchangée)

Ne jamais introduire une solution propriétaire parce qu'elle est plus facile.
Si une fonctionnalité l'exige : ne pas l'intégrer, chercher une alternative
libre, vérifier sa licence, documenter le compromis, et s'il n'existe rien
d'autre, l'isoler derrière une abstraction en la laissant désactivée.

C'est exactement ce qui a été fait pour libvirtualhid (§2) : identifié,
évalué, écarté sur la licence, et l'abstraction reste prête si son modèle
change.

---

## 19. Critères d'acceptation, corrigés

Réalisables :

- [x] une manette navigateur est détectée et convertie en `GamepadState`
- [x] transmise par le DataChannel WebRTC, reçue par le host
- [x] un périphérique virtuel est créé, boutons/sticks/gâchettes fonctionnent
- [x] rumble de bout en bout
- [ ] plusieurs contrôleurs simultanément
- [ ] ViGEmBus absent : encart proposé **uniquement** en `Loopback`, invisible
      depuis un autre PC du LAN et depuis le rendez-vous (§9.1)
- [ ] la déconnexion détruit les périphériques
- [ ] XInput validé par une application XInput réelle
- [ ] DualShock 4
- [ ] Linux / uinput
- [x] aucun composant commercial, aucune licence payante
- [x] dépendances documentées et compatibles
- [x] aucun serveur gamepad supplémentaire

⚠️ Retirés du périmètre, avec leur raison :

- ~~Generic HID~~ — exige un pilote UMDF2 signé (§2.2)
- ~~DualSense, Switch Pro~~ — idem
- ~~volants et palonniers~~ — descripteur HID sur mesure (§2.3)
- ~~macOS host~~ — statut DriverKit à vérifier (§9)
