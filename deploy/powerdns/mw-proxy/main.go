// MoonlightWeb — mw-proxy: a least-privilege filtering gateway in front of the
// PowerDNS REST API.
//
// Why this exists
// ---------------
// A PowerDNS API key grants broad control (delete zones, rewrite NS/SOA, disable
// DNSSEC, touch any zone). MoonlightWeb instances only ever need to
// create/update/delete their own A record plus two TXT records (_owner ownership
// marker and _acme-challenge for DNS-01), so they are granted DNS access on a
// least-privilege basis and never hold full PowerDNS credentials.
//
// mw-proxy holds the real PowerDNS key server-side only. Clients authenticate
// with a SEPARATE restricted key (MW_PDNS_PROXY_KEY) and mw-proxy:
//
//  1. accepts only GET/PATCH on the single configured zone;
//  2. accepts only well-formed record names/types the app actually produces:
//     {uid}.{zone}                A     (uid = 8 lowercase hex chars)
//     _owner.{uid}.{zone}         TXT
//     _acme-challenge.{uid}.{zone} TXT
//     — every other name/type/path/method is rejected with 403;
//  3. requires a rrset_name filter on GET (so nobody can dump the whole zone
//     and harvest every uid);
//  4. enforces per-instance ownership server-side (Trust-On-First-Use): the
//     first writer of a uid presents an owner token in X-MW-Owner; mw-proxy
//     stores HMAC_S(token) keyed by uid and requires a matching token on every
//     later write. The token is never published in DNS, so it cannot be
//     harvested and replayed. One uid per owner is enforced;
//  5. rate-limits NEW claims per client IP to bound bulk creation;
//  6. forwards allowed requests to PowerDNS with the real key injected.
//
// It is deliberately dependency-free (Go stdlib only) so it stays easy to audit.
package main

import (
	"bytes"
	"crypto/hmac"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"log"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"time"
)

// ── Configuration (env-driven) ──────────────────────────────────────────────

type config struct {
	listen       string // MW_PROXY_LISTEN            (default ":8080")
	upstream     string // MW_PDNS_UPSTREAM           (default "http://pdns:8081")
	zone         string // derived from MW_DOMAIN, with trailing dot
	realKey      string // MW_PDNS_API_KEY            (the full PowerDNS key)
	proxyKey     string // MW_PDNS_PROXY_KEY          (restricted key clients present)
	ownerSecret  []byte // MW_PDNS_OWNER_SECRET       (HMAC key for the owner store)
	storePath    string // MW_PROXY_STORE             (default "/data/owners.json")
	maxNewPerHr  int    // MW_PROXY_MAX_NEW_PER_HOUR  (default 20; 0 = unlimited)
	maxBodyBytes int64  // MW_PROXY_MAX_BODY_BYTES    (default 64 KiB)

	// The update relay served on /v1/update — see update.go.
	update updateConfig

	// The session census served on /v1/session — see session.go.
	session sessionConfig
}

func mustEnv(name string) string {
	v := os.Getenv(name)
	if v == "" {
		log.Fatalf("[mw-proxy] required environment variable %s is empty", name)
	}
	return v
}

func envOr(name, def string) string {
	if v := os.Getenv(name); v != "" {
		return v
	}
	return def
}

