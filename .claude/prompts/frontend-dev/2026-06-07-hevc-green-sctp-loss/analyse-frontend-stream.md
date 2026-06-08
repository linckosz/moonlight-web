Tu es frontend-dev. Analyse les fichiers suivants pour comprendre le réassembly des chunks vidéo, la gestion des frames corrompues, et les erreurs du décodeur.

Lis ces fichiers :
1. `d:\Code\moonlight-web-deepseek\frontend\js\api\WebRtcDataChannel.js`
2. `d:\Code\moonlight-web-deepseek\frontend\js\ui\StreamView.js`

Réponds précisément à ces questions :
1. Comment les chunks SCTP sont-ils réassemblés en frames vidéo côté navigateur ? Décris le mécanisme exact (buffering, vérification d'intégrité, timeout).
2. Que se passe-t-il si un chunk est perdu ? La frame entière est-elle abandonnée ou essaye-t-on de la décoder partiellement ?
3. Y a-t-il une détection de frame corrompue avant de l'envoyer au VideoDecoder ?
4. Y a-t-il des logs d'erreur VideoDecoder dans StreamView.js ? Des erreurs `decode()` détectées ?
5. Comment le backpressure est-il géré côté frontend ? Y a-t-il un mécanisme pour signaler au backend de ralentir ?
6. Le DataChannel vidéo utilise-t-il `ordered: false` ou `ordered: true` ? Y a-t-il des `maxRetransmits` configurés ?
7. Y a-t-il des `bufferedAmount` gérés pour éviter le surremplissage du buffer SCTP ?

En fin de travail, écris ton résumé dans `.claude/results/frontend-dev/2026-06-07-hevc-green-sctp-loss/Resume-2026-06-07.md`. Inclus uniquement tes résultats/conclusions.
