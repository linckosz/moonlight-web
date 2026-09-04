# Plan annexe — optimisations des autres pipelines (Sunshine, Apollo, Wolf, MultiSeat)

> Le host natif est prioritaire : la grande majorité des utilisateurs passeront par
> lui. Mais les hosts externes doivent rester pleinement servis. Ce fichier est donc
> **un plan à part**, pas une liste d'oublis : tout ce que le chantier natif révèle
> d'améliorable chez eux est consigné ici, avec l'endroit et l'effet attendu, et sera
> **traité dans une session dédiée**, avec sa propre non-régression (quatre sessions
> simultanées, co-op Wolf, MultiSeat). Rien d'ici ne s'applique pendant le chantier
> natif. Les **bugs**, eux, se corrigent directement et ne figurent pas ici.

| Date | Où | Constat | Effet attendu si appliqué |
|---|---|---|---|
| 02/09/2026 | `backend/src/streaming/FrameSender.h:87` | `kMaxQueued = 8` : jusqu'à huit frames en attente avant la première éviction, soit 133 ms de tampon à 60 fps, pour tous les moteurs | Profondeur 1 à 2 pour les deltas : la latence sous charge cesse de s'accumuler avant que la contre-pression agisse |
| 02/09/2026 | `backend/src/streaming/DataChannelRelay.cpp:796` | Chaque message d'input est marshalé vers le thread principal Qt, qui traite aussi chaque frame vidéo en file et le scan NAL des keyframes | Parse et transmission sur le thread libdatachannel : l'input ne fait plus la queue derrière la vidéo |
| 02/09/2026 | `backend/src/streaming/DataChannelRelay.cpp:457` | `videoFrameReady` connecté en `AutoConnection` : le relais DC paie une file Qt vers le thread principal puis une file vers `FrameSender`. `MediaTrackRelay` a déjà le mode direct (`m_DirectVideoSend`) | Un réveil de thread de moins par frame, thread principal hors du chemin chaud |
| 02/09/2026 | `backend/src/streaming/DataChannelRelay.cpp` | Le relais DC ne lit pas `clientstats` ; seul `MediaTrackRelay` le fait | Retour du récepteur disponible pour un rate control ou un diagnostic sur le transport par défaut |
| 02/09/2026 | `backend/src/streaming/DataChannelRelay.cpp:860` | Le patch VPS/SPS HEVC scanne les NAL de chaque keyframe jusqu'au premier succès, sur le thread qui envoie | Scan borné à la première keyframe, hors du chemin chaud |
| 04/09/2026 | `frontend/js/api/WebRtcDataChannel.js` `_assembleFrame` | Le canal vidéo est **ordonné** (`unordered = false`, 3 retransmissions) pour tous les moteurs : une image incomplète qu'une plus récente a dépassée ne se complétera jamais, mais elle n'est déclarée perdue qu'au bout des 500 ms de `FRAME_TIMEOUT_MS`. Le natif la déclare à l'instant (G1, gardé par `healsByInvalidation`) | Même déclaration immédiate pour Sunshine/Wolf : l'invalidation de référence et la demande de keyframe partent 500 ms plus tôt, le gel visible est plus court d'autant |
| 04/09/2026 | `frontend/js/api/WebRtcDataChannel.js` ride-out | Le chien de garde du ride-out compte 2,5 s d'horloge pour les hôtes qui ne disent pas la longueur de leur vague d'intra-refresh ; un hôte GameStream qui l'annoncerait (Sunshine expose la période dans sa config) permettrait de compter en images comme le natif (G2) | Plus de faux « ride-out failed » quand le jeu présente lentement sous un stream rapide |
| 02/09/2026 | `backend/src/streaming/DataChannelRelay.h:164` + `third_party/libdatachannel/src/impl/sctptransport.cpp:106` | `kHighWatermark = 256 Kio` est lu sur `bufferedAmount`, qui ne compte que ce que libdatachannel garde **après** refus d'usrsctp ; or le tampon d'émission usrsctp fait **1 Mio** (`sctp_sendspace`). La contre-pression du relais DC ne se déclenche donc qu'au-delà de ~1,25 Mo de retard, soit 500 ms sur un lien à 20 Mbps, pour tous les moteurs | `rtc::Configuration` permet `sendBufferSize` : le ramener à l'ordre d'une ou deux frames rendrait `bufferedAmount` vrai et la contre-pression réactive ; à mesurer sur Sunshine et Wolf avant d'y toucher (le tampon amortit aussi les rafales de keyframes) |
