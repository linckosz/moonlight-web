package main

import (
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"sync/atomic"
	"testing"
	"time"
)

const testReleaseJSON = `{"tag_name":"v0.3.1","html_url":"https://example.invalid/r","assets":[]}`

// testRelay wires a relay to a fake GitHub, returning the hit counter so tests
// can assert the cache actually spares the upstream.
func testRelay(t *testing.T, ttl time.Duration) (*updateRelay, *atomic.Int64) {
	t.Helper()
	// Atomic: the counter is bumped from the httptest server goroutine and read
	// from the test goroutine, with background refreshes possibly still in flight.
	hits := &atomic.Int64{}
	upstream := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		hits.Add(1)
		_, _ = w.Write([]byte(testReleaseJSON))
	}))
	t.Cleanup(upstream.Close)
	return newUpdateRelay(updateConfig{releaseURL: upstream.URL, cacheTTL: ttl}, "test-key"), hits
}

func updateRequest(query string) *http.Request {
	r := httptest.NewRequest(http.MethodGet, "/v1/update"+query, nil)
	r.Header.Set("X-API-Key", "test-key")
	r.Header.Set("X-Forwarded-For", "203.0.113.7")
	return r
}

func TestCleanVersion(t *testing.T) {
	cases := map[string]string{
		"0.3.1":         "0.3.1",
		"v0.3.1":        "0.3.1",
		" 1.2.3.4 ":     "1.2.3.4",
		"0.3.1-rc.1":    "0.3.1-rc.1",
		"":              "unknown",
		"latest":        "unknown",
		"0.3.1; DROP":   "unknown",
		"../../etc":     "unknown",
		"0.3.1/../evil": "unknown",
		"99999.1":       "unknown", // 5-digit component: not a version we ship
		// A dev build's git-describe version. Deliberately bucketed as unknown:
		// these are not releases, and letting them through would add one bucket
		// per commit to the dashboard.
		"0.2.4.48.g7436e7a": "unknown",
	}
	for in, want := range cases {
		if got := cleanVersion(in); got != want {
			t.Errorf("cleanVersion(%q) = %q, want %q", in, got, want)
		}
	}
}

func TestCleanToken(t *testing.T) {
	if got := cleanToken("Windows", knownOS); got != "windows" {
		t.Errorf("os token should be lowercased, got %q", got)
	}
	if got := cleanToken("plan9", knownOS); got != "other" {
		t.Errorf("unknown os must collapse to \"other\", got %q", got)
	}
	if got := cleanToken("arm64", knownArch); got != "arm64" {
		t.Errorf("arm64 should pass, got %q", got)
	}
	if got := cleanToken("", knownArch); got != "other" {
		t.Errorf("empty arch must collapse to \"other\", got %q", got)
	}
}

func TestUpdateRelayRejectsBadRequests(t *testing.T) {
	relay, hits := testRelay(t, time.Minute)

	noKey := httptest.NewRequest(http.MethodGet, "/v1/update", nil)
	w := httptest.NewRecorder()
	relay.ServeHTTP(w, noKey)
	if w.Code != http.StatusUnauthorized {
		t.Errorf("missing key: got %d, want 401", w.Code)
	}

	wrongMethod := httptest.NewRequest(http.MethodPost, "/v1/update", nil)
	wrongMethod.Header.Set("X-API-Key", "test-key")
	w = httptest.NewRecorder()
	relay.ServeHTTP(w, wrongMethod)
	if w.Code != http.StatusMethodNotAllowed {
		t.Errorf("POST: got %d, want 405", w.Code)
	}

	if n := hits.Load(); n != 0 {
		t.Errorf("a rejected request must not reach the upstream (%d hits)", n)
	}
}

func TestUpdateRelayServesReleaseFromCache(t *testing.T) {
	relay, hits := testRelay(t, time.Minute)

	for i := 0; i < 3; i++ {
		w := httptest.NewRecorder()
		relay.ServeHTTP(w, updateRequest("?v=0.3.1&os=windows&arch=x64"))
		if w.Code != http.StatusOK {
			t.Fatalf("request %d: got %d, want 200", i, w.Code)
		}
		// The body must be the upstream JSON verbatim — the client parses it as
		// if it came from GitHub.
		if w.Body.String() != testReleaseJSON {
			t.Fatalf("request %d: body was rewritten: %s", i, w.Body.String())
		}
	}
	if n := hits.Load(); n != 1 {
		t.Errorf("cache should collapse 3 requests into 1 upstream fetch, got %d", n)
	}
}

