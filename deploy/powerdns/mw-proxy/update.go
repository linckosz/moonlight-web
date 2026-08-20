// MoonlightWeb — update relay: a cached mirror of the project's latest GitHub
// release, doubling as the source of the installed-version census.
//
// Why this lives here
// -------------------
// Every instance already asks GitHub "is there a newer MoonlightWeb?" every few
// hours (backend/src/network/UpdateChecker.cpp). That request tells GitHub which
// versions are alive and tells us nothing, so version-migration planning has no
// data behind it. Pointing the same check at updates.{zone} changes only its
// destination: the instance sends the version/OS/arch it already knows, we hand
// back the very same GitHub JSON it would have fetched itself (so the client
// parser is untouched), and we count the answer.
//
// Three properties this must keep:
//
//  1. Never become a single point of failure for updates. The client falls back
//     to api.github.com on any error, and this relay serves a stale cached
//     release rather than an error whenever GitHub is unreachable.
//  2. Never fan out to GitHub. One fetch per cacheTTL window feeds the whole
//     fleet, well inside the 60 req/h unauthenticated budget.
//  3. Never store what a client sends. Version/OS/arch are matched against
//     allowlists and collapsed to "unknown"/"other" before going anywhere;
//     nothing is written to disk here — the counting happens in Umami, which
//     buckets by a daily-rotating hash and keeps no raw address.
package main

import (
	"bytes"
	"context"
	"crypto/subtle"
	"encoding/json"
	"errors"
	"io"
	"log"
	"net/http"
	"regexp"
	"strings"
	"sync"
	"time"
)

// ── Configuration ───────────────────────────────────────────────────────────

type updateConfig struct {
	releaseURL   string        // MW_GITHUB_RELEASE_URL   (default: this project's latest release)
	cacheTTL     time.Duration // MW_UPDATE_CACHE_SECONDS (default 600)
	umamiURL     string        // MW_UMAMI_URL            (default http://umami:3000; "" disables)
	umamiWebsite string        // MW_UMAMI_WEBSITE_ID     ("" disables reporting entirely)
	hostname     string        // "updates.{domain}" — the hostname Umami records
}

const defaultReleaseURL = "https://api.github.com/repos/linckosz/moonlight-web/releases/latest"

// ── Client-supplied field validation ────────────────────────────────────────
//
// Anything below reaches an analytics dashboard, so it is matched, not escaped:
// a value that does not match becomes a constant. That bounds cardinality (a
// forged client cannot invent a million distinct "versions" and drown the real
// ones) and guarantees the dashboard only ever shows strings we chose.

var versionRe = regexp.MustCompile(`^[0-9]{1,4}(\.[0-9]{1,4}){0,3}(-[0-9A-Za-z.]{1,24})?$`)

// Canonical platform tokens the app sends. UpdateChecker builds them from Q_OS_*
// macros and QSysInfo, so they are compile-time constants rather than strings
// scraped off the running system.
var knownOS = map[string]bool{"windows": true, "macos": true, "linux": true}
var knownArch = map[string]bool{"x64": true, "arm64": true}

func cleanVersion(v string) string {
	v = strings.TrimPrefix(strings.TrimSpace(v), "v")
	if versionRe.MatchString(v) {
		return v
	}
	return "unknown"
}

func cleanToken(v string, known map[string]bool) string {
	v = strings.ToLower(strings.TrimSpace(v))
	if known[v] {
		return v
	}
	return "other"
}

// ── Cached GitHub release ───────────────────────────────────────────────────

var errUpstream = errors.New("upstream release fetch failed")

