Analyse à mener pour le curseur tactile :
1. Lire `frontend/js/ui/StreamView.js` — chercher comment le curseur est géré (propriété cursor, CSS cursor style, canvas style, mouse events, touch events)
2. Lire `frontend/css/stream.css` — chercher les règles CSS liées au curseur
3. Analyser si un overlay de curseur est déjà présent (élément DOM pour le curseur)
4. Vérifier comment les événements touch sont traduits en événements souris

Rapporte :
- Les lignes exactes qui gèrent le curseur
- Si un overlay canvas ou DOM est utilisé
- Comment le style cursor CSS est appliqué
- Comment les touch events sont gérés
- La taille/pixel ratio actuel du curseur
