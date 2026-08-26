// MoonlightWeb — session census: how the fleet actually streams.
//
// What this answers
// -----------------
// The version census (update.go) says which builds are alive. It says nothing
// about how they are used, so questions like "is anyone still streaming 720p?",
// "did AV1 take off?", "how long does a session last?" have no data behind them
// and every product decision is a guess. This endpoint collects the answer:
// each instance reports the shape of a session when it starts and how long it
// lasted when it ends.
//
// What is deliberately NOT collected
// ----------------------------------
// Nothing that identifies a person, a machine or what they were doing:
//
//   - no host name, host UUID, account, session token or pairing identity;
//   - no application or game id — what someone plays is their business;
//   - no address is stored anywhere: Umami hashes address+UA behind a salt that
//     rotates daily, which is the whole reason this reports through Umami
//     rather than into a table of our own;
//   - no free-text field of any kind. Every value below is matched against an
//     allowlist or collapsed into a bucket, so the dashboard can only ever show
//     strings chosen here, and a forged client cannot smuggle anything in.
//
// The instance decides whether to report at all (settings.json
// session_metrics_enabled, or MW_NO_TELEMETRY in its environment), and a
// self-built binary has no key for this endpoint in the first place.
//
// How to read it in Umami
// -----------------------
// A session start is a PAGEVIEW at /s/{height}p{fps}, so with a website of its
// own the built-in reports need no configuration:
//
//	Overview → Views      sessions per day
//	Overview → Visitors   distinct instances that streamed (daily hash)
//	Pages → Path          the resolution/frame-rate histogram
//	Pages → URL           the same, split by codec/backend/transport/…
//	Events → dur-*        how long sessions lasted
//	Events → launch-failed  attempts that never reached a picture
package main

import (
	"crypto/subtle"
	"encoding/json"
	"io"
	"net/http"
	"strconv"
)

// ── Configuration ───────────────────────────────────────────────────────────

type sessionConfig struct {
	umamiURL     string // MW_UMAMI_URL                    ("" disables)
	umamiWebsite string // MW_UMAMI_SESSIONS_WEBSITE_ID    ("" disables)
	hostname     string // "metrics.{domain}" — the hostname Umami records
}

// ── What a client may say ───────────────────────────────────────────────────

// sessionReport is the request body. Every field is advisory: anything missing,
// unknown or out of range lands in a catch-all bucket rather than being
// rejected, so an older or newer client never fails to report.
type sessionReport struct {
	Event string `json:"event"` // "start" | "end" | "failed"

	// Which build is reporting (same tokens as the version census).
	Version string `json:"v"`
	OS      string `json:"os"`
	Arch    string `json:"arch"`

	// The shape of the stream.
	Height  int    `json:"h"`
	Fps     int    `json:"fps"`
	Codec   string `json:"codec"` // negotiated, not requested
	Hdr     bool   `json:"hdr"`
	Yuv444  bool   `json:"yuv444"`
	Bitrate int    `json:"bitrate"` // kbps
	Backend string `json:"backend"`

	// How it was carried and by whom.
	Transport string `json:"transport"` // webrtc-dc-udp, wss, …
	Net       string `json:"net"`       // loopback | private | tunnel | public
	Client    string `json:"client"`    // desktop | mobile | tablet | tv
	Kind      string `json:"kind"`      // owner | player

	Seconds int `json:"seconds"` // "end" only — how long it lasted
	Code    int `json:"code"`    // "failed" only — the HTTP status the launch got
}

