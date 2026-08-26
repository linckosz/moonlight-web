package main

import (
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

// captured is one record as the fake Umami saw it, including the two headers
// that decide how it is attributed.
type captured struct {
	event umamiEvent
	raw   map[string]any // to assert on fields the struct does not model
	ua    string
	xff   string
}

// testSink wires a session sink to a fake Umami and returns the channel the
// records arrive on. website == "" leaves reporting unconfigured.
func testSink(t *testing.T, website string) (*sessionSink, chan captured) {
	t.Helper()
	got := make(chan captured, 8)
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
		got <- captured{ev, raw, r.Header.Get("User-Agent"), r.Header.Get("X-Forwarded-For")}
		w.WriteHeader(http.StatusOK)
	}))
	t.Cleanup(umami.Close)

	sink := newSessionSink(sessionConfig{
		umamiURL:     umami.URL,
		umamiWebsite: website,
		hostname:     "metrics.moonlightweb.top",
	}, "test-key")
	return sink, got
}

func sessionRequest(body string) *http.Request {
	r := httptest.NewRequest(http.MethodPost, "/v1/session", strings.NewReader(body))
	r.Header.Set("X-API-Key", "test-key")
	r.Header.Set("X-Forwarded-For", "203.0.113.7")
	return r
}

// A full, well-formed start report — the shape the backend actually posts.
const startBody = `{"event":"start","v":"0.3.0","os":"windows","arch":"x64",
	"h":1080,"fps":60,"codec":"hevc","hdr":true,"yuv444":false,"bitrate":20000,
	"backend":"gamestream","transport":"webrtc-dc-udp","net":"public",
	"client":"desktop","kind":"owner"}`

func post(t *testing.T, sink *sessionSink, body string) *httptest.ResponseRecorder {
	t.Helper()
	w := httptest.NewRecorder()
	sink.ServeHTTP(w, sessionRequest(body))
	return w
}

func waitFor(t *testing.T, got chan captured) captured {
	t.Helper()
	select {
	case c := <-got:
		return c
	case <-time.After(5 * time.Second):
		t.Fatal("no record arrived")
		return captured{}
	}
}

func TestSessionSinkRejectsBadRequests(t *testing.T) {
	sink, _ := testSink(t, "test-website-id")

	w := httptest.NewRecorder()
	sink.ServeHTTP(w, httptest.NewRequest(http.MethodGet, "/v1/session", nil))
	if w.Code != http.StatusMethodNotAllowed {
		t.Errorf("GET: got %d, want 405", w.Code)
	}

	w = httptest.NewRecorder()
	sink.ServeHTTP(w, httptest.NewRequest(http.MethodPost, "/v1/session", strings.NewReader("{}")))
	if w.Code != http.StatusUnauthorized {
		t.Errorf("no key: got %d, want 401", w.Code)
	}

	if code := post(t, sink, "not json").Code; code != http.StatusBadRequest {
		t.Errorf("malformed body: got %d, want 400", code)
	}
}

// A start must be a PAGEVIEW: no event name, or Umami files it under Events and
// leaves Views/Pages — the whole point of this census — at zero.
func TestSessionStartIsPageview(t *testing.T) {
	sink, got := testSink(t, "test-website-id")

	if code := post(t, sink, startBody).Code; code != http.StatusNoContent {
		t.Fatalf("got %d, want 204", code)
	}

	c := waitFor(t, got)
	if _, named := c.raw["payload"].(map[string]any)["name"]; named {
		t.Error("a session start must stay a pageview: no \"name\" field")
	}
	if c.event.Payload.Website != "test-website-id" {
		t.Errorf("website = %q", c.event.Payload.Website)
	}
	want := "/s/1080p60?codec=hevc&hdr=1&bitrate=20-30m&backend=gamestream" +
		"&transport=webrtc-dc-udp&net=public&client=desktop&kind=owner"
	if c.event.Payload.URL != want {
		t.Errorf("url  = %q\nwant = %q", c.event.Payload.URL, want)
	}
	// The version rides in the User-Agent because Umami hashes UA + address
	// into the visitor id; without it an upgraded instance keeps its old bucket.
	if !strings.Contains(c.ua, "MoonlightWeb/0.3.0") {
		t.Errorf("user-agent = %q, want the reporting version", c.ua)
	}
	if c.xff != "203.0.113.7" {
		t.Errorf("X-Forwarded-For = %q, want the instance address", c.xff)
	}
}