func loadConfig() config {
	domain := strings.ToLower(strings.TrimSuffix(mustEnv("MW_DOMAIN"), "."))
	c := config{
		listen:       envOr("MW_PROXY_LISTEN", ":8080"),
		upstream:     strings.TrimRight(envOr("MW_PDNS_UPSTREAM", "http://pdns:8081"), "/"),
		zone:         domain + ".",
		realKey:      mustEnv("MW_PDNS_API_KEY"),
		proxyKey:     mustEnv("MW_PDNS_PROXY_KEY"),
		ownerSecret:  []byte(mustEnv("MW_PDNS_OWNER_SECRET")),
		storePath:    envOr("MW_PROXY_STORE", "/data/owners.json"),
		maxNewPerHr:  20,
		maxBodyBytes: 64 * 1024,
		// Analytics stays optional: an empty website id (the default in
		// .env.sample) leaves the relay serving updates and counting nothing.
		update: updateConfig{
			releaseURL:   envOr("MW_GITHUB_RELEASE_URL", defaultReleaseURL),
			cacheTTL:     10 * time.Minute,
			umamiURL:     strings.TrimRight(envOr("MW_UMAMI_URL", "http://umami:3000"), "/"),
			umamiWebsite: os.Getenv("MW_UMAMI_WEBSITE_ID"),
			hostname:     "updates." + domain,
		},
		// A website of its OWN, not the version census's: sessions and update
		// checks are different populations counted at different rates, and
		// mixing them would make the headline "views" number mean nothing.
		session: sessionConfig{
			umamiURL:     strings.TrimRight(envOr("MW_UMAMI_URL", "http://umami:3000"), "/"),
			umamiWebsite: os.Getenv("MW_UMAMI_SESSIONS_WEBSITE_ID"),
			hostname:     "metrics." + domain,
		},
	}
	if v := os.Getenv("MW_PROXY_MAX_NEW_PER_HOUR"); v != "" {
		if n, err := strconv.Atoi(v); err == nil && n >= 0 {
			c.maxNewPerHr = n
		}
	}
	if v := os.Getenv("MW_UPDATE_CACHE_SECONDS"); v != "" {
		if n, err := strconv.Atoi(v); err == nil && n > 0 {
			c.update.cacheTTL = time.Duration(n) * time.Second
		}
	}
	return c
}

// ── Record-name validation ──────────────────────────────────────────────────

type recordKind int

const (
	kindA     recordKind = iota // {uid}.{zone}                 A
	kindOwner                   // _owner.{uid}.{zone}          TXT
	kindAcme                    // _acme-challenge.{uid}.{zone} TXT
)

type nameMatchers struct {
	a     *regexp.Regexp
	owner *regexp.Regexp
	acme  *regexp.Regexp
}

func buildMatchers(zone string) nameMatchers {
	z := regexp.QuoteMeta(zone)
	return nameMatchers{
		a:     regexp.MustCompile(`^([0-9a-f]{8})\.` + z + `$`),
		owner: regexp.MustCompile(`^_owner\.([0-9a-f]{8})\.` + z + `$`),
		acme:  regexp.MustCompile(`^_acme-challenge\.([0-9a-f]{8})\.` + z + `$`),
	}
}

// classify returns the uid and kind for an allowed name, or ok=false. The 8-hex
// uid constraint mirrors the app's generateUniqueId() and automatically excludes
// every reserved label (www/api/stats/stream/ns1/ns2/mail/apex and anything
// starting with '_'), since none of them are 8 hex characters.
func (m nameMatchers) classify(name string) (uid string, kind recordKind, ok bool) {
	n := strings.ToLower(name)
	if g := m.a.FindStringSubmatch(n); g != nil {
		return g[1], kindA, true
	}
	if g := m.owner.FindStringSubmatch(n); g != nil {
		return g[1], kindOwner, true
	}
	if g := m.acme.FindStringSubmatch(n); g != nil {
		return g[1], kindAcme, true
	}
	return "", 0, false
}

// typeAllowed checks that the rrset type matches the record kind.
func typeAllowed(kind recordKind, rrType string) bool {
	switch kind {
	case kindA:
		return rrType == "A"
	case kindOwner, kindAcme:
		return rrType == "TXT"
	}
	return false
}

// ── Ownership store (uid → HMAC_S(owner token)) ─────────────────────────────

type ownerStore struct {
	mu     sync.Mutex
	path   string
	secret []byte
	owners map[string]string // uid → hex(HMAC_S(token))
}