var (
	knownCodec = map[string]bool{"h264": true, "hevc": true, "av1": true}
	// gamestream covers Sunshine/Apollo/GFE, which serverinfo genuinely cannot
	// tell apart (see docs/wiki — the probe only distinguishes what it can).
	knownBackend = map[string]bool{
		"gamestream": true, "wolf": true, "multiseat": true, "gfe": true,
	}
	knownTransport = map[string]bool{
		"webrtc-dc-udp": true, "webrtc-dc-tcp": true,
		"webrtc-media-udp": true, "webrtc-media-tcp": true, "wss": true,
	}
	knownNet = map[string]bool{
		"loopback": true, "private": true, "tunnel": true, "public": true,
	}
	knownClient = map[string]bool{
		"desktop": true, "mobile": true, "tablet": true, "tv": true,
	}
	knownKind = map[string]bool{"owner": true, "player": true}

	// Heights and frame rates the UI can actually produce. Anything else is a
	// hand-edited setting and belongs in "other" rather than in a row of its own.
	knownHeight = map[int]bool{
		360: true, 480: true, 540: true, 600: true, 720: true, 768: true,
		900: true, 1080: true, 1200: true, 1440: true, 1600: true, 2160: true,
	}
	knownFps = map[int]bool{
		24: true, 30: true, 45: true, 50: true, 60: true, 75: true,
		90: true, 100: true, 120: true, 144: true, 165: true, 240: true,
	}
)

// cleanInt keeps a value only if it is one the UI offers; everything else
// becomes "other". Buckets, not clamps: a stray 1081 must not invent a row.
func cleanInt(v int, known map[int]bool) string {
	if known[v] {
		return strconv.Itoa(v)
	}
	return "other"
}

func boolToken(b bool) string {
	if b {
		return "1"
	}
	return "0"
}

// bitrateBucket collapses kbps into the ranges the quality ladder moves
// between. The exact number is never useful and would be a near-unique value
// per session once the ladder starts adapting.
func bitrateBucket(kbps int) string {
	switch {
	case kbps <= 0:
		return "unknown"
	case kbps < 5000:
		return "0-5m"
	case kbps < 10000:
		return "5-10m"
	case kbps < 20000:
		return "10-20m"
	case kbps < 30000:
		return "20-30m"
	case kbps < 50000:
		return "30-50m"
	default:
		return "50m+"
	}
}

// durationBucket is the event NAME for a finished session, so the Events panel
// is the duration histogram with nothing to configure. Ordered lexically on
// purpose — Umami sorts by count, but a tie should still read sensibly.
func durationBucket(seconds int) string {
	switch {
	case seconds < 0:
		return "dur-unknown"
	case seconds < 60:
		return "dur-0-1m"
	case seconds < 300:
		return "dur-1-5m"
	case seconds < 900:
		return "dur-5-15m"
	case seconds < 1800:
		return "dur-15-30m"
	case seconds < 3600:
		return "dur-30-60m"
	case seconds < 7200:
		return "dur-1-2h"
	case seconds < 14400:
		return "dur-2-4h"
	default:
		return "dur-4h+"
	}
}

// statusBucket keeps launch failures to the handful of codes the backend can
// answer with, so a client cannot turn this into a free integer field.
func statusBucket(code int) string {
	switch code {
	case 400, 401, 403, 404, 500, 502, 503, 504:
		return strconv.Itoa(code)
	case 0:
		return "none"
	default:
		return "other"
	}
}

// ── Sanitised view ──────────────────────────────────────────────────────────

// facts is a report with every field already reduced to an allowlisted token.
// Nothing past this point can carry a client-supplied string.
type facts struct {
	version, os, arch               string
	height, fps                     string
	codec, hdr, yuv444, bitrate     string
	backend, transport, net, client string
	kind                            string
}

func clean(r sessionReport) facts {
	return facts{
		version:   cleanVersion(r.Version),
		os:        cleanToken(r.OS, knownOS),
		arch:      cleanToken(r.Arch, knownArch),
		height:    cleanInt(r.Height, knownHeight),
		fps:       cleanInt(r.Fps, knownFps),
		codec:     cleanToken(r.Codec, knownCodec),
		hdr:       boolToken(r.Hdr),
		yuv444:    boolToken(r.Yuv444),
		bitrate:   bitrateBucket(r.Bitrate),
		backend:   cleanToken(r.Backend, knownBackend),
		transport: cleanToken(r.Transport, knownTransport),
		net:       cleanToken(r.Net, knownNet),
		client:    cleanToken(r.Client, knownClient),
		kind:      cleanToken(r.Kind, knownKind),
	}
}