// The end of a session is a named event, so it never inflates the session
// count, and the name IS the duration bucket so the Events panel reads as a
// histogram with nothing to configure.
func TestSessionEndIsNamedDurationEvent(t *testing.T) {
	sink, got := testSink(t, "test-website-id")

	body := `{"event":"end","v":"0.3.0","os":"linux","arch":"x64","h":720,"fps":30,
		"codec":"h264","bitrate":8000,"backend":"wolf","transport":"wss",
		"net":"private","client":"mobile","kind":"player","seconds":2500}`
	if code := post(t, sink, body).Code; code != http.StatusNoContent {
		t.Fatalf("got %d, want 204", code)
	}

	c := waitFor(t, got)
	if c.event.Payload.Name != "dur-30-60m" {
		t.Errorf("name = %q, want dur-30-60m", c.event.Payload.Name)
	}
	if c.event.Payload.Data["duration"] != "dur-30-60m" {
		t.Errorf("data.duration = %q", c.event.Payload.Data["duration"])
	}
	if c.event.Payload.Data["backend"] != "wolf" || c.event.Payload.Data["kind"] != "player" {
		t.Errorf("unexpected data: %+v", c.event.Payload.Data)
	}
	// Same path as the start, so the two are attributable to each other.
	if !strings.HasPrefix(c.event.Payload.URL, "/s/720p30?") {
		t.Errorf("url = %q, want the /s/720p30 path", c.event.Payload.URL)
	}
}

func TestSessionFailureIsItsOwnEvent(t *testing.T) {
	sink, got := testSink(t, "test-website-id")

	body := `{"event":"failed","v":"0.3.0","os":"windows","arch":"x64","h":1080,
		"fps":60,"backend":"multiseat","kind":"owner","code":503}`
	post(t, sink, body)

	c := waitFor(t, got)
	if c.event.Payload.Name != "launch-failed" {
		t.Errorf("name = %q, want launch-failed", c.event.Payload.Name)
	}
	if c.event.Payload.Data["code"] != "503" {
		t.Errorf("data.code = %q, want 503", c.event.Payload.Data["code"])
	}
}

// Nothing a client sends may reach the dashboard unchecked: unknown tokens
// collapse to a constant, so a forged report can neither inject markup nor
// invent a million distinct rows.
func TestSessionSanitisesEverything(t *testing.T) {
	sink, got := testSink(t, "test-website-id")

	body := `{"event":"start","v":"<script>","os":"haiku","arch":"ppc","h":1081,
		"fps":61,"codec":"vp9","bitrate":-5,"backend":"../../etc/passwd",
		"transport":"carrier pigeon","net":"moon","client":"fridge","kind":"root"}`
	post(t, sink, body)

	c := waitFor(t, got)
	want := "/s/other?codec=other&hdr=0&bitrate=unknown&backend=other" +
		"&transport=other&net=other&client=other&kind=other"
	if c.event.Payload.URL != want {
		t.Errorf("url  = %q\nwant = %q", c.event.Payload.URL, want)
	}
	if c.event.Payload.Data["version"] != "unknown" || c.event.Payload.Data["os"] != "other" {
		t.Errorf("unexpected data: %+v", c.event.Payload.Data)
	}
	if strings.Contains(c.event.Payload.URL, "<") {
		t.Error("client markup reached the dashboard")
	}
}

// An unknown verb from a future client is accepted and dropped — the instance
// must never see a census disagreement as a streaming error.
func TestSessionIgnoresUnknownEvent(t *testing.T) {
	sink, got := testSink(t, "test-website-id")

	if code := post(t, sink, `{"event":"teleported","v":"0.9.0"}`).Code; code != http.StatusNoContent {
		t.Fatalf("got %d, want 204", code)
	}
	select {
	case <-got:
		t.Error("an unknown event must not be recorded")
	case <-time.After(300 * time.Millisecond):
	}
}

// Reporting off must be indistinguishable from the outside: still 204, nothing
// sent. This is the state every install starts in.
func TestSessionSkipsWithoutWebsiteID(t *testing.T) {
	sink, got := testSink(t, "")

	if code := post(t, sink, startBody).Code; code != http.StatusNoContent {
		t.Fatalf("got %d, want 204", code)
	}
	select {
	case <-got:
		t.Error("no record may be sent when MW_UMAMI_SESSIONS_WEBSITE_ID is unset")
	case <-time.After(300 * time.Millisecond):
	}
}

func TestBuckets(t *testing.T) {
	bitrates := map[int]string{
		0: "unknown", -1: "unknown", 4999: "0-5m", 5000: "5-10m", 20000: "20-30m",
		30000: "30-50m", 80000: "50m+",
	}
	for in, want := range bitrates {
		if got := bitrateBucket(in); got != want {
			t.Errorf("bitrateBucket(%d) = %q, want %q", in, got, want)
		}
	}

	durations := map[int]string{
		-1: "dur-unknown", 0: "dur-0-1m", 59: "dur-0-1m", 60: "dur-1-5m",
		900: "dur-15-30m", 3599: "dur-30-60m", 3600: "dur-1-2h", 100000: "dur-4h+",
	}
	for in, want := range durations {
		if got := durationBucket(in); got != want {
			t.Errorf("durationBucket(%d) = %q, want %q", in, got, want)
		}
	}

	statuses := map[int]string{0: "none", 503: "503", 418: "other", -3: "other"}
	for in, want := range statuses {
		if got := statusBucket(in); got != want {
			t.Errorf("statusBucket(%d) = %q, want %q", in, got, want)
		}
	}

	if got := cleanInt(1080, knownHeight); got != "1080" {
		t.Errorf("cleanInt(1080) = %q", got)
	}
	if got := cleanInt(1081, knownHeight); got != "other" {
		t.Errorf("cleanInt(1081) = %q, want other", got)
	}
}