func TestUpdateRelayServesStaleWhenUpstreamFails(t *testing.T) {
	fail := &atomic.Bool{}
	upstream := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		if fail.Load() {
			w.WriteHeader(http.StatusInternalServerError)
			return
		}
		_, _ = w.Write([]byte(testReleaseJSON))
	}))
	defer upstream.Close()

	// TTL of 1ns: every request finds the cache stale and kicks a refresh.
	relay := newUpdateRelay(updateConfig{releaseURL: upstream.URL, cacheTTL: time.Nanosecond}, "test-key")

	w := httptest.NewRecorder()
	relay.ServeHTTP(w, updateRequest("?v=0.3.1&os=linux&arch=x64"))
	if w.Code != http.StatusOK {
		t.Fatalf("warm-up: got %d, want 200", w.Code)
	}

	fail.Store(true)
	for i := 0; i < 5; i++ {
		w = httptest.NewRecorder()
		relay.ServeHTTP(w, updateRequest("?v=0.3.1&os=linux&arch=x64"))
		if w.Code != http.StatusOK || w.Body.String() != testReleaseJSON {
			t.Fatalf("upstream down: got %d %q, want the stale release", w.Code, w.Body.String())
		}
	}
}

func TestUpdateRelayColdCacheFailureIs502(t *testing.T) {
	upstream := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusInternalServerError)
	}))
	defer upstream.Close()

	relay := newUpdateRelay(updateConfig{releaseURL: upstream.URL, cacheTTL: time.Minute}, "test-key")
	w := httptest.NewRecorder()
	relay.ServeHTTP(w, updateRequest(""))
	if w.Code != http.StatusBadGateway {
		t.Errorf("cold cache + dead upstream: got %d, want 502 (client falls back to GitHub)", w.Code)
	}
}

func TestUpdateRelayReportsSanitizedEvent(t *testing.T) {
	type capture struct {
		event umamiEvent
		raw   map[string]any // to assert on fields the struct does not model
		ua    string
		xff   string
	}
	got := make(chan capture, 4)
	umami := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		body, err := io.ReadAll(r.Body)
		if err != nil {
			t.Errorf("umami could not read the body: %v", err)
		}
		var ev umamiEvent
		if err := json.Unmarshal(body, &ev); err != nil {
			t.Errorf("umami received invalid JSON: %v", err)
		}
		raw := map[string]any{}
		_ = json.Unmarshal(body, &raw)
		got <- capture{ev, raw, r.Header.Get("User-Agent"), r.Header.Get("X-Forwarded-For")}
		w.WriteHeader(http.StatusOK)
	}))
	defer umami.Close()

	relay, _ := testRelay(t, time.Minute)
	relay.cfg.umamiURL = umami.URL
	relay.cfg.umamiWebsite = "test-website-id"
	relay.cfg.hostname = "updates.moonlightweb.top"

	// A forged version must not reach the dashboard as-is.
	w := httptest.NewRecorder()
	relay.ServeHTTP(w, updateRequest("?v=<script>&os=windows&arch=arm64"))
	if w.Code != http.StatusOK {
		t.Fatalf("got %d, want 200", w.Code)
	}

	select {
	case c := <-got:
		if c.event.Type != "event" || c.event.Payload.Website != "test-website-id" {
			t.Errorf("unexpected envelope: %+v", c.event)
		}
		// A "name" would make Umami file this as a custom event, which does not
		// feed Visitors/Views/Pages — the reports the census is read from.
		if _, named := c.raw["payload"].(map[string]any)["name"]; named {
			t.Error("payload must stay a pageview: no \"name\" field")
		}
		if c.event.Payload.URL != "/uc/unknown?os=windows&arch=arm64" {
			t.Errorf("url = %q, want /uc/unknown?os=windows&arch=arm64", c.event.Payload.URL)
		}
		if c.event.Payload.Data["version"] != "unknown" || c.event.Payload.Data["arch"] != "arm64" {
			t.Errorf("unexpected event data: %+v", c.event.Payload.Data)
		}
		// Without the forwarded address Umami would hash its own container IP
		// and the whole fleet would count as one visitor.
		if c.xff != "203.0.113.7" {
			t.Errorf("X-Forwarded-For = %q, want the client address", c.xff)
		}
		if c.ua == "" {
			t.Error("Umami rejects an event with no User-Agent")
		}
	case <-time.After(5 * time.Second):
		t.Fatal("no analytics event arrived")
	}
}

func TestUpdateRelaySkipsReportWithoutWebsiteID(t *testing.T) {
	called := make(chan struct{}, 1)
	umami := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		called <- struct{}{}
		w.WriteHeader(http.StatusOK)
	}))
	defer umami.Close()

	relay, _ := testRelay(t, time.Minute)
	relay.cfg.umamiURL = umami.URL // configured, but no website id

	w := httptest.NewRecorder()
	relay.ServeHTTP(w, updateRequest("?v=0.3.1&os=macos&arch=arm64"))
	if w.Code != http.StatusOK {
		t.Fatalf("got %d, want 200", w.Code)
	}
	select {
	case <-called:
		t.Error("no event may be sent when MW_UMAMI_WEBSITE_ID is unset")
	case <-time.After(300 * time.Millisecond):
	}
}
