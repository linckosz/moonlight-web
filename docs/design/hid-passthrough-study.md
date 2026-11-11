# Passthrough HID — étude de faisabilité (« USB over IP » sans installation côté client)

> Étude du 29/09/2026. **Aucun code.** Elle remplace l'étude « retour de force »
> (ex-T5 du plan radios, fusionnée le même jour à la demande de Bruno).
>
> **Statut au 20/10/2026** : le plan (`flickering-dazzling-feigenbaum.md`) a franchi ses portes P0, P1, P2
> et P4.
> - Windows : le pilote « MoonlightWeb Virtual HID » (WinUHid, MIT, `backend/native-host/third_party/winuhid`
>   et `drivers/vhid`) est posé par l'installeur avec notre propre certificat, sans mode test, Secure Boot
>   actif compris. Pas sur ARM64 (§3.2).
> - Linux : `uhid`, et `uinput` pour un volant à retour de force.
> - Essais de Bruno, sous Windows et sous Linux : le G923 dans ACC, retour de force compris, et la TX12 dans
>   Liftoff.
> - Le volant recréé porte ses propres identifiants PnP (§3.3, 20/10).
> - Reste : un vrai volant HID PID (P3).
> - Écart à la §3.2 : SignPath a refusé MoonlightWeb (02/10), et Bruno ne paie pas de signature (11/10).
>
> Légende : **[V]** vérifié (code Chromium, documentation officielle, dépôt du
> projet cité ou notre propre dépôt) ; **[S]** supposition, à confirmer au banc.

## 0. Verdict

- **« USB over IP » au sens strict, c'est-à-dire transférer les échanges USB bruts
  de n'importe quel appareil : impossible depuis un navigateur sous Windows.**
  Chrome ne donne jamais l'accès USB brut à un périphérique de jeu (§2.1).
- **« HID over IP », c'est-à-dire transférer les rapports HID de l'appareil :
  possible sans rien installer sur le client**, dans Chrome et Edge sur
  ordinateur, grâce à WebHID (§2.2). Il faut un clic, dans le sélecteur de Chrome.
- **Sur l'hôte Windows, un pilote est obligatoire** (§3.1). En revanche, un pilote
  **UMDF2 posé sur VHF** n'a besoin ni de certificat EV ni de Microsoft. Notre
  signature SignPath suffit, comme pour le pilote d'écran virtuel que l'installeur
  pose déjà (§3.2).
- ⚠️ **Correction de fond.** `gamepad-virtual.md` §2.2 et
  `gamepad-dependencies.md` affirmaient qu'un tel pilote coûte « EV + attestation
  Microsoft ». C'est faux pour UMDF. L'exclusion des profils Generic HID, DualSense
  et volants reposait sur cette prémisse.
- **Recommandation : à inscrire à la feuille de route**, comme **complément** du
  mapping (plan radios) et non comme remplaçant. On commence par un POC sans
  pilote sur l'hôte Linux, et on obtient avant tout pilote l'accord de SignPath
  pour signer un catalogue de pilote (§7.4).

## 1. La question posée

Situation : un laptop Windows modeste, Chrome, une session MoonlightWeb vers un
hôte Windows qui fait tourner le jeu. Un périphérique USB est branché sur le
laptop : volant G29/G920, joystick, radio RC…

L'idée : au lieu d'interpréter ce périphérique dans le navigateur (ce que fait le
mapping, périphérique par périphérique), transmettre ses communications à l'hôte.
L'hôte exposerait alors un périphérique virtuel que Windows et le jeu verraient
comme branché localement.

Contrainte fondamentale : **rien à installer sur le client**, qui reste un simple
Chrome. Tout le travail, pilote compris, se fait sur l'hôte, avec MoonlightWeb
pour intermédiaire réseau.

## 2. Ce que le navigateur peut récupérer

### 2.1 WebUSB : une impasse sous Windows

- **Classes protégées [V].** Chrome refuse `claimInterface` sur l'audio (0x01),
  le **HID (0x03)**, le stockage de masse (0x08), la carte à puce (0x0B), la vidéo
  (0x0E), l'audio/vidéo (0x10) et le contrôleur sans fil (0xE0). La réponse est
  `SecurityError`.
- **Contrôles aussi [V].** Depuis 2026, sur un appareil qui a une interface
  protégée, les requêtes de contrôle « class » et « vendor » sont bloquées, et les
  requêtes « standard » limitées à la lecture.
- **Le pilote, surtout [V].** Sous Windows, `open()` échoue si l'appareil n'est
  pas lié à WinUSB (ou au composite `usbccgp`), et `claimInterface` échoue si
  l'interface n'est pas sous WinUSB. Or :
  - volants, joysticks et radios EdgeTX sont liés à `hidusb` ;
  - la manette 360 est liée à `xusb22`, la manette Xbox One au pilote GIP.
  Changer de pilote (Zadig) est une installation, donc exclu.
