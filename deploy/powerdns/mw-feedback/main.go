// MoonlightWeb — mw-feedback: the "Send feedback" relay.
//
// What it does
// ------------
// The owner of an instance writes a message (and, if they want an answer, an
// e-mail address) in the server settings. Their instance — never the browser,
// never a guest — sends it here, and this process posts it to a private Discord
// channel read by the moderators. That is all it does.
//
// Why a process of its own
// ------------------------
// The Discord webhook URL is a credential: whoever holds it can post as us. It
// is given to this service alone (MW_FEEDBACK_WEBHOOK, in the production .env),
// not to mw-proxy, which holds the PowerDNS key — parsing free text from the
// Internet next to that key is a risk worth one more small container. The URL
// is never in the app, the repository, the CI or a log: a webhook shipped in a
// binary would be public. If it ever leaks, it is regenerated in Discord and
// the .env updated; nothing else holds it.
//
// What is kept
// ------------
// Nothing. No file, no volume; the process logs counters and Discord's status
// codes, never a message, an e-mail, an address or the webhook. Caddy in front
// rate-limits per subscriber in memory and keeps no access log. The message
// itself lives in Discord until a moderator deletes it.
//
// What keeps it from being a spam cannon
// --------------------------------------
// Caddy: a few messages an hour per subscriber, and the challenge path limited
// too. Here: a challenge that must be at least challengeMinAge old and come
// with a proof of work (challenge.go), a hidden field that only a bot fills, a
// cap per hour and per day for everybody together, bounded sizes, and no
// queue — when Discord says "slow down", the instance is told "unavailable".
//
// Stdlib only, like mw-proxy and mw-rendezvous.
package main

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"log"
	"net/http"
	"net/url"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"
)

const (
	maxBody         = 8 << 10
	defaultPowBits  = 20
	defaultMaxNonce = 10000
)

type config struct {
	listen     string // MW_FEEDBACK_LISTEN (default ":8091")
	webhook    string // MW_FEEDBACK_WEBHOOK — SECRET; empty = feedback off (503)
	maxPerHour int    // MW_FEEDBACK_MAX_PER_HOUR, everybody together (default 30)
	maxPerDay  int    // MW_FEEDBACK_MAX_PER_DAY (default 100)
	powBits    int    // MW_FEEDBACK_POW_BITS (default 20, 8..26)
}

func envInt(name string, def, lo, hi int) int {
	v, err := strconv.Atoi(strings.TrimSpace(os.Getenv(name)))
	if err != nil || v < lo || v > hi {
		return def
	}
	return v
}

func loadConfig() config {
	c := config{
		listen:     os.Getenv("MW_FEEDBACK_LISTEN"),
		webhook:    os.Getenv("MW_FEEDBACK_WEBHOOK"),
		maxPerHour: envInt("MW_FEEDBACK_MAX_PER_HOUR", 30, 1, 10000),
		maxPerDay:  envInt("MW_FEEDBACK_MAX_PER_DAY", 100, 1, 100000),
		powBits:    envInt("MW_FEEDBACK_POW_BITS", defaultPowBits, 8, 26),
	}
	if c.listen == "" {
		c.listen = ":8091"
	}
	return c
}

type server struct {
	hook   *url.URL // nil = feedback is off
	client *http.Client
	chal   *challenger
	now    func() time.Time

	maxPerHour, maxPerDay int
	mu                    sync.Mutex
	hourStart, dayStart   time.Time
	hourCount, dayCount   int
}

func newServer(cfg config) (*server, error) {
	chal, err := newChallenger(cfg.powBits, defaultMaxNonce)
	if err != nil {
		return nil, err
	}
	s := &server{client: newDiscordClient(), chal: chal, now: time.Now,
		maxPerHour: cfg.maxPerHour, maxPerDay: cfg.maxPerDay}
	if cfg.webhook != "" {
		hook, err := parseWebhook(cfg.webhook)
		if err != nil {
			// Said without the value: a mistyped URL is still mostly a token.
			log.Printf("[mw-feedback] MW_FEEDBACK_WEBHOOK is not https://discord.com/api/webhooks/<id>/<token> — feedback OFF")
		} else {
			s.hook = hook
		}
	}
	return s, nil
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}

