package main

import (
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
	"time"
)

const mwsrTestOrigin = "https://moonlightweb.top"

var mwsrTestNow = time.Date(2026, 11, 4, 8, 47, 13, 0, time.UTC)

func testMwsrSink(t *testing.T, mut func(*mwsrConfig)) (*mwsrSink, chan captured) {
	t.Helper()
	got := make(chan captured, 8)
	umami := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		body, _ := io.ReadAll(r.Body)
		var ev umamiEvent
		if err := json.Unmarshal(body, &ev); err != nil {
			t.Errorf("umami received invalid JSON: %v", err)
		}
		got <- captured{event: ev, ua: r.Header.Get("User-Agent"), xff: r.Header.Get("X-Forwarded-For")}
		w.WriteHeader(http.StatusOK)
	}))
	t.Cleanup(umami.Close)
	cfg := mwsrConfig{
		umamiURL:     umami.URL,
		umamiWebsite: "site",
		hostname:     "moonlightweb.top",
		origin:       mwsrTestOrigin,
		dir:          filepath.Join(t.TempDir(), "measures"),
		maxPerDay:    100,
		maxBytes:     1 << 20,
	}
	if mut != nil {
		mut(&cfg)
	}
	s := newMwsrSink(cfg)
	s.now = func() time.Time { return mwsrTestNow }
	return s, got
}

func postMwsr(t *testing.T, s *mwsrSink, body string) *httptest.ResponseRecorder {
	t.Helper()
	req := httptest.NewRequest(http.MethodPost, "/v1/mwsr", strings.NewReader(body))
	req.Header.Set("Origin", mwsrTestOrigin)
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("X-Forwarded-For", "203.0.113.7")
	req.Header.Set("User-Agent", "Mozilla/5.0 secret-browser")
	req.Header.Set("Accept-Language", "fr-FR")
	req.Header.Set("Referer", "https://elsewhere.example/page")
	w := httptest.NewRecorder()
	s.ServeHTTP(w, req)
	return w
}

func readMwsrLog(t *testing.T, s *mwsrSink) string {
	t.Helper()
	b, err := os.ReadFile(filepath.Join(s.cfg.dir, "2026-11.jsonl"))
	if err != nil {
		if os.IsNotExist(err) {
			return ""
		}
		t.Fatal(err)
	}
	return string(b)
}

const mwsrGoodBody = `{"v":1,"page":"abc1234","shader":"mwsr-j","browser":"Safari","browserMajor":27,
"os":"ios","osMajor":26,"mobile":true,"gpuVendor":"apple","gpuArch":"common-7","f16":true,
"canvasW":2532,"canvasH":1170,"inputW":1266,"inputH":585,"dpr":3,"gamut":"p3","picture":"re9-street",
"scale":"x2","chroma":"420","path":"webgpu",
"results":[{"k":"mwsr","half":true,"lpips":0.12,"psnr":34.5,"gpuMs":1.5,"gpuSpread":0.1,"dropped":1}]}`

func TestMwsrKeepsTheCleanedLineAndCountsIt(t *testing.T) {
	s, got := testMwsrSink(t, nil)
	if w := postMwsr(t, s, mwsrGoodBody); w.Code != http.StatusNoContent {
		t.Fatalf("status %d", w.Code)
	}
	var rec mwsrRecord
	if err := json.Unmarshal([]byte(readMwsrLog(t, s)), &rec); err != nil {
		t.Fatal(err)
	}
	if rec.At != "2026-11-04T08Z" {
		t.Errorf("the time must stop at the hour, got %q", rec.At)
	}
	if rec.Browser != "safari" || rec.GpuVendor != "apple" || len(rec.Results) != 1 || *rec.Results[0].GpuMs != 1.5 {
		t.Errorf("the record lost a measure: %+v", rec)
	}
	c := <-got
	if c.event.Payload.Name != "" {
		t.Errorf("a measure must be a pageview, got event %q", c.event.Payload.Name)
	}
	if !strings.HasPrefix(c.event.Payload.URL, "/m/webgpu/apple?") {
		t.Errorf("url %q", c.event.Payload.URL)
	}
}

func TestMwsrNothingAboutThePersonGoesAnywhere(t *testing.T) {
	s, got := testMwsrSink(t, nil)
	// Everything a forged client could try: extra fields and hostile values too.
	body := `{"v":1,"ip":"198.51.100.9","lang":"fr-FR","userAgent":"Mozilla secret","referrer":"https://x.example",
"timezone":"Europe/Paris","picture":"my holiday photo.png","page":"<script>","gpuVendor":"apple",
"results":[{"k":"mwsr","name":"bruno"}]}`
	postMwsr(t, s, body)
	line := readMwsrLog(t, s)
	c := <-got
	sent, _ := json.Marshal(c.event)
	for _, where := range []string{line, string(sent)} {
		for _, bad := range []string{
			"203.0.113.7", "198.51.100.9", "fr-FR", "secret", "x.example", "elsewhere",
			"Europe", "holiday", "script", "bruno",
		} {
			if strings.Contains(where, bad) {
				t.Errorf("%q went through: %s", bad, where)
			}
		}
	}
	if c.xff != mwsrUmamiAddr || c.ua != mwsrUmamiUA {
		t.Errorf("umami must see only the fixed address and UA, got %q / %q", c.xff, c.ua)
	}
}

