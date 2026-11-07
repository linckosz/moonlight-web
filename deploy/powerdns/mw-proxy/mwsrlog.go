// MoonlightWeb — the MWSR page's measure log: what the shader did on real GPUs.
//
// What this answers
// -----------------
// The page at /mwsr/ times the upscalers on the visitor's own GPU when they press
// its button. A shader that a browser refuses, or that runs far slower on some
// GPU than on ours, is otherwise invisible until someone reports it. Each finished
// measure is posted here once, kept in a file to be read later, and counted in an
// Umami website of its own.
//
// What is deliberately NOT collected
// ----------------------------------
// Technical data and measured metrics only, nothing about a person:
//
//   - no address: the handler never reads one, and it does not reach Umami, whose
//     records all carry the same fixed address and User-Agent. The per-subscriber
//     rate limit lives in Caddy's memory, and that site block keeps no access log;
//   - no cookie, identifier, User-Agent string, language, time zone or referrer;
//   - no picture, and no file name: a visitor's own picture is the token "own";
//   - the record's time stops at the hour, so it cannot be matched to a visit.
//
// Every field below is matched against a pattern or range, or dropped. Anything
// unknown in the body is ignored, so nothing can be stored by simply sending it.
//
// How to read it in Umami
// -----------------------
//
//	Overview → Views    measures per day
//	Pages → Path        /m/{webgpu|webgl2|other}/{GPU vendor}
//	Pages → URL         the same, split by OS, browser, picture and scale
//	Events → failed-*   the measures that failed, by step
//
// Visitors means nothing on this website: every record has the same fixed address.
//
// Bounds
// ------
// Caddy allows 3 posts per subscriber per 5 minutes. Past it, a global cap per day
// and one on the directory keep a botnet from filling the disk or the dashboard;
// over either one a post is answered like any other and dropped.
package main

import (
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"sync"
	"time"
)

const (
	mwsrMaxBody    = 32 << 10 // well above a real report of a dozen result lines
	mwsrMaxResults = 16
	mwsrMaxMsg     = 200

	// Every record's address and User-Agent as Umami sees them, so that it cannot
	// tell two visitors apart. Umami needs both: without an address it would use
	// this container's, which is just as fixed but less obviously on purpose.
	//
	// The User-Agent has the shape of the update census's, "Name/version (a; b)",
	// which Umami counts, and no word its bot filter knows: that filter answers
	// 200 and drops the record, so a wrong agent fails without a trace. Umami
	// 3.2.0 dropped both "MoonlightWeb-MWSR-page/1" and "MoonlightWeb/1 (mwsr;
	// measure)" (it answered {"beep":"boop"}), and took this one.
	mwsrUmamiAddr = "127.0.0.1"
	mwsrUmamiUA   = "MoonlightWeb/1 (mwsr; page)"
)

type mwsrConfig struct {
	umamiURL     string // MW_UMAMI_URL                ("" disables the count)
	umamiWebsite string // MW_UMAMI_MWSR_WEBSITE_ID    ("" disables the count)
	hostname     string // the site's own host, which Umami records
	origin       string // "https://{domain}", the only page that may post
	dir          string // MW_MWSR_LOG_DIR             (default /data/mwsr-measures)
	maxPerDay    int    // past it a post is dropped (2000)
	maxBytes     int64  // the directory's cap, past it likewise (2 GiB)
}

// ── What the page may say ───────────────────────────────────────────────────

type mwsrResult struct {
	Kernel    string   `json:"k"`
	Half      bool     `json:"half,omitempty"`
	LPIPS     *float64 `json:"lpips,omitempty"`
	VmafNeg   *float64 `json:"vmafNeg,omitempty"`
	PSNR      *float64 `json:"psnr,omitempty"`
	SSIM      *float64 `json:"ssim,omitempty"`
	MSSSIM    *float64 `json:"msssim,omitempty"`
	FLIP      *float64 `json:"flip,omitempty"`
	Overshoot *float64 `json:"overshoot,omitempty"`
	GpuMs     *float64 `json:"gpuMs,omitempty"`
	GpuSpread *float64 `json:"gpuSpread,omitempty"`
	Dropped   int      `json:"dropped,omitempty"`
}

type mwsrFail struct {
	Step     string `json:"step,omitempty"`
	Shader   string `json:"shader,omitempty"`
	Msg      string `json:"msg,omitempty"`
	Lost     string `json:"lost,omitempty"`
	Fallback bool   `json:"fallback,omitempty"`
}