func writeErr(w http.ResponseWriter, status int, code string) {
	writeJSON(w, status, map[string]string{"error": code})
}

// takeSlot charges one message to the hour's and the day's budgets, or says
// there is none left. It is charged before Discord is asked, so a Discord
// that fails does not get asked again and again.
func (s *server) takeSlot(now time.Time) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	hour := now.UTC().Truncate(time.Hour)
	day := time.Date(now.UTC().Year(), now.UTC().Month(), now.UTC().Day(), 0, 0, 0, 0, time.UTC)
	if !hour.Equal(s.hourStart) {
		s.hourStart, s.hourCount = hour, 0
	}
	if !day.Equal(s.dayStart) {
		s.dayStart, s.dayCount = day, 0
	}
	if s.hourCount >= s.maxPerHour || s.dayCount >= s.maxPerDay {
		return false
	}
	s.hourCount++
	s.dayCount++
	return true
}

// ── GET /v1/feedback/challenge ──────────────────────────────────────────────

func (s *server) handleChallenge(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		writeErr(w, http.StatusMethodNotAllowed, "method")
		return
	}
	if s.hook == nil {
		writeErr(w, http.StatusServiceUnavailable, "unavailable")
		return
	}
	token, err := s.chal.issue(s.now())
	if err != nil {
		writeErr(w, http.StatusServiceUnavailable, "unavailable")
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"challenge":  token,
		"bits":       s.chal.bits,
		"minAgeSecs": int(challengeMinAge / time.Second),
		"maxAgeSecs": int(challengeMaxAge / time.Second),
		"maxMessage": maxMessageUnits,
	})
}

// ── POST /v1/feedback ───────────────────────────────────────────────────────

type feedbackRequest struct {
	Challenge string `json:"challenge"`
	Pow       string `json:"pow"`
	Message   string `json:"message"`
	Email     string `json:"email"`
	Website   string `json:"website"` // the hidden field: only a bot fills it
	Meta      *struct {
		Version string `json:"version"`
		Edition string `json:"edition"`
		OS      string `json:"os"`
	} `json:"meta"`
}

func (s *server) handleFeedback(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeErr(w, http.StatusMethodNotAllowed, "method")
		return
	}
	if s.hook == nil {
		writeErr(w, http.StatusServiceUnavailable, "unavailable")
		return
	}
	body, err := io.ReadAll(http.MaxBytesReader(w, r.Body, maxBody))
	if err != nil {
		writeErr(w, http.StatusRequestEntityTooLarge, "too_large")
		return
	}
	var req feedbackRequest
	if err := json.Unmarshal(body, &req); err != nil {
		writeErr(w, http.StatusBadRequest, "invalid_json")
		return
	}
	if req.Website != "" {
		// Answered like a success, so a bot learns nothing; nothing is sent.
		log.Printf("[mw-feedback] dropped: hidden field filled")
		writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
		return
	}

	now := s.now()
	if err := s.chal.verify(req.Challenge, req.Pow, now); err != nil {
		switch err {
		case errChallengeEarly:
			writeErr(w, http.StatusTooEarly, err.Error())
		case errNoncesFull:
			log.Printf("[mw-feedback] refused: challenge set full")
			writeErr(w, http.StatusServiceUnavailable, "unavailable")
		default:
			writeErr(w, http.StatusBadRequest, err.Error())
		}
		return
	}

	f, code := validate(req)
	if code != "" {
		writeErr(w, http.StatusBadRequest, code)
		return
	}
	payload, err := f.payload()
	if err != nil || utf16Len(f.content()) > discordMaxContent {
		writeErr(w, http.StatusBadRequest, "message_length")
		return
	}
	if !s.takeSlot(now) {
		log.Printf("[mw-feedback] refused: global cap reached")
		writeErr(w, http.StatusTooManyRequests, "rate_limited")
		return
	}

	ctx, cancel := context.WithTimeout(r.Context(), discordTimeout)
	defer cancel()
	switch status := post(ctx, s.client, s.hook, payload); {
	case status >= 200 && status < 300:
		log.Printf("[mw-feedback] sent")
		writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
	case status == http.StatusTooManyRequests:
		// No queue and no retry: the instance shows "unavailable, try later".
		log.Printf("[mw-feedback] discord: 429")
		writeErr(w, http.StatusServiceUnavailable, "unavailable")
	case status == 0:
		log.Printf("[mw-feedback] discord: no answer")
		writeErr(w, http.StatusBadGateway, "upstream")
	default:
		log.Printf("[mw-feedback] discord: %d", status)
		writeErr(w, http.StatusBadGateway, "upstream")
	}
}

