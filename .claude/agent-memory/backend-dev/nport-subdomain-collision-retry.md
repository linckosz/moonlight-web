---
name: nport-subdomain-collision-retry
description: Subdomain collision retry for nport tunnel - auto-retries with suffix fallback when Cloudflare holds old subdomain
metadata:
  type: reference
---

## nport Subdomain Collision Retry

When the user quickly stops/restarts the nport tunnel, Cloudflare still holds the old subdomain (up to 4h auto-cleanup). The retry mechanism handles this:

### Files modified
- `backend/src/network/NportClient.h` — added `MAX_RETRIES` (5), `m_SubdomainSuffix`, `m_RetryCount`, `m_LastStderr`, `m_UseSubdomain`, `buildSubdomain()`, `subdomainSuffixChanged` signal
- `backend/src/network/NportClient.cpp` — collision detection in `onProcessFinished()`, retry logic with suffix increment, `buildSubdomain()` implementation, counter resets in `stop()` and success paths
- `backend/src/server/AppSettings.h` — added `nportSubdomainSuffix()` / `setNportSubdomainSuffix(int)`
- `backend/src/server/AppSettings.cpp` — JSON key `"nport_subdomain_suffix"`, default 0
- `backend/src/main.cpp` — load suffix from AppSettings, connect `subdomainSuffixChanged` to persist changes

### Retry strategy
1st collision: retry with same suffix (old tunnel might be expiring)
2nd collision: increment `m_SubdomainSuffix` → subdomain becomes `moonlightweb-<hex>-N`
Max 5 retries exhausted: launch without `-s` (random subdomain fallback)

Counters reset on: successful URL parse (`tunnelReady`), explicit `stop()`, and `onRefreshTimeout()` (which calls `stop()`).
Suffix is persisted to `settings.json` and survives restarts.
