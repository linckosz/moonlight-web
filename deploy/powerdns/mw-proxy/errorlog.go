// MoonlightWeb — error reports: what went wrong on real machines, kept to be read.
//
// What this answers
// -----------------
// A stream that fails on someone else's machine is invisible until they write in,
// and then the logs that would explain it are a week gone. An instance that saw
// an error during a stream sends one short report when the stream ends or fails:
// its own error lines, and those of the browser pages that took part. They are
// kept here in a file per day, to be fetched over SSH and deleted once read
// (deploy/powerdns/errors.sh).
//
// What is deliberately NOT collected
// ----------------------------------
// The instance has already put every line through the "Download logs" scrubber
// (backend/src/server/LogScrubber.h): keys, links and what a person typed are
// hidden, machine and user names and addresses replaced, and the hosts' app
// titles taken out. This handler is the second net, not the first:
//
//   - no address: the handler never reads one, and the Caddy block keeps no
//     access log. The per-subscriber rate limit lives in Caddy's memory;
//   - no User-Agent, cookie, identifier, language or time zone; a page's browser
//     is a family and a major version, matched against a list;
//   - no absolute time on a line, only how long before the report it was
//     written, and the record's own time stops at the hour;
//   - every line goes through errLine(): what still looks like a link, an
//     e-mail, an IP address or a long token is replaced before it is written.
//
// Every field below is matched against a list or a range, or dropped. Anything
// unknown in the body is ignored, so nothing can be stored by simply sending it.
//
// Bounds
// ------
// Caddy allows a few reports per subscriber per hour. Past it, a cap per day (in
// reports and in bytes) and one on the directory keep a botnet from filling the
// disk: over the day's cap a report is answered like any other and dropped; near
// the directory's cap the oldest days are deleted to make room (rotation), and
// no day is kept more than 90 days.
package main

import (
	"crypto/subtle"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
	"unicode"
)

const (
	errMaxBody      = 64 << 10 // the instance caps its report at 48 KiB of lines
	errMaxHostLines = 300
	errMaxPages     = 8
	errMaxPageLines = 60
	errMaxLine      = 1024
	errMaxAgo       = 7 * 24 * 3600
)

type errConfig struct {
	dir        string        // MW_ERROR_LOG_DIR (default /data/error-reports)
	maxPerDay  int           // reports a day; past it a report is dropped (3000)
	maxDayByte int64         // bytes a day; likewise (150 MiB)
	maxBytes   int64         // the directory's cap (1 GiB)
	maxAge     time.Duration // a day's file older than this is deleted (90 days)
}

// rotateAt and rotateTo: from 90 % of the cap, the oldest days go until 75 %.
func (c errConfig) rotateAt() int64 { return c.maxBytes / 10 * 9 }
func (c errConfig) rotateTo() int64 { return c.maxBytes / 4 * 3 }

// ── What an instance may say ────────────────────────────────────────────────

type errLineIn struct {
	Ago   int    `json:"ago"`         // seconds before the report was built
	Level string `json:"l"`           // error | warn
	Count int    `json:"n,omitempty"` // the same line, this many times
	Text  string `json:"m"`
}

type errPageIn struct {
	Who     string      `json:"who"`     // owner | guest | host
	Browser string      `json:"browser"` // a family
	Major   int         `json:"major"`
	Dropped int         `json:"dropped,omitempty"`
	Lines   []errLineIn `json:"lines"`
}

// errReport is the request body; errRecord, once cleaned, is the line kept.
type errReport struct {
	Version   string      `json:"v"`
	OS        string      `json:"os"`
	Arch      string      `json:"arch"`
	Event     string      `json:"event"`   // end | failed
	Code      int         `json:"code"`    // failed: the HTTP status the launch got
	Seconds   int         `json:"seconds"` // end: how long the stream lasted
	Height    int         `json:"h"`
	Fps       int         `json:"fps"`
	Codec     string      `json:"codec"`
	Hdr       bool        `json:"hdr"`
	Yuv444    bool        `json:"yuv444"`
	Backend   string      `json:"backend"`
	Transport string      `json:"transport"`
	Net       string      `json:"net"`
	Client    string      `json:"client"`
	Kind      string      `json:"kind"`
	Dropped   int         `json:"dropped"` // host lines the instance let go
	Host      []errLineIn `json:"host"`
	Pages     []errPageIn `json:"pages"`
}

type errLine struct {
	Ago   int    `json:"ago"`
	Level string `json:"l"`
	Count int    `json:"n,omitempty"`
	Text  string `json:"m"`
}

