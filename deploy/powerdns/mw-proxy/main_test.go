package main

import (
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

const testZone = "moonlightweb.top."

func testProxy(t *testing.T, maxNew int) *proxy {
	t.Helper()
	store, err := newOwnerStore(t.TempDir()+"/owners.json", []byte("test-secret"))
	if err != nil {
		t.Fatalf("store: %v", err)
	}
	cfg := config{zone: testZone, maxNewPerHr: maxNew, maxBodyBytes: 64 * 1024}
	p := newProxy(cfg, store)
	return p
}

func TestClassify(t *testing.T) {
	m := buildMatchers(testZone)
	cases := []struct {
		name string
		uid  string
		kind recordKind
		ok   bool
	}{
		{"ab12cd34.moonlightweb.top.", "ab12cd34", kindA, true},
		{"_owner.ab12cd34.moonlightweb.top.", "ab12cd34", kindOwner, true},
		{"_acme-challenge.ab12cd34.moonlightweb.top.", "ab12cd34", kindAcme, true},
		{"AB12CD34.moonlightweb.top.", "ab12cd34", kindA, true}, // case-insensitive
		{"www.moonlightweb.top.", "", 0, false},                 // reserved label
		{"api.moonlightweb.top.", "", 0, false},                 // reserved label
		{"ab12cd3.moonlightweb.top.", "", 0, false},             // 7 hex, too short
		{"ab12cd345.moonlightweb.top.", "", 0, false},           // 9 hex, too long
		{"zz12cd34.moonlightweb.top.", "", 0, false},            // non-hex
		{"ab12cd34.evil.top.", "", 0, false},                    // wrong zone
		{"ns.ab12cd34.moonlightweb.top.", "", 0, false},         // extra label
	}
	for _, c := range cases {
		uid, kind, ok := m.classify(c.name)
		if ok != c.ok || (ok && (uid != c.uid || kind != c.kind)) {
			t.Errorf("classify(%q) = (%q,%v,%v), want (%q,%v,%v)",
				c.name, uid, kind, ok, c.uid, c.kind, c.ok)
		}
	}
}

func TestTypeAllowed(t *testing.T) {
	if !typeAllowed(kindA, "A") || typeAllowed(kindA, "TXT") {
		t.Error("A record must only allow type A")
	}
	if !typeAllowed(kindOwner, "TXT") || typeAllowed(kindOwner, "A") {
		t.Error("_owner must only allow type TXT")
	}
	if typeAllowed(kindA, "NS") || typeAllowed(kindA, "SOA") {
		t.Error("NS/SOA must never be allowed")
	}
}

func doPatch(p *proxy, owner, ip, body string) (int, string) {
	r := httptest.NewRequest("PATCH", "/api/v1/servers/localhost/zones/"+testZone, strings.NewReader(body))
	if owner != "" {
		r.Header.Set("X-MW-Owner", owner)
	}
	r.Header.Set("X-Forwarded-For", ip)
	commit, code, msg := p.checkPatch(r, []byte(body))
	if code == 0 {
		commit() // simulate a successful upstream forward
	}
	return code, msg
}

func aReplace(uid string) string {
	return `{"rrsets":[{"name":"` + uid + `.` + testZone + `","type":"A","changetype":"REPLACE"}]}`
}
func aDelete(uid string) string {
	return `{"rrsets":[{"name":"` + uid + `.` + testZone + `","type":"A","changetype":"DELETE"}]}`
}

func TestOwnershipTOFU(t *testing.T) {
	p := testProxy(t, 0)

	// First writer with a token claims the subdomain.
	if code, msg := doPatch(p, "tokenA", "1.1.1.1", aReplace("ab12cd34")); code != 0 {
		t.Fatalf("first claim rejected: %d %s", code, msg)
	}
	// Same owner can update it.
	if code, _ := doPatch(p, "tokenA", "1.1.1.1", aReplace("ab12cd34")); code != 0 {
		t.Fatalf("owner update rejected: %d", code)
	}
	// A different token cannot overwrite it (hijack attempt).
	if code, _ := doPatch(p, "tokenB", "2.2.2.2", aReplace("ab12cd34")); code != 403 {
		t.Fatalf("hijack should be 403, got %d", code)
	}
	// No token at all is rejected too.
	if code, _ := doPatch(p, "", "2.2.2.2", aReplace("ab12cd34")); code != 403 {
		t.Fatalf("missing token should be 403, got %d", code)
	}
}

func TestClaimRequiresToken(t *testing.T) {
	p := testProxy(t, 0)
	if code, _ := doPatch(p, "", "1.1.1.1", aReplace("ab12cd34")); code != 403 {
		t.Fatalf("claiming without a token should be 403, got %d", code)
	}
}

func TestOnePerOwner(t *testing.T) {
	p := testProxy(t, 0)
	if code, _ := doPatch(p, "tokenA", "1.1.1.1", aReplace("ab12cd34")); code != 0 {
		t.Fatalf("first claim rejected: %d", code)
	}
	// Same owner claiming a second, different uid is refused.
	if code, _ := doPatch(p, "tokenA", "1.1.1.1", aReplace("ffffffff")); code != 409 {
		t.Fatalf("second subdomain should be 409, got %d", code)
	}
	// After releasing the first (A delete), the owner may claim a new one.
	if code, _ := doPatch(p, "tokenA", "1.1.1.1", aDelete("ab12cd34")); code != 0 {
		t.Fatalf("release rejected: %d", code)
	}
	if code, _ := doPatch(p, "tokenA", "1.1.1.1", aReplace("ffffffff")); code != 0 {
		t.Fatalf("claim after release rejected: %d", code)
	}
}

func TestQuota(t *testing.T) {
	p := testProxy(t, 2) // 2 new claims per IP per hour
	ip := "9.9.9.9"
	if code, _ := doPatch(p, "t1", ip, aReplace("aaaaaaaa")); code != 0 {
		t.Fatalf("claim 1 rejected: %d", code)
	}
	if code, _ := doPatch(p, "t2", ip, aReplace("bbbbbbbb")); code != 0 {
		t.Fatalf("claim 2 rejected: %d", code)
	}
	if code, _ := doPatch(p, "t3", ip, aReplace("cccccccc")); code != 429 {
		t.Fatalf("claim 3 should be 429, got %d", code)
	}
	// A different IP still has budget.
	if code, _ := doPatch(p, "t4", "8.8.8.8", aReplace("dddddddd")); code != 0 {
		t.Fatalf("other IP claim rejected: %d", code)
	}
}

func TestRejectForbiddenTypes(t *testing.T) {
	p := testProxy(t, 0)
	// An NS rewrite disguised on a valid-looking name must be refused by type.
	body := `{"rrsets":[{"name":"ab12cd34.` + testZone + `","type":"NS","changetype":"REPLACE"}]}`
	if code, _ := doPatch(p, "tokenA", "1.1.1.1", body); code != 403 {
		t.Fatalf("NS rewrite should be 403, got %d", code)
	}
	// A TXT on the bare uid (should be A only) is refused.
	body = `{"rrsets":[{"name":"ab12cd34.` + testZone + `","type":"TXT","changetype":"REPLACE"}]}`
	if code, _ := doPatch(p, "tokenA", "1.1.1.1", body); code != 403 {
		t.Fatalf("TXT on A-name should be 403, got %d", code)
	}
}

// testProxyUpstream builds a proxy wired to a stub PowerDNS that always answers
// with the given status, so the full ServeHTTP path (including the forward) can
// be exercised.
func testProxyUpstream(t *testing.T, status int) (*proxy, *httptest.Server) {
	t.Helper()
	up := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(status)
	}))
	t.Cleanup(up.Close)
	store, err := newOwnerStore(t.TempDir()+"/owners.json", []byte("test-secret"))
	if err != nil {
		t.Fatalf("store: %v", err)
	}
	cfg := config{
		zone:         testZone,
		upstream:     up.URL,
		realKey:      "real-pdns-key",
		proxyKey:     "restricted-key",
		maxBodyBytes: 64 * 1024,
	}
	return newProxy(cfg, store), up
}