// mwsrRecord is both the request body and, once cleaned, the line kept on disk.
// At is the only field the page cannot set.
type mwsrRecord struct {
	At           string       `json:"at"`
	V            int          `json:"v"`
	Page         string       `json:"page,omitempty"`
	Shader       string       `json:"shader,omitempty"`
	Browser      string       `json:"browser,omitempty"`
	BrowserMajor int          `json:"browserMajor,omitempty"`
	OS           string       `json:"os,omitempty"`
	OSMajor      int          `json:"osMajor,omitempty"`
	Mobile       bool         `json:"mobile,omitempty"`
	GpuVendor    string       `json:"gpuVendor,omitempty"`
	GpuArch      string       `json:"gpuArch,omitempty"`
	F16          bool         `json:"f16,omitempty"`
	Timestamps   bool         `json:"timestamps,omitempty"`
	Batches      int          `json:"batches,omitempty"` // per text timed: 3 at the first measure after a load, 6 after
	CanvasW      int          `json:"canvasW,omitempty"`
	CanvasH      int          `json:"canvasH,omitempty"`
	InputW       int          `json:"inputW,omitempty"`
	InputH       int          `json:"inputH,omitempty"`
	DPR          *float64     `json:"dpr,omitempty"`
	Gamut        string       `json:"gamut,omitempty"`
	HDR          bool         `json:"hdr,omitempty"`
	Picture      string       `json:"picture,omitempty"`
	Scale        string       `json:"scale,omitempty"`
	Chroma       string       `json:"chroma,omitempty"`
	C444         bool         `json:"c444,omitempty"`
	Path         string       `json:"path,omitempty"`
	Results      []mwsrResult `json:"results,omitempty"`
	Fail         *mwsrFail    `json:"fail,omitempty"`
}

var (
	mwsrIDRe  = regexp.MustCompile(`^[a-z0-9][a-z0-9._+-]{0,39}$`)
	mwsrGpuRe = regexp.MustCompile(`^[a-z0-9][a-z0-9 ._-]{0,39}$`)
	mwsrURLRe = regexp.MustCompile(`[a-zA-Z][a-zA-Z0-9+.-]*://\S*`)

	mwsrKnownBrowser = map[string]bool{
		"chrome": true, "edge": true, "firefox": true, "safari": true,
	}
	mwsrKnownOS = map[string]bool{
		"windows": true, "macos": true, "linux": true, "android": true, "ios": true,
	}
	mwsrKnownGamut  = map[string]bool{"srgb": true, "p3": true, "rec2020": true}
	mwsrKnownPath   = map[string]bool{"webgpu": true, "webgl2": true}
	mwsrKnownVendor = map[string]bool{
		"nvidia": true, "amd": true, "intel": true, "apple": true, "qualcomm": true,
		"arm": true, "imagination": true, "microsoft": true, "google": true,
	}
)

func mwsrID(v string) string {
	v = strings.ToLower(strings.TrimSpace(v))
	if mwsrIDRe.MatchString(v) {
		return v
	}
	return ""
}

func mwsrGpu(v string) string {
	v = strings.ToLower(strings.TrimSpace(v))
	if mwsrGpuRe.MatchString(v) {
		return v
	}
	return ""
}

func mwsrKnown(v string, known map[string]bool) string {
	v = strings.ToLower(strings.TrimSpace(v))
	if known[v] {
		return v
	}
	return ""
}

// mwsrInt drops a value outside the range rather than clamping it: a clamped
// number would read as a measure that nobody made.
func mwsrInt(v, max int) int {
	if v < 0 || v > max {
		return 0
	}
	return v
}

func mwsrFloat(v *float64, min, max float64) *float64 {
	if v == nil || *v < min || *v > max {
		return nil
	}
	return v
}

// mwsrMessage keeps a browser's error text readable and harmless: printable ASCII
// on one line, no address of any kind (a shader compiler quotes where it read from).
func mwsrMessage(s string) string {
	s = mwsrURLRe.ReplaceAllString(s, "<url>")
	var b strings.Builder
	for _, c := range s {
		if b.Len() >= mwsrMaxMsg {
			break
		}
		switch {
		case c >= 0x20 && c < 0x7f:
			b.WriteRune(c)
		case c == '\n' || c == '\r' || c == '\t':
			b.WriteByte(' ')
		}
	}
	return strings.TrimSpace(b.String())
}

