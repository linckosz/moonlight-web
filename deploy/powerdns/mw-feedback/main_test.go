package main

import (
	"bytes"
	"encoding/json"
	"errors"
	"io"
	"log"
	"net/http"
	"net/http/httptest"
	"os"
	"strconv"
	"strings"
	"testing"
	"time"
)

// No test opens a socket: Discord is a RoundTripper below.

const (
	testToken = "S3CR3T-T0KEN-do-not-log"
	testHook  = "https://discord.com/api/webhooks/123456789/" + testToken
	testBits  = 8 // the real 20 would cost each test about a second
)

var t0 = time.Date(2026, 11, 26, 19, 30, 0, 0, time.UTC)

type stubDiscord struct {
	calls  int
	bodies [][]byte
	urls   []string
	status int
	err    error
	header http.Header
}

func (d *stubDiscord) RoundTrip(r *http.Request) (*http.Response, error) {
	d.calls++
	d.urls = append(d.urls, r.URL.String())
	b, _ := io.ReadAll(r.Body)
	d.bodies = append(d.bodies, b)
	if d.err != nil {
		return nil, d.err
	}
	h := d.header
	if h == nil {
		h = http.Header{}
	}
	return &http.Response{StatusCode: d.status, Header: h,
		Body: io.NopCloser(strings.NewReader("{}")), Request: r}, nil
}

func captureLog(t *testing.T) *bytes.Buffer {
	t.Helper()
	var buf bytes.Buffer
	log.SetOutput(&buf)
	t.Cleanup(func() { log.SetOutput(os.Stderr) })
	return &buf
}

func testServer(t *testing.T, mut func(*config)) (*server, *stubDiscord, *time.Time) {
	t.Helper()
	cfg := config{webhook: testHook, maxPerHour: 30, maxPerDay: 100, powBits: testBits}
	if mut != nil {
		mut(&cfg)
	}
	s, err := newServer(cfg)
	if err != nil {
		t.Fatal(err)
	}
	d := &stubDiscord{status: http.StatusOK}
	s.client.Transport = d
	now := t0
	s.now = func() time.Time { return now }
	return s, d, &now
}

func solve(token string, bits int) string {
	for i := int64(0); ; i++ {
		p := strconv.FormatInt(i, 36)
		if leadingZeroBits(token, p) >= bits {
			return p
		}
	}
}

func getChallenge(t *testing.T, s *server) string {
	t.Helper()
	w := httptest.NewRecorder()
	newMux(s).ServeHTTP(w, httptest.NewRequest(http.MethodGet, "/v1/feedback/challenge", nil))
	if w.Code != http.StatusOK {
		t.Fatalf("challenge: %d %s", w.Code, w.Body)
	}
	var out struct {
		Challenge string `json:"challenge"`
		Bits      int    `json:"bits"`
	}
	if err := json.Unmarshal(w.Body.Bytes(), &out); err != nil || out.Bits != testBits {
		t.Fatalf("challenge body %s", w.Body)
	}
	return out.Challenge
}

func postFeedback(t *testing.T, s *server, body map[string]any) *httptest.ResponseRecorder {
	t.Helper()
	b, _ := json.Marshal(body)
	w := httptest.NewRecorder()
	newMux(s).ServeHTTP(w, httptest.NewRequest(http.MethodPost, "/v1/feedback", bytes.NewReader(b)))
	return w
}

// fresh: a challenge issued at *now, solved, and *now moved past the minimum age.
func fresh(t *testing.T, s *server, now *time.Time, fields map[string]any) map[string]any {
	t.Helper()
	tok := getChallenge(t, s)
	*now = now.Add(challengeMinAge + time.Second)
	m := map[string]any{"challenge": tok, "pow": solve(tok, testBits)}
	for k, v := range fields {
		m[k] = v
	}
	return m
}

const okMessage = "The stream stutters when I switch apps."

// ── Challenge ───────────────────────────────────────────────────────────────

func TestChallengeAges(t *testing.T) {
	c, _ := newChallenger(testBits, 100)
	tok, _ := c.issue(t0)
	pow := solve(tok, testBits)
	if err := c.verify(tok, pow, t0.Add(4*time.Second)); err != errChallengeEarly {
		t.Fatalf("4 s old: %v, want early", err)
	}
	if err := c.verify(tok, pow, t0.Add(challengeMaxAge+time.Second)); err != errChallengeExpired {
		t.Fatalf("past max age: %v, want expired", err)
	}
	// The early and expired attempts spent nothing: the token still works once.
	if err := c.verify(tok, pow, t0.Add(challengeMinAge)); err != nil {
		t.Fatalf("at min age: %v", err)
	}
	if err := c.verify(tok, pow, t0.Add(challengeMinAge+time.Second)); err != errChallengeReplay {
		t.Fatalf("second use: %v, want replayed", err)
	}
}