func newOwnerStore(path string, secret []byte) (*ownerStore, error) {
	s := &ownerStore{path: path, secret: secret, owners: map[string]string{}}
	data, err := os.ReadFile(path)
	if errors.Is(err, os.ErrNotExist) {
		return s, nil
	}
	if err != nil {
		return nil, err
	}
	var on struct {
		Owners map[string]string `json:"owners"`
	}
	if err := json.Unmarshal(data, &on); err != nil {
		return nil, err
	}
	if on.Owners != nil {
		s.owners = on.Owners
	}
	return s, nil
}

// hash derives the non-reversible owner fingerprint stored on disk. Keeping only
// HMAC_S(token) means a leak of the store file reveals no usable credential.
func (s *ownerStore) hash(token string) string {
	mac := hmac.New(sha256.New, s.secret)
	mac.Write([]byte(token))
	return hex.EncodeToString(mac.Sum(nil))
}

// persist writes the store atomically (temp file + rename). Caller holds s.mu.
func (s *ownerStore) persist() error {
	if err := os.MkdirAll(filepath.Dir(s.path), 0o700); err != nil {
		return err
	}
	body, err := json.MarshalIndent(struct {
		Owners map[string]string `json:"owners"`
	}{s.owners}, "", "  ")
	if err != nil {
		return err
	}
	tmp := s.path + ".tmp"
	if err := os.WriteFile(tmp, body, 0o600); err != nil {
		return err
	}
	return os.Rename(tmp, s.path)
}

// ── Per-IP new-claim quota (sliding 1h window) ──────────────────────────────

// How often, and above which size, the whole map is swept. Time-based alone is
// enough in practice — every entry costs the client a TLS connection through
// Caddy, so addresses cannot be conjured — but the size trigger keeps a burst of
// many distinct addresses inside one interval from mattering either.
const (
	quotaSweepEvery   = 15 * time.Minute
	quotaSweepEntries = 4096
)

type quota struct {
	mu        sync.Mutex
	max       int
	window    time.Duration
	hits      map[string][]time.Time
	lastSweep time.Time
}

func newQuota(max int) *quota {
	return &quota{max: max, window: time.Hour, hits: map[string][]time.Time{}, lastSweep: time.Now()}
}

// sweep drops every address whose recorded attempts have all aged out of the
// window. Without it the map is append-only: one entry per distinct address,
// kept for the life of the process, and pruned only if that same address ever
// comes back. Caller holds q.mu.
func (q *quota) sweep(now time.Time) {
	cut := now.Add(-q.window)
	for ip, ts := range q.hits {
		live := ts[:0]
		for _, t := range ts {
			if t.After(cut) {
				live = append(live, t)
			}
		}
		if len(live) == 0 {
			delete(q.hits, ip)
			continue
		}
		q.hits[ip] = live
	}
	q.lastSweep = now
}

// allow records a new-claim attempt for ip and reports whether it is within the
// window budget. A max of 0 disables the quota.
func (q *quota) allow(ip string) bool {
	if q.max <= 0 {
		return true
	}
	q.mu.Lock()
	defer q.mu.Unlock()
	now := time.Now()
	if now.Sub(q.lastSweep) >= quotaSweepEvery || len(q.hits) >= quotaSweepEntries {
		q.sweep(now)
	}
	cut := now.Add(-q.window)
	kept := q.hits[ip][:0]
	for _, t := range q.hits[ip] {
		if t.After(cut) {
			kept = append(kept, t)
		}
	}
	if len(kept) >= q.max {
		q.hits[ip] = kept
		return false
	}
	q.hits[ip] = append(kept, now)
	return true
}

// ── PowerDNS PATCH body shapes ──────────────────────────────────────────────

type pdnsRRSet struct {
	Name       string `json:"name"`
	Type       string `json:"type"`
	ChangeType string `json:"changetype"`
}