type errPage struct {
	Who     string    `json:"who"`
	Browser string    `json:"browser"`
	Major   int       `json:"major,omitempty"`
	Dropped int       `json:"dropped,omitempty"`
	Lines   []errLine `json:"lines"`
}

type errRecord struct {
	At        string    `json:"at"`
	Version   string    `json:"v"`
	OS        string    `json:"os"`
	Arch      string    `json:"arch"`
	Event     string    `json:"event"`
	Code      string    `json:"code,omitempty"`
	Duration  string    `json:"duration,omitempty"`
	Height    string    `json:"h"`
	Fps       string    `json:"fps"`
	Codec     string    `json:"codec"`
	Hdr       bool      `json:"hdr,omitempty"`
	Yuv444    bool      `json:"yuv444,omitempty"`
	Backend   string    `json:"backend"`
	Transport string    `json:"transport"`
	Net       string    `json:"net"`
	Client    string    `json:"client"`
	Kind      string    `json:"kind"`
	Dropped   int       `json:"dropped,omitempty"`
	Host      []errLine `json:"host,omitempty"`
	Pages     []errPage `json:"pages,omitempty"`
}

var (
	errKnownEvent   = map[string]bool{"end": true, "failed": true}
	errKnownLevel   = map[string]bool{"error": true, "warn": true}
	errKnownWho     = map[string]bool{"owner": true, "guest": true, "host": true}
	errKnownBrowser = map[string]bool{
		"chrome": true, "edge": true, "firefox": true, "safari": true, "opera": true,
		"samsung": true, "webview": true, "other": true,
	}

	// The second net. The instance's scrubber writes its stand-ins with braces
	// and x's (192.168.x.{a1}, 256.x.x.{1}, fd::{d1}), which none of these match.
	errURLRe  = regexp.MustCompile(`[a-zA-Z][a-zA-Z0-9+.-]*://\S*`)
	errMailRe = regexp.MustCompile(`[A-Za-z0-9._%+-]+@[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)*\.[A-Za-z]{2,}`)
	errIPv4Re = regexp.MustCompile(`\b(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})\b`)
	// The forms with "::" come first: Go keeps the first alternative that
	// matches, and the full form stops right before a "::", leaving the rest.
	errIPv6Re = regexp.MustCompile(`\b[0-9a-fA-F]{1,4}(?::[0-9a-fA-F]{1,4})*::[0-9a-fA-F]{1,4}(?::[0-9a-fA-F]{1,4})*\b` +
		`|\b[0-9a-fA-F]{1,4}(?::[0-9a-fA-F]{1,4}){1,6}::` +
		`|\b[0-9a-fA-F]{1,4}(?::[0-9a-fA-F]{1,4}){3,7}\b`)
	errTokenRe = regexp.MustCompile(`[A-Za-z0-9_+=-]{32,}`)
)

// errIPv4 keeps what is the same on every machine (loopback, 0.0.0.0) and what
// is not an address at all (an octet over 255: a version number).
func errIPv4(m string) string {
	p := errIPv4Re.FindStringSubmatch(m)
	for _, o := range p[1:] {
		if n, _ := strconv.Atoi(o); n > 255 {
			return m
		}
	}
	if p[1] == "127" || m == "0.0.0.0" {
		return m
	}
	return "(address)"
}

// errToken hides a long run that carries digits: a key, a token, a hash. A long
// word without a digit is a name in the code (a function, a class) and stays.
func errToken(m string) string {
	if strings.IndexFunc(m, unicode.IsDigit) < 0 {
		return m
	}
	return "(hidden)"
}

// errText keeps a line readable and harmless: printable characters on one line
// (no control or bidi formatting), at most errMaxLine bytes, and nothing that
// still looks like a link, an e-mail, an address or a token.
func errText(s string) string {
	var b strings.Builder
	for _, c := range s {
		if b.Len() >= 4*errMaxLine {
			break
		}
		switch {
		case c == '\n' || c == '\r' || c == '\t':
			b.WriteByte(' ')
		case unicode.IsPrint(c) && !unicode.Is(unicode.Cf, c):
			b.WriteRune(c)
		}
	}
	s = b.String()
	s = errURLRe.ReplaceAllString(s, "<url>")
	s = errMailRe.ReplaceAllString(s, "(hidden)")
	s = errIPv4Re.ReplaceAllStringFunc(s, errIPv4)
	s = errIPv6Re.ReplaceAllStringFunc(s, func(m string) string {
		if m == "::1" || m == "0:0:0:0:0:0:0:1" {
			return m
		}
		return "(address)"
	})
	s = errTokenRe.ReplaceAllStringFunc(s, errToken)
	s = strings.TrimSpace(s)
	if len(s) > errMaxLine {
		cut := errMaxLine
		for cut > 0 && !utf8Start(s[cut]) {
			cut--
		}
		s = s[:cut] + " […]"
	}
	return s
}