func TestChallengeSignatureAndWork(t *testing.T) {
	c, _ := newChallenger(testBits, 100)
	other, _ := newChallenger(testBits, 100)
	tok, _ := c.issue(t0)
	at := t0.Add(6 * time.Second)

	foreign, _ := other.issue(t0) // signed with another key
	if err := c.verify(foreign, solve(foreign, testBits), at); err != errChallengeInvalid {
		t.Fatalf("foreign key: %v", err)
	}
	// One character changed anywhere in the token breaks the signature.
	for _, i := range []int{0, 5, 20, len(tok) - 2} {
		b := []byte(tok)
		if b[i] == 'A' {
			b[i] = 'B'
		} else {
			b[i] = 'A'
		}
		if err := c.verify(string(b), solve(string(b), testBits), at); err != errChallengeInvalid {
			t.Fatalf("tampered at %d: %v", i, err)
		}
	}
	for _, bad := range []string{"", "x", tok + "A", strings.Repeat("A", len(tok))} {
		if err := c.verify(bad, "0", at); err != errChallengeInvalid {
			t.Fatalf("malformed %q: %v", bad, err)
		}
	}
	// Wrong work, or a counter that is not plain, is refused and spends nothing.
	wrong := "0"
	for leadingZeroBits(tok, wrong) >= testBits {
		wrong += "0"
	}
	for _, p := range []string{wrong, "", "a b", strings.Repeat("1", maxPowLen+1), "é"} {
		if err := c.verify(tok, p, at); err != errPowInvalid {
			t.Fatalf("pow %q: %v", p, err)
		}
	}
	if err := c.verify(tok, solve(tok, testBits), at); err != nil {
		t.Fatalf("right work after wrong ones: %v", err)
	}
}

func TestChallengeNonceSetBounded(t *testing.T) {
	c, _ := newChallenger(testBits, 2)
	spend := func(issued, at time.Time) error {
		tok, _ := c.issue(issued)
		return c.verify(tok, solve(tok, testBits), at)
	}
	at := t0.Add(6 * time.Second)
	if spend(t0, at) != nil || spend(t0, at) != nil {
		t.Fatal("first two")
	}
	if err := spend(t0, at); err != errNoncesFull {
		t.Fatalf("third while full: %v", err)
	}
	// Once the remembered ones are past their max age, they are purged.
	later := t0.Add(challengeMaxAge + time.Minute)
	if err := spend(later.Add(-6*time.Second), later); err != nil {
		t.Fatalf("after expiry: %v", err)
	}
	if len(c.used) != 1 {
		t.Fatalf("set holds %d, want 1", len(c.used))
	}
}

// ── The endpoint ────────────────────────────────────────────────────────────

func TestSendPostsSafePayload(t *testing.T) {
	logs := captureLog(t)
	s, d, now := testServer(t, nil)
	w := postFeedback(t, s, fresh(t, s, now, map[string]any{
		"message": okMessage, "email": "player@example.org",
		"meta": map[string]string{"version": "0.4.4", "edition": "release", "os": "windows"},
	}))
	if w.Code != http.StatusOK || d.calls != 1 {
		t.Fatalf("code %d, calls %d: %s", w.Code, d.calls, w.Body)
	}
	if !strings.Contains(d.urls[0], "wait=true") {
		t.Fatalf("url without wait=true")
	}
	raw := string(d.bodies[0])
	if !strings.Contains(raw, `"allowed_mentions":{"parse":[]}`) || !strings.Contains(raw, `"flags":4`) {
		t.Fatalf("payload lacks the mention or embed guard: %s", raw)
	}
	var p discordPayload
	_ = json.Unmarshal(d.bodies[0], &p)
	want := "**New feedback**\n```text\nEmail: player@example.org\nSent from: 0.4.4, release, windows\n\n" +
		okMessage + "\n```"
	if p.Content != want {
		t.Fatalf("content\n%q\nwant\n%q", p.Content, want)
	}
	// F3/F9: the log says it was sent, and nothing of what was sent.
	for _, secret := range []string{"stutters", "player@example.org", "0.4.4", testToken, "discord.com"} {
		if strings.Contains(logs.String(), secret) {
			t.Fatalf("log contains %q: %s", secret, logs)
		}
	}
	if !strings.Contains(logs.String(), "[mw-feedback] sent") {
		t.Fatalf("log: %s", logs)
	}
}