// Umami's bot filter drops a record without an error, so a wrong agent fails
// silently. The census agents it does count all read "MoonlightWeb/version (a; b)":
// keep that shape, with no word a bot list knows.
func TestMwsrUmamiAgentHasTheShapeUmamiCounts(t *testing.T) {
	shape := regexp.MustCompile(`^MoonlightWeb/[0-9.]+ \([a-z0-9]+; [a-z0-9]+\)$`)
	if !shape.MatchString(mwsrUmamiUA) {
		t.Errorf("%q does not read like the census agents Umami counts", mwsrUmamiUA)
	}
	lower := strings.ToLower(mwsrUmamiUA)
	// "measure" is not a guess: Umami 3.2.0 dropped "MoonlightWeb/1 (mwsr; measure)".
	for _, bot := range []string{"bot", "crawl", "spider", "http", "headless", "fetch", "scan", "monitor", "check", "measure"} {
		if strings.Contains(lower, bot) {
			t.Errorf("%q contains %q, which a bot filter matches", mwsrUmamiUA, bot)
		}
	}
}

func TestMwsrBrowserMessageIsBounded(t *testing.T) {
	got := mwsrMessage("Fragment library\nfailed at https://moonlightweb.top/mwsr/a.js:1 " + strings.Repeat("é€x", 500))
	if strings.Contains(got, "moonlightweb") || !strings.Contains(got, "<url>") {
		t.Errorf("an address survived: %q", got)
	}
	if strings.ContainsAny(got, "\né€") {
		t.Errorf("not one line of ASCII: %q", got)
	}
	if len(got) > mwsrMaxMsg {
		t.Errorf("message of %d bytes", len(got))
	}
}

func TestMwsrOutOfRangeValueIsDroppedNotClamped(t *testing.T) {
	s, got := testMwsrSink(t, nil)
	postMwsr(t, s, `{"v":1,"canvasW":99999999,"results":[{"k":"mwsr","ssim":5,"gpuMs":-1,"psnr":30}]}`)
	<-got
	var rec mwsrRecord
	if err := json.Unmarshal([]byte(readMwsrLog(t, s)), &rec); err != nil {
		t.Fatal(err)
	}
	x := rec.Results[0]
	if rec.CanvasW != 0 || x.SSIM != nil || x.GpuMs != nil || x.PSNR == nil {
		t.Errorf("%+v / %+v", rec, x)
	}
}

// The page times each text in 3 batches at the first measure after a load, 6 after: a spread
// reads only next to its count, so the count is kept, and a count out of range is dropped.
func TestMwsrKeepsTheBatchCount(t *testing.T) {
	for _, c := range []struct {
		body string
		want int
	}{
		{`{"v":1,"batches":3,"results":[{"k":"mwsr","gpuMs":1.5,"gpuSpread":0.1}]}`, 3},
		{`{"v":1,"batches":6,"results":[{"k":"mwsr","gpuMs":1.5,"gpuSpread":0.1}]}`, 6},
		{`{"v":1,"batches":99999,"results":[{"k":"mwsr","gpuMs":1.5}]}`, 0},
		{`{"v":1,"batches":-3,"results":[{"k":"mwsr","gpuMs":1.5}]}`, 0},
	} {
		s, got := testMwsrSink(t, nil)
		postMwsr(t, s, c.body)
		<-got
		var rec mwsrRecord
		if err := json.Unmarshal([]byte(readMwsrLog(t, s)), &rec); err != nil {
			t.Fatal(err)
		}
		if rec.Batches != c.want {
			t.Errorf("%s: kept %d batches, want %d", c.body, rec.Batches, c.want)
		}
	}
}

func TestMwsrFailureAddsANamedEvent(t *testing.T) {
	s, got := testMwsrSink(t, nil)
	postMwsr(t, s, `{"v":1,"path":"webgpu","fail":{"step":"pipeline","shader":"mwsr-down-1","msg":"Fragment library could not be created"}}`)
	names := map[string]bool{}
	for i := 0; i < 2; i++ {
		names[(<-got).event.Payload.Name] = true
	}
	if !names[""] || !names["failed-pipeline"] {
		t.Errorf("expected a pageview and a failed-pipeline event, got %v", names)
	}
}

