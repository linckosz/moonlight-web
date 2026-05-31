Task: Read and analyze the following frontend files related to the Admin page link display.

Files to read:
1. `d:\Code\moonlight-web-deepseek\frontend\js\ui\AdminView.js` — focus on:
   - How the DuckDNS / external URL link is currently displayed (look for `_render`, `render`, or similar methods that build the admin page HTML)
   - How internetAccessEnabled or similar settings affect the display
   - What settings/state the view uses

2. `d:\Code\moonlight-web-deepseek\frontend\js\api\BackendClient.js` — focus on:
   - Any endpoint that returns host info, settings, or local IP (e.g. `/api/hosts`, `/api/settings`, `/api/status`)
   - How the AdminView fetches the data it needs

3. `d:\Code\moonlight-web-deepseek\frontend\js\app.js` — focus on:
   - How AdminView is instantiated and what data is passed to it

Please provide:
- The full code of the relevant methods that render the admin page (especially the link/URL section)
- The API calls the AdminView makes and what data it expects back
- How internet access / DuckDNS toggle currently works in the UI
- The structure of the fetched settings/hosts data

Write your findings in `.claude/results/frontend-dev/{session}/Resume-YYYY-MM-DD.md` where session is "admin-local-ip-link".