func utf8Start(b byte) bool { return b&0xC0 != 0x80 }

func errLines(in []errLineIn, max int) []errLine {
	var out []errLine
	for _, l := range in {
		if len(out) == max {
			break
		}
		text := errText(l.Text)
		if text == "" {
			continue
		}
		level := strings.ToLower(l.Level)
		if !errKnownLevel[level] {
			level = "error"
		}
		out = append(out, errLine{
			Ago:   mwsrInt(l.Ago, errMaxAgo),
			Level: level,
			Count: mwsrInt(l.Count, 100000),
			Text:  text,
		})
	}
	return out
}

// clean rebuilds the report field by field. Nothing is copied wholesale, so a
// field added to the request without a rule here is never kept.
func (r errReport) clean(now time.Time) errRecord {
	c := errRecord{
		At:        now.UTC().Format("2006-01-02T15Z"),
		Version:   cleanVersion(r.Version),
		OS:        cleanToken(r.OS, knownOS),
		Arch:      cleanToken(r.Arch, knownArch),
		Event:     cleanToken(r.Event, errKnownEvent),
		Height:    cleanInt(r.Height, knownHeight),
		Fps:       cleanInt(r.Fps, knownFps),
		Codec:     cleanToken(r.Codec, knownCodec),
		Hdr:       r.Hdr,
		Yuv444:    r.Yuv444,
		Backend:   cleanToken(r.Backend, knownBackend),
		Transport: cleanToken(r.Transport, knownTransport),
		Net:       cleanToken(r.Net, knownNet),
		Client:    cleanToken(r.Client, knownClient),
		Kind:      cleanToken(r.Kind, knownKind),
		Dropped:   mwsrInt(r.Dropped, 1000000),
		Host:      errLines(r.Host, errMaxHostLines),
	}
	switch c.Event {
	case "failed":
		c.Code = statusBucket(r.Code)
	case "end":
		c.Duration = durationBucket(r.Seconds)
	}
	for _, p := range r.Pages {
		if len(c.Pages) == errMaxPages {
			break
		}
		lines := errLines(p.Lines, errMaxPageLines)
		if len(lines) == 0 {
			continue
		}
		c.Pages = append(c.Pages, errPage{
			Who:     cleanToken(p.Who, errKnownWho),
			Browser: cleanToken(p.Browser, errKnownBrowser),
			Major:   mwsrInt(p.Major, 1000),
			Dropped: mwsrInt(p.Dropped, 1000000),
			Lines:   lines,
		})
	}
	return c
}

// ── The sink ────────────────────────────────────────────────────────────────

type errSink struct {
	cfg    errConfig
	apiKey string
	now    func() time.Time

	mu        sync.Mutex
	day       string
	dayCount  int
	dayBytes  int64
	bytes     int64 // the directory's, as last counted plus what was written since
	lastSweep string
}

func newErrSink(cfg errConfig, apiKey string) *errSink {
	s := &errSink{cfg: cfg, apiKey: apiKey, now: time.Now}
	s.mu.Lock()
	s.sweepLocked(s.now())
	s.mu.Unlock()
	return s
}

// dayFiles lists the directory's day files, oldest first. Anything else in it
// (a file someone left there) is neither counted nor deleted.
func (s *errSink) dayFiles() []os.FileInfo {
	entries, err := os.ReadDir(s.cfg.dir)
	if err != nil {
		return nil
	}
	var files []os.FileInfo
	for _, e := range entries {
		name := e.Name()
		if !strings.HasSuffix(name, ".jsonl") {
			continue
		}
		if _, err := time.Parse("2006-01-02", strings.TrimSuffix(name, ".jsonl")); err != nil {
			continue
		}
		if info, err := e.Info(); err == nil && info.Mode().IsRegular() {
			files = append(files, info)
		}
	}
	sort.Slice(files, func(i, j int) bool { return files[i].Name() < files[j].Name() })
	return files
}