// url is the address recorded for the session. The path is the one thing worth
// a histogram of its own — resolution and frame rate, which is what the Pages
// panel then shows without any configuration — and the query carries the rest,
// which the same panel's URL tab keeps separate when the detail is wanted.
func (f facts) url() string {
	// One bucket rather than "/s/otherpother": a hand-edited resolution is
	// worth a row saying "not one of ours", not a row that reads like a bug.
	// The two values survive individually in data() either way.
	path := "/s/other"
	if f.height != "other" && f.fps != "other" {
		path = "/s/" + f.height + "p" + f.fps
	}
	return path +
		"?codec=" + f.codec +
		"&hdr=" + f.hdr +
		"&bitrate=" + f.bitrate +
		"&backend=" + f.backend +
		"&transport=" + f.transport +
		"&net=" + f.net +
		"&client=" + f.client +
		"&kind=" + f.kind
}

// data is the same information as key/value properties, for the reports that
// can break down by property rather than by URL.
func (f facts) data() map[string]string {
	return map[string]string{
		"version": f.version, "os": f.os, "arch": f.arch,
		"height": f.height, "fps": f.fps, "codec": f.codec,
		"hdr": f.hdr, "yuv444": f.yuv444, "bitrate": f.bitrate,
		"backend": f.backend, "transport": f.transport,
		"net": f.net, "client": f.client, "kind": f.kind,
	}
}

// userAgent is what Umami attributes the record to. Same construction as the
// version census: half the visitor hash, and it carries the version so an
// instance that upgrades is not counted under its old one for another month.
func (f facts) userAgent() string {
	return "MoonlightWeb/" + f.version + " (" + f.os + "; " + f.arch + ")"
}

// ── HTTP handler ────────────────────────────────────────────────────────────

type sessionSink struct {
	rep    *umamiReporter
	apiKey string
}

func newSessionSink(cfg sessionConfig, apiKey string) *sessionSink {
	rep := newUmamiReporter(umamiConfig{
		url:      cfg.umamiURL,
		website:  cfg.umamiWebsite,
		hostname: cfg.hostname,
	}, "session", nil)
	return &sessionSink{rep: rep, apiKey: apiKey}
}

// ServeHTTP answers POST /v1/session with 204, always — including when
// reporting is switched off here. The instance must never learn anything about
// our configuration from this endpoint, and must never treat a census failure
// as a streaming failure.
func (s *sessionSink) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeErr(w, http.StatusMethodNotAllowed, "only POST is allowed")
		return
	}
	// The same restricted key as every other client-facing path here. Embedded
	// in official builds, so it gates stray scanners rather than keeping a
	// secret — see update.go.
	if subtle.ConstantTimeCompare([]byte(r.Header.Get("X-API-Key")), []byte(s.apiKey)) != 1 {
		writeErr(w, http.StatusUnauthorized, "invalid API key")
		return
	}

	// 4 KiB is an order of magnitude above the real body and small enough that
	// a hostile client cannot make us read anything worth reading.
	body, err := io.ReadAll(io.LimitReader(r.Body, 4096))
	if err != nil {
		writeErr(w, http.StatusBadRequest, "unreadable body")
		return
	}
	var rep sessionReport
	if err := json.Unmarshal(body, &rep); err != nil {
		writeErr(w, http.StatusBadRequest, "malformed body")
		return
	}

	f := clean(rep)
	ip := clientIP(r)
	switch rep.Event {
	case "start":
		// No event name → a pageview. This is the record that makes Views the
		// count of sessions and Pages the resolution histogram.
		s.rep.send("", f.url(), f.userAgent(), ip, f.data())
	case "end":
		d := f.data()
		d["duration"] = durationBucket(rep.Seconds)
		s.rep.send(durationBucket(rep.Seconds), f.url(), f.userAgent(), ip, d)
	case "failed":
		d := f.data()
		d["code"] = statusBucket(rep.Code)
		s.rep.send("launch-failed", f.url(), f.userAgent(), ip, d)
	default:
		// Unknown verb from a future client: accepted and ignored, never an
		// error the instance would have to handle.
	}

	w.WriteHeader(http.StatusNoContent)
}