- **Isochrone [V]** : pas pris en charge sous Windows.
- **Linux et macOS [V/S].** Chrome ne détache pas `usbhid` ni `xpad` sous Linux de
  bureau. Sous macOS, une interface tenue par un pilote du système n'est pas
  réclamable **[S]**.
- **Ce qu'on peut quand même lire [V]** : les descripteurs de périphérique et de
  configuration, lus par Windows à l'énumération (VID, PID, bcdDevice, chaînes,
  interfaces). Mais ni le descripteur HID ni le descripteur de rapport, qui
  exigeraient `open()`.

### 2.2 WebHID : la voie

**Disponibilité [V].** Chrome et Edge sur ordinateur : Windows, macOS, Linux,
ChromeOS. Firefox le juge « harmful » et Safari ne l'a pas. Android est en cours :
annonce en août 2026, sortie visée pour Chrome 157.

**Ce qui est autorisé [V].**
- Autorisées : Joystick (0x01/0x04), Game Pad (0x05), Multi-axis (0x08), la page
  Simulation (0x02), la page PID (0x0F, retour de force), Consumer Control (0x0C).
- Bloqués : la page Keyboard, les collections Pointer, Mouse, Keyboard et Keypad
  (en entrée et en sortie), System Control, FIDO.

**Ce qui est exposé [V].**
- Identité : `vendorId`, `productId`, `productName`. Rien d'autre : ni numéro de
  série, ni bcdDevice, ni numéro d'interface, ni descripteurs USB.
- Structure : `collections`, un arbre de `HIDCollectionInfo` (usagePage, usage,
  type, enfants, rapports d'entrée, de sortie et feature). Chaque `HIDReportItem`
  donne :
  - `isAbsolute`, `isArray`, `isBufferedBytes`, `isConstant`, `isLinear`, `isRange`,
    `isVolatile`, `hasNull`, `hasPreferredState`, `wrap` ;
  - `usages`, `usageMinimum` et `usageMaximum` ;
  - `reportSize`, `reportCount` ;
  - les unités (`unitSystem`, exposants) ;
  - les bornes logiques et physiques ;
  - `strings`.
- ⚠️ **Deux formes d'arbre (source de Chromium, vu le 04/11 avec un volant Moza
  depuis un Mac).** Le parseur de Chrome (macOS, Linux) range chaque champ dans sa
  collection **et dans tous ses ancêtres** : une collection de tête porte chaque
  rapport en entier, dans l'ordre, et ses enfants en répètent des morceaux. Sous
  Windows, Chrome donne une collection plate par collection de tête, sans enfants.
  L'hôte écrit donc une collection de tête depuis ses seules listes quand elles
  contiennent celles de ses enfants (`childRuns()`) ; écrire aussi les enfants
  doublait des champs, et Windows refusait le descripteur (erreur 552). Autre
  perte du parseur de Chrome : une plage d'un seul usage arrive sans usage
  (`repairUsages()`).
- ⚠️ **Le descripteur de rapport brut n'est pas exposé.** Chromium l'a sous Linux
  et macOS (`HidDeviceInfo.report_descriptor`), mais ne le passe pas au web. Sous
  Windows, Chrome ne l'a même pas : il reconstruit l'arbre à partir des données
  « preparsed » de Windows. Le code dit lui-même « inferred and may be wrong or
  incomplete ». Aucune proposition en cours pour l'exposer.
- **Conséquence [S].** On reconstruit un descripteur **fonctionnellement
  équivalent** : mêmes identifiants de rapport, mêmes bits, usages, bornes et
  unités. On perd l'encodage d'origine, Push/Pop, les délimiteurs et les index de
  chaînes. Pour un modèle qui l'exige, une table de descripteurs exacts par
  VID:PID, relevés une fois sur Linux (`hidraw`), peut compléter.

**Flux [V].**
- **Entrée :** l'événement `inputreport` (`reportId` + octets) arrive à chaque
  rapport. Sous Windows, une lecture est toujours en attente ; ce n'est pas un
  relevé périodique.
- **Sortie et feature :** `sendReport`, `sendFeatureReport` et
  `receiveFeatureReport` fonctionnent. Preuves :
  - `nondebug/g29-wheel-demo` pilote le moteur d'une G29 depuis Chrome ;
  - `dualshock-tools` écrit la calibration d'une DualSense en rapports feature.
- **Workers :** WebHID est disponible en Dedicated Worker depuis Chrome 131 sur
  ordinateur. `requestDevice` reste réservé à la fenêtre ; `getDevices` marche
  dans le worker.

