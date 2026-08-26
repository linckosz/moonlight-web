// MoonlightWeb — the one place that talks to Umami.
//
// Two features report through here: the installed-version census (update.go)
// and the session census (session.go). They send different things to different
// Umami websites, but the wire format, the failure policy and the privacy rules
// are identical, so they live together rather than being copied.
//
// The two shapes Umami understands
// --------------------------------
// A payload WITHOUT a "name" is a PAGEVIEW: it feeds Visitors / Views / Pages,
// and the Pages panel becomes a histogram of whatever we put in the URL. A
// payload WITH a "name" is a custom EVENT: it feeds the Events panel and leaves
// the pageview counters alone. Getting this backwards is not an error anywhere —
// the dashboard just stays empty where you were looking — so `send` takes the
// name as its first argument and an empty one means pageview, deliberately.
//
// Privacy rules that apply to every caller
// ----------------------------------------
//   - Nothing reaches this file that was not first matched against an allowlist
//     or collapsed into a bucket. That bounds cardinality and guarantees the
//     dashboard only ever shows strings we chose.
//   - Nothing is stored here or anywhere else on this box. Umami keeps a daily
//     rotating hash of address+UA and no raw address.
//   - Reporting is best-effort and never blocks the request that triggered it:
//     the caller has already been answered by the time the goroutine runs.
package main

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"log"
	"net/http"
	"time"
)

type umamiConfig struct {
	url      string // MW_UMAMI_URL ("" disables)
	website  string // per-feature website id ("" disables)
	hostname string // the hostname Umami records for this feature
}

// umamiPayload is Umami's /api/send envelope. Name is omitted when empty —
// see the pageview/event distinction above.
type umamiPayload struct {
	Website  string            `json:"website"`
	Hostname string            `json:"hostname"`
	URL      string            `json:"url"`
	Name     string            `json:"name,omitempty"`
	Data     map[string]string `json:"data,omitempty"`
}

type umamiEvent struct {
	Type    string       `json:"type"`
	Payload umamiPayload `json:"payload"`
}

type umamiReporter struct {
	cfg    umamiConfig
	client *http.Client
	tag    string // log prefix identifying the caller ("update", "session")

	// Bounds concurrent posts so a burst of clients cannot spawn an unbounded
	// number of goroutines. When the semaphore is full the event is dropped,
	// never queued: a missing sample is cheaper than unbounded memory.
	slots chan struct{}
}

func newUmamiReporter(cfg umamiConfig, tag string, client *http.Client) *umamiReporter {
	if client == nil {
		client = &http.Client{Timeout: 15 * time.Second}
	}
	return &umamiReporter{cfg: cfg, client: client, tag: tag, slots: make(chan struct{}, 32)}
}

// enabled reports whether anything will actually be sent. Callers use it to
// keep quiet rather than to change behaviour: every feature that reports must
// work identically with reporting off.
func (u *umamiReporter) enabled() bool {
	return u != nil && u.cfg.url != "" && u.cfg.website != ""
}

// send posts one record, fire-and-forget. An empty name records a pageview, a
// non-empty one a custom event.
//
// ua is the User-Agent Umami attributes the record to — it is also half of the
// visitor hash, so it must describe the reporting instance and carry its
// version, otherwise a machine that upgrades keeps being counted under its old
// one. ip is the instance's address: Umami runs with
// CLIENT_IP_HEADER=x-forwarded-for, so without it every record would carry this
// container's address and the whole fleet would collapse into one "visitor".
func (u *umamiReporter) send(name, url, ua, ip string, data map[string]string) {
	if !u.enabled() {
		return
	}
	select {
	case u.slots <- struct{}{}:
	default:
		return // saturated: drop rather than pile up
	}

	go func() {
		defer func() { <-u.slots }()

		body, err := json.Marshal(umamiEvent{
			Type: "event",
			Payload: umamiPayload{
				Website:  u.cfg.website,
				Hostname: u.cfg.hostname,
				URL:      url,
				Name:     name,
				Data:     data,
			},
		})
		if err != nil {
			return
		}

		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		req, err := http.NewRequestWithContext(ctx, http.MethodPost,
			u.cfg.url+"/api/send", bytes.NewReader(body))
		if err != nil {
			return
		}
		req.Header.Set("Content-Type", "application/json")
		// Umami rejects a request with no User-Agent at all.
		req.Header.Set("User-Agent", ua)
		req.Header.Set("X-Forwarded-For", ip)

		resp, err := u.client.Do(req)
		if err != nil {
			log.Printf("[mw-proxy] %s: analytics post failed: %v", u.tag, err)
			return
		}
		defer resp.Body.Close()
		_, _ = io.Copy(io.Discard, io.LimitReader(resp.Body, 4096))
		if resp.StatusCode >= 400 {
			log.Printf("[mw-proxy] %s: analytics rejected the record (%d)", u.tag, resp.StatusCode)
		}
	}()
}

// describeWebsite renders a website id for a startup log line: enough to tell
// two ids apart, not enough to copy out of a log.
func describeWebsite(id string) string {
	if id == "" {
		return "none"
	}
	if len(id) > 8 {
		return id[:8] + "…"
	}
	return id
}