type pdnsPatch struct {
	RRSets []pdnsRRSet `json:"rrsets"`
}

// ── Server ──────────────────────────────────────────────────────────────────

type proxy struct {
	cfg      config
	matchers nameMatchers
	store    *ownerStore
	quota    *quota
	client   *http.Client
	zonePath string // /api/v1/servers/localhost/zones/{zone}
}

func newProxy(cfg config, store *ownerStore) *proxy {
	return &proxy{
		cfg:      cfg,
		matchers: buildMatchers(cfg.zone),
		store:    store,
		quota:    newQuota(cfg.maxNewPerHr),
		client:   &http.Client{Timeout: 20 * time.Second},
		zonePath: "/api/v1/servers/localhost/zones/" + cfg.zone,
	}
}

func writeErr(w http.ResponseWriter, status int, msg string) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(map[string]string{"error": msg})
}

func clientIP(r *http.Request) string {
	// Caddy sits in front and sets X-Forwarded-For; take the first hop.
	if xff := r.Header.Get("X-Forwarded-For"); xff != "" {
		if i := strings.IndexByte(xff, ','); i >= 0 {
			return strings.TrimSpace(xff[:i])
		}
		return strings.TrimSpace(xff)
	}
	if host, _, err := net.SplitHostPort(r.RemoteAddr); err == nil {
		return host
	}
	return r.RemoteAddr
}

func (p *proxy) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	// 1. Authenticate the client with the restricted proxy key (constant time).
	presented := r.Header.Get("X-API-Key")
	if subtle.ConstantTimeCompare([]byte(presented), []byte(p.cfg.proxyKey)) != 1 {
		writeErr(w, http.StatusUnauthorized, "invalid API key")
		return
	}

	// 2. Only the configured zone endpoint, only GET/PATCH.
	if r.URL.Path != p.zonePath {
		writeErr(w, http.StatusForbidden, "only the configured zone endpoint is allowed")
		return
	}
	switch r.Method {
	case http.MethodGet:
		if code, msg := p.checkGet(r); code != 0 {
			writeErr(w, code, msg)
			return
		}
		p.forward(w, r, nil)
	case http.MethodPatch:
		body, err := io.ReadAll(io.LimitReader(r.Body, p.cfg.maxBodyBytes))
		if err != nil {
			writeErr(w, http.StatusBadRequest, "cannot read body")
			return
		}
		commit, code, msg := p.checkPatch(r, body)
		if code != 0 {
			writeErr(w, code, msg)
			return
		}
		// Record ownership only once PowerDNS has actually accepted the write.
		// On a 4xx/5xx — or an upstream that never answered — nothing changed in
		// the zone, and claiming the uid anyway would strand it: the owner would
		// hold a subdomain that does not exist, and the one-uid-per-owner rule
		// would then refuse them every other one, with no record to delete to
		// get out of it.
		if status := p.forward(w, r, body); status >= 200 && status < 300 {
			commit()
		}
	default:
		writeErr(w, http.StatusMethodNotAllowed, "only GET and PATCH are allowed")
	}
}

// checkGet enforces that reads always carry a valid rrset_name filter, so the
// endpoint cannot be used to dump the entire zone (which would leak every uid).
func (p *proxy) checkGet(r *http.Request) (int, string) {
	q := r.URL.Query()
	name := q.Get("rrset_name")
	rrType := q.Get("rrset_type")
	if name == "" || rrType == "" {
		return http.StatusForbidden, "rrset_name and rrset_type are required"
	}
	uid, kind, ok := p.matchers.classify(name)
	if !ok {
		return http.StatusForbidden, "rrset_name is not an allowed record"
	}
	if !typeAllowed(kind, rrType) {
		return http.StatusForbidden, "rrset_type not allowed for this name"
	}
	_ = uid
	return 0, ""
}

