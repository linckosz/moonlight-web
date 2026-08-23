// Identifiers, the ownership claim store, and the per-IP budget.
//
// The claim model is lifted from mw-proxy: Trust On First Use, with only
// HMAC_S(token) on disk so a leak of the file yields no usable credential. It is
// a SEPARATE store with a SEPARATE secret, on purpose — the mw-proxy token
// authorises a DNS zone claim and dies with the sub-domain mechanism in February
// 2027, while this one authorises a rendezvous line. Recycling one token for both
// would merge two unrelated authorisations and outlive the one that should end.
package main

import (
	"crypto/hmac"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/hex"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"sync"
	"time"
)

// ── Identifiers ─────────────────────────────────────────────────────────────

// idLength is 26 Crockford base32 characters: 130 bits of encoding space over
// the 128 bits of randomness §2.3 calls for. Long because the id is a locator
// that anyone may hold — the pairing signature is what grants access — and
// short identifiers are enumerable.
const idLength = 26

// Crockford's alphabet minus its four ambiguous letters (i, l, o, u), so an id
// survives being read aloud or copied off a screen.
var idPattern = regexp.MustCompile(`^[0-9abcdefghjkmnpqrstvwxyz]{26}$`)

// normaliseID folds the forms a human might produce back to canonical form:
// case, grouping hyphens, and the digit/letter pairs Crockford calls out. The
// host and the browser must apply exactly this function, or a claim made in one
// form would never be found again in another.
func normaliseID(raw string) string {
	var b strings.Builder
	b.Grow(len(raw))
	for _, r := range strings.ToLower(strings.TrimSpace(raw)) {
		switch r {
		case '-', ' ':
			continue
		case 'i', 'l':
			b.WriteRune('1')
		case 'o':
			b.WriteRune('0')
		case 'u':
			b.WriteRune('v')
		default:
			b.WriteRune(r)
		}
	}
	return b.String()
}

func validID(id string) bool { return idPattern.MatchString(id) }

// minOwnerToken rejects a token short enough to be guessed. The host generates
// 256 bits; anything under 32 characters is not something this service issued.
const minOwnerToken = 32

// ── Claim store ─────────────────────────────────────────────────────────────

type claim struct {
	Owner   string `json:"owner"`   // hex(HMAC_S(owner token))
	Created int64  `json:"created"` // unix seconds, for operational insight only
}

type claimStore struct {
	mu     sync.Mutex
	path   string
	secret []byte
	claims map[string]claim // id → claim
}

func newClaimStore(path string, secret []byte) (*claimStore, error) {
	s := &claimStore{path: path, secret: secret, claims: map[string]claim{}}
	data, err := os.ReadFile(path)
	if errors.Is(err, os.ErrNotExist) {
		return s, nil
	}
	if err != nil {
		return nil, err
	}
	var on struct {
		Claims map[string]claim `json:"claims"`
	}
	if err := json.Unmarshal(data, &on); err != nil {
		return nil, err
	}
	if on.Claims != nil {
		s.claims = on.Claims
	}
	return s, nil
}

// fingerprint derives the non-reversible owner marker kept on disk.
func (s *claimStore) fingerprint(token string) string {
	mac := hmac.New(sha256.New, s.secret)
	mac.Write([]byte(token))
	return hex.EncodeToString(mac.Sum(nil))
}

// persist writes the store atomically. Caller holds s.mu.
func (s *claimStore) persist() error {
	if err := os.MkdirAll(filepath.Dir(s.path), 0o700); err != nil {
		return err
	}
	body, err := json.MarshalIndent(struct {
		Claims map[string]claim `json:"claims"`
	}{s.claims}, "", "  ")
	if err != nil {
		return err
	}
	tmp := s.path + ".tmp"
	if err := os.WriteFile(tmp, body, 0o600); err != nil {
		return err
	}
	return os.Rename(tmp, s.path)
}

// Outcomes of a claim attempt.
type claimResult int