func serveClaim(p *proxy, owner, uid string) *httptest.ResponseRecorder {
	body := aReplace(uid)
	r := httptest.NewRequest("PATCH", "/api/v1/servers/localhost/zones/"+testZone, strings.NewReader(body))
	r.Header.Set("X-API-Key", "restricted-key")
	r.Header.Set("X-MW-Owner", owner)
	r.Header.Set("X-Forwarded-For", "1.1.1.1")
	w := httptest.NewRecorder()
	p.ServeHTTP(w, r)
	return w
}

func TestOwnershipRecordedOnlyWhenPowerDNSAccepts(t *testing.T) {
	// PowerDNS refuses the write: nothing may be claimed. Claiming anyway would
	// strand the uid — its owner would hold a subdomain that has no record, and
	// the one-uid-per-owner rule would then refuse them every other one, with
	// nothing to delete to get out of it.
	p, _ := testProxyUpstream(t, http.StatusUnprocessableEntity)
	if w := serveClaim(p, "tokenA", "ab12cd34"); w.Code != http.StatusUnprocessableEntity {
		t.Fatalf("upstream status should be relayed, got %d", w.Code)
	}
	if len(p.store.owners) != 0 {
		t.Fatalf("a refused write must claim nothing, store = %v", p.store.owners)
	}

	// An accepted write does record it.
	p2, _ := testProxyUpstream(t, http.StatusNoContent)
	if w := serveClaim(p2, "tokenA", "ab12cd34"); w.Code != http.StatusNoContent {
		t.Fatalf("accepted write should be 204, got %d", w.Code)
	}
	if p2.store.owners["ab12cd34"] != p2.store.hash("tokenA") {
		t.Fatalf("accepted write must record ownership, store = %v", p2.store.owners)
	}
}