func TestReplayIsRefused(t *testing.T) {
	s, d, now := testServer(t, nil)
	body := fresh(t, s, now, map[string]any{"message": okMessage})
	if w := postFeedback(t, s, body); w.Code != http.StatusOK {
		t.Fatalf("first: %d", w.Code)
	}
	if w := postFeedback(t, s, body); w.Code != http.StatusBadRequest || !strings.Contains(w.Body.String(), "challenge_replayed") {
		t.Fatalf("replay: %d %s", w.Code, w.Body)
	}
	if d.calls != 1 {
		t.Fatalf("discord called %d times", d.calls)
	}
}

func TestTooEarlyIs425(t *testing.T) {
	s, d, now := testServer(t, nil)
	tok := getChallenge(t, s)
	*now = now.Add(2 * time.Second)
	w := postFeedback(t, s, map[string]any{"challenge": tok, "pow": solve(tok, testBits), "message": okMessage})
	if w.Code != http.StatusTooEarly || d.calls != 0 {
		t.Fatalf("code %d, calls %d", w.Code, d.calls)
	}
}

func TestHiddenFieldDropsSilently(t *testing.T) {
	s, d, now := testServer(t, nil)
	w := postFeedback(t, s, fresh(t, s, now, map[string]any{"message": okMessage, "website": "http://spam"}))
	if w.Code != http.StatusOK || d.calls != 0 {
		t.Fatalf("code %d, calls %d", w.Code, d.calls)
	}
}

func TestGlobalCaps(t *testing.T) {
	s, d, now := testServer(t, func(c *config) { c.maxPerHour = 2; c.maxPerDay = 3 })
	send := func() int {
		return postFeedback(t, s, fresh(t, s, now, map[string]any{"message": okMessage})).Code
	}
	if send() != 200 || send() != 200 {
		t.Fatal("first two")
	}
	if c := send(); c != http.StatusTooManyRequests {
		t.Fatalf("third in the hour: %d", c)
	}
	*now = now.Add(time.Hour)
	if c := send(); c != 200 {
		t.Fatalf("next hour: %d", c)
	}
	if c := send(); c != http.StatusTooManyRequests {
		t.Fatalf("over the day: %d", c)
	}
	if d.calls != 3 {
		t.Fatalf("discord called %d times", d.calls)
	}
}

func TestDiscordAnswers(t *testing.T) {
	for _, tc := range []struct{ discord, want int }{
		{http.StatusNoContent, 200},
		{http.StatusTooManyRequests, http.StatusServiceUnavailable},
		{http.StatusInternalServerError, http.StatusBadGateway},
		{http.StatusNotFound, http.StatusBadGateway},
	} {
		s, d, now := testServer(t, nil)
		d.status = tc.discord
		if w := postFeedback(t, s, fresh(t, s, now, map[string]any{"message": okMessage})); w.Code != tc.want {
			t.Fatalf("discord %d → %d, want %d", tc.discord, w.Code, tc.want)
		}
		if d.calls != 1 {
			t.Fatalf("discord %d: %d calls, no retry expected", tc.discord, d.calls)
		}
	}
}

func TestOffWithoutWebhook(t *testing.T) {
	for _, hook := range []string{"", "https://evil.example/api/webhooks/1/x"} {
		logs := captureLog(t)
		s, _, _ := testServer(t, func(c *config) { c.webhook = hook })
		w := httptest.NewRecorder()
		newMux(s).ServeHTTP(w, httptest.NewRequest(http.MethodGet, "/v1/feedback/challenge", nil))
		if w.Code != http.StatusServiceUnavailable {
			t.Fatalf("%q: challenge %d", hook, w.Code)
		}
		if w := postFeedback(t, s, map[string]any{"message": okMessage}); w.Code != http.StatusServiceUnavailable {
			t.Fatalf("%q: post %d", hook, w.Code)
		}
		if hook != "" && strings.Contains(logs.String(), "evil") {
			t.Fatalf("invalid webhook value logged: %s", logs)
		}
	}
}