type updateRelay struct {
	cfg    updateConfig
	apiKey string // clients present the restricted MW_PDNS_PROXY_KEY, as on the DNS path
	client *http.Client

	mu        sync.Mutex
	body      []byte        // last good release JSON (nil until the first success)
	fetchedAt time.Time     // when body was refreshed
	inflight  chan struct{} // non-nil while a fetch runs; closed when it finishes

	// Bounds concurrent analytics posts so a burst of clients cannot spawn an
	// unbounded number of goroutines. Reporting is best-effort: when the
	// semaphore is full the event is dropped, never queued.
	reportSlots chan struct{}
}

func newUpdateRelay(cfg updateConfig, apiKey string) *updateRelay {
	if cfg.releaseURL == "" {
		cfg.releaseURL = defaultReleaseURL
	}
	if cfg.cacheTTL <= 0 {
		cfg.cacheTTL = 10 * time.Minute
	}
	return &updateRelay{
		cfg:         cfg,
		apiKey:      apiKey,
		client:      &http.Client{Timeout: 15 * time.Second},
		reportSlots: make(chan struct{}, 32),
	}
}

// release returns the cached release JSON, refreshing it in the background when
// stale. A warm cache never blocks: a stale body is served immediately while the
// refresh runs (stale-while-revalidate). Only a cold cache waits for GitHub.
func (u *updateRelay) release(ctx context.Context) ([]byte, error) {
	u.mu.Lock()
	body := u.body
	if body != nil && time.Since(u.fetchedAt) <= u.cfg.cacheTTL {
		u.mu.Unlock()
		return body, nil
	}
	if u.inflight == nil {
		u.inflight = make(chan struct{})
		go u.fetch()
	}
	done := u.inflight
	u.mu.Unlock()

	if body != nil {
		return body, nil // stale, but a real answer beats an error
	}

	select {
	case <-done:
		u.mu.Lock()
		body = u.body
		u.mu.Unlock()
		if body == nil {
			return nil, errUpstream
		}
		return body, nil
	case <-ctx.Done():
		return nil, ctx.Err()
	}
}

// fetch refreshes the cache once. It always closes (and clears) the inflight
// channel, success or not, so a failed fetch releases every waiter and the next
// request is free to try again.
func (u *updateRelay) fetch() {
	var body []byte
	defer func() {
		u.mu.Lock()
		if body != nil {
			u.body = body
			u.fetchedAt = time.Now()
		}
		close(u.inflight)
		u.inflight = nil
		u.mu.Unlock()
	}()

	ctx, cancel := context.WithTimeout(context.Background(), 12*time.Second)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, u.cfg.releaseURL, nil)
	if err != nil {
		log.Printf("[mw-proxy] update: bad release URL %q: %v", u.cfg.releaseURL, err)
		return
	}
	// GitHub rejects requests without a User-Agent; Accept pins the v3 media type.
	req.Header.Set("User-Agent", "MoonlightWeb-UpdateRelay")
	req.Header.Set("Accept", "application/vnd.github+json")

	resp, err := u.client.Do(req)
	if err != nil {
		log.Printf("[mw-proxy] update: release fetch failed: %v", err)
		return
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		log.Printf("[mw-proxy] update: release fetch returned %d", resp.StatusCode)
		return
	}
	// 1 MiB is far above a release payload (~30 KiB) and far below anything that
	// could pressure memory if the upstream ever misbehaves.
	data, err := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	if err != nil || !json.Valid(data) {
		log.Printf("[mw-proxy] update: release payload unusable (err=%v)", err)
		return
	}
	body = data
}

// ── HTTP handler ────────────────────────────────────────────────────────────