// sweepLocked counts the directory again (files deleted by hand once read are
// no longer counted), deletes the days past maxAge, then the oldest days while
// the directory is over rotateAt, down to rotateTo. Today's file always stays.
// Caller holds s.mu.
func (s *errSink) sweepLocked(now time.Time) {
	today := now.UTC().Format("2006-01-02") + ".jsonl"
	oldest := now.UTC().Add(-s.cfg.maxAge).Format("2006-01-02") + ".jsonl"
	files := s.dayFiles()
	var total int64
	var kept []os.FileInfo
	for _, f := range files {
		if f.Name() < oldest && f.Name() != today {
			s.remove(f.Name(), "older than the retention")
			continue
		}
		kept = append(kept, f)
		total += f.Size()
	}
	if total > s.cfg.rotateAt() {
		for _, f := range kept {
			if total <= s.cfg.rotateTo() || f.Name() == today {
				break
			}
			if s.remove(f.Name(), "the directory is near its cap") {
				total -= f.Size()
			}
		}
	}
	s.bytes = total
	s.lastSweep = now.UTC().Format("2006-01-02")
}

func (s *errSink) remove(name, why string) bool {
	if err := os.Remove(filepath.Join(s.cfg.dir, name)); err != nil {
		log.Printf("[mw-proxy] errors: cannot delete %s: %v", name, err)
		return false
	}
	log.Printf("[mw-proxy] errors: deleted %s (%s)", name, why)
	return true
}

// store appends one line to today's file, and says whether it was kept.
func (s *errSink) store(rec errRecord, now time.Time) bool {
	line, err := json.Marshal(rec)
	if err != nil {
		return false
	}
	line = append(line, '\n')
	size := int64(len(line))

	s.mu.Lock()
	defer s.mu.Unlock()
	day := now.UTC().Format("2006-01-02")
	if day != s.day {
		s.day, s.dayCount, s.dayBytes = day, 0, 0
		if fi, err := os.Stat(filepath.Join(s.cfg.dir, day+".jsonl")); err == nil {
			s.dayBytes = fi.Size() // a restart in the middle of the day
		}
	}
	if s.dayCount >= s.cfg.maxPerDay || s.dayBytes+size > s.cfg.maxDayByte {
		return false
	}
	if day != s.lastSweep || s.bytes+size > s.cfg.rotateAt() {
		s.sweepLocked(now)
		if s.bytes+size > s.cfg.maxBytes {
			return false // today alone fills it: nothing older is left to delete
		}
	}
	if err := os.MkdirAll(s.cfg.dir, 0o700); err != nil {
		log.Printf("[mw-proxy] errors: cannot create the directory: %v", err)
		return false
	}
	f, err := os.OpenFile(filepath.Join(s.cfg.dir, day+".jsonl"),
		os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o600)
	if err != nil {
		log.Printf("[mw-proxy] errors: cannot open today's file: %v", err)
		return false
	}
	defer f.Close()
	if _, err := f.Write(line); err != nil {
		log.Printf("[mw-proxy] errors: cannot write today's file: %v", err)
		return false
	}
	s.dayCount++
	s.dayBytes += size
	s.bytes += size
	return true
}

// ServeHTTP answers POST /v1/errors with 204 whether the report was kept or
// dropped: the instance has nothing to do about either, and a client probing the
// caps learns nothing. The request's address is never read, and the handler logs
// nothing a request sent.
func (s *errSink) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeErr(w, http.StatusMethodNotAllowed, "only POST is allowed")
		return
	}
	// The restricted key of official builds, as on /v1/session: it keeps stray
	// scanners out, it is not a secret (it ships in every installer).
	if subtle.ConstantTimeCompare([]byte(r.Header.Get("X-API-Key")), []byte(s.apiKey)) != 1 {
		writeErr(w, http.StatusUnauthorized, "invalid API key")
		return
	}
	body, err := io.ReadAll(io.LimitReader(r.Body, errMaxBody+1))
	if err != nil || len(body) > errMaxBody {
		writeErr(w, http.StatusRequestEntityTooLarge, "body too large")
		return
	}
	var rep errReport
	if err := json.Unmarshal(body, &rep); err != nil {
		writeErr(w, http.StatusBadRequest, "malformed body")
		return
	}
	now := s.now()
	rec := rep.clean(now)
	if len(rec.Host) > 0 || len(rec.Pages) > 0 {
		s.store(rec, now)
	}
	w.WriteHeader(http.StatusNoContent)
}

func (s *errSink) describe() string {
	return fmt.Sprintf("kept in %s, at most %d reports and %d MiB a day, %d MiB in all, %d days",
		s.cfg.dir, s.cfg.maxPerDay, s.cfg.maxDayByte>>20, s.cfg.maxBytes>>20,
		int(s.cfg.maxAge/(24*time.Hour)))
}
