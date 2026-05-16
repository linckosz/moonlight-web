## Tache : Investiguer le parsing NAL, la configuration VideoDecoder et le rendu Canvas

### Contexte
L'utilisateur streame en H.264. Sur LAN, les couleurs sont correctes. Sur Internet via UPnP, les couleurs sont incorrectes. La seule difference est le timing du buffer keyframe.

### Fichiers a examiner

1. **`d:\Code\moonlight-web-deepseek\frontend\js\util\Av1Utils.js`** — parsing NAL (utilise aussi pour H.264 ?)
2. **`d:\Code\moonlight-web-deepseek\frontend\js\util\Mp4Muxer.js`** — muxing NAL en fMP4 (deprecie ?)
3. **`d:\Code\moonlight-web-deepseek\frontend\js\ui\StreamView.js`** — init du VideoDecoder, rendu canvas
4. **`d:\Code\moonlight-web-deepseek\frontend\js\stream\` (ou equivalent)** — gestion du stream, bufferisation des NAL

### Questions detaillees

#### 1. Parsing SPS/PPS
- Comment le SPS est-il parse ? Y a-t-il une extraction des VUI parameters ?
  * `colour_primaries`
  * `transfer_characteristics`
  * `matrix_coefficients`
  * `video_full_range_flag`
- Si les VUI sont extraits, sont-ils passes au VideoDecoder via `colorSpace` dans la config ?
- Si non : WebCodecs utilise-t-il des valeurs par defaut qui different entre LAN et UPnP ?

#### 2. VideoDecoder config
- Comment le `description` (AvcDecoderConfig = SPS + PPS) est-il construit ?
- Le VideoDecoder recoit-il une config complete avant le premier decode ?
- Y a-t-il une reconfiguration apres chaque keyframe ? (configure() est-il rappele ?)
- Est-ce que `configure()` est appele AVANT ou PENDANT que les NAL data arrivent ?

#### 3. Canvas rendering
- Comment le VideoFrame est-il dessine sur le canvas ?
- Y a-t-il une conversion manuelle (ex: decodeImageData, ImageBitmap, transferControlToOffscreen) ?
- Le canvas a-t-il un colorSpace specifique ?
- Est-ce que `context.drawImage(frame)` ou `canvas2D.transferFromImageBitmap(bitmap)` est utilise ?

#### 4. Timing / bufferisation
- Comment les NAL fragments sont-ils reassembles cote frontend ?
- Y a-t-il un etat mutable dans le parser NAL qui pourrait etre different si la keyframe arrive bufferisee vs en temps reel ?
- Si plusieurs keyframes arrivent bufferisees, est-ce que la SPS/PPS de la 2eme peut ecraser la 1ere ?
- Comment gere-t-on le cas ou le VideoDecoder n'est pas encore configure quand les NAL arrivent ?

### Rapport
Ecrit ton resume dans `.claude/results/frontend-dev/2026-05-16-color-investigation/Resume-2026-05-16.md`.
Inclus des extraits de code pertinents et ton diagnostic.
