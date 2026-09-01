# Manette virtuelle — dépendances et licences

> Exigé par `docs/design/gamepad-virtual.md` §17. Une seule question est posée
> pour chaque dépendance : **peut-elle rester là si MoonlightWeb devient un
> produit commercial ?** La contrainte fondamentale du plan est que tout soit
> gratuit, libre et redistribuable, sans licence à acheter et sans compte
> fournisseur.

## Ce qui est utilisé aujourd'hui

| Dépendance | Version | Licence | Rôle | Redistribué | Commercial |
|---|---|---|---|---|---|
| **ViGEmClient** | 1.16.18.0 | **MIT** | parler au bus virtuel Windows | oui, vendoré (`backend/native-host/third_party/vigem-client/`) | libre |
| **ViGEmBus** (pilote) | 1.22.0 | **BSD-3** | créer le périphérique côté noyau | non — téléchargé depuis l'amont à l'installation | libre |
| **uinput** | — | API du noyau Linux | créer le périphérique sous Linux | rien à redistribuer | libre |

⚠️ Le plan initial et le document d'architecture notaient ViGEmClient en BSD-3.
**C'est MIT**, vérifié dans le `LICENSE` de l'amont. MIT est strictement plus
simple pour une relicence commerciale.

### ViGEmClient — pourquoi vendoré

~58 Ko de source, compilés directement dans `mw-native-host` plutôt que
d'produire une DLL. Le build ne doit pas dépendre du réseau, et une montée de
version doit être un commit visible plutôt qu'un événement qui arrive à celui qui
compile ensuite. Byte-for-byte amont, sans modification.

### ViGEmBus — pourquoi il n'est pas redistribué

C'est un pilote signé. Il est **installé depuis l'amont** (l'installeur le pose
silencieusement, et l'encart de §9.1 rattrape les cas où ça a échoué), jamais
recopié dans nos artefacts : redistribuer un pilote signé engage la signature de
quelqu'un d'autre.

Son absence n'est pas une erreur : `vigem_connect()` renvoie
`VIGEM_ERROR_BUS_NOT_FOUND`, la session continue sans manette, clavier et souris
intacts.

### uinput — pourquoi il n'y a rien à documenter

C'est une interface du noyau Linux, pas une bibliothèque. Rien n'est lié, rien
n'est embarqué : le code ouvre `/dev/uinput` et écrit dedans. La seule chose que
le paquet pose est une règle udev (`70-moonlightweb-uinput.rules`) pour que le
serveur n'ait pas besoin de rester root.

C'est ce qui rend Linux la plateforme la plus simple des trois : ce que Windows
fait payer en signature de pilote, le noyau le fournit.

## Ce qui a été évalué et écarté

| Solution | Licence | Pourquoi écartée |
|---|---|---|
| **libvirtualhid** (LizardByte) | lib MIT, **pilote payant** | licence par utilisateur — exactement la dépendance commerciale que le plan interdit |
| **vJoy** | **GPL** | licence incompatible avec la relicence du module natif, et pas de XInput (DirectInput seulement) |
| **Pilote UMDF2 maison** (VHF) | à nous | signature EV + attestation Microsoft : plusieurs centaines d'euros par an. C'est une décision d'investissement, pas une tâche à planifier |

Conséquence assumée : les profils **DualSense**, **Switch Pro** et **Generic
HID**, ainsi que les **volants et palonniers**, sont hors de portée sous Windows
sans écrire et signer un pilote. Ils restent dans l'architecture, pas dans le
périmètre livrable — voir §2.2 et §2.3 du plan.

## ViGEmBus est archivé — et ce n'est pas un problème technique

Retiré le **2 novembre 2023** pour un **conflit de marque** avec ViGEM GmbH. Pas
pour un défaut, pas pour une incompatibilité. Le pilote fonctionne sous
Windows 11, il est signé, et DS4Windows comme DualSenseX en dépendent toujours.

Un successeur (*VirtualPad*) est annoncé mais n'est pas sorti. **Ne pas le
remplacer par réflexe** : l'abstraction de §5 contient le changement à un seul
fichier le jour où il existera.

## La règle, en une phrase

Ne jamais introduire une solution propriétaire parce qu'elle est plus facile. Si
une fonctionnalité l'exige : ne pas l'intégrer, chercher une alternative libre,
vérifier sa licence, documenter le compromis, et s'il n'existe rien d'autre,
l'isoler derrière une abstraction en la laissant désactivée.