func TestUnreachableUpstreamClaimsNothing(t *testing.T) {
	p, up := testProxyUpstream(t, http.StatusNoContent)
	up.Close() // the request never reaches PowerDNS

	if w := serveClaim(p, "tokenA", "ab12cd34"); w.Code != http.StatusBadGateway {
		t.Fatalf("unreachable upstream should be 502, got %d", w.Code)
	}
	if len(p.store.owners) != 0 {
		t.Fatalf("nothing may be claimed, store = %v", p.store.owners)
	}
}

// ── Sub-domain retirement ───────────────────────────────────────────────────
//
// The promise being tested is not "new registrations are refused" — it is the
// pair. Refusing new names is worthless if it also breaks the names already out
// there, because those have to keep working until February 2027 on installs
// nobody can update remotely.

// A PowerDNS stand-in that knows which uids exist, so the existence check has
// something truthful to ask. It answers PATCHes with 204.
func testProxyZone(t *testing.T, closed bool, existing ...string) *proxy {
	t.Helper()
	has := map[string]bool{}
	for _, uid := range existing {
		has[uid] = true
	}
	up := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet {
			w.WriteHeader(http.StatusNoContent)
			return
		}
		name := r.URL.Query().Get("rrset_name")
		uid := strings.TrimSuffix(name, "."+testZone)
		w.Header().Set("Content-Type", "application/json")
		if has[uid] {
			_, _ = w.Write([]byte(`{"rrsets":[{"records":[{"content":"1.2.3.4"}]}]}`))
		} else {
			_, _ = w.Write([]byte(`{"rrsets":[]}`))
		}
	}))
	t.Cleanup(up.Close)
	store, err := newOwnerStore(t.TempDir()+"/owners.json", []byte("test-secret"))
	if err != nil {
		t.Fatalf("store: %v", err)
	}
	cfg := config{
		zone:               testZone,
		upstream:           up.URL,
		realKey:            "real-pdns-key",
		proxyKey:           "restricted-key",
		maxBodyBytes:       64 * 1024,
		registrationClosed: closed,
	}
	return newProxy(cfg, store)
}

func TestClosedRegistrationRefusesNewNames(t *testing.T) {
	p := testProxyZone(t, true)
	code, msg := doPatch(p, "tokenA", "1.1.1.1", aReplace("ab12cd34"))
	if code != http.StatusForbidden {
		t.Fatalf("a name that does not exist should be refused, got %d", code)
	}
	// The message is the whole point of refusing here rather than earlier: an
	// 0.2.x instance shows this text to someone who cannot read a log.
	for _, want := range []string{"0.3.0", "local network", "February 2027"} {
		if !strings.Contains(msg, want) {
			t.Errorf("refusal should mention %q, got %q", want, msg)
		}
	}
	if len(p.store.owners) != 0 {
		t.Fatalf("a refused claim must record nothing, store = %v", p.store.owners)
	}
}