**Accès partagé sous Windows.**
- [V] Chrome ouvre l'appareil en partage.
- [S] L'API Gamepad (Raw Input / XInput), G HUB et WebHID peuvent le lire en même
  temps. Les écritures de G HUB (recentrage, retour de force) peuvent entrer en
  conflit avec les nôtres : à tester.

**Permission [V].**
- `requestDevice` exige un geste de l'utilisateur ; dans une iframe, il faut
  `allow="hid"` (à vérifier pour la page chargée dans le shell).
- La permission n'est **mémorisée que si l'appareil a un numéro de série et un
  nom de produit**. Sinon, il faut la redonner à chaque branchement.
- Les stratégies d'entreprise `WebHidAllowDevicesForUrls` et suivantes peuvent la
  pré-accorder, mais seulement sur un Chrome administré.

**Cas particuliers.**
- [S] La manette 360 (`xusb22`) et la manette Xbox One (GIP) ne sont pas des
  périphériques HID : WebHID ne les voit pas.
- [V] Le G920 démarre en mode Xbox (`c261`, classe « vendor ») et doit être
  basculé en mode HID (`c262`), ce que fait G HUB. [S] Sans G HUB sur le client,
  il pourrait rester invisible.

### 2.3 API Gamepad : rien de brut

**[V]** L'API Gamepad ne donne que `id`, les axes normalisés, les boutons,
`touches` et `vibrationActuator` (rumble seulement). Dans Chrome :
- elle est lue toutes les 4 ms ;
- elle est limitée à 16 axes, 32 boutons et 8 manettes ;
- elle n'a aucun retour de force de volant.
C'est la base du mapping (plan radios), pas d'un passthrough.

### 2.4 Ce que font les autres

Aucun produit ne transmet un périphérique de jeu HID depuis un navigateur **[V]** :
- **Citrix Workspace HTML5** : redirection USB générique en WebUSB, en Chrome sur
  ChromeOS, Mac et Linux, **pas sur Windows**. Deux appareils au plus, et « HID…
  might result in low performance ».
- **Horizon HTML Access** : redirection USB avec Chrome ou Edge 87+. La doc dit :
  « many USB devices cannot be redirected ». Seuls des imprimantes et un scanner
  sont testés.
- **Windows App web (AVD / W365)** : pas de redirection USB dans le navigateur.
- **Kasm** : manettes par l'API Gamepad, 4 au plus.
- **Projets libres :** `usbip-browser` fait le sens inverse (le navigateur est
  client USB/IP) ; `webrtc-hid-sdk` de Ribbon se limite aux boutons de casque en VDI.

**MoonlightWeb serait le premier.**

## 3. Recréer l'appareil sur l'hôte

### 3.1 Windows : un pilote est obligatoire

**[V]** Windows n'offre aucun moyen de créer un HID ou un USB virtuel depuis le mode
utilisateur sans pilote :
- **VHF** (`vhf.sys`, intégré à Windows) exige un pilote « source ». Il peut être
  **UMDF2** : `VHF_CONFIG.FileHandle` est documenté « Required for user-mode
  drivers », et WinUHid est lié à `VhfUm.lib`.
- **UDE** (`UdeCx`) exige un pilote **KMDF**.
- Le seul outil intégré, `InputInjector.InitializeGamepadInjection`, crée une
  manette de type Xbox visible seulement par Windows.Gaming.Input. Il exige en
  plus une capacité restreinte.

### 3.2 ⚠️ Ce que coûte la signature : moins que ce que disait le design

- **KMDF** (source VHF ou UDE) **[V]** :
  - avec Secure Boot, seuls les pilotes signés par le portail Microsoft se chargent ;
  - l'attestation exige un certificat EV (environ 280 à 560 $ par an, clé sur jeton
    matériel) et un compte Partner Center ;
  - elle est classée « for testing purposes only » depuis mars 2026 ;
  - la politique d'avril 2026 retire la confiance aux pilotes cross-signés.
  - Conclusion : à éviter.
- **UMDF2** **[V, dans notre dépôt]** : la politique de signature du noyau ne s'y
  applique pas. Une signature Authenticode du catalogue suffit.
  - `backend/installer/drivers/vdd/mttvdd.cat` et `MttVDD.dll` (IddCx, UMDF) sont
    signés « SignPath Foundation », sans Microsoft.
  - L'installeur les pose en ajoutant ce certificat à TrustedPublisher le temps de
    l'installation (`VirtualDisplayApply.cpp:200-225`).
  - HIDMaestro fait de même avec un certificat auto-signé, sans mode test, sur
    Windows 11 build 26200.
  - ⚠️ **Sauf sur Windows sur ARM [V, banc mw-arm du 20/10/2026]**. Le paquet ARM64 du
    pilote 0.1.2.0, signé par notre certificat (mis dans Root et TrustedPublisher
    pendant la pose), est refusé à l'import : `SetupCopyOEMInf` rend 0x800b0109, et
    `setupapi.dev.log` indique « Driver package catalog file certificate does not belong
    to Trusted Root Certificates, and Code Integrity is enforced ». Le même paquet en x64
    passe, Secure Boot actif compris. Sur ARM64, même un pilote UMDF doit être signé par
    Microsoft (WHQL ou attestation). L'ARM64 reste donc sans pilote : le passthrough y est
    indisponible, et l'entrée passe par SendInput. À revoir après la v0.4.1.
