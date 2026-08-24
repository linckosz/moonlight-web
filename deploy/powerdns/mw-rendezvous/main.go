// MoonlightWeb — mw-rendezvous: the introduction server.
//
// What it does
// ------------
// It puts two peers in touch and gets out of the way. A host holds one WebSocket
// open; a browser arrives with that host's identifier; the two exchange WebRTC
// signalling through this process until they have a direct path, and every byte
// of video, audio and input then flows between them without touching this box.
//
// What it replaces
// ----------------
// The per-instance sub-domain. That mechanism put a residential IP address in
// public DNS and, because every instance took a publicly trusted certificate,
// published the entire fleet in Certificate Transparency. Neither happens here:
// one identifier, no DNS record, no certificate, and reachability comes from a
// held socket rather than a published address.
//
// What it is trusted with, and what it is not
// -------------------------------------------
// Inherent to the role, and not removable: which identifiers exist, which are
// online, and the residential addresses visible on the sockets while they are
// connected. None of it is written down here — the only durable state is the
// identifier-to-owner claim.
//
// It relays the DTLS fingerprints the two peers need to find each other, so a
// compromised instance of this service could substitute its own and sit in the
// middle. That is why MW-BIND-v1 signs the fingerprint with the pairing key,
// which this service never sees: the payloads below are copied byte for byte and
// are meaningless to it. It also never serves the page — the bootstrap is hosted
// separately, so breaking in here does not yield code execution in anyone's
// browser.
//
// Stdlib only, like mw-proxy, and for the same reason: a poisoned dependency is
// a realistic way into this box, and there is nothing here worth importing one
// for.
package main

import (
	"encoding/json"
	"errors"
	"io"
	"log"
	"net"
	"net/http"
	"os"
	"strconv"
	"strings"
	"time"
)

// ── Configuration (env-driven) ──────────────────────────────────────────────

type config struct {
	listen      string        // MW_RDV_LISTEN             (default ":8090")
	storePath   string        // MW_RDV_STORE              (default "/data/rendezvous.json")
	secret      []byte        // MW_RDV_OWNER_SECRET       (HMAC key for the claim store)
	maxNewPerHr int           // MW_RDV_MAX_NEW_PER_HOUR   (default 20; 0 = unlimited)
	maxPeerPerM int           // MW_RDV_MAX_PEER_PER_MIN   (default 30; 0 = unlimited)
	maxSessions int           // MW_RDV_MAX_SESSIONS       (default 4, per host)
	maxMessage  int           // MW_RDV_MAX_MESSAGE_BYTES  (default 64 KiB)
	pingEvery   time.Duration // MW_RDV_PING_SECONDS       (default 45s)
}

func mustEnv(name string) string {
	v := os.Getenv(name)
	if v == "" {
		log.Fatalf("[mw-rendezvous] required environment variable %s is empty", name)
	}
	return v
}

func envOr(name, def string) string {
	if v := os.Getenv(name); v != "" {
		return v
	}
	return def
}

func envInt(name string, def, min, max int) int {
	v := os.Getenv(name)
	if v == "" {
		return def
	}
	n, err := strconv.Atoi(v)
	if err != nil || n < min || n > max {
		log.Printf("[mw-rendezvous] WARN ignoring %s=%q (want %d..%d), using %d", name, v, min, max, def)
		return def
	}
	return n
}

func loadConfig() config {
	return config{
		listen:    envOr("MW_RDV_LISTEN", ":8090"),
		storePath: envOr("MW_RDV_STORE", "/data/rendezvous.json"),
		secret:    []byte(mustEnv("MW_RDV_OWNER_SECRET")),
		// 0 is meaningful (unlimited), so the floor is 0 rather than 1.
		maxNewPerHr: envInt("MW_RDV_MAX_NEW_PER_HOUR", 20, 0, 100000),
		maxPeerPerM: envInt("MW_RDV_MAX_PEER_PER_MIN", 30, 0, 100000),
		maxSessions: envInt("MW_RDV_MAX_SESSIONS", 4, 1, 64),
		maxMessage:  envInt("MW_RDV_MAX_MESSAGE_BYTES", 64*1024, 1024, 1024*1024),
		pingEvery:   time.Duration(envInt("MW_RDV_PING_SECONDS", 45, 10, 300)) * time.Second,
	}
}

