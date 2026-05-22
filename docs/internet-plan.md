# Goal: MWServer — Internet Access via deSEC Dynamic DNS + WebRTC Direct Connection

## Context

This goal implements the full "Internet Access" feature: dynamic DNS via deSEC.io, TLS certificate automation,
UPnP port mapping, public IP detection via STUN, and optimized WebRTC direct connection (no TURN).

---

## 1. Unique ID & deSEC Domain Registration

### At installation:
- Reuse the existing uniqueid mechanism (or generate one if absent).
- Build the target domain: `moonlightweb-{uniqueId}.dedyn.io`
- Call the deSEC API to check if the subdomain is already taken:
  ```
  GET https://desec.io/api/v1/domains/{domain}/
  Authorization: Token {DESEC_TOKEN}
  ```
  - If 404 → domain is available → register it:
    ```
    POST https://desec.io/api/v1/domains/
    { "name": "moonlightweb-{uniqueId}.dedyn.io" }
    ```
  - If 200 → already taken → generate a new uniqueId and retry (max 5 attempts).
- Store `uniqueId`, `domain`, and `desec_token` in persistent settings.
- By default `desec_token` is "auto" or empty "", this means that it uses the Token provided in the code at the compilation time if available as deployment variable via Github Action.

### If no Internet at install time:
- Skip domain registration silently.
- Set a flag `pending_registration: true` in settings.
- On each MWServer startup, if `pending_registration: true`, retry the registration flow as soon as connectivity is detected (poll every 30s until success).

---

## 2. GDPR Consent Step (Installation UI)

Display a clear consent screen during installation with the following:

**Title:** "Enable Internet Access (optional)"

**Body:**
> To allow remote access over the Internet, MWServer needs to:
> - Share your **public IP address** with **deSEC.io** (a non-profit DNS provider) to create a secure subdomain pointing to your network.
> - Optionally enable **UPnP** on your router to automatically open the required port.
>
> This data is used solely to make your server reachable. You can disable this feature at any time in Settings.

**Actions:**
- [ ] Enable Internet Access ← checkbox, default OFF
- [ ] Enable UPnP (requires Internet Access) ← checkbox, default OFF

Store consent result as `internet_access_enabled: bool` and `upnp_enabled: bool` in persistent settings.
If consent is refused, the entire Internet Access feature is disabled and no external calls are made.

---

## 3. Settings Schema

Add the following fields to the persistent settings store:

```json
{
  "internet_access_enabled": false,
  "upnp_enabled": false,
  "unique_id": "abc123",
  "domain": "moonlightweb-abc123.dedyn.io",
  "desec_token": "auto",
  "public_ip": "",
  "auto_ip_detection": true,
  "transport_mode": "auto",
  "pending_registration": false
}
```

- `auto_ip_detection`: if true, STUN is used to detect public IP automatically.
- `public_ip`: editable by user in admin UI; overrides STUN result if `auto_ip_detection` is false.
- `transport_mode`: `"auto"` | `"webrtc-media-udp"` | `"webrtc-dc-udp"` | `"webrtc-media-tcp"` | `"webrtc-dc-tcp"` | `"wss"`

---

## 4. Public IP Detection via STUN

Use the following STUN servers in order (fallback chain):

```
1. stun:stun.l.google.com:19302
2. stun:stun1.l.google.com:19302
3. stun:stun.cloudflare.com:3478
4. stun:stun.nextcloud.com:443
5. stun:relay.metered.ca:80
```

Detection logic:
- If `auto_ip_detection` is true → run STUN at startup and before each ICE negotiation.
- If STUN fails on all servers → use `public_ip` from settings if set, else log warning.
- After successful detection, update `public_ip` in settings.

---

## 5. A Record Management

### At installation:
- If STUN succeeded → create A record: `moonlightweb-{uniqueId}.dedyn.io → {public_ip}`
- If STUN failed → create A record pointing to `127.0.0.1` as placeholder.

### A record create/update API call:
```
PUT https://desec.io/api/v1/domains/{domain}/rrsets/
Authorization: Token {DESEC_TOKEN}
Content-Type: application/json

[{
  "subname": "",
  "type": "A",
  "ttl": 300,
  "records": ["{public_ip}"]
}]
```

---

## 6. Periodic Checks (every 5 minutes)

Run a background timer every 5 minutes that:

1. **IP change check:**
   - If `auto_ip_detection` is true → run STUN to get current public IP.
   - If result ≠ `settings.public_ip` → update `settings.public_ip` → update A record via deSEC API.

2. **Domain resolution check:**
   - Resolve `moonlightweb-{uniqueId}.dedyn.io` via DNS.
   - If resolved IP ≠ `settings.public_ip` → update A record via deSEC API.
   - If resolution fails entirely → log warning in admin UI.

---

## 7. TLS Certificate (Let's Encrypt via DNS-01 challenge)