func TestBodyBounds(t *testing.T) {
	s, d, now := testServer(t, nil)
	big := fresh(t, s, now, map[string]any{"message": strings.Repeat("a", maxBody)})
	if w := postFeedback(t, s, big); w.Code != http.StatusRequestEntityTooLarge {
		t.Fatalf("oversized body: %d", w.Code)
	}
	w := httptest.NewRecorder()
	newMux(s).ServeHTTP(w, httptest.NewRequest(http.MethodPost, "/v1/feedback", strings.NewReader("{not json")))
	if w.Code != http.StatusBadRequest {
		t.Fatalf("bad json: %d", w.Code)
	}
	w = httptest.NewRecorder()
	newMux(s).ServeHTTP(w, httptest.NewRequest(http.MethodGet, "/v1/feedback", nil))
	if w.Code != http.StatusMethodNotAllowed {
		t.Fatalf("GET: %d", w.Code)
	}
	if d.calls != 0 {
		t.Fatal("discord called")
	}
}

// ── F4: the webhook never leaves this process ───────────────────────────────

func TestParseWebhook(t *testing.T) {
	good := map[string]string{
		testHook:                        testHook + "?wait=true",
		testHook + "?thread_id=42":      testHook + "?thread_id=42&wait=true",
		"  " + testHook + "\n":          testHook + "?wait=true",
		testHook + "?wait=false":        testHook + "?wait=true",
	}
	for in, want := range good {
		u, err := parseWebhook(in)
		if err != nil || u.String() != want {
			t.Fatalf("%q → %v %v, want %q", in, u, err, want)
		}
	}
	for _, bad := range []string{
		"http://discord.com/api/webhooks/1/x",
		"https://discordapp.com/api/webhooks/1/x",
		"https://discord.com.evil.example/api/webhooks/1/x",
		"https://discord.com@evil.example/api/webhooks/1/x",
		"https://discord.com/api/webhooks/1/x/../../../users",
		"https://discord.com/api/webhooks/1",
		"https://discord.com/api/webhooks/1/x#frag",
		"https://discord.com/api/channels/1/messages",
	} {
		if _, err := parseWebhook(bad); err == nil {
			t.Fatalf("accepted %q", bad)
		}
	}
}

func TestNetworkErrorLogsNoURL(t *testing.T) {
	logs := captureLog(t)
	s, d, now := testServer(t, nil)
	d.err = errors.New("dial tcp: connection refused")
	w := postFeedback(t, s, fresh(t, s, now, map[string]any{"message": okMessage}))
	if w.Code != http.StatusBadGateway {
		t.Fatalf("code %d", w.Code)
	}
	for _, leak := range []string{testToken, "discord.com", "webhooks", "123456789"} {
		if strings.Contains(logs.String(), leak) || strings.Contains(w.Body.String(), leak) {
			t.Fatalf("%q leaked: log %q body %q", leak, logs, w.Body)
		}
	}
}

func TestRedirectNotFollowed(t *testing.T) {
	s, d, now := testServer(t, nil)
	d.status = http.StatusFound
	d.header = http.Header{"Location": {"https://evil.example/collect"}}
	w := postFeedback(t, s, fresh(t, s, now, map[string]any{"message": okMessage}))
	if w.Code != http.StatusBadGateway || d.calls != 1 {
		t.Fatalf("code %d, calls %d", w.Code, d.calls)
	}
}

func TestRecovererLogsNothingOfThePanic(t *testing.T) {
	logs := captureLog(t)
	h := recoverer(http.HandlerFunc(func(http.ResponseWriter, *http.Request) {
		panic("boom " + testToken + " player@example.org")
	}))
	w := httptest.NewRecorder()
	h.ServeHTTP(w, httptest.NewRequest(http.MethodPost, "/v1/feedback?x="+testToken, nil))
	if w.Code != http.StatusInternalServerError {
		t.Fatalf("code %d", w.Code)
	}
	if strings.Contains(logs.String(), testToken) || strings.Contains(logs.String(), "player") ||
		!strings.Contains(logs.String(), "internal error") {
		t.Fatalf("log: %q", logs)
	}
}

// ── F7: Discord's 2000 characters hold the whole message ────────────────────