- **SignPath Foundation [V]** : certificat OV seulement, gratuit pour les projets
  libres. Pas de pilote noyau, mais le précédent du VDD montre qu'un pilote UMDF
  passe.
- ⚠️ **Point à vérifier en premier** : que SignPath accepte de signer un catalogue
  de pilote pour MoonlightWeb.
- **Si MoonlightWeb devient commercial** (question de
  `gamepad-dependencies.md`) : SignPath Foundation ne sert que les projets libres.
  Il faudrait un certificat OV à nous, payant, mais sans EV ni compte Microsoft.

### 3.3 Options côté Windows

| Option | Licence | Signature | Identité vue par le jeu | Verdict |
|---|---|---|---|---|
| **Pilote maison UMDF2 + VHF**, fork de **WinUHid** (cgutman) | MIT | SignPath (UMDF) | VID/PID, nom, descripteur, `HardwareIDs` au choix | **Retenu** |
| **usbip-win2** + serveur USB/IP synthétique sur 127.0.0.1 | BSD-2 | certifié Microsoft WHLK (x64) | un vrai périphérique USB | En réserve |
| **vJoy** (fork BrunnerInnovation 2.2.x) | **MIT** | EV + attestation | fixe, « vJoy Device » (8 axes, 128 boutons, 4 POV, retour de force) | Repli, pas un passthrough |
| ViGEmBus | BSD-3 | signé | X360 ou DS4 seulement | Hors sujet |
| VirtualPad (Nefarius) | commercial, partenaires | — | — | Exclu |
| libvirtualhid (LizardByte) | pilote sous licence « source-available » payante | — | — | Exclu |

**Pourquoi WinUHid.**
- Il fournit une API C calquée sur `uhid` : `WinUHidCreateDevice` (VID, PID,
  descripteur, `HardwareIDs`) et des événements GET/SET_FEATURE, WRITE_REPORT et
  READ_REPORT.
- C'est exactement ce que le backend doit appeler.
- **[S]** Il n'a pas de release, son dernier push date du 28/05/2025, et il est
  livré avec une signature de test : on le forke et on le signe.
- Un pilote UMDF qui plante ne fait pas d'écran bleu ; il fait tomber son
  processus hôte.

**Les identifiants PnP du volant recréé [V, banc UM790Pro du 20/10/2026].**
- Sans `HardwareIDs`, VHF nomme la source `VHF\HID_DEVICE_SYSTEM_VHF`, et ses
  collections `HID\HID_DEVICE_SYSTEM_VHF&ColNN`.
- VHF ajoute toujours `HID_DEVICE_SYSTEM_VHF` en tête des identifiants donnés :
  `hidvhf.inf`, le seul INF de la source, se lie donc dans tous les cas.
- Le premier identifiant donné devient celui de l'appareil et doit porter un
  énumérateur (`VHF\…`). Sans lui, aucun appareil n'apparaît : c'était le « aucun
  enfant » du 04/10.
- hidclass nomme chaque collection « HID\ », plus ce premier identifiant sans son
  énumérateur, plus « &ColNN ».
- L'hôte donne `VHF\MOONLIGHTWEB_VHID&VID_046D&PID_C26E`, puis
  `VHF\VID_046D&PID_C26E` (`VhidDevice.cpp`). Résultat :
  - les collections s'appellent `HID\MOONLIGHTWEB_VHID&VID_046D&PID_C26E&COL01` à
    `COL03` ;
  - elles se lient à `input.inf` comme avant et gardent tous les anciens
    identifiants ;
  - `HidD_GetAttributes` rend toujours 046D:C26E.
- La chaîne de produit reste « HID VHF Driver » : c'est VHF qui la fournit.
- Essai de Bruno : ACC garde ses commandes sans remappage, retour de force
  compris.
- Au passage, la bibliothèque cliente de WinUHid mesurait mal une liste de deux
  identifiants ou plus : elle lisait au-delà du tampon. Corrigé (`7bc375b6`).

**Pourquoi usbip-win2 reste en réserve.**
- Il donne une identité USB complète : descripteurs de périphérique et de
  configuration, interfaces. Des pilotes constructeurs qui se lient à
  `USB\VID_…&MI_00` pourraient donc s'attacher.