const (
	claimCreated  claimResult = iota // first ever writer of this id
	claimExisting                    // same owner re-presenting an id it holds
	claimTaken                       // someone else owns this id
	claimHasOther                    // this owner already holds a different id
)

// claim records ownership of id for token, or reports why it cannot.
//
// The whole operation runs under one lock. mw-proxy's equivalent releases the
// lock between validation and commit because it must forward to PowerDNS in
// between, which leaves a window where two owners both pass validation; there is
// no upstream here, so that window simply does not need to exist.
func (s *claimStore) claim(id, token string) claimResult {
	s.mu.Lock()
	defer s.mu.Unlock()

	fp := s.fingerprint(token)
	if existing, ok := s.claims[id]; ok {
		if subtle.ConstantTimeCompare([]byte(existing.Owner), []byte(fp)) == 1 {
			return claimExisting
		}
		return claimTaken
	}
	// One line per owner: without this a single token could reserve identifiers
	// in bulk, and the per-IP budget alone would not stop a patient caller.
	for other, c := range s.claims {
		if other != id && subtle.ConstantTimeCompare([]byte(c.Owner), []byte(fp)) == 1 {
			return claimHasOther
		}
	}
	s.claims[id] = claim{Owner: fp, Created: time.Now().Unix()}
	return claimCreated
}

// owns reports whether token is the owner of id. Used on every host handshake.
func (s *claimStore) owns(id, token string) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	existing, ok := s.claims[id]
	if !ok {
		return false
	}
	return subtle.ConstantTimeCompare([]byte(existing.Owner), []byte(s.fingerprint(token))) == 1
}

// release drops a claim, letting a reinstalled host take a fresh id without an
// operator having to edit the store by hand. Only the owner may do it.
func (s *claimStore) release(id, token string) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	existing, ok := s.claims[id]
	if !ok {
		return false
	}
	if subtle.ConstantTimeCompare([]byte(existing.Owner), []byte(s.fingerprint(token))) != 1 {
		return false
	}
	delete(s.claims, id)
	return true
}

func (s *claimStore) count() int {
	s.mu.Lock()
	defer s.mu.Unlock()
	return len(s.claims)
}

// flush persists the current state; callers use it after a mutating operation.
func (s *claimStore) flush() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.persist()
}

// ── Per-IP budget (sliding window) ──────────────────────────────────────────

// Sweep thresholds, matching mw-proxy: without them the map is append-only, one
// entry per distinct address kept for the life of the process.
const (
	quotaSweepEvery   = 15 * time.Minute
	quotaSweepEntries = 4096
)

type quota struct {
	mu        sync.Mutex
	max       int
	window    time.Duration
	hits      map[string][]time.Time
	lastSweep time.Time
}

func newQuota(max int, window time.Duration) *quota {
	return &quota{max: max, window: window, hits: map[string][]time.Time{}, lastSweep: time.Now()}
}

// sweep drops every address whose attempts have all aged out. Caller holds q.mu.
func (q *quota) sweep(now time.Time) {
	cut := now.Add(-q.window)
	for ip, ts := range q.hits {
		live := ts[:0]
		for _, t := range ts {
			if t.After(cut) {
				live = append(live, t)
			}
		}
		if len(live) == 0 {
			delete(q.hits, ip)
			continue
		}
		q.hits[ip] = live
	}
	q.lastSweep = now
}

// allow records an attempt for ip and reports whether it fits the budget.
// A max of 0 disables the quota.
func (q *quota) allow(ip string) bool {
	if q.max <= 0 {
		return true
	}
	q.mu.Lock()
	defer q.mu.Unlock()
	now := time.Now()
	if now.Sub(q.lastSweep) >= quotaSweepEvery || len(q.hits) >= quotaSweepEntries {
		q.sweep(now)
	}
	cut := now.Add(-q.window)
	kept := q.hits[ip][:0]
	for _, t := range q.hits[ip] {
		if t.After(cut) {
			kept = append(kept, t)
		}
	}
	if len(kept) >= q.max {
		q.hits[ip] = kept
		return false
	}
	q.hits[ip] = append(kept, now)
	return true
}