// validate turns the request into what will be posted, or names the field that
// is refused. Attached metadata that is not on its list is dropped, not refused:
// it is optional, and the message still deserves to arrive.
func validate(req feedbackRequest) (feedback, string) {
	var f feedback
	f.message = cleanMessage(req.Message)
	if len([]rune(f.message)) < minMessageRunes {
		return f, "message_short"
	}
	if utf16Len(f.message) > maxMessageUnits {
		return f, "message_long"
	}
	f.email = strings.TrimSpace(req.Email)
	if f.email != "" && (len(f.email) > maxEmailLen || !emailRe.MatchString(f.email)) {
		return f, "email_invalid"
	}
	if m := req.Meta; m != nil {
		if len(m.Version) <= maxVersionLen && versionRe.MatchString(m.Version) {
			f.version = m.Version
		}
		if editions[m.Edition] {
			f.edition = m.Edition
		}
		if osFamily[m.OS] {
			f.os = m.OS
		}
	}
	return f, ""
}

// recoverer replaces net/http's own panic handler, which logs the panic value
// and the peer: neither may reach a log here.
func recoverer(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		defer func() {
			if v := recover(); v != nil {
				if v == http.ErrAbortHandler {
					return
				}
				log.Printf("[mw-feedback] internal error")
				writeErr(w, http.StatusInternalServerError, "internal")
			}
		}()
		next.ServeHTTP(w, r)
	})
}

func newMux(s *server) http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/feedback", s.handleFeedback)
	mux.HandleFunc("/v1/feedback/challenge", s.handleChallenge)
	mux.HandleFunc("/healthz", func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusOK)
		_, _ = w.Write([]byte("ok"))
	})
	return recoverer(mux)
}

func main() {
	log.SetFlags(log.LstdFlags | log.LUTC)
	cfg := loadConfig()
	s, err := newServer(cfg)
	if err != nil {
		log.Fatalf("[mw-feedback] cannot draw the challenge key")
	}
	if s.hook == nil {
		log.Printf("[mw-feedback] feedback OFF (MW_FEEDBACK_WEBHOOK empty or invalid): every request gets 503")
	} else {
		log.Printf("[mw-feedback] feedback ON: %d/h and %d/day in all, proof of work %d bits",
			cfg.maxPerHour, cfg.maxPerDay, cfg.powBits)
	}
	srv := &http.Server{
		Addr:              cfg.listen,
		Handler:           newMux(s),
		ReadHeaderTimeout: 10 * time.Second,
		ReadTimeout:       15 * time.Second,
		WriteTimeout:      30 * time.Second,
		IdleTimeout:       60 * time.Second,
		MaxHeaderBytes:    16 << 10,
		// net/http's own messages name the peer; the peer is Caddy, but the
		// rule is simpler without exceptions.
		ErrorLog: log.New(io.Discard, "", 0),
	}
	log.Printf("[mw-feedback] listening on %s", cfg.listen)
	if err := srv.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
		log.Fatalf("[mw-feedback] listener stopped")
	}
}