// readIdle is how long a socket may stay silent before it is reaped: two ping
// intervals plus slack, so a single lost ping or pong never costs a live host
// its line.
func (c config) readIdle() time.Duration { return 2*c.pingEvery + 15*time.Second }

// ── Server ──────────────────────────────────────────────────────────────────

type server struct {
	cfg        config
	store      *claimStore
	hub        *hub
	claimQuota *quota
	peerQuota  *quota
}

func newServer(cfg config, store *claimStore) *server {
	return &server{
		cfg:        cfg,
		store:      store,
		hub:        newHub(cfg.maxSessions),
		claimQuota: newQuota(cfg.maxNewPerHr, time.Hour),
		peerQuota:  newQuota(cfg.maxPeerPerM, time.Minute),
	}
}

func writeErr(w http.ResponseWriter, status int, msg string) {
	writeErrCode(w, status, msg, "")
}

// writeErrCode adds a machine-readable code beside the prose.
//
// The two 409s mean opposite things to a host — one says "draw another
// identifier", the other says "you already have one, this is not it" — and the
// only alternative to a code is matching on the sentence, which turns a wording
// change into a client bug.
func writeErrCode(w http.ResponseWriter, status int, msg, code string) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	body := map[string]string{"error": msg}
	if code != "" {
		body["code"] = code
	}
	_ = json.NewEncoder(w).Encode(body)
}

// Machine-readable error codes. Keep in step with RendezvousClient.cpp.
const (
	errIDTaken       = "id_taken"        // someone else owns this identifier
	errOwnerHasOther = "owner_has_other" // this credential already holds a different one
)

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}

