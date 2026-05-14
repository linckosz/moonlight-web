---
name: nport-sbdomain-release
description: NportClient::releaseSubdomain() calls DELETE /v1/tunnels API on stop() to free the subdomain, preventing "already in use" on restart
metadata:
  type: project
---

NportClient now calls the nport API DELETE endpoint synchronously (via QEventLoop, 5s timeout) in releaseSubdomain(), invoked at the top of stop() before killing the nport process. This prevents "subdomain already in use" collisions on restart. Non-fatal on failure — the 30-min nport auto-cleanup handles it.

**Why:** The nport subdomain was not being released when the tunnel was stopped, causing the next start() with the same subdomain to fail ("already in use") requiring MAX_RETRIES fallback to random subdomain.

**How to apply:** Any future nport-related changes should ensure releaseSubdomain() stays called before the process kill in stop(). The sync QEventLoop approach is acceptable here because stop() is a top-level synchronous API call, not nested inside an async callback chain.
