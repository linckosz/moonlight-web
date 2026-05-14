---
name: nport-direct-api
description: Switch from nport CLI (node) to direct nport API POST/DELETE + cloudflared --token
metadata:
  type: project
---

NportClient now bypasses the nport Node.js CLI entirely. The flow is:
1. POST `https://api.nport.link` with `{"subdomain":"moonlightweb-<hex>"}` -> returns `tunnelId`, `tunnelToken` (JWT), `url`
2. Launch `cloudflared tunnel run --token <tunnelToken>` (bundled binary from nport npm package)
3. Detect readiness from cloudflared stdout ("Registered tunnel connection")
4. DELETE `https://api.nport.link` with `{"subdomain":"...", "tunnelId":"..."}` in stop()

**Why:** The nport CLI doesn't expose `tunnelId`, so the DELETE call in `releaseSubdomain()` was sending only the subdomain, which the API rejects without a tunnelId. By calling the API directly we control the full lifecycle.

**How to apply:** `findCloudflaredBinary()` searches `runtime/nport/node_modules/nport/bin/cloudflared.exe`. No Node.js runtime or nport CLI script is needed anymore -- only the cloudflared binary bundled with the nport npm package.