### At installation:
- Use **acme.sh** or **certbot** with the deSEC DNS plugin to issue a TLS certificate for `moonlightweb-{uniqueId}.dedyn.io` via DNS-01 challenge (no need for port 80).
- deSEC supports RFC 2136 / acme.sh natively.
- Store certificate path and `cert_expiry` date in settings.
- Certificate validity: 90 days (Let's Encrypt maximum).

### Auto-renewal:
- On every MWServer startup and every 24h, check `cert_expiry`.
- If less than 30 days remaining → trigger certificate renewal automatically.
- On renewal success → reload TLS on the WSS/HTTPS listener without restart.

### Suggested tooling:
- Use `acme.sh --dns dns_desec` with the deSEC API token.
- Or integrate `node-acme-client` if the stack is Node.js-based.

---

## 8. UPnP Port Mapping

- If `upnp_enabled` is true → at startup, attempt UPnP `AddPortMapping` for the MWServer port (TCP + UDP).
- If UPnP fails (any reason: disabled, double NAT, unsupported router):
  - Do NOT block startup.
  - Set `upnp_status: "failed"` in runtime state.
  - Display a notice in the admin UI:

> **Manual port forwarding required**
> UPnP could not configure your router automatically.
> Please forward port **{PORT}** (TCP and UDP) to this machine's local IP in your router settings.
> This allows the domain `moonlightweb-{uniqueId}.dedyn.io` to reach MWServer.

- Detect double NAT: compare UPnP external IP with STUN public IP. If different → show specific warning:
  > "Your router is behind a carrier-grade NAT (CGNAT). Port forwarding may not be possible. Contact your ISP."

---

## 9. Transport Protocol Selection

### Priority order (auto mode):

When `transport_mode` is `"auto"`, attempt protocols in this order at session start:

```
1. WebRTC MediaTrack  — ICE over UDP   (preferred, lowest latency)
2. WebRTC DataChannel — ICE over UDP
3. WebRTC MediaTrack  — ICE over TCP
4. WebRTC DataChannel — ICE over TCP
5. WSS (WebSocket Secure fallback)
```

### Protocol selection logic:
- Before committing to a transport, validate SDP negotiation:
  - Check that the selected video/audio codec is listed in the browser's `RTCRtpReceiver.getCapabilities()`.
  - If codec not supported → skip to next transport option.
- Store the selected transport in session state for diagnostics.

### ICE configuration:
```javascript
const iceConfig = {
  iceServers: [
    { urls: "stun:stun.l.google.com:19302" },
    { urls: "stun:stun.cloudflare.com:3478" },
    { urls: "stun:stun.nextcloud.com:443" },
    { urls: "stun:relay.metered.ca:80" }
  ],
  iceTransportPolicy: "all",   // accept both UDP and TCP candidates
  bundlePolicy: "max-bundle",
  rtcpMuxPolicy: "require"
};
```

### ICE candidate prioritization:
- Filter and sort ICE candidates: UDP host > UDP srflx > TCP host > TCP srflx.
- Do NOT add any TURN candidates (no TURN, direct only).
- Force UDP candidates first in SDP before sending offer:
```javascript
// Sort candidates: udp before tcp
sdp = sdp.replace(/a=candidate:.*\r\n/g, (match) => match)
  // reorder: put udp lines before tcp lines in candidate block
```

### Force ICE policy on browser side:
```javascript
// Accept both UDP and TCP, no relay
pc.setConfiguration({ iceTransportPolicy: "all" });
```

---

## 10. WebRTC MediaTrack Low-Latency Optimizations

Apply the following SDP and API optimizations when using WebRTC MediaTrack:

### SDP transformations (apply to offer before setLocalDescription):
```javascript
let sdp = offer.sdp;

// Direction: receive only (client is viewer)
sdp = sdp.replace(/a=sendrecv/g, 'a=recvonly');

// Reduce packetization time → less audio/video buffering
sdp = sdp.replace(/a=ptime:\d+/g, 'a=ptime:10');

// Remove FEC (optional — trades reliability for latency)
sdp = sdp.replace(/ulpfec/g, '');

// Keep profile-level-id as-is (do not downgrade codec profile)
```

### RTCRtpReceiver / jitter buffer:
```javascript
// Set jitter buffer target to 0 for minimum latency
const receivers = pc.getReceivers();
for (const receiver of receivers) {
  if (receiver.jitterBufferTarget !== undefined) {
    receiver.jitterBufferTarget = 0;
  }
}
```

### RTCPeerConnection constraints:
```javascript
const offerOptions = {
  offerToReceiveVideo: true,
  offerToReceiveAudio: true,
};
```

### Additional low-latency hints via RTCRtpSender (if applicable):
```javascript
const params = sender.getParameters();
params.encodings[0].networkPriority = "high";
params.encodings[0].priority = "high";
await sender.setParameters(params);
```

---

## 11. Admin UI Additions

In the settings/admin panel, add:

- **Internet Access** toggle (reflects `internet_access_enabled`)
- **Domain** display: `moonlightweb-{uniqueId}.dedyn.io` (read-only)
- **Public IP** field: editable, with "Auto-detect" toggle (`auto_ip_detection`)
- **Transport Mode** selector: Auto / force specific protocol
- **UPnP Status** indicator: OK / Failed + manual port forwarding instructions if failed
- **TLS Certificate** status: expiry date + "Renew now" button
- **DNS Status** indicator: last check timestamp + resolved IP

---

## 12. Security Notes

- The deSEC API token must be stored encrypted at rest (not in plain text config).
- Never log the deSEC token in application logs.
- TLS certificate private key must have restricted file permissions (600).
- Public IP is shared with deSEC only — no other third party.

---

## Out of scope for this goal

- TURN server support (explicitly excluded — direct connection only)
- China-specific routing
- Multi-user / multi-instance coordination