func TestClosedRegistrationKeepsExistingNamesWorking(t *testing.T) {
	// The zone has this sub-domain but the store has no owner for it — exactly
	// the state of every name registered before ownership was recorded. It must
	// still be updatable, and updating it records the owner from then on.
	p := testProxyZone(t, true, "ab12cd34")
	if code, msg := doPatch(p, "tokenA", "1.1.1.1", aReplace("ab12cd34")); code != 0 {
		t.Fatalf("an existing sub-domain must still be updatable: %d %s", code, msg)
	}
	if p.store.owners["ab12cd34"] != p.store.hash("tokenA") {
		t.Fatalf("the update should record ownership, store = %v", p.store.owners)
	}
	// And its certificate must keep renewing: the ACME challenge is a TXT under
	// a uid whose A record exists, and the check never looks at TXT names.
	acme := `{"rrsets":[{"name":"_acme-challenge.ab12cd34.` + testZone +
		`","type":"TXT","changetype":"REPLACE"}]}`
	if code, msg := doPatch(p, "tokenA", "1.1.1.1", acme); code != 0 {
		t.Fatalf("certificate renewal must survive the closure: %d %s", code, msg)
	}
	// So must releasing it.
	if code, msg := doPatch(p, "tokenA", "1.1.1.1", aDelete("ab12cd34")); code != 0 {
		t.Fatalf("deleting an owned sub-domain must still work: %d %s", code, msg)
	}
}

func TestOpenRegistrationIsTheDefault(t *testing.T) {
	// A config with the field unset — a missing variable, or a test like this
	// one — must leave the door as it is today. Closing by accident would strand
	// every new install with no way to diagnose it.
	p := testProxyZone(t, false)
	if code, msg := doPatch(p, "tokenA", "1.1.1.1", aReplace("ab12cd34")); code != 0 {
		t.Fatalf("registration should be open by default: %d %s", code, msg)
	}
}

func TestClosedRegistrationAllowsWhenTheZoneCannotBeAsked(t *testing.T) {
	// PowerDNS unreachable. The choice here is between refusing someone who can
	// do nothing about it and letting a registration through during an outage;
	// the second is recoverable, the first is not.
	p := testProxyZone(t, true)
	p.cfg.upstream = "http://127.0.0.1:1" // nothing listens
	if code, msg := doPatch(p, "tokenA", "1.1.1.1", aReplace("ab12cd34")); code != 0 {
		t.Fatalf("an unanswerable existence check must not refuse: %d %s", code, msg)
	}
}

func TestQuotaSweepsIdleAddresses(t *testing.T) {
	q := newQuota(2)

	// Attempts that have aged out of the window, from addresses that never came
	// back. Pruning only happens on that same address's next call, so without a
	// sweep these stay in the map for the life of the process.
	stale := time.Now().Add(-2 * time.Hour)
	for _, ip := range []string{"1.0.0.1", "1.0.0.2", "1.0.0.3"} {
		q.hits[ip] = []time.Time{stale}
	}
	q.hits["2.0.0.1"] = []time.Time{time.Now()} // still inside the window
	q.lastSweep = time.Now().Add(-quotaSweepEvery - time.Minute)

	if !q.allow("3.0.0.1") {
		t.Fatal("a first claim from a new address must be allowed")
	}
	if _, ok := q.hits["1.0.0.1"]; ok {
		t.Errorf("aged-out address still in the map: %v", q.hits)
	}
	if len(q.hits) != 2 { // the live address + the caller
		t.Errorf("map should hold only live addresses, got %v", q.hits)
	}
	// Sweeping must not cost a live address its recorded attempts.
	if !q.allow("2.0.0.1") {
		t.Error("live address still under budget must be allowed")
	}
	if q.allow("2.0.0.1") {
		t.Error("live address over budget must be refused")
	}
}

func TestCheckGetRequiresFilter(t *testing.T) {
	p := testProxy(t, 0)
	// No rrset_name → refused (prevents full-zone dumps).
	r := httptest.NewRequest("GET", "/api/v1/servers/localhost/zones/"+testZone, nil)
	if code, _ := p.checkGet(r); code != 403 {
		t.Fatalf("GET without rrset_name should be 403, got %d", code)
	}
	// Valid filter → allowed.
	r = httptest.NewRequest("GET", "/api/v1/servers/localhost/zones/"+testZone+"?rrset_name=ab12cd34."+testZone+"&rrset_type=A", nil)
	if code, _ := p.checkGet(r); code != 0 {
		t.Fatalf("valid GET should pass, got %d", code)
	}
}
