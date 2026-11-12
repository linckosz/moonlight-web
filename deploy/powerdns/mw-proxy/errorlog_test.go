package main

import (
	"bufio"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

var errTestNow = time.Date(2026, 11, 12, 14, 35, 10, 0, time.UTC)

// Every test here answers through a recorder: none opens a socket.
func testErrSink(t *testing.T, mut func(*errConfig)) *errSink {
	t.Helper()
	cfg := errConfig{
		dir:        filepath.Join(t.TempDir(), "reports"),
		maxPerDay:  100,
		maxDayByte: 1 << 20,
		maxBytes:   8 << 20,
		maxAge:     90 * 24 * time.Hour,
	}
	if mut != nil {
		mut(&cfg)
	}
	s := newErrSink(cfg, "key")
	s.now = func() time.Time { return errTestNow }
	return s
}

func postErr(t *testing.T, s *errSink, key, body string) *httptest.ResponseRecorder {
	t.Helper()
	req := httptest.NewRequest(http.MethodPost, "/v1/errors", strings.NewReader(body))
	req.Header.Set("X-API-Key", key)
	w := httptest.NewRecorder()
	s.ServeHTTP(w, req)
	return w
}

func readErrRecords(t *testing.T, s *errSink, day string) []errRecord {
	t.Helper()
	f, err := os.Open(filepath.Join(s.cfg.dir, day+".jsonl"))
	if os.IsNotExist(err) {
		return nil
	}
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	var out []errRecord
	sc := bufio.NewScanner(f)
	sc.Buffer(make([]byte, 0, 1<<20), 1<<20)
	for sc.Scan() {
		var r errRecord
		if err := json.Unmarshal(sc.Bytes(), &r); err != nil {
			t.Fatalf("a kept line is not JSON: %v", err)
		}
		out = append(out, r)
	}
	return out
}

const errSampleBody = `{"v":"0.4.4","os":"windows","arch":"x64","event":"failed","code":503,
 "h":1080,"fps":60,"codec":"hevc","backend":"gamestream","transport":"webrtc-dc-udp",
 "net":"private","client":"desktop","kind":"owner",
 "host":[{"ago":12,"l":"error","m":"[Stream] RTSP setup failed on {B} (192.168.x.{a1})"}],
 "pages":[{"who":"owner","browser":"chrome","major":131,
   "lines":[{"ago":3,"l":"error","n":4,"m":"[StreamView] decoder error: EncodingError"}]}]}`

func TestErrorsKeepsAReport(t *testing.T) {
	s := testErrSink(t, nil)
	if w := postErr(t, s, "key", errSampleBody); w.Code != http.StatusNoContent {
		t.Fatalf("status %d, want 204", w.Code)
	}
	recs := readErrRecords(t, s, "2026-11-12")
	if len(recs) != 1 {
		t.Fatalf("%d records, want 1", len(recs))
	}
	r := recs[0]
	if r.At != "2026-11-12T14Z" || r.Event != "failed" || r.Code != "503" || r.Codec != "hevc" ||
		r.Height != "1080" || r.Kind != "owner" {
		t.Fatalf("header not kept as sent: %+v", r)
	}
	if len(r.Host) != 1 || r.Host[0].Text != "[Stream] RTSP setup failed on {B} (192.168.x.{a1})" {
		t.Fatalf("the scrubber's stand-ins must pass the second net untouched: %+v", r.Host)
	}
	if len(r.Pages) != 1 || r.Pages[0].Browser != "chrome" || r.Pages[0].Major != 131 ||
		r.Pages[0].Lines[0].Count != 4 {
		t.Fatalf("page not kept as sent: %+v", r.Pages)
	}
}

func TestErrorsRefusesWithoutTheKeyOrPost(t *testing.T) {
	s := testErrSink(t, nil)
	if w := postErr(t, s, "wrong", errSampleBody); w.Code != http.StatusUnauthorized {
		t.Fatalf("wrong key: status %d, want 401", w.Code)
	}
	req := httptest.NewRequest(http.MethodGet, "/v1/errors", nil)
	req.Header.Set("X-API-Key", "key")
	w := httptest.NewRecorder()
	s.ServeHTTP(w, req)
	if w.Code != http.StatusMethodNotAllowed {
		t.Fatalf("GET: status %d, want 405", w.Code)
	}
	if w := postErr(t, s, "key", "{"+strings.Repeat(" ", errMaxBody)+"}"); w.Code != http.StatusRequestEntityTooLarge {
		t.Fatalf("large body: status %d, want 413", w.Code)
	}
	if recs := readErrRecords(t, s, "2026-11-12"); len(recs) != 0 {
		t.Fatalf("%d records kept from refused posts", len(recs))
	}
}

func TestErrorsSecondNet(t *testing.T) {
	cases := map[string]string{
		"see https://stream.example/abc?x=1 now":          "see <url> now",
		"mail bob.smith@example.co.uk failed":             "mail (hidden) failed",
		"peer 82.64.10.200:47998 refused":                 "peer (address):47998 refused",
		"bound 127.0.0.1 and 0.0.0.0":                     "bound 127.0.0.1 and 0.0.0.0",
		"driver 32.0.15.6094 loaded":                      "driver 32.0.15.6094 loaded",
		"ice 2a01:e0a:1f2:3b40::12 host":                  "ice (address) host",
		"ice 2a01:e0a:1f2:3b40:1:2:3:4 host":              "ice (address) host",
		"prefix 2a01:e0a:: routed":                        "prefix (address) routed",
		"link fe80::1%eth0 up":                            "link (address)%eth0 up",
		"lo ::1 stays":                                    "lo ::1 stays",
		"stand-in fd::{d1} and 2xxx::{1} stay":            "stand-in fd::{d1} and 2xxx::{1} stay",
		"at 12:34:56 fine":                                "at 12:34:56 fine",
		"token a8f3k29dk3m1q8z7x6c5v4b3n2m1l0k9 leaked":   "token (hidden) leaked",
		"in VideoDecoderConfigurationRecordParserImpl ok": "in VideoDecoderConfigurationRecordParserImpl ok",
		"two\nlines\tand\x07bell":                         "two lines and" + "bell",
		"bidi ‮evil":                                      "bidi evil",
		"accès refusé":                                    "accès refusé",
	}
	for in, want := range cases {
		if got := errText(in); got != want {
			t.Errorf("errText(%q) = %q, want %q", in, got, want)
		}
	}
	long := errText(strings.Repeat("é", 2*errMaxLine))
	if len(long) > errMaxLine+len(" […]") || !strings.HasSuffix(long, " […]") {
		t.Fatalf("a long line must be cut on a character, got %d bytes", len(long))
	}
}

func TestErrorsUnknownValuesAreBucketed(t *testing.T) {
	s := testErrSink(t, nil)
	body := `{"v":"evil<script>","os":"plan9","event":"boom","h":1081,"codec":"vp9",
	  "host":[{"ago":-5,"l":"fatal","n":-1,"m":"x"}],
	  "pages":[{"who":"admin","browser":"lynx","major":99999,"lines":[{"m":"y"}]},
	           {"who":"guest","browser":"safari","lines":[]}],
	  "secret":"never kept"}`
	postErr(t, s, "key", body)
	recs := readErrRecords(t, s, "2026-11-12")
	if len(recs) != 1 {
		t.Fatalf("%d records, want 1", len(recs))
	}
	r := recs[0]
	if r.Version != "unknown" || r.OS != "other" || r.Event != "other" || r.Height != "other" ||
		r.Codec != "other" || r.Code != "" || r.Duration != "" {
		t.Fatalf("unknown values not bucketed: %+v", r)
	}
	if r.Host[0].Ago != 0 || r.Host[0].Level != "error" || r.Host[0].Count != 0 {
		t.Fatalf("out-of-range line fields kept: %+v", r.Host[0])
	}
	if len(r.Pages) != 1 || r.Pages[0].Who != "other" || r.Pages[0].Browser != "other" || r.Pages[0].Major != 0 {
		t.Fatalf("page values not bucketed, or an empty page kept: %+v", r.Pages)
	}
	raw, _ := os.ReadFile(filepath.Join(s.cfg.dir, "2026-11-12.jsonl"))
	if strings.Contains(string(raw), "never kept") || strings.Contains(string(raw), "script") {
		t.Fatalf("a field without a rule reached the disk: %s", raw)
	}
}

func TestErrorsNothingToKeep(t *testing.T) {
	s := testErrSink(t, nil)
	if w := postErr(t, s, "key", `{"v":"0.4.4","event":"end","host":[{"m":"  "}]}`); w.Code != http.StatusNoContent {
		t.Fatalf("status %d, want 204", w.Code)
	}
	if _, err := os.Stat(filepath.Join(s.cfg.dir, "2026-11-12.jsonl")); !os.IsNotExist(err) {
		t.Fatalf("a report with no line left must not be written")
	}
}

func TestErrorsLineCaps(t *testing.T) {
	s := testErrSink(t, nil)
	var host, page []string
	for i := 0; i < errMaxHostLines+50; i++ {
		host = append(host, `{"l":"error","m":"h"}`)
	}
	for i := 0; i < errMaxPageLines+50; i++ {
		page = append(page, `{"l":"error","m":"p"}`)
	}
	var pages []string
	for i := 0; i < errMaxPages+3; i++ {
		pages = append(pages, `{"who":"owner","browser":"chrome","lines":[`+strings.Join(page, ",")+`]}`)
	}
	body := `{"event":"end","host":[` + strings.Join(host, ",") + `],"pages":[` + strings.Join(pages, ",") + `]}`
	if len(body) > errMaxBody {
		t.Skip("the body outgrew the cap; the 413 test covers it")
	}
	postErr(t, s, "key", body)
	r := readErrRecords(t, s, "2026-11-12")[0]
	if len(r.Host) != errMaxHostLines || len(r.Pages) != errMaxPages || len(r.Pages[0].Lines) != errMaxPageLines {
		t.Fatalf("caps not applied: host %d, pages %d, page lines %d",
			len(r.Host), len(r.Pages), len(r.Pages[0].Lines))
	}
}

func TestErrorsDayCaps(t *testing.T) {
	s := testErrSink(t, func(c *errConfig) { c.maxPerDay = 3 })
	for i := 0; i < 5; i++ {
		if w := postErr(t, s, "key", errSampleBody); w.Code != http.StatusNoContent {
			t.Fatalf("post %d: status %d, want 204 even when dropped", i, w.Code)
		}
	}
	if n := len(readErrRecords(t, s, "2026-11-12")); n != 3 {
		t.Fatalf("%d records, want the day's cap of 3", n)
	}
	// The next day starts again.
	s.now = func() time.Time { return errTestNow.Add(24 * time.Hour) }
	postErr(t, s, "key", errSampleBody)
	if n := len(readErrRecords(t, s, "2026-11-13")); n != 1 {
		t.Fatalf("%d records the next day, want 1", n)
	}

	b := testErrSink(t, func(c *errConfig) { c.maxDayByte = 600 })
	postErr(t, b, "key", errSampleBody)
	postErr(t, b, "key", errSampleBody)
	if n := len(readErrRecords(t, b, "2026-11-12")); n != 1 {
		t.Fatalf("%d records, want 1 under a byte cap that fits one", n)
	}
}

func writeDay(t *testing.T, dir, day string, size int) {
	t.Helper()
	if err := os.MkdirAll(dir, 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, day+".jsonl"), []byte(strings.Repeat("x", size)), 0o600); err != nil {
		t.Fatal(err)
	}
}