// checkPatch validates every rrset and the ownership rules. It returns a commit
// closure to be run once the write has been forwarded successfully.
func (p *proxy) checkPatch(r *http.Request, body []byte) (commit func(), code int, msg string) {
	noop := func() {}
	var patch pdnsPatch
	if err := json.Unmarshal(body, &patch); err != nil {
		return noop, http.StatusBadRequest, "invalid JSON body"
	}
	if len(patch.RRSets) == 0 {
		return noop, http.StatusBadRequest, "no rrsets in request"
	}

	ownerToken := r.Header.Get("X-MW-Owner")
	ip := clientIP(r)

	p.store.mu.Lock()
	defer p.store.mu.Unlock()

	otHash := ""
	if ownerToken != "" {
		otHash = p.store.hash(ownerToken)
	}

	// Pending mutations, applied by commit() only on a successful forward.
	claims := map[string]string{} // uid → otHash
	releases := map[string]bool{} // uid → true
	newClaims := 0

	for _, rr := range patch.RRSets {
		uid, kind, ok := p.matchers.classify(rr.Name)
		if !ok {
			return noop, http.StatusForbidden, "record name not allowed: " + rr.Name
		}
		if !typeAllowed(kind, rr.Type) {
			return noop, http.StatusForbidden, "type " + rr.Type + " not allowed for " + rr.Name
		}
		ct := strings.ToUpper(rr.ChangeType)
		if ct != "REPLACE" && ct != "DELETE" {
			return noop, http.StatusForbidden, "changetype not allowed: " + rr.ChangeType
		}

		existing := p.store.owners[uid]

		if ct == "DELETE" {
			if existing == "" {
				continue // nothing owns it — idempotent delete, allow
			}
			if subtle.ConstantTimeCompare([]byte(otHash), []byte(existing)) != 1 {
				return noop, http.StatusForbidden, "not the owner of " + uid
			}
			if kind == kindA {
				releases[uid] = true // deleting the A record releases the subdomain
			}
			continue
		}

		// REPLACE (create or update).
		if existing == "" {
			if otHash == "" {
				return noop, http.StatusForbidden, "X-MW-Owner required to claim a subdomain"
			}
			if other, held := p.ownerHoldsOtherUID(otHash, uid, claims, releases); held {
				return noop, http.StatusConflict,
					"this owner already holds subdomain " + other + " — release it first"
			}
			newClaims++
			claims[uid] = otHash
		} else {
			if subtle.ConstantTimeCompare([]byte(otHash), []byte(existing)) != 1 {
				return noop, http.StatusForbidden, "not the owner of " + uid
			}
		}
	}

	if newClaims > 0 && !p.quota.allow(ip) {
		return noop, http.StatusTooManyRequests, "too many new subdomains from this address, retry later"
	}

	commit = func() {
		p.store.mu.Lock()
		defer p.store.mu.Unlock()
		changed := false
		for uid := range releases {
			if _, ok := p.store.owners[uid]; ok {
				delete(p.store.owners, uid)
				changed = true
			}
		}
		for uid, h := range claims {
			if p.store.owners[uid] != h {
				p.store.owners[uid] = h
				changed = true
			}
		}
		if changed {
			if err := p.store.persist(); err != nil {
				log.Printf("[mw-proxy] WARN failed to persist owner store: %v", err)
			}
		}
	}
	return commit, 0, ""
}

// ownerHoldsOtherUID reports whether otHash already owns a uid other than the one
// being claimed, honouring pending claims/releases within the same request.
func (p *proxy) ownerHoldsOtherUID(otHash, self string, claims map[string]string, releases map[string]bool) (string, bool) {
	for uid, h := range p.store.owners {
		if uid == self || releases[uid] {
			continue
		}
		if h == otHash {
			return uid, true
		}
	}
	for uid, h := range claims {
		if uid != self && h == otHash {
			return uid, true
		}
	}
	return "", false
}