// clientIP returns the address the budgets are keyed on. Caddy terminates TLS
// in front, so the socket peer is always the proxy — X-Forwarded-For is the only
// place the real address appears. Nothing but rate limiting is decided from it.
func clientIP(r *http.Request) string {
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

// ownerToken pulls and sanity-checks the claim credential.
func ownerToken(r *http.Request) (string, bool) {
	t := strings.TrimSpace(r.Header.Get("X-MW-Owner"))
	if len(t) < minOwnerToken || len(t) > 512 {
		return "", false
	}
	return t, true
}

// requestID normalises and validates the id carried in the query string.
func requestID(r *http.Request) (string, bool) {
	id := normaliseID(r.URL.Query().Get("id"))
	return id, validID(id)
}

// ── /v1/claim ───────────────────────────────────────────────────────────────

type claimRequest struct {
	ID string `json:"id"`
}

func (s *server) handleClaim(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost && r.Method != http.MethodDelete {
		writeErr(w, http.StatusMethodNotAllowed, "only POST and DELETE are allowed")
		return
	}
	token, ok := ownerToken(r)
	if !ok {
		writeErr(w, http.StatusUnauthorized, "X-MW-Owner is missing or too short")
		return
	}
	body, err := io.ReadAll(io.LimitReader(r.Body, 4096))
	if err != nil {
		writeErr(w, http.StatusBadRequest, "cannot read body")
		return
	}
	var req claimRequest
	if err := json.Unmarshal(body, &req); err != nil {
		writeErr(w, http.StatusBadRequest, "invalid JSON body")
		return
	}
	id := normaliseID(req.ID)
	if !validID(id) {
		writeErr(w, http.StatusBadRequest, "id must be 26 Crockford base32 characters")
		return
	}

	if r.Method == http.MethodDelete {
		if !s.store.release(id, token) {
			// Same answer whether the id is unknown or owned by someone else:
			// a caller holding a token learns nothing about ids it does not own.
			writeErr(w, http.StatusForbidden, "not the owner of this id")
			return
		}
		if err := s.store.flush(); err != nil {
			log.Printf("[mw-rendezvous] WARN cannot persist claim store: %v", err)
		}
		writeJSON(w, http.StatusOK, map[string]any{"id": id, "released": true})
		return
	}

	// Budget only NEW claims. Re-presenting an id already held is what a host
	// does on every restart, and charging for that would lock out a machine that
	// reboots often — or a household behind one address.
	if !s.store.owns(id, token) && !s.claimQuota.allow(clientIP(r)) {
		writeErr(w, http.StatusTooManyRequests, "too many new claims from this address, retry later")
		return
	}

	switch s.store.claim(id, token) {
	case claimCreated:
		if err := s.store.flush(); err != nil {
			log.Printf("[mw-rendezvous] WARN cannot persist claim store: %v", err)
		}
		writeJSON(w, http.StatusOK, map[string]any{"id": id, "claimed": true})
	case claimExisting:
		writeJSON(w, http.StatusOK, map[string]any{"id": id, "claimed": false})
	case claimTaken:
		writeErrCode(w, http.StatusConflict, "this id is already claimed", errIDTaken)
	case claimHasOther:
		writeErrCode(w, http.StatusConflict,
			"this owner already holds an id — release it first", errOwnerHasOther)
	}
}

// ── /v1/host — the held line ────────────────────────────────────────────────

func (s *server) handleHost(w http.ResponseWriter, r *http.Request) {
	id, ok := requestID(r)
	if !ok {
		writeErr(w, http.StatusBadRequest, "id must be 26 Crockford base32 characters")
		return
	}
	token, hasToken := ownerToken(r)
	if !hasToken || !s.store.owns(id, token) {
		writeErr(w, http.StatusUnauthorized, "not the owner of this id")
		return
	}

	conn, err := wsUpgrade(w, r, s.cfg.maxMessage)
	if err != nil {
		return // wsUpgrade already answered
	}
	conn.SetReadIdle(s.cfg.readIdle())

	line := &hostLine{id: id, conn: conn, sessions: map[string]*session{}}
	if displaced := s.hub.register(line); displaced != nil {
		go hangUpLine(displaced, "replaced by a newer connection from the same owner")
	}
	log.Printf("[mw-rendezvous] host line up: %s… (%d online)", id[:8], s.hub.onlineCount())

	done := make(chan struct{})
	go pinger(conn, s.cfg.pingEvery, done)

	defer func() {
		close(done)
		s.hub.unregister(line)
		for _, sess := range line.shutdown() {
			_ = sendPeer(sess.conn, peerFrame{T: msgError, Code: codeHostGone})
			_ = sess.conn.WriteClose(closeNormal, codeHostGone)
			_ = sess.conn.Close()
		}
		_ = conn.Close()
		log.Printf("[mw-rendezvous] host line down: %s… (%d online)", id[:8], s.hub.onlineCount())
	}()

	for {
		op, payload, err := conn.ReadMessage()
		if err != nil {
			if !errors.Is(err, io.EOF) {
				_ = conn.WriteClose(closeCodeFor(err), "")
			}
			return
		}
		if op != opText {
			continue // signalling is JSON; anything else is not for us
		}
		var f hostFrame
		if json.Unmarshal(payload, &f) != nil {
			continue
		}
		switch f.T {
		case msgMsg:
			sess := line.session(f.S)
			if sess == nil {
				continue // the browser left; nothing to deliver to
			}
			if err := sendPeer(sess.conn, peerFrame{T: msgMsg, D: f.D}); err != nil {
				// The browser is wedged or gone. Drop it rather than let it hold
				// up the line — every other session on this host shares it.
				if line.dropSession(f.S) {
					_ = sendHost(conn, hostFrame{T: msgClose, S: f.S})
				}
				_ = sess.conn.Close()
			}
		case msgClose:
			if sess := line.session(f.S); sess != nil {
				line.dropSession(f.S)
				_ = sess.conn.WriteClose(closeNormal, "")
				_ = sess.conn.Close()
			}
		}
	}
}

// hangUpLine closes a displaced host line without disturbing its successor.
func hangUpLine(line *hostLine, why string) {
	for _, sess := range line.shutdown() {
		_ = sendPeer(sess.conn, peerFrame{T: msgError, Code: codeHostGone})
		_ = sess.conn.Close()
	}
	_ = line.conn.WriteClose(closePolicy, why)
	_ = line.conn.Close()
}

// ── /v1/peer — a browser ────────────────────────────────────────────────────

func (s *server) handlePeer(w http.ResponseWriter, r *http.Request) {
	id, ok := requestID(r)
	if !ok {
		// A malformed id is rejected before the upgrade: it reveals nothing,
		// since no id of that shape could exist in the first place.
		writeErr(w, http.StatusBadRequest, "id must be 26 Crockford base32 characters")
		return
	}
	if !s.peerQuota.allow(clientIP(r)) {
		writeErr(w, http.StatusTooManyRequests, "too many connections from this address, retry later")
		return
	}

	// Upgrade BEFORE looking the id up, so an unknown id and a known one are
	// indistinguishable at the HTTP layer: both get 101, then the same refusal
	// frame. Deciding earlier would answer the enumeration question for free.
	conn, err := wsUpgrade(w, r, s.cfg.maxMessage)
	if err != nil {
		return
	}
	conn.SetReadIdle(s.cfg.readIdle())

	line := s.hub.line(id)
	if line == nil {
		refuse(conn, codeOffline)
		return
	}
	sess := &session{id: newSessionID(), conn: conn}
	if code := line.addSession(sess, s.hub.maxSessions); code != "" {
		refuse(conn, code)
		return
	}
	if err := sendHost(line.conn, hostFrame{T: msgOpen, S: sess.id}); err != nil {
		line.dropSession(sess.id)
		refuse(conn, codeOffline)
		return
	}
	if err := sendPeer(conn, peerFrame{T: msgReady}); err != nil {
		line.dropSession(sess.id)
		_ = sendHost(line.conn, hostFrame{T: msgClose, S: sess.id})
		_ = conn.Close()
		return
	}

	done := make(chan struct{})
	go pinger(conn, s.cfg.pingEvery, done)

	defer func() {
		close(done)
		if line.dropSession(sess.id) {
			_ = sendHost(line.conn, hostFrame{T: msgClose, S: sess.id})
		}
		_ = conn.Close()
	}()

	for {
		op, payload, err := conn.ReadMessage()
		if err != nil {
			if !errors.Is(err, io.EOF) {
				_ = conn.WriteClose(closeCodeFor(err), "")
			}
			return
		}
		if op != opText {
			continue
		}
		var f peerFrame
		if json.Unmarshal(payload, &f) != nil {
			continue
		}
		if f.T != msgMsg || len(f.D) == 0 {
			continue
		}
		// The session id is stamped here, never taken from the browser: a peer
		// that could name a session could inject into someone else's.
		if err := sendHost(line.conn, hostFrame{T: msgMsg, S: sess.id, D: f.D}); err != nil {
			return
		}
	}
}

// ── Wiring ──────────────────────────────────────────────────────────────────

func (s *server) routes() *http.ServeMux {
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/claim", s.handleClaim)
	mux.HandleFunc("/v1/host", s.handleHost)
	mux.HandleFunc("/v1/peer", s.handlePeer)
	mux.HandleFunc("/healthz", func(w http.ResponseWriter, _ *http.Request) {
		writeJSON(w, http.StatusOK, map[string]any{
			"ok":     true,
			"online": s.hub.onlineCount(),
			"claims": s.store.count(),
		})
	})
	return mux
}

func main() {
	log.SetFlags(log.LstdFlags | log.LUTC)
	cfg := loadConfig()

	store, err := newClaimStore(cfg.storePath, cfg.secret)
	if err != nil {
		log.Fatalf("[mw-rendezvous] cannot load claim store %q: %v", cfg.storePath, err)
	}
	s := newServer(cfg, store)

	srv := &http.Server{
		Addr:    cfg.listen,
		Handler: s.routes(),
		// Only the header phase is bounded. A read timeout would apply to the
		// hijacked socket too and cut every held line at the same age, which is
		// the one thing this service must not do.
		ReadHeaderTimeout: 10 * time.Second,
	}
	log.Printf("[mw-rendezvous] listening on %s — %d claims on file, ping %s, %d sessions/host, %d KiB max message",
		cfg.listen, store.count(), cfg.pingEvery, cfg.maxSessions, cfg.maxMessage/1024)
	log.Fatal(srv.ListenAndServe())
}
