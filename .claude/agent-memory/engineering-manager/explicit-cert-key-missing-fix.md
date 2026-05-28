---
name: explicit-cert-key-missing-fix
description: Fix when explicit cert_path fullchain.pem exists but key.pem is missing, SSL falls back to self-signed
metadata:
  type: reference
---

## Root cause

The explicit certificate at `cert_path` (`brunoocto_moonlightweb_top/fullchain.pem`) existed on disk but `key.pem` was **missing** from the same directory. Two code paths silently failed:

1. **HttpServer::loadCert()** — called `loadCertFilesExplicit(certPath, keyPath)` which failed with "Failed to open key file" but the connection to the root cause was unclear.
2. **InternetAccessManager::checkCertificate()** — only verified cert file existence and CN match, never checked for the private key.

The self-signed `cert.pem` + `key.pem` in the root `cert/` directory was then loaded as fallback.

## Fix

### HttpServer::loadCert()
- Before calling `loadCertFilesExplicit()`, explicitly check if `key.pem` exists alongside the cert.
- If missing, log a **clear `[CERT]` warning** stating "key.pem is MISSING in [dir]" so the user knows why the explicit cert was skipped.

### InternetAccessManager::checkCertificate()
- After verifying the cert file exists, also check for `key.pem` in the same directory (and one level up as fallback).
- If key is missing, trigger certificate **re-issuance** via `issueCertificate()`.

## When this happens
- lego/ACME saves fullchain in a domain subdirectory but the key is stored elsewhere
- Manual certificate setup where only the fullchain was copied
- After a partial certificate migration