// clean rebuilds the record field by field. Nothing is copied wholesale, so a
// field added to the struct without a rule here is simply never kept.
func (r mwsrRecord) clean(now time.Time) mwsrRecord {
	c := mwsrRecord{
		At:           now.UTC().Format("2006-01-02T15Z"),
		V:            mwsrInt(r.V, 1000),
		Page:         mwsrID(r.Page),
		Shader:       mwsrID(r.Shader),
		Browser:      mwsrKnown(r.Browser, mwsrKnownBrowser),
		BrowserMajor: mwsrInt(r.BrowserMajor, 1000),
		OS:           mwsrKnown(r.OS, mwsrKnownOS),
		OSMajor:      mwsrInt(r.OSMajor, 1000),
		Mobile:       r.Mobile,
		GpuVendor:    mwsrGpu(r.GpuVendor),
		GpuArch:      mwsrGpu(r.GpuArch),
		F16:          r.F16,
		Timestamps:   r.Timestamps,
		Batches:      mwsrInt(r.Batches, 1000),
		CanvasW:      mwsrInt(r.CanvasW, 32768),
		CanvasH:      mwsrInt(r.CanvasH, 32768),
		InputW:       mwsrInt(r.InputW, 32768),
		InputH:       mwsrInt(r.InputH, 32768),
		DPR:          mwsrFloat(r.DPR, 0, 16),
		Gamut:        mwsrKnown(r.Gamut, mwsrKnownGamut),
		HDR:          r.HDR,
		Picture:      mwsrID(r.Picture),
		Scale:        mwsrID(r.Scale),
		Chroma:       mwsrID(r.Chroma),
		C444:         r.C444,
		Path:         mwsrKnown(r.Path, mwsrKnownPath),
	}
	for i, x := range r.Results {
		if i == mwsrMaxResults {
			break
		}
		k := mwsrID(x.Kernel)
		if k == "" {
			continue
		}
		c.Results = append(c.Results, mwsrResult{
			Kernel:    k,
			Half:      x.Half,
			LPIPS:     mwsrFloat(x.LPIPS, 0, 10),
			VmafNeg:   mwsrFloat(x.VmafNeg, -100, 200),
			PSNR:      mwsrFloat(x.PSNR, 0, 1000),
			SSIM:      mwsrFloat(x.SSIM, -1, 1),
			MSSSIM:    mwsrFloat(x.MSSSIM, -1, 1),
			FLIP:      mwsrFloat(x.FLIP, 0, 10),
			Overshoot: mwsrFloat(x.Overshoot, 0, 1000),
			GpuMs:     mwsrFloat(x.GpuMs, 0, 100000),
			GpuSpread: mwsrFloat(x.GpuSpread, 0, 100000),
			Dropped:   mwsrInt(x.Dropped, 100000),
		})
	}
	if r.Fail != nil {
		c.Fail = &mwsrFail{
			Step:     mwsrID(r.Fail.Step),
			Shader:   mwsrID(r.Fail.Shader),
			Msg:      mwsrMessage(r.Fail.Msg),
			Lost:     mwsrID(r.Fail.Lost),
			Fallback: r.Fail.Fallback,
		}
	}
	return c
}

// orOther is the dashboard's token for a field that is missing or unknown.
func orOther(v string) string {
	if v == "" {
		return "other"
	}
	return v
}

// umamiURL is the address the measure is counted under. Only values that passed
// an exact list or the pattern above get here, and they are escaped regardless.
func (r mwsrRecord) umamiURL() string {
	q := url.Values{}
	q.Set("os", orOther(r.OS))
	q.Set("browser", orOther(r.Browser))
	q.Set("picture", orOther(r.Picture))
	q.Set("scale", orOther(r.Scale))
	return "/m/" + orOther(r.Path) + "/" + orOther(mwsrKnown(r.GpuVendor, mwsrKnownVendor)) +
		"?" + q.Encode()
}

// ── The sink ────────────────────────────────────────────────────────────────

type mwsrSink struct {
	cfg mwsrConfig
	rep *umamiReporter
	now func() time.Time

	mu       sync.Mutex
	day      string
	dayCount int
	bytes    int64
}