func TestContentWorstCaseFitsDiscord(t *testing.T) {
	email := strings.Repeat("a", 64) + "@" + strings.Repeat("b", 63) + "." +
		strings.Repeat("c", 63) + "." + strings.Repeat("d", 61) // 254
	if len(email) != maxEmailLen || !emailRe.MatchString(email) {
		t.Fatalf("test e-mail: %d", len(email))
	}
	version := "123.123.123-" + strings.Repeat("x", 20) // 32
	if len(version) != maxVersionLen || !versionRe.MatchString(version) {
		t.Fatal("test version")
	}
	// Astral characters count two units each, as in Discord.
	for _, msg := range []string{strings.Repeat("é", maxMessageUnits), strings.Repeat("😀", maxMessageUnits/2)} {
		f, code := validate(feedbackRequest{Message: msg, Email: email,
			Meta: &struct {
				Version string `json:"version"`
				Edition string `json:"edition"`
				OS      string `json:"os"`
			}{version, "release", "windows"}})
		if code != "" {
			t.Fatalf("at the limit: %s", code)
		}
		if n := utf16Len(f.content()); n > discordMaxContent {
			t.Fatalf("worst case is %d units", n)
		}
	}
	if _, code := validate(feedbackRequest{Message: strings.Repeat("é", maxMessageUnits+1)}); code != "message_long" {
		t.Fatalf("one over: %q", code)
	}
	if _, code := validate(feedbackRequest{Message: strings.Repeat("😀", maxMessageUnits/2) + "a"}); code != "message_long" {
		t.Fatalf("astral one over: %q", code)
	}
	if _, code := validate(feedbackRequest{Message: "  short\u202E  "}); code != "message_short" {
		t.Fatalf("short: %q", code)
	}
}

// ── F8: nothing in the text can mention, link or escape ─────────────────────

func TestMessageNeutralised(t *testing.T) {
	msg := "@everyone @here <@123> <@&456> [click](https://evil.example)\n" +
		"```\nescaped?\n``` `inline` " +
		"\x00\x07\x1b[31m\u202Eevil\u202C\u2066iso\u2069\u200B\uFEFF\u200E" +
		"\r\nnext\u2028line 👨\u200D👩"
	f, code := validate(feedbackRequest{Message: msg})
	if code != "" {
		t.Fatal(code)
	}
	c := f.content()
	inner := strings.TrimSuffix(strings.TrimPrefix(c, contentHead), contentTail)
	if strings.Contains(inner, "`") {
		t.Fatalf("a backtick survived: %q", inner)
	}
	if strings.Count(c, "```") != 2 {
		t.Fatalf("code block not intact: %q", c)
	}
	for _, r := range inner {
		if r == '\u200D' || r == '\n' || r == '\t' {
			continue
		}
		if r < 0x20 || r == 0x7f || (r >= 0x80 && r < 0xa0) || r == '\r' ||
			(r >= 0x202a && r <= 0x202e) || (r >= 0x2066 && r <= 0x2069) ||
			r == 0x200b || r == 0xfeff || r == 0x200e || r == 0x2028 {
			t.Fatalf("U+%04X survived in %q", r, inner)
		}
	}
	// Mentions are inert text inside the block, and the payload forbids pings.
	if !strings.Contains(inner, "@everyone") || !strings.Contains(inner, "👨\u200D👩") ||
		!strings.Contains(inner, "\nnext\nline") {
		t.Fatalf("text altered beyond neutralising: %q", inner)
	}
	p, _ := f.payload()
	if !bytes.Contains(p, []byte(`"allowed_mentions":{"parse":[]}`)) || !bytes.Contains(p, []byte(`"flags":4`)) {
		t.Fatalf("payload: %s", p)
	}
}

func TestEmailAndMetaTraps(t *testing.T) {
	for _, e := range []string{
		"@everyone", "a`b@example.org", "a@example.org\n```", "a@example.org```",
		"<@123>@example.org", "a@b", "a b@example.org", "a@exa\u202Emple.org",
		"[x](https://evil.example)@example.org", "a@example.org\x00",
		strings.Repeat("a", 65) + "@example.org",
	} {
		if _, code := validate(feedbackRequest{Message: okMessage, Email: e}); code != "email_invalid" {
			t.Fatalf("e-mail %q accepted", e)
		}
	}
	if f, code := validate(feedbackRequest{Message: okMessage, Email: " First.Last+mw@sub.example.co.uk "}); code != "" || f.email != "First.Last+mw@sub.example.co.uk" {
		t.Fatalf("plain e-mail refused: %q", code)
	}
	// Unknown metadata is dropped, never written.
	f, code := validate(feedbackRequest{Message: okMessage, Meta: &struct {
		Version string `json:"version"`
		Edition string `json:"edition"`
		OS      string `json:"os"`
	}{"0.4.4```@everyone", "nightly", "Windows 11 Pro – DESKTOP-ABC123"}})
	if code != "" || f.sentFrom() != "" {
		t.Fatalf("meta kept: %q %q", code, f.sentFrom())
	}
	if strings.Contains(f.content(), "Sent from") || strings.Contains(f.content(), "Email:") {
		t.Fatalf("empty lines written: %q", f.content())
	}
}