- **[V]** HIDMaestro et VIIPER servent déjà un périphérique synthétique sur le
  loopback par ce biais.
- Mais c'est un pilote **noyau** tiers sur le PC de jeu :
  - redémarrage à l'installation et à la désinstallation ;
  - tous les périphériques USB redémarrent pendant l'installation ;
  - une version passée faisait des écrans bleus ;
  - les notes de la 0.9.8.1 annoncent elles-mêmes des « possible regressions ».
- Surtout, WebHID ne sait pas relayer les requêtes USB propres au constructeur :
  l'identité USB complète ne suffirait pas à G HUB (§5.1).

### 3.4 Linux

- **`/dev/uhid` [V]** suffit, sans pilote :
  - `UHID_CREATE2` accepte un nom, le bus, le VID/PID et un descripteur de
    4096 octets au plus ;
  - flux : `UHID_INPUT2` en entrée, `UHID_OUTPUT` en sortie ;
  - `UHID_GET_REPORT` et `SET_REPORT` attendent une réponse `*_REPLY` ;
  - droits : une règle udev, comme celle de Wolf (`KERNEL=="uhid", MODE="0660",
    GROUP="input", TAG+="uaccess"`), à côté de notre
    `70-moonlightweb-uinput.rules`.
- ⚠️ **Retour de force [V, code du noyau]** :
  - `hid-lg` refuse un appareil qui n'est pas USB (`hid_is_usb`), donc **pas de
    `hid-lg4ff`** pour une G29 créée en `uhid` ;
  - le PID générique (`hid-pidff`) n'est branché que par `usbhid`.
  - La voie propre est `vhci-hcd` (dans le noyau) avec un serveur USB/IP sur le
    loopback, comme VIIPER.

### 3.5 macOS

Impossible, sans changement : CoreHID et DriverKit exigent un entitlement accordé
par Apple (`gamepad-virtual.md` §9), et DriverKit un compte développeur payant.
Décision de Bruno (29/09/2026) : on ne paie rien à Apple, le cas est marginal.

Même franchi, ce mur n'ouvrirait pas grand-chose :
- peu de jeux de simulation (vol, course, drone) sortent sur Mac ;
- une partie d'entre eux lit les manettes par le framework GameController
  d'Apple, qui ne reconnaît qu'une liste de modèles : une radio ou un joystick
  recréés n'y apparaîtraient pas.

Le Mac reste couvert **comme client** : Chrome sur macOS a WebHID.

## 4. Assez générique ?

**Oui pour tout ce qui est HID**, c'est-à-dire l'essentiel des périphériques de
jeu :
- volants, pédaliers, joysticks, HOTAS, palonniers ;
- radios EdgeTX et OpenTX, boîtes de boutons ;
- DualSense et DualShock 4, avec gyroscope, pavé tactile, gâchettes adaptatives et
  lumière (en rapports de sortie). Le jeu voit une vraie DualSense.

Aucun mapping par appareil : l'hôte recrée l'appareil tel quel.

**Non couverts :**
- la manette Xbox (360 et One), qui n'est pas HID sous Windows. Elle reste sur
  l'API Gamepad, où elle est standard et déjà bien servie ;
- le G920 en mode Xbox (§2.2) ;
- tout ce qui n'est pas HID : audio, webcam, stockage, adaptateurs série ;
- les haptiques de la DualSense, qui passent par un flux audio isochrone.

## 5. Limites importantes

### 5.1 Types de transfert

- Seuls passent les **rapports HID** : les points de terminaison interrupt, et les
  GET/SET_REPORT sur le point de contrôle.
- **Ni bulk, ni isochrone, ni requêtes propres au constructeur.** Les logiciels qui
  en dépendent ne marcheront pas sur l'hôte : G HUB (qui installe aussi une
  interface WinUSB `LGHUBWinUSB`), les utilitaires Fanatec ou Moza, les mises à
  jour de firmware.

### 5.2 Retour de force

- **Volants HID PID**, la voie générique : Moza, Simagic et beaucoup de bases
  direct drive.
  - **[V]** Le `pid.dll` de Windows fournit le retour de force DirectInput à un
    appareil virtuel si le descripteur contient **tout** le bloc PID (HIDMaestro :
    « The complete Output report set is required »).
  - **[S]** La création d'effet (Create New Effect puis Block Load) est un
    GET_FEATURE synchrone pour le jeu, donc un aller-retour réseau par effet créé.
    Les jeux créent surtout leurs effets au chargement, puis les mettent à jour par
    des rapports de sortie asynchrones. À mesurer ; au besoin, on émule le pool
    PID sur l'hôte avec une table de correspondance des blocs.
  - ⚠️ Bruno n'a pas de volant PID pour le banc.
