package main

import (
	"net/http/httptest"
	"strings"
	"testing"
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