func wantDays(t *testing.T, dir string, want map[string]bool) {
	t.Helper()
	for day, present := range want {
		_, err := os.Stat(filepath.Join(dir, day+".jsonl"))
		if got := err == nil; got != present {
			t.Errorf("%s present = %v, want %v", day, got, present)
		}
	}
}

func TestErrorsRotationAndAge(t *testing.T) {
	dir := filepath.Join(t.TempDir(), "reports")
	// 4 days of 2 MiB each in an 8 MiB directory (90 % = 7.2 MiB), one past 90 days,
	// and a file that is not a day: neither counted nor deleted.
	writeDay(t, dir, "2026-08-01", 1000)
	writeDay(t, dir, "2026-11-08", 2<<20)
	writeDay(t, dir, "2026-11-09", 2<<20)
	writeDay(t, dir, "2026-11-10", 2<<20)
	writeDay(t, dir, "2026-11-11", 2<<20)
	if err := os.WriteFile(filepath.Join(dir, "notes.txt"), []byte("mine"), 0o600); err != nil {
		t.Fatal(err)
	}
	cfg := errConfig{dir: dir, maxPerDay: 100, maxDayByte: 1 << 20, maxBytes: 8 << 20, maxAge: 90 * 24 * time.Hour}
	s := &errSink{cfg: cfg, apiKey: "key", now: func() time.Time { return errTestNow }}
	s.mu.Lock()
	s.sweepLocked(errTestNow)
	s.mu.Unlock()
	// The day past 90 days goes; then, 8 MiB being over 90 %, the oldest days
	// until 6 MiB (75 %).
	wantDays(t, dir, map[string]bool{
		"2026-08-01": false, "2026-11-08": false, "2026-11-09": true, "2026-11-10": true, "2026-11-11": true,
	})
	if s.bytes != 6<<20 {
		t.Fatalf("counted %d bytes, want the three days left", s.bytes)
	}
	// A report that would take the directory past 90 % sweeps first: under a
	// 6.5 MiB cap (90 % = 5.85 MiB, 75 % = 4.875 MiB) 2026-11-09 goes, and
	// today's file is written.
	s.cfg.maxBytes = 13 << 19
	postErr(t, s, "key", errSampleBody)
	wantDays(t, dir, map[string]bool{
		"2026-11-09": false, "2026-11-10": true, "2026-11-11": true, "2026-11-12": true,
	})
	if _, err := os.Stat(filepath.Join(dir, "notes.txt")); err != nil {
		t.Fatalf("a file that is not a day must stay")
	}
}

func TestErrorsRecountsAfterAPurge(t *testing.T) {
	s := testErrSink(t, nil)
	postErr(t, s, "key", errSampleBody)
	// Bruno deletes the day once read (errors.sh purge); the next day's sweep
	// counts the directory again instead of carrying the old total.
	if err := os.Remove(filepath.Join(s.cfg.dir, "2026-11-12.jsonl")); err != nil {
		t.Fatal(err)
	}
	s.now = func() time.Time { return errTestNow.Add(24 * time.Hour) }
	postErr(t, s, "key", errSampleBody)
	fi, err := os.Stat(filepath.Join(s.cfg.dir, "2026-11-13.jsonl"))
	if err != nil {
		t.Fatal(err)
	}
	if s.bytes != fi.Size() {
		t.Fatalf("counted %d bytes, want %d", s.bytes, fi.Size())
	}
}