- **Logitech (G29) et Thrustmaster**, protocoles propriétaires.
  - **[V]** Le retour de force d'une G29 passe par le pilote de G HUB. Avec
    VirtualHere, qui transmet l'USB brut, G HUB voit la G29 ; chez nous, les
    requêtes du constructeur ne passeraient pas.
  - La voie qui reste, **[V]** comme `g27-ffb-driverless` : enregistrer notre propre
    `IDirectInputEffectDriver` (clé `OEMForceFeedback`), qui traduit les effets en
    commandes lg4ff de 7 octets, renvoyées au client par `sendReport`.
  - **C'est un adaptateur par marque**, justement ce que la piste voulait éviter.
    À garder en option.

### 5.3 Latence

- **Meilleure que le mapping.** Chaque rapport part à son arrivée, alors que l'API
  Gamepad est relevée à 250 Hz (T3 du plan radios, faite le 01/10/2026 ; avant,
  au rythme des images).
- **Volume :** 500 à 1000 rapports/s pour un volant direct drive **[S]**, soit
  environ 40 Ko/s.
- **Lecture :** dans un worker WebHID, pour échapper au fil principal. **[S]** Si
  Chrome ne sait pas transférer le `RTCDataChannel` au worker, un `MessagePort`
  vers le fil principal fait le relais.
- **Canal :** un nouveau canal `hid`, binaire.
  - Rapports d'état **absolus** : non fiable et non ordonné, où le dernier gagne,
    avec un numéro de séquence et une répétition lente contre les pertes.
  - Rapports à tableaux ou relatifs, sorties et features : canal fiable.
- **Coût côté hôte :** une remise de rapport à VHF ou `uhid` par message, sans
  file intermédiaire.

### 5.4 Portée

- Chrome ou Edge sur ordinateur, et **hôte natif seulement**. GameStream (Sunshine,
  Wolf, Apollo) n'a aucun canal pour ça.
- Une permission par appareil, à redonner à chaque branchement s'il n'a pas de
  numéro de série (§2.2). À vérifier pour la TX12 et la G29.

### 5.5 Sécurité

L'hôte crée un appareil système à partir de données réseau. Il faut donc :
- n'accepter que les usages de jeu : Joystick, Game Pad, Multi-axis, Simulation,
  PID. Consumer Control, bien qu'autorisé par WebHID, est refusé ici : ce serait
  des touches multimédia injectées ;
- borner la taille du descripteur (4096 octets, comme `uhid`), le nombre de
  rapports et le nombre d'appareils ;
- réserver la fonction au propriétaire, et aux invités seulement si la politique
  du lien de partage le permet (`InputPolicy`) ;
- détruire l'appareil à la fin de la session.

**Chien de garde.** Un lien muet 3 s remet aujourd'hui la manette au repos
(`InputWatchdog`). Pour un appareil quelconque, le repos se déduit des collections :
boutons à 0, chapeau à sa valeur nulle, axes absolus au centre logique. Les
pédales et les gaz font exception : pour eux, garder le dernier état est plus
sûr **[S]**.

### 5.6 Double entrée

Un appareil transmis en HID doit être exclu du `GamepadManager` (par son VID:PID).
Sinon, le jeu reçoit une manette Xbox **et** le vrai volant.

### 5.7 Anti-triche

**[V]** Un appareil virtuel est détectable par un anti-triche noyau ; HIDMaestro
le dit lui-même. ViGEm est déjà dans ce cas. **[S]** Certains titres refusent les
pilotes de remappage connus.

## 6. Comparaison avec le mapping prévu (plan radios)

| | Mapping (Gamepad API → Xbox 360) | Passthrough HID (WebHID → HID virtuel) |
|---|---|---|
| Navigateurs | tous, mobiles compris | Chrome et Edge sur ordinateur (Android plus tard) |
| Hôtes | natif, Sunshine, Wolf, Apollo | natif Windows et Linux ; pas macOS |
| Pilote sur l'hôte | ViGEmBus, déjà posé | pilote UMDF maison signé SignPath ; `uhid` sous Linux |
| Axes et boutons | 4 + 2 axes, 15 boutons | tous |
| Ce que voit le jeu | une Xbox 360 | le vrai modèle (VID/PID, nom, descripteur) |
| Profils automatiques des jeux | le profil manette | le profil de l'appareil (ACC reconnaît une G29) **[S]** |
| Travail par appareil | un profil intégré ou l'assistant | aucun, sauf retour de force propriétaire |
| Retour de force | non | PID : oui ; Logitech : adaptateur par marque |
| Latence d'entrée | relevé à 250 Hz | à chaque rapport |
| Permission | aucune | le sélecteur de Chrome |
| Coût | petit, déjà planifié | gros : pilote, protocole, installeur |