func TestMwsrRefusals(t *testing.T) {
	s, _ := testMwsrSink(t, nil)

	req := httptest.NewRequest(http.MethodGet, "/v1/mwsr", nil)
	req.Header.Set("Origin", mwsrTestOrigin)
	w := httptest.NewRecorder()
	s.ServeHTTP(w, req)
	if w.Code != http.StatusMethodNotAllowed {
		t.Errorf("GET: %d", w.Code)
	}

	req = httptest.NewRequest(http.MethodPost, "/v1/mwsr", strings.NewReader(mwsrGoodBody))
	req.Header.Set("Origin", "https://elsewhere.example")
	w = httptest.NewRecorder()
	s.ServeHTTP(w, req)
	if w.Code != http.StatusForbidden {
		t.Errorf("foreign origin: %d", w.Code)
	}

	if w := postMwsr(t, s, "{"+strings.Repeat(" ", mwsrMaxBody+1)+"}"); w.Code != http.StatusRequestEntityTooLarge {
		t.Errorf("large body: %d", w.Code)
	}
	if w := postMwsr(t, s, "not json"); w.Code != http.StatusBadRequest {
		t.Errorf("bad body: %d", w.Code)
	}
	if line := readMwsrLog(t, s); line != "" {
		t.Errorf("a refused post was kept: %s", line)
	}
}

// Safari on iOS posts from the page itself with "Origin: null": taken only when
// the browser also says the request is same-origin.
func TestMwsrNullOriginOnlyFromThePage(t *testing.T) {
	s, got := testMwsrSink(t, nil)
	for _, c := range []struct {
		site string
		want int
	}{
		{"same-origin", http.StatusNoContent},
		{"cross-site", http.StatusForbidden},
		{"same-site", http.StatusForbidden},
		{"", http.StatusForbidden},
	} {
		req := httptest.NewRequest(http.MethodPost, "/v1/mwsr", strings.NewReader(mwsrGoodBody))
		req.Header.Set("Origin", "null")
		req.Header.Set("Content-Type", "application/json")
		if c.site != "" {
			req.Header.Set("Sec-Fetch-Site", c.site)
		}
		w := httptest.NewRecorder()
		s.ServeHTTP(w, req)
		if w.Code != c.want {
			t.Errorf("Origin: null, Sec-Fetch-Site %q: %d, want %d", c.site, w.Code, c.want)
		}
	}
	<-got
	if n := strings.Count(readMwsrLog(t, s), "\n"); n != 1 {
		t.Errorf("kept %d lines, only the same-origin post may be", n)
	}
}

func TestMwsrCapsDropWithoutSayingSo(t *testing.T) {
	s, got := testMwsrSink(t, func(c *mwsrConfig) { c.maxPerDay = 2 })
	for i := 0; i < 3; i++ {
		if w := postMwsr(t, s, mwsrGoodBody); w.Code != http.StatusNoContent {
			t.Fatalf("post %d: %d", i, w.Code)
		}
	}
	if n := strings.Count(readMwsrLog(t, s), "\n"); n != 2 {
		t.Errorf("kept %d lines, the cap is 2", n)
	}
	<-got
	<-got
	select {
	case c := <-got:
		t.Errorf("a dropped measure was counted: %+v", c.event)
	case <-time.After(100 * time.Millisecond):
	}

	// The next day starts a new budget.
	s.now = func() time.Time { return mwsrTestNow.Add(24 * time.Hour) }
	postMwsr(t, s, mwsrGoodBody)
	<-got

	s2, _ := testMwsrSink(t, func(c *mwsrConfig) { c.maxBytes = 10 })
	postMwsr(t, s2, mwsrGoodBody)
	if line := readMwsrLog(t, s2); line != "" {
		t.Errorf("past the size cap, a line was kept: %s", line)
	}
}

func TestMwsrSizeOfExistingFilesCountsAfterARestart(t *testing.T) {
	s, _ := testMwsrSink(t, nil)
	if err := os.MkdirAll(s.cfg.dir, 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(s.cfg.dir, "2026-10.jsonl"), make([]byte, 1234), 0o600); err != nil {
		t.Fatal(err)
	}
	if again := newMwsrSink(s.cfg); again.bytes != 1234 {
		t.Errorf("bytes = %d", again.bytes)
	}
}

func TestMwsrWorksWithTheCountOff(t *testing.T) {
	s, _ := testMwsrSink(t, func(c *mwsrConfig) { c.umamiWebsite = "" })
	if w := postMwsr(t, s, mwsrGoodBody); w.Code != http.StatusNoContent {
		t.Fatalf("status %d", w.Code)
	}
	if readMwsrLog(t, s) == "" {
		t.Error("the file must be kept even with the Umami count off")
	}
}
