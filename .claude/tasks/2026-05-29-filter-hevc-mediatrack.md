# Tache pour backend-dev : filterTransportsByCodec — skip HEVC en mode MediaTrack

## Fichier
`backend/src/main.cpp`

## Contexte
Il y a une lambda `filterTransportsByCodec` (cherche avec `filterTransportsByCodec` ou `webrtc-media` ou `VideoCodec::AV1`).

Actuellement, elle skip AV1 pour les transports `webrtc-media` :
```cpp
if (effective == VideoCodec::AV1 && t.startsWith("webrtc-media")) {
    continue;
}
```

## Modification a faire
Etendre cette condition pour aussi skipper HEVC :

```cpp
if ((effective == VideoCodec::AV1 || effective == VideoCodec::HEVC) 
    && t.startsWith("webrtc-media")) {
    qInfo() << "[Auto] Skipping" << t 
            << "(MediaTrack only supports H.264, codec is" 
            << static_cast<int>(effective) << ")";
    continue;
}
```

## Verification
Apres la modification, fais un grep de la lambda pour confirmer que le changement est bien en place :
```
grep -n -A5 "filterTransportsByCodec" backend/src/main.cpp
```

## Format de reponse
Confirme que la modification a ete faite et montre le resultat du grep.
