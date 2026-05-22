Instruction pour frontend-dev :
Fais les 4 modifications suivantes dans le fichier D:\Code\moonlight-web-deepseek\frontend\js\ui\AdminView.js, dans le cadre "Internet" (section DuckDNS/desec) :

1. Supprimer entièrement le texte : "Register a deSEC dynamic DNS domain to enable remote streaming from outside your home network. An A record pointing to your public IP is created and a Let's Encrypt TLS certificate is obtained automatically."

2. Supprimer le label/titre "Domain" qui précède l'affichage du nom de domaine.

3. Faire en sorte que le domaine `0d71ad30.moonlightweb.dedyn.io` apparaisse comme `https://0d71ad30.moonlightweb.dedyn.io` (préfixe https:// visible en permanence, même quand le domaine n'est pas encore actif). Si le domaine est cliquable (lien), garde le lien ; sinon affiche-le simplement comme texte avec le https://.

4. Remplacer le texte de port forwarding. Chercher le texte contenant "Source (Any) → 443 → Destination (this machine), TCP and UDP." et le remplacer par : "Source (Any:443) → Destination (192.168.?.?:443), TCP and UDP." — où ?.? est l'IP locale réelle du serveur. Il faut récupérer l'IP locale depuis le backend (peut-être un appel API existant ou un champ dans la réponse DuckDNS). Si l'IP n'est pas encore connue, afficher "192.168.?.?" tel quel.

En fin de travail, écris ton résumé dans
`.claude/results/frontend-dev/{session}/Resume-YYYY-MM-DD.md`.
Inclus uniquement tes résultats/conclusions (pas la réflexion intermédiaire).
Format : tâche accomplie, fichiers modifiés, décisions prises, points bloquants.

Session ID : {session}