// ServeHTTP answers GET /v1/update?v=&os=&arch= with the upstream release JSON,
// verbatim, so the client's existing parser needs no change.
func (u *updateRelay) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		writeErr(w, http.StatusMethodNotAllowed, "only GET is allowed")
		return
	}
	// Same restricted key as the DNS path. It is embedded in the official
	// builds, so this is a gate against stray scanners, not a secret: a
	// self-built instance simply has no key and checks GitHub directly.
	presented := r.Header.Get("X-API-Key")
	if subtle.ConstantTimeCompare([]byte(presented), []byte(u.apiKey)) != 1 {
		writeErr(w, http.StatusUnauthorized, "invalid API key")
		return
	}

	body, err := u.release(r.Context())
	if err != nil {
		// The client falls back to api.github.com on this, so an outage here
		// costs a round trip, not an update.
		writeErr(w, http.StatusBadGateway, "release information unavailable")
		return
	}

	q := r.URL.Query()
	u.report(cleanVersion(q.Get("v")), cleanToken(q.Get("os"), knownOS),
		cleanToken(q.Get("arch"), knownArch), clientIP(r))

	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "public, max-age=300")
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write(body)
}

// ── Version census (Umami) ──────────────────────────────────────────────────

// Deliberately without a "name" field: in Umami, a payload carrying a name is a
// custom EVENT (it shows up under Events and leaves Visitors/Views/Pages at
// zero), while a nameless one is a PAGEVIEW. We want the pageview — that is
// what turns the built-in Pages report into the version histogram.
type umamiPayload struct {
	Website  string            `json:"website"`
	Hostname string            `json:"hostname"`
	URL      string            `json:"url"`
	Data     map[string]string `json:"data"`
}

type umamiEvent struct {
	Type    string       `json:"type"`
	Payload umamiPayload `json:"payload"`
}

// eventURL is the URL recorded for one check: the version as the *path*, the
// platform as the *query string*. That split is what makes both built-in
// reports useful without any dashboard configuration — Umami's Pages panel has
// a "Path" tab, which collapses every platform into one row per version (the
// version histogram), and a "URL" tab, which keeps them apart when you want the
// OS/arch breakdown. Values are allowlisted upstream, so no escaping is needed.
func eventURL(version, platform, arch string) string {
	return "/uc/" + version + "?os=" + platform + "&arch=" + arch
}

// report records one check as an Umami event, fire-and-forget.
func (u *updateRelay) report(version, platform, arch, ip string) {
	if u.cfg.umamiURL == "" || u.cfg.umamiWebsite == "" {
		return // analytics not configured — the relay still serves updates
	}
	select {
	case u.reportSlots <- struct{}{}:
	default:
		return // saturated: drop rather than pile up
	}

	go func() {
		defer func() { <-u.reportSlots }()

		event := umamiEvent{
			Type: "event",
			Payload: umamiPayload{
				Website:  u.cfg.umamiWebsite,
				Hostname: u.cfg.hostname,
				URL:      eventURL(version, platform, arch),
				Data:     map[string]string{"version": version, "os": platform, "arch": arch},
			},
		}
		body, err := json.Marshal(event)
		if err != nil {
			return
		}

		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		req, err := http.NewRequestWithContext(ctx, http.MethodPost,
			u.cfg.umamiURL+"/api/send", bytes.NewReader(body))
		if err != nil {
			return
		}
		req.Header.Set("Content-Type", "application/json")
		// Umami rejects a request with no User-Agent, and derives the visitor
		// hash from UA + address. An honest UA carrying the version keeps a
		// machine that upgrades from being counted under its old version.
		req.Header.Set("User-Agent", "MoonlightWeb/"+version+" ("+platform+"; "+arch+")")
		// Umami runs with CLIENT_IP_HEADER=x-forwarded-for, so it hashes the
		// instance's address rather than this container's — without which the
		// whole fleet would collapse into a single "visitor".
		req.Header.Set("X-Forwarded-For", ip)

		resp, err := u.client.Do(req)
		if err != nil {
			log.Printf("[mw-proxy] update: analytics post failed: %v", err)
			return
		}
		defer resp.Body.Close()
		_, _ = io.Copy(io.Discard, io.LimitReader(resp.Body, 4096))
		if resp.StatusCode >= 400 {
			log.Printf("[mw-proxy] update: analytics rejected the event (%d)", resp.StatusCode)
		}
	}()
}