**Les deux se complètent.** Le mapping reste la voie universelle : Safari,
mobiles, hôtes GameStream, sans nouveau pilote. Le plan radios garde donc toute
sa valeur. Le passthrough est la voie fidèle, pour Chrome sur ordinateur face à
l'hôte natif.

## 7. Architecture proposée

### 7.1 Client

- **Panneau « Transmettre un périphérique »** dans Réglages → Manettes, et dans le
  menu du flux. Il appelle `navigator.hid.requestDevice({ filters })`, avec pour
  filtres Joystick, Game Pad, Multi-axis et la page Simulation.
- **Un worker HID** (Chrome 131 et plus) qui ouvre l'appareil (`getDevices`) et lit
  les `inputreport`.
- **À l'ouverture**, message `hidattach { slot, vid, pid, name, collections }` sur
  le canal fiable.
- **Rapports**, sur le canal binaire `hid` : `[slot, reportId, seq, octets]`.
- **Retour de l'hôte**, exécuté puis acquitté :
  - `hidout { slot, reportId, data }` → `sendReport` ;
  - `hidsetfeature` → `sendFeatureReport` ;
  - `hidgetfeature { slot, reportId, req }` → `receiveFeatureReport`, puis la
    réponse repart.
- **Le `GamepadManager` exclut** le VID:PID transmis (§5.6).

### 7.2 Backend

- **Relais :** `DataChannelRelay` jette aujourd'hui tout message binaire
  (`DataChannelRelay.cpp:980`). Il faut créer le canal `hid` et son gestionnaire,
  à côté du canal `input` (JSON, fiable, ordonné, id 2).
- **Descripteur :** un encodeur « collections → descripteur de rapport », testé
  dans les deux sens contre des descripteurs réels (TX12, G29, DualSense). En
  option, la table de descripteurs exacts par VID:PID.
- **Hôte natif :** une interface `IVirtualHid` à côté d'`IInputSink`, avec
  `create(descripteur, identité)`, `input(slot, octets)` et les rappels
  sortie/feature. Pourquoi une interface à part : `InputEvent` est une structure
  plate de type XInput (`InputEvent.h:37-150`), sans place pour un rapport brut.
  Cette interface vit dans le worker, qui tourne en utilisateur de la console
  (ou en SYSTEM, selon le mode). Le périphérique de contrôle du pilote doit donc
  autoriser cet utilisateur.

### 7.3 Hôte

- **Windows :** le pilote « MoonlightWeb Virtual HID » (fork de WinUHid).
  - Il est posé par l'installeur comme le VDD : SetupAPI, certificat SignPath dans
    TrustedPublisher pendant l'installation, empreintes SHA-256 épinglées comme
    dans `VirtualDisplay.h`.
  - Son absence n'est pas une erreur : la fonction est alors grisée, avec un encart
    comme celui de ViGEmBus.
- **Linux :** `/dev/uhid` pour les entrées. Pour le retour de force, `vhci-hcd` et
  un serveur USB/IP sur le loopback.
- **macOS :** non.

### 7.4 Étapes, portes et effort

1. **P0 — POC sans pilote** (`high`).
   - WebHID lit la TX12 et la G29 dans Chrome sur DualRTX : collections, rapports,
     cadence réelle, présence d'un numéro de série (pour la permission).
   - Le descripteur reconstruit est posé en `uhid` sur l'UM790Pro, puis comparé au
     vrai (`hid-decode`) et essayé avec `evtest` et un testeur de manette.
2. **Porte P0.** Deux conditions :
   - la reconstruction est fidèle, au moins sur la TX12 et la G29 ;
   - **SignPath accepte de signer un catalogue de pilote** pour MoonlightWeb.
     Sinon, décision de Bruno : certificat auto-signé posé dans TrustedPublisher
     (comme HIDMaestro), ou certificat OV payant.
3. **P1 — Pilote Windows** (`max`) : fork de WinUHid, signature, installeur.
   - La TX12 clonée apparaît dans `joy.cpl`, puis vole dans Liftoff.
   - La G29 clonée roule dans ACC.
4. **P2 — Produit** (`high`) : panneau, canal `hid`, exclusion dans l'API Gamepad,
   invités, chien de garde, documentation.
5. **P3 — Retour de force PID** (`xhigh`) : il faut un volant PID pour le banc.
6. **P4, option — Adaptateur Logitech** (`xhigh`) : pilote d'effets DirectInput,
   puis commandes lg4ff jusqu'à la G29 du client.

## 8. Licences (la question de `gamepad-dependencies.md`)

