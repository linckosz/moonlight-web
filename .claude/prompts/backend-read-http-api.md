Task: Read and analyze the following backend files related to APIs that expose local IP address, host info, or settings that the AdminView uses.

Files to read:
1. `d:\Code\moonlight-web-deepseek\backend\src\server\HttpServer.h` — focus on:
   - Route handlers related to `/api/settings`, `/api/hosts`, `/api/status` or similar
   - Any endpoint that returns local IP or host information

2. `d:\Code\moonlight-web-deepseek\backend\src\server\HttpServer.cpp` — focus on:
   - The actual implementation of the settings/hosts endpoints
   - How the local IP is determined (look for QNetworkInterface, localAddress, or similar)
   - What fields are returned in JSON responses for settings/hosts

3. Any other file that exposes local IP info in the API (maybe `AppSettings.h/cpp`, `ComputerManager.h/cpp`, or `NetworkManager.h/cpp`)

Please provide:
- The full handler code for `/api/settings` and `/api/hosts` endpoints
- How local IP is currently determined and exposed
- The JSON response structure returned by these endpoints
- Any relevant AppSettings keys related to "localIP", "lanAddress", "externalUrl", "domain", etc.

Write your findings in `.claude/results/backend-dev/{session}/Resume-YYYY-MM-DD.md` where session is "admin-local-ip-link".
