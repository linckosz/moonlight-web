# ICE-TCP passif — un chemin pour les réseaux qui ne laissent sortir aucun UDP

> Une école, un bureau ou un Wi-Fi d'invités qui bloque l'UDP sortant ne laisse aujourd'hui aucun
> chemin par le lien d'entrée : le tunnel et chaque rung WebRTC ont besoin d'ICE, et notre ICE-TCP
> ne peut former aucune paire avec un navigateur. Ce document décrit l'écoute TCP « passive » que
> l'hôte ajoute à libjuice pour combler ce trou, et sa sécurité, écrite avant le code.
> Go de Bruno le 23/10/2026 (« fais-le »), avec cette condition : « ne pas ouvrir une plus grande
> surface d'attaque ; si la sécurité est en régression, la proposer en booléen dans settings.json ».

## 1. Le constat, corrigé

**Le 22/10**, on a lu ceci : libjuice 1.7.1 ne fait que le TCP *actif* (il appelle, il n'écoute
jamais), et Chrome ne propose aucun candidat TCP. Actif contre rien, aucune paire. La première
moitié est juste. La seconde est **fausse** :

**Test 1 du 23/10, 09:26.** Le montage : Chrome 155, profil par défaut, sans fenêtre, à qui l'on a
donné à la main des candidats distants `tcp … typ host tcptype passive` vers deux écouteurs
(scripts : `scratchpad\ptcp\` de la session 271382d3).
- Chrome **appelle** : la connexion TCP part dans la seconde qui suit `addIceCandidate`, vers un
  autre hôte (WSL) comme vers 127.0.0.1.
- Suivent des STUN Binding Request en cadrage RFC 4571 (2 octets de longueur, puis 100 octets),
  USERNAME `distant:local`, environ 8 par seconde.
- Chrome a bien des candidats locaux `tcp/host/active :9`, visibles dans `getStats`, mais il ne les
  signale **jamais** dans `onicecandidate`. C'est ce qui faisait croire qu'il « n'a pas de TCP ».
- libwebrtc (`VerifyCandidate`) refuse un candidat distant sous 1024, sauf 80 et 443, et refuse 80
  et 443 sur une adresse privée. Un port haut passe partout.

Donc il suffit que l'hôte **annonce un candidat passif et écoute derrière**. Le navigateur n'a rien
à changer.

## 2. Ce qui change

**Patch libjuice** `third_party/patches/libjuice-ice-tcp-passive.patch` (mode `poll` seulement,
celui de libdatachannel ; `mux` et `thread` l'ignorent) :
- un mode `JUICE_ICE_TCP_MODE_ACTIVE_PASSIVE` : le TCP actif d'avant, plus une écoute ;
- à la création de l'agent, un socket TCP en écoute sur **la même adresse et le même port** que son
  socket UDP ; s'il ne peut pas s'y lier, pas de candidat passif, et rien d'autre ne change ;
- un candidat hôte `tcptype passive` par famille d'adresses, sur ce port ;
- une connexion acceptée n'est rattachée à l'agent qu'après la porte du §3.2. Elle devient alors
  son unique connexion TCP. Le candidat distant prflx qu'elle crée est TCP (`tcptype active`), et
  la paire qu'il forme est permise en mode passif ;
- `TCP_NODELAY` sur la connexion rattachée : le flux y passe, image par image.

**Patch libdatachannel** `third_party/patches/libdatachannel-ice-tcp-passive.patch` :
`rtc::Configuration::enableIceTcpPassive` (faux par défaut), lu seulement avec `enableIceTcp`.

**Serveur** :
- le réglage `ice_tcp_passive` (§3.6), transmis au worker comme `packet_marking` ;
- il vaut pour les rungs `-tcp` du flux (`SignalingServer::buildIceConfig`) et pour le tunnel
  (`ControlTunnel`), puisque c'est le tunnel qui échoue d'abord quand l'UDP est bloqué ;
- le candidat passif est réécrit vers l'IP publique et le port externe, comme le candidat UDP. Le
  trou TCP existe déjà : `RouterPortCore` mappe le port en TCP aussi, au même numéro que l'UDP.
  Pour le tunnel, qui ne réécrit que l'UDP, on ajoute un seul candidat TCP passif public.

## 3. Sécurité

### 3.1 Comparé à l'état actuel

| | Aujourd'hui | Avec l'écoute passive |
|---|---|---|
| Port TCP de la session (média ou tunnel) | mappé par UPnP sur le routeur, **personne derrière** : le SYN est refusé, ou jeté par le pare-feu de l'hôte | une écoute derrière, pendant la vérification ICE de cette session seulement |
| Code exposé avant authentification | sur le port UDP : `stun_read` + HMAC (MESSAGE-INTEGRITY) pour tout paquet | sur le port TCP : `accept`, la lecture d'**une** trame RFC 4571 de 1 Kio au plus, puis **le même** `stun_read` + HMAC. Aucun nouveau parseur |
| Ressources qu'un inconnu peut tenir | aucune côté TCP | au plus 4 connexions en attente par agent, 2 par source, 2 s chacune, la plus ancienne cédant sa place |
| Après authentification | DTLS (empreinte liée par MW-BIND-v1), puis SCTP/SRTP | identique : la connexion TCP n'est qu'un autre tuyau pour les mêmes octets |

### 3.2 La porte, avant toute allocation

Une connexion acceptée n'a rien d'autre qu'une entrée dans une petite table d'attente (socket,
adresse, échéance, tampon de 1 Kio fixe, dans la structure de l'agent : pas de `malloc` par
connexion). Rien d'autre ne lui est alloué : pas de candidat, pas de paire, pas d'entrée STUN.
Elle est **fermée** dès que l'une de ces conditions tombe :
- la longueur annoncée de la première trame est < 20 octets (un en-tête STUN) ou > 1024 ;
- la trame n'est pas un message STUN (`is_stun_datagram`), ou `stun_read` la refuse ;
- ce n'est pas un **Binding Request** avec MESSAGE-INTEGRITY ;
- `agent_verify_stun_binding` la refuse : le USERNAME doit commencer par notre ufrag local, et le
  HMAC doit être bon avec notre mot de passe ICE local (22 caractères base64, soit 132 bits
  aléatoires par agent, qui ne
  voyagent que dans la description signalée au navigateur, donc dans le tunnel ou le canal de
  signalisation authentifié) ;
- l'agent n'est pas en vérification (`CONNECTING` ou `CONNECTED`).

Ce n'est qu'une fois la porte passée que la trame entre dans `agent_input`, exactement comme un
paquet UDP. Elle y crée alors le candidat prflx et la paire, et l'agent répond.

### 3.3 Bornes

- **Première trame en 2 s** après l'`accept`, sinon fermeture (slowloris). Un navigateur l'envoie
  dans le même aller-retour que la poignée de main.
- **4 connexions en attente** au plus par agent, et **2 par source** (une adresse IPv4, un /64
  IPv6, le bloc qu'un abonné reçoit entier) : la 3ᵉ d'une même source prend la place de sa plus
  ancienne. Liste pleine, la nouvelle prend la place de la **plus ancienne**, quoi qu'elle ait
  envoyé : aucune n'est authentifiée, et un navigateur envoie toute sa trame dans l'aller-retour de
  la poignée de main. Qui remplit les places, même d'un octet chacune, ne le tient plus dehors
  (revue de sécurité du 26/10, T2 ; d'abord seules les connexions muettes cédaient leur place, et
  deux adresses qui envoyaient un octet tenaient les quatre).
- Trames suivantes, une fois rattachée : le cadrage de libjuice tel qu'il est pour le TCP actif,
  soit 2 048 octets gardés au plus par trame et le reste jeté.
- **Une seule connexion rattachée par agent.** La première authentifiée gagne. Dès qu'elle l'est,
  l'écoute et toutes les connexions en attente sont fermées.

### 3.4 Durée de vie

L'écoute naît avec l'agent (début de la collecte des candidats) et se ferme au premier de ces
événements :
- une connexion TCP est rattachée ;
- ICE aboutit (`COMPLETED`), quel que soit le chemin : si l'UDP passe, l'écoute disparaît en
  quelques centaines de millisecondes ;
- ICE échoue (`FAILED`, au plus tard au bout des 39,5 s de `ICE_PAC_TIMEOUT`) ;
- la connexion de l'agent est finie (erreur de socket), même si l'agent vit encore (26/10, T4) ;
- l'agent est détruit (fin de session ou rung suivant).

Hors d'une session en cours de vérification, **aucun port TCP n'écoute**. C'est plus strict que
le trou UPnP, qui reste mappé pendant toute la vie du slot.

### 3.5 Après la porte

Une fois la connexion rattachée, le reste suit exactement les mêmes contrôles que l'UDP :
- le contrôle de consentement (RFC 7675) ;
- DTLS, dont l'empreinte est vérifiée contre celle que MW-BIND-v1 a liée ;
- l'authentification de session : PIN ou invitation.

Un tiers qui ne connaît pas le mot de passe ICE ne passe pas la porte, et un tiers qui le connaît
tient déjà le canal de signalisation.

### 3.6 Le reste, et le booléen

**Il reste une augmentation de la surface d'attaque, petite mais réelle.** Pendant la vérification
ICE, une écoute TCP joignable depuis Internet par le trou déjà mappé. Avant authentification, elle
expose une lecture de 1 Kio et un parseur STUN déjà exposé en UDP. Le port n'est pas un secret : il
vient de petites plages publiées (§4). Occuper les 4 places d'attente pour tenir le navigateur
dehors ne marche plus depuis le 26/10 (§3.3) : chaque nouvelle connexion chasse la plus ancienne,
si bien qu'il faudrait arriver entre la poignée de main du navigateur et sa trame, un aller-retour,
et recommencer à chaque essai.

La règle de Bruno s'appliquait donc : **`ice_tcp_passive`** dans `settings.json`, désactivé par
défaut et sans interface, jusqu'à une mesure sur un vrai réseau. **Activé par défaut depuis la
v0.4.1**, décision de Bruno du 26/10 après le test depuis son bureau (§5, étape 6) et les
correctifs de la revue de sécurité (T1 à T4, §3.3, §3.4, §3.7, §4). Aucune version publiée
avant la v0.4.1 n'écrivait la clé : un `false` dans le fichier est donc un choix, et il garde
tous les ports TCP fermés.

Pour un banc depuis un réseau qui laisse sortir l'UDP, une clé de débogage, **`debug_ice_drop_udp`**,
tapée à la main dans `settings.json` : désactivée par défaut, jamais écrite par le serveur, sans API
ni interface. Avec elle, aucun candidat UDP ne passe, ni les nôtres vers le navigateur ni les siens
vers notre agent, sur le flux comme sur le tunnel du lien d'entrée, et les barreaux UDP quittent
la chaîne. Il ne reste que la paire où le navigateur appelle notre candidat passif
(`streaming/IceUdpDrop.h`). Elle prouve le chemin de l'hôte, pas la sonde STUN du navigateur.

### 3.7 Défauts du chemin TCP de libjuice, corrigés dans le même patch

Notre ICE-TCP ne se connectait jamais à un navigateur, donc le chemin TCP de libjuice n'avait
jamais servi chez nous. Une fois rendu vivant, ASan et UBSan l'ont pris en défaut sous Linux (WSL,
23/10). Ces défauts sont corrigés dans le même patch :
- **Le fil de poll vidait une trame en attente sans le verrou d'envoi**, pendant qu'un fil
  applicatif (`juice_send`) écrivait le même contexte. Les octets s'entrelaçaient sur le fil, ce
  qui casse définitivement le cadrage RFC 4571 : une lecture hors limites sous ASan, et une
  donnée perdue une fois sur trente.
- La fermeture de la connexion se faisait aussi sans ce verrou.
- **Une trame à moitié écrite (tampon du noyau plein)** était signalée comme une erreur, avec
  l'errno de `send()` (-1) pris pour un code. Le poll n'était pas réveillé pour attendre POLLOUT :
  la fin de la trame attendait le prochain réveil.
- **Une connexion ouverte pendant le tour de poll** (TCP actif) faisait lire une case au-delà du
  tableau de `poll()`. Le traitement suit maintenant ce que la préparation a mis dans le tableau.
- **Un candidat « tcptype active » annoncé dans la description** (26/10, revue de sécurité, T1)
  faisait une paire TCP que personne n'appelle, et prenait l'unique place TCP de l'agent : la
  connexion qui arrivait ensuite n'avait plus de paire, et le minuteur d'échec de 39,5 s ne
  tournait plus. Seul un actif **peer-reflexive**, né de la requête reçue sur la connexion, fait
  maintenant une paire.
- **Socket plein, datagrammes jetés** (24/10) : libjuice gardait un datagramme de côté quand le
  socket TCP bloquait, et jetait les suivants jusqu'à ce qu'il parte. Sur un long aller-retour,
  SCTP perdait un datagramme à chaque remplissage et attendait sa retransmission. Ils attendent
  maintenant dans une file bornée à 256 Ko, cadrés et dans l'ordre, et partent sur POLLOUT.
  Au-delà, on jette encore, comme l'UDP, plutôt que de laisser grandir un retard que le contrôle
  de congestion du flux ne voit pas.

Reste, en amont et hors du patch :
- le parseur STUN lit des `uint32_t` non alignés (UBSan), sans effet sur x86-64 et ARM64 ;
- un échec d'intégrité STUN logue le mot de passe ICE local en WARN. Chez nous, ces lignes ne
  sortent qu'en mode verbeux (`RunFlags::applyVerboseLogging`).

## 4. Ports et pare-feu

- **Windows** : la règle de l'installeur porte sur le programme, tous protocoles : le TCP entrant
  passe sans autre règle. Une `--dev` est couverte par `scripts/dev-firewall-allow.ps1`, qui ouvre
  48550-48573 en TCP et en UDP.
- **Linux** (D3, v0.4.2) : le `.deb`, le `.rpm` et le paquet AUR ouvrent désormais dans ufw et
  firewalld, en plus de `48550:48573/udp`, le même bloc en TCP et les ports du tunnel en TCP
  (`3478:3481`, `5349:5352`, `46000:46031`). L'édition DEV ouvre son propre bloc de tunnel,
  `46064:46095/tcp`. Le README de Docker et `install.sh` donnent les mêmes lignes, puisqu'un
  conteneur ne touche pas au pare-feu de sa machine. Un pare-feu à états laisse revenir une
  réponse UDP, jamais un SYN venu de l'extérieur : sans ces règles, un hôte Linux au pare-feu
  actif refuse la connexion TCP. Rien n'écoute sur ces ports entre deux connexions, et pendant
  l'une d'elles seul un contrôle signé pour elle est admis (§3).
  - Limite : sans UPnP, le tunnel prend un port éphémère, qu'aucune règle ne peut ouvrir d'avance.
- **macOS** : le pare-feu applicatif porte sur le programme.
- **Le trou TCP du routeur** suit maintenant la règle du trou UDP (26/10, revue de sécurité, T3).
  On ne l'écrit, ne le renouvelle et ne le retire que s'il est libre ou à nous. Avant, une
  redirection TCP d'un voisin sur le même numéro était repointée vers nous, ou retirée puis
  remplacée par la nôtre sur un routeur qui garde l'ancien bail (`RouterPortCore::tcpWritable`).
- **Le port 443** : le seul que certaines écoles laissent sortir. Sur un même hôte, il sert déjà le
  HTTPS. Le partager imposerait de trier les connexions dans le listener (ClientHello TLS `0x16`
  contre une longueur RFC 4571 suivie d'un en-tête STUN). Ce sera en dernier, et seulement si les
  ports hauts sont bloqués là où ça compte.

## 5. Tests et étapes

1. ✅ **Test 1** (23/10, 09:26) : Chrome appelle un candidat passif (§1).
2. ✅ **Patch et tests unitaires** (23/10) : `backend/tests/test_ice_tcp_passive.c`, cible
   `run_juice_tests`, lancée par `run_coverage.bat` après `run_tests`. Deux agents libjuice dans
   le même processus, sur 127.0.0.1 seulement (aucune alerte du pare-feu).
   - Le premier écoute (`ACTIVE_PASSIVE`). Le second ne reçoit que ses candidats TCP passifs, et
     appelle en TCP actif.
   - On attend qu'ils se connectent et échangent des données sans aucun UDP.
   - Le test choisit d'avance un port libre en TCP et en UDP pour l'agent qui écoute. Laissé au
     système, le port UDP peut être refusé en TCP : Windows réserve des blocs de 100 ports ou plus
     à un seul protocole (`netsh int ipv4 show excludedportrange protocol=tcp`, erreur 10013) et
     distribue les ports UDP en séquence, donc l'agent suivant retombe dans le même bloc. Le test
     échouait ainsi 4 fois sur 60 (03/11). En production le port est épinglé dans 48550-48573, hors
     de la plage dynamique, sauf si tout le bloc est pris.
   - Tests de la porte : sans trame, la connexion est fermée en 2 s ; une longueur hors bornes, un
     message qui n'est pas du STUN, un mauvais ufrag ou un mauvais HMAC la ferment ; la 5ᵉ
     connexion en attente aussi ; après `COMPLETED`, l'écoute refuse.
   - Passés sous Windows (MSVC, 10 sur 10) et sous Linux (gcc `-Wall -Wextra`, ASan, UBSan,
     ThreadSanitizer, en série).
3. ✅ **Banc** (23/10, 10:12) : une `--dev`, sur un créneau de 59, un Chrome du WSL dont l'UDP
   sortant est bloqué par nft (et non par une stratégie Chrome, qui retire tous les candidats). Le
   tunnel puis le flux passent en TCP passif, à la place de `noRouteError`.
4. ✅ **Mesure depuis Internet** (23/10, 22:44) : un Chromium dans un conteneur jetable sur la VM de
   prod, UDP sortant jeté. Trois passes sur trois avec image ; la passe témoin, réglage à false,
   échoue comme prévu. Le partage du 443 attend un réseau qui l'exige.
5. **La première visite** (24/10) : sur un profil neuf, le bootstrap télécharge toute l'interface
   (180 fichiers, 5,1 Mo) par le tunnel. Le temps vient de l'aller-retour, pas du débit : au même
   banc avec 115 ms ajoutées (le RTT entre la VM et la maison), ce téléchargement prend 26,5 s en
   TCP et 19,4 s en UDP, contre 0,4 s sur le LAN. Chaque réponse attend un aller-retour avant de
   démarrer, et les trois plus gros fichiers (`StreamView.js`, `gamepadDb.js`, `app.js`) traînent
   seuls à la fin. Le TCP coûtait un tiers de plus, parce que libjuice jetait les datagrammes que
   le socket ne prenait pas encore.
   - v0.4.1 : une file de 256 Ko dans le patch libjuice (§3.7) ; 12 téléchargements à la fois, les
     plus gros d'abord, avec les tailles du manifeste (`sizes`, une liste à côté de `files`, pour
     les bootstraps et les hôtes d'avant). Le nouveau `boot.js` part en prod avec la release.
   - Mesuré au même banc le 24/10 (profil neuf, +115 ms, 174 fichiers). Le nouveau `boot.js` est
     substitué par CDP, sans déploiement.

     | Passe | Interface | Image |
     |---|---|---|
     | TCP avant | 26,6 s | 37,8 s |
     | TCP, file seule | 12,1 s | 23,6 s |
     | TCP, file + `boot.js` | 5,6 s | 16,1 s |
     | UDP avant | 19,4 s | 27,9 s |
     | UDP, `boot.js` | 2,8 s | 10,7 s |
   - v0.4.2 : compresser l'interface dans le tunnel (le JS se réduit d'environ 4 fois). La
     négociation se fait par requête plutôt que par un drapeau dans le manifeste. `tunnel.js`
     met `"z":"deflate"` dans la tête d'une requête quand le navigateur a `DecompressionStream`.
     L'hôte compresse alors la réponse en zlib (`qCompress`) et marque sa propre tête de la même
     façon ; `tunnel.js` la décompresse avant de construire la `Response`. Les empreintes et la
     liste officielle voient donc les mêmes octets qu'avant. Un hôte ancien ignore le champ, et
     un navigateur sans `DecompressionStream` ne le demande pas : chaque mélange d'anciens et de
     nouveaux reçoit le corps tel quel. Ne sont compressés que les fichiers de l'interface
     (GET hors `/api/`, statut 200, type texte, 1 Ko ou plus, manifeste web exclu). Une réponse
     de l'API porte des sessions et des clés à côté de texte choisi par l'appelant, et sa
     longueur compressée en dirait trop (la forme de BREACH). Les vecteurs zlib sont partagés
     entre `backend/tests/test_tunnel_frame.cpp` et `frontend/test/tunnelFrame.test.js`.
   - Mesuré le 07/11 (21:1x-21:3x) au même banc : une `--dev` par stream.dev, UPnP coupé, un
     Chrome du WSL sur profil neuf, +115 ms. `boot.js` et `tunnel.js` du dépôt sont substitués
     par CDP. Le témoin est une copie de `tunnel.js` qui ne demande jamais la compression. Pour
     le TCP, l'UDP sortant du WSL est coupé par nft, sauf vers son résolveur DNS ; la seule
     connexion de l'hôte était alors sur l'écoute TCP passive. On mesure du début du
     téléchargement au 194ᵉ fichier :

     | Chemin | Sans compression | Avec compression |
     |---|---|---|
     | TCP | 14,2 s, 25,0 s | 3,9 s, 3,9 s |
     | UDP | 3,5 s, 3,8 s | 3,6 s, 3,1 s |

     - 174 des 194 fichiers arrivent compressés : 4,73 Mo deviennent 1,37 Mo sur le fil.
     - La vérification des empreintes passe à chaque fois.
     - En UDP, le gain se perd dans le bruit : le temps vient des allers-retours.
     - Le TCP n'est plus le parent pauvre de l'UDP.
6. ✅ **Depuis un vrai réseau** (26/10, 09:41-09:43) : Bruno, au bureau, sur un Mac d'entreprise
   sans droits d'administration. Le réseau laisse sortir l'UDP : la `--dev` de 59 portait la clé de
   banc `debug_ice_drop_udp` (§3.6). Le tunnel passe en TCP sur 46064 et le flux en
   `webrtc-dc-tcp`, sur la paire `host 192.168.1.66:48550/TCP ↔ prflx <IP du bureau>/TCP`. Deux
   minutes à 52-60 i/s, sans trou ni gel ni perte, 24,7 ms de bout en bout. Bruno : « Très
   stable ». Défaut passé à vrai pour la v0.4.1, après les correctifs T1 à T4.
