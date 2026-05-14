Task: Read the current state of tunnel-related backend files for the nport "Internet Access" feature.

You are backend-dev. Please read the following files and report their FULL contents (or key sections) to me:

1. `d:\Code\moonlight-web-deepseek\backend\src\network\NportClient.cpp` — full file
2. `d:\Code\moonlight-web-deepseek\backend\src\network\NportClient.h` — full file
3. `d:\Code\moonlight-web-deepseek\backend\src\server\AppSettings.h` — look for nport-related fields/methods
4. `d:\Code\moonlight-web-deepseek\backend\src\server\AppSettings.cpp` — look for nport-related getters/setters
5. `d:\Code\moonlight-web-deepseek\backend\src\main.cpp` — look for tunnel setup, route registration, status endpoint
6. `d:\Code\moonlight-web-deepseek\backend\src\server\HttpServer.h` — look for the route registration for /api/tunnel/*
7. `d:\Code\moonlight-web-deepseek\backend\src\server\HttpServer.cpp` — look for the tunnel route handlers

For each file, provide:
- The full content if small (< 100 lines)
- For larger files, provide the relevant sections with line numbers
- Note any existing patterns for route handlers, especially how /api/tunnel/status is handled

Report everything in a structured format. This will be used to plan the refactoring of the Internet Access UI section.

Do NOT make any changes. READ ONLY.