| Brique | Licence | Usage prévu | Commercial |
|---|---|---|---|
| WinUHid | MIT | base du pilote, forkée | libre |
| usbip-win2 | BSD-2 | en réserve, installé depuis l'amont | libre |
| vJoy | **MIT** (et non GPL, comme l'écrivait `gamepad-dependencies.md`) | repli éventuel | libre |
| HIDMaestro | MIT | lecture seulement (SDK en C#/.NET) | — |
| VIIPER | cœur **GPL-3.0**, clients MIT | lecture seulement, jamais lié | incompatible avec la relicence du module natif |
| g27-ffb-driverless | à vérifier | modèle pour P4 | à vérifier |
| libvirtualhid | pilote payant | exclu | non |
| Signature SignPath Foundation | gratuite, projets libres | catalogue du pilote | un certificat OV à nous |

## 9. Recommandation

**Oui, à inscrire à la feuille de route,** comme plan à part après le plan radios
(ordre à confirmer par Bruno), en commençant par P0 et sa porte.
- Le mapping n'est pas remplacé.
- Le retour de force générique (PID) vient avec P3 ; celui des Logitech reste une
  option, par marque.

## 10. Sources

Navigateur :
- WebUSB, spec et liste de blocage : https://raw.githubusercontent.com/WICG/webusb/main/index.bs ·
  https://raw.githubusercontent.com/WICG/webusb/main/blocklist.txt
- Chromium WebUSB : https://github.com/chromium/chromium/blob/main/content/browser/usb/web_usb_service_impl.cc ·
  https://github.com/chromium/chromium/blob/main/services/device/usb/usb_device_handle_win.cc ·
  https://github.com/chromium/chromium/blob/main/services/device/usb/usb_interface_detach_allowlist.cc
- WebHID, spec, explainer et liste de blocage : https://wicg.github.io/webhid/ ·
  https://github.com/WICG/webhid/blob/main/EXPLAINER.md ·
  https://github.com/WICG/webhid/blob/main/blocklist.txt
- Chromium WebHID : https://github.com/chromium/chromium/blob/main/services/device/public/cpp/hid/hid_blocklist.cc ·
  https://github.com/chromium/chromium/blob/main/services/device/hid/hid_service_win.cc ·
  https://github.com/chromium/chromium/blob/main/services/device/hid/hid_preparsed_data.cc ·
  https://github.com/chromium/chromium/blob/main/chrome/browser/hid/hid_chooser_context.cc
- Chromium Gamepad : https://github.com/chromium/chromium/blob/main/device/gamepad/gamepad_provider.cc
- Positions et annonces : https://github.com/mozilla/standards-positions/issues/459 ·
  https://github.com/WebKit/standards-positions/issues/510 ·
  https://groups.google.com/a/chromium.org/g/blink-dev/c/0fsOSvHrxf4
- Démos et outils : https://github.com/nondebug/g29-wheel-demo ·
  https://github.com/dualshock-tools/dualshock-tools.github.io
- Produits : https://docs.citrix.com/en-us/citrix-workspace-app-for-html5/peripherals.html ·
  https://docs.omnissa.com/bundle/HorizonHTMLAccessGuideV2406/page/UseUSBDevicesinaRemoteDesktop.html ·
  https://learn.microsoft.com/en-us/windows-app/compare-platforms-features ·
  https://docs.kasm.com/docs/latest/guide/gamepad_passthrough/index.html ·
  https://github.com/beriberikix/usbip-browser

Hôte :
- VHF, UDE, injection : https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/virtual-hid-framework--vhf- ·
  https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/vhf/ns-vhf-_vhf_config ·
  https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/developing-windows-drivers-for-emulated-usb-host-controllers-and-devices ·
  https://learn.microsoft.com/en-us/uwp/api/windows.ui.input.preview.injection.inputinjector.initializegamepadinjection
- Signature : https://learn.microsoft.com/en-us/windows-hardware/drivers/install/kernel-mode-code-signing-policy--windows-vista-and-later- ·
  https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/driver-signing-offerings ·
  https://www.magicsword.io/blog/microsoft-is-killing-cross-signed-drivers ·
  https://signpath.org/terms.html · https://github.com/OSSign
- USB/IP : https://docs.kernel.org/usb/usbip_protocol.html · https://github.com/vadimgrn/usbip-win2
- Périphériques virtuels : https://github.com/cgutman/WinUHid · https://github.com/hifihedgehog/HIDMaestro ·
  https://github.com/Alia5/VIIPER · https://github.com/BrunnerInnovation/vJoy/releases ·
  https://docs.nefarius.at/projects/VirtualPad/ ·
  https://docs.lizardbyte.dev/projects/libvirtualhid/latest/md_docs_2windows-driver.html
- Linux : https://docs.kernel.org/hid/uhid.html
- Retour de force : https://github.com/gcampa/g27-ffb-driverless · https://www.virtualhere.com/node/3454
