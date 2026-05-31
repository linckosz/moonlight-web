## Session: 2026-05-30-6-fixes
## Agent: frontend-dev
## Task: Corriger 3 issues frontend

Tu travailles sur `d:\Code\moonlight-web-deepseek`. Corrige les 3 issues suivantes.

### Issue 2: Champ nom de machine pas assez clair (LoginView)

**Problème**: L'input pour le nom de machine dans la LoginView ne ressemble pas à un champ éditable. Il faut le styliser pour qu'on voie clairement que c'est modifiable.

**Fichier**: `frontend/js/ui/LoginView.js`

**Action**:
1. Lis `frontend/js/ui/LoginView.js` pour trouver l'input du hostname.
2. Ajoute des styles CSS (soit inline soit via une classe existante) pour que l'input ait :
   - Une bordure visible (1px solid, couleur discrète)
   - Un fond légèrement différent (ex: `#fafafa` ou `#f5f5f5`)
   - Un curseur texte (`cursor: text`)
   - Un padding pour que le texte ne touche pas les bords
   - `border-radius` pour cohérence avec le reste de l'UI
   - Au hover: bordure qui change de couleur (feedback visuel)
   - Au focus: bordure de focus visible (ex: bleu `#1976d2` ou la couleur primaire du projet)
3. Si le fichier utilise des styles CSS dans une feuille séparée, regarde `frontend/css/` pour voir s'il y a un fichier de styles existant pour LoginView ou un fichier common.

### Issue 3: PIN par défaut "--------" (partie frontend)

**Problème**: L'AdminView affiche un PIN généré automatiquement au lieu d'afficher `"--------"` (8 tirets). Le PIN ne doit être généré QUE quand l'admin clique sur "Generate" ou quand l'Internet Access est activé.

**Fichier**: `frontend/js/ui/AdminView.js`

**Action**:
1. Lis `frontend/js/ui/AdminView.js` pour comprendre comment le PIN est chargé et affiché.
2. Cherche où le PIN est récupéré depuis le backend (appel API) et affiché dans le DOM.
3. Modifie l'affichage pour que si le backend retourne `"--------"` comme PIN, il soit affiché tel quel (8 tirets).
4. Ne modifie PAS la logique de génération — c'est le backend qui décide.

### Issue 4: Bouton "Clear" pour le PIN (partie frontend)

**Problème**: Ajouter un bouton "Clear" dans l'AdminView qui remet le PIN à `"--------"`.

**Fichier**: `frontend/js/ui/AdminView.js`

**Action**:
1. Dans le même fichier AdminView.js, ajoute un bouton "Clear" à côté du bouton "Generate PIN" existant.
2. Le bouton "Clear" doit :
   - Être stylisé pour ressembler au bouton Generate (même hauteur, style secondaire/neutre)
   - Faire un appel API pour effacer le PIN (probablement POST `/api/admin/pin/clear` ou similaire)
   - Mettre à jour l'affichage du PIN à `"--------"` après succès
   - Gérer les erreurs (Toast notification)
3. Vérifie d'abord comment le bouton "Generate PIN" existant fonctionne (appel API, callback, mise à jour DOM) et suis le même pattern.

---

### Instructions finales

1. Lis chaque fichier avant de le modifier.
2. Fais les modifications une par une.
3. Teste visuellement en servant le frontend si possible (optionnel).
4. Écris ton résumé dans `.claude/results/frontend-dev/2026-05-30-6-fixes/Resume-2026-05-30.md`.

Format du résumé : tâche accomplie, fichiers modifiés, décisions prises, points bloquants éventuels.