func newMwsrSink(cfg mwsrConfig) *mwsrSink {
	s := &mwsrSink{
		cfg: cfg,
		rep: newUmamiReporter(umamiConfig{
			url:      cfg.umamiURL,
			website:  cfg.umamiWebsite,
			hostname: cfg.hostname,
		}, "mwsr", nil),
		now: time.Now,
	}
	// The directory's size survives a restart; the day's count does not need to.
	if entries, err := os.ReadDir(cfg.dir); err == nil {
		for _, e := range entries {
			if info, err := e.Info(); err == nil {
				s.bytes += info.Size()
			}
		}
	}
	return s
}

// store appends one line to this month's file, and says whether it was kept.
func (s *mwsrSink) store(rec mwsrRecord, now time.Time) bool {
	line, err := json.Marshal(rec)
	if err != nil {
		return false
	}
	line = append(line, '\n')

	s.mu.Lock()
	defer s.mu.Unlock()
	day := now.UTC().Format("2006-01-02")
	if day != s.day {
		s.day, s.dayCount = day, 0
	}
	if s.dayCount >= s.cfg.maxPerDay || s.bytes+int64(len(line)) > s.cfg.maxBytes {
		return false
	}
	if err := os.MkdirAll(s.cfg.dir, 0o700); err != nil {
		log.Printf("[mw-proxy] mwsr: cannot create the log directory: %v", err)
		return false
	}
	name := filepath.Join(s.cfg.dir, now.UTC().Format("2006-01")+".jsonl")
	f, err := os.OpenFile(name, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o600)
	if err != nil {
		log.Printf("[mw-proxy] mwsr: cannot open the log: %v", err)
		return false
	}
	defer f.Close()
	if _, err := f.Write(line); err != nil {
		log.Printf("[mw-proxy] mwsr: cannot write the log: %v", err)
		return false
	}
	s.dayCount++
	s.bytes += int64(len(line))
	return true
}

// ServeHTTP answers POST /v1/mwsr. A dropped report and a kept one look the same
// from outside: the page has nothing to do about either, and a client probing the
// caps learns nothing. The request's address and headers other than Origin and
// Sec-Fetch-Site are never read, and the handler logs nothing a request sent.
func (s *mwsrSink) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeErr(w, http.StatusMethodNotAllowed, "only POST is allowed")
		return
	}
	if !s.fromThePage(r) {
		writeErr(w, http.StatusForbidden, "wrong origin")
		return
	}
	body, err := io.ReadAll(io.LimitReader(r.Body, mwsrMaxBody+1))
	if err != nil || len(body) > mwsrMaxBody {
		writeErr(w, http.StatusRequestEntityTooLarge, "body too large")
		return
	}
	var rep mwsrRecord
	if err := json.Unmarshal(body, &rep); err != nil {
		writeErr(w, http.StatusBadRequest, "malformed body")
		return
	}

	now := s.now()
	rec := rep.clean(now)
	if s.store(rec, now) {
		u := rec.umamiURL()
		data := map[string]string{
			"path": orOther(rec.Path), "os": orOther(rec.OS), "browser": orOther(rec.Browser),
			"picture": orOther(rec.Picture), "scale": orOther(rec.Scale),
		}
		s.rep.send("", u, mwsrUmamiUA, mwsrUmamiAddr, data)
		if rec.Fail != nil {
			s.rep.send("failed-"+orOther(rec.Fail.Step), u, mwsrUmamiUA, mwsrUmamiAddr, data)
		}
	}
	w.WriteHeader(http.StatusNoContent)
}

// fromThePage says whether a post comes from the site's own page. Not a secret,
// as the page is public: this keeps other sites' pages out, and a JSON content
// type makes a browser ask first, which is never answered.
//
// Safari on iOS sends "Origin: null" from the page itself (seen with its web
// inspector, iOS 26.5): that alone would refuse every iPhone. A null origin is
// then taken only with Sec-Fetch-Site: same-origin, which the browser sets and a
// page cannot. Another site's page, or a sandboxed frame, says cross-site and
// stays out. A client outside a browser could always send any header it likes:
// this was never a guard against it.
func (s *mwsrSink) fromThePage(r *http.Request) bool {
	switch r.Header.Get("Origin") {
	case s.cfg.origin:
		return true
	case "null":
		return r.Header.Get("Sec-Fetch-Site") == "same-origin"
	}
	return false
}

func (s *mwsrSink) describe() string {
	return fmt.Sprintf("kept in %s, at most %d a day, counted in website %s",
		s.cfg.dir, s.cfg.maxPerDay, describeWebsite(s.cfg.umamiWebsite))
}