// forward relays the (already validated) request to PowerDNS with the real key.
// It returns the status code PowerDNS answered with, or 0 when the request never
// reached it — the caller uses that to tell a real write from a refused one.
func (p *proxy) forward(w http.ResponseWriter, r *http.Request, body []byte) int {
	var reqBody io.Reader
	if body != nil {
		reqBody = bytes.NewReader(body)
	}
	target := p.cfg.upstream + r.URL.Path
	if r.URL.RawQuery != "" {
		target += "?" + r.URL.RawQuery
	}
	req, err := http.NewRequestWithContext(r.Context(), r.Method, target, reqBody)
	if err != nil {
		writeErr(w, http.StatusInternalServerError, "cannot build upstream request")
		return 0
	}
	req.Header.Set("X-API-Key", p.cfg.realKey)
	req.Header.Set("Accept", "application/json")
	if ct := r.Header.Get("Content-Type"); ct != "" {
		req.Header.Set("Content-Type", ct)
	}

	resp, err := p.client.Do(req)
	if err != nil {
		writeErr(w, http.StatusBadGateway, "upstream request failed")
		return 0
	}
	defer resp.Body.Close()

	if ct := resp.Header.Get("Content-Type"); ct != "" {
		w.Header().Set("Content-Type", ct)
	}
	w.WriteHeader(resp.StatusCode)
	_, _ = io.Copy(w, resp.Body)
	return resp.StatusCode
}

func main() {
	log.SetFlags(log.LstdFlags | log.LUTC)
	cfg := loadConfig()

	store, err := newOwnerStore(cfg.storePath, cfg.ownerSecret)
	if err != nil {
		log.Fatalf("[mw-proxy] cannot load owner store %q: %v", cfg.storePath, err)
	}

	p := newProxy(cfg, store)

	mux := http.NewServeMux()
	mux.Handle("/", p)
	// Update relay (updates.{domain} in the Caddyfile). Exact path, so it is
	// matched ahead of the catch-all DNS gateway above.
	mux.Handle("/v1/update", newUpdateRelay(cfg.update, cfg.proxyKey))
	// Say out loud whether the census is on. Silence is ambiguous here — the
	// relay serves updates identically either way, and a missing website id
	// (an .env predating the feature, a typo) otherwise looks exactly like a
	// working install until someone wonders why the dashboard stays empty.
	if cfg.update.umamiWebsite == "" || cfg.update.umamiURL == "" {
		log.Printf("[mw-proxy] update relay on /v1/update — version census DISABLED " +
			"(MW_UMAMI_WEBSITE_ID is empty)")
	} else {
		log.Printf("[mw-proxy] update relay on /v1/update — version census → %s (website %s)",
			cfg.update.umamiURL, describeWebsite(cfg.update.umamiWebsite))
	}
	// Session census (metrics.{domain} in the Caddyfile). Same reasoning as
	// above: say out loud whether it counts, because it accepts and answers
	// identically either way.
	mux.Handle("/v1/session", newSessionSink(cfg.session, cfg.proxyKey))
	if cfg.session.umamiWebsite == "" || cfg.session.umamiURL == "" {
		log.Printf("[mw-proxy] session census on /v1/session — DISABLED " +
			"(MW_UMAMI_SESSIONS_WEBSITE_ID is empty)")
	} else {
		log.Printf("[mw-proxy] session census on /v1/session → %s (website %s)",
			cfg.session.umamiURL, describeWebsite(cfg.session.umamiWebsite))
	}
	mux.HandleFunc("/healthz", func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusOK)
		_, _ = w.Write([]byte("ok"))
	})

	srv := &http.Server{
		Addr:              cfg.listen,
		Handler:           mux,
		ReadHeaderTimeout: 10 * time.Second,
	}
	log.Printf("[mw-proxy] listening on %s → %s (zone %s)", cfg.listen, cfg.upstream, cfg.zone)
	log.Fatal(srv.ListenAndServe())
}
