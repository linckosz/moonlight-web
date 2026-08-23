package main

import (
	"bytes"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
	"time"
)

// Test fixtures ─────────────────────────────────────────────────────────────

const (
	idA = "abcdefghjkmnpqrstvwxyz0123"
	idB = "0123456789abcdefghjkmnpqrs"
	// 64 hex characters, the shape a host actually generates.
	tokenA = "1111111111111111111111111111111111111111111111111111111111111111"
	tokenB = "2222222222222222222222222222222222222222222222222222222222222222"
)

func testServer(t *testing.T) (*server, *httptest.Server) {
	t.Helper()
	cfg := config{
		storePath:   filepath.Join(t.TempDir(), "rendezvous.json"),
		secret:      []byte("test-secret"),
		maxNewPerHr: 20,
		maxPeerPerM: 30,
		maxSessions: 2,
		maxMessage:  64 * 1024,
		// Long enough that no test races the keepalive.
		pingEvery: 60 * time.Second,
	}
	store, err := newClaimStore(cfg.storePath, cfg.secret)
	if err != nil {
		t.Fatalf("store: %v", err)
	}
	s := newServer(cfg, store)
	ts := httptest.NewServer(s.routes())
	t.Cleanup(ts.Close)
	return s, ts
}

func postClaim(t *testing.T, ts *httptest.Server, method, token, id string) (int, map[string]any) {
	t.Helper()
	body, _ := json.Marshal(claimRequest{ID: id})
	req, err := http.NewRequest(method, ts.URL+"/v1/claim", bytes.NewReader(body))
	if err != nil {
		t.Fatalf("request: %v", err)
	}
	if token != "" {
		req.Header.Set("X-MW-Owner", token)
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatalf("do: %v", err)
	}
	defer resp.Body.Close()
	var out map[string]any
	_ = json.NewDecoder(resp.Body).Decode(&out)
	return resp.StatusCode, out
}

// claimAndConnect claims id for token and brings its host line up.
func claimAndConnect(t *testing.T, ts *httptest.Server, token, id string) *testClient {
	t.Helper()
	if status, body := postClaim(t, ts, http.MethodPost, token, id); status != http.StatusOK {
		t.Fatalf("claim %s: status %d (%v)", id, status, body)
	}
	host, status := dialWS(t, ts.URL, "/v1/host?id="+id, map[string]string{"X-MW-Owner": token})
	if status != http.StatusSwitchingProtocols {
		t.Fatalf("host line: status %d, want 101", status)
	}
	t.Cleanup(host.Close)
	return host
}

// Identifiers ───────────────────────────────────────────────────────────────

func TestNormaliseIDFoldsTheFormsAHumanProduces(t *testing.T) {
	// Case, grouping hyphens, and Crockford's ambiguous letters all fold to the
	// one canonical form. If host and browser disagreed here, a claim made in
	// one form would simply never be found again in the other.
	cases := map[string]string{
		"ABCDEFGHJKMNPQRSTVWXYZ0123": idA,
		"abcde-fghjk-mnpqr-stvwx-yz0123": "abcdefghjkmnpqrstvwxyz0123",
		"O123456789abcdefghjkmnpqrs":     idB, // letter O → digit zero
		"0I23456789abcdefghjkmnpqrs":     idB, // letter I → digit one
		"0l23456789abcdefghjkmnpqrs":     idB, // letter l → digit one
	}
	for in, want := range cases {
		if got := normaliseID(in); got != want {
			t.Errorf("normaliseID(%q) = %q, want %q", in, got, want)
		}
	}
}

func TestValidIDRejectsShortAndAmbiguousForms(t *testing.T) {
	bad := []string{
		"",
		"abcdefghjkmnpqrstvwxyz012",   // 25 — one short
		"abcdefghjkmnpqrstvwxyz01234", // 27 — one long
		"abcdefghjkmnpqrstvwxyz012!",
		"ABCDEFGHJKMNPQRSTVWXYZ0123", // upper case is only valid after folding
	}
	for _, id := range bad {
		if validID(id) {
			t.Errorf("validID(%q) = true", id)
		}
	}
	if !validID(idA) {
		t.Errorf("validID(%q) = false", idA)
	}
}

// Claims ────────────────────────────────────────────────────────────────────

func TestClaimIsTrustOnFirstUse(t *testing.T) {
	_, ts := testServer(t)

	status, body := postClaim(t, ts, http.MethodPost, tokenA, idA)
	if status != http.StatusOK || body["claimed"] != true {
		t.Fatalf("first claim: status %d body %v", status, body)
	}
	// The same owner coming back — every restart does this — is not a new claim.
	status, body = postClaim(t, ts, http.MethodPost, tokenA, idA)
	if status != http.StatusOK || body["claimed"] != false {
		t.Fatalf("re-claim by owner: status %d body %v", status, body)
	}
	// Anyone else is refused, which is the whole point: without this, a stranger
	// could take over an identifier already in someone's bookmarks.
	if status, _ = postClaim(t, ts, http.MethodPost, tokenB, idA); status != http.StatusConflict {
		t.Fatalf("claim by another owner: status %d, want 409", status)
	}
}

func TestOneLinePerOwner(t *testing.T) {
	_, ts := testServer(t)
	if status, _ := postClaim(t, ts, http.MethodPost, tokenA, idA); status != http.StatusOK {
		t.Fatalf("first claim: %d", status)
	}
	// Without this rule one token could reserve identifiers in bulk, and the
	// per-address budget alone would not stop a patient caller.
	if status, _ := postClaim(t, ts, http.MethodPost, tokenA, idB); status != http.StatusConflict {
		t.Fatalf("second id for same owner: status %d, want 409", status)
	}
	// Releasing the first frees the owner to take another — the reinstall path.
	if status, _ := postClaim(t, ts, http.MethodDelete, tokenA, idA); status != http.StatusOK {
		t.Fatalf("release: %d", status)
	}
	if status, _ := postClaim(t, ts, http.MethodPost, tokenA, idB); status != http.StatusOK {
		t.Fatalf("claim after release: %d", status)
	}
}

func TestReleaseRequiresOwnership(t *testing.T) {
	_, ts := testServer(t)
	postClaim(t, ts, http.MethodPost, tokenA, idA)
	if status, _ := postClaim(t, ts, http.MethodDelete, tokenB, idA); status != http.StatusForbidden {
		t.Fatalf("release by a stranger: status %d, want 403", status)
	}
	// Still owned by A afterwards.
	if status, body := postClaim(t, ts, http.MethodPost, tokenA, idA); status != http.StatusOK || body["claimed"] != false {
		t.Fatalf("owner lost the claim: status %d body %v", status, body)
	}
}

func TestClaimRejectsAShortToken(t *testing.T) {
	_, ts := testServer(t)
	if status, _ := postClaim(t, ts, http.MethodPost, "short", idA); status != http.StatusUnauthorized {
		t.Fatalf("short token: status %d, want 401", status)
	}
	if status, _ := postClaim(t, ts, http.MethodPost, "", idA); status != http.StatusUnauthorized {
		t.Fatalf("absent token: status %d, want 401", status)
	}
}

func TestStoreKeepsNoUsableCredential(t *testing.T) {
	s, ts := testServer(t)
	postClaim(t, ts, http.MethodPost, tokenA, idA)

	data, err := os.ReadFile(s.cfg.storePath)
	if err != nil {
		t.Fatalf("read store: %v", err)
	}
	// A leak of this file must hand an attacker nothing they can present back.
	if strings.Contains(string(data), tokenA) {
		t.Fatal("the owner token was written to disk in the clear")
	}
	if !strings.Contains(string(data), idA) {
		t.Fatal("the id is missing from the store")
	}
}

func TestNewClaimBudgetIsPerAddress(t *testing.T) {
	cfg := config{
		storePath:   filepath.Join(t.TempDir(), "s.json"),
		secret:      []byte("k"),
		maxNewPerHr: 1,
		maxPeerPerM: 30,
		maxSessions: 2,
		maxMessage:  4096,
		pingEvery:   time.Minute,
	}
	store, _ := newClaimStore(cfg.storePath, cfg.secret)
	ts := httptest.NewServer(newServer(cfg, store).routes())
	defer ts.Close()

	if status, _ := postClaim(t, ts, http.MethodPost, tokenA, idA); status != http.StatusOK {
		t.Fatalf("first claim: %d", status)
	}
	if status, _ := postClaim(t, ts, http.MethodPost, tokenB, idB); status != http.StatusTooManyRequests {
		t.Fatalf("second new claim: status %d, want 429", status)
	}
	// A host re-presenting an id it already owns is not a new claim and must not
	// be charged — otherwise a machine that reboots often locks itself out.
	if status, _ := postClaim(t, ts, http.MethodPost, tokenA, idA); status != http.StatusOK {
		t.Fatalf("re-claim under an exhausted budget: status %d, want 200", status)
	}
}

// The held line ─────────────────────────────────────────────────────────────

func TestHostLineRequiresOwnership(t *testing.T) {
	_, ts := testServer(t)
	postClaim(t, ts, http.MethodPost, tokenA, idA)

	if _, status := dialWS(t, ts.URL, "/v1/host?id="+idA, map[string]string{"X-MW-Owner": tokenB}); status != http.StatusUnauthorized {
		t.Fatalf("wrong token: status %d, want 401", status)
	}
	if _, status := dialWS(t, ts.URL, "/v1/host?id="+idA, nil); status != http.StatusUnauthorized {
		t.Fatalf("no token: status %d, want 401", status)
	}
	// An id nobody claimed is not a line either.
	if _, status := dialWS(t, ts.URL, "/v1/host?id="+idB, map[string]string{"X-MW-Owner": tokenA}); status != http.StatusUnauthorized {
		t.Fatalf("unclaimed id: status %d, want 401", status)
	}
}

func TestSignallingIsRelayedBothWays(t *testing.T) {
	_, ts := testServer(t)
	host := claimAndConnect(t, ts, tokenA, idA)

	peer, status := dialWS(t, ts.URL, "/v1/peer?id="+idA, nil)
	if status != http.StatusSwitchingProtocols {
		t.Fatalf("peer: status %d", status)
	}
	defer peer.Close()

	// The host is told a browser arrived, and given the session id it must quote.
	var open hostFrame
	host.ReadJSON(t, &open)
	if open.T != msgOpen || open.S == "" {
		t.Fatalf("host got %+v, want an open frame with a session id", open)
	}

	var ready peerFrame
	peer.ReadJSON(t, &ready)
	if ready.T != msgReady {
		t.Fatalf("peer got %+v, want ready", ready)
	}

	// Browser → host.
	peer.WriteJSON(t, peerFrame{T: msgMsg, D: json.RawMessage(`{"sdp":"offer"}`)})
	var up hostFrame
	host.ReadJSON(t, &up)
	if up.T != msgMsg || up.S != open.S || string(up.D) != `{"sdp":"offer"}` {
		t.Fatalf("host received %+v", up)
	}

	// Host → browser.
	host.WriteJSON(t, hostFrame{T: msgMsg, S: open.S, D: json.RawMessage(`{"sdp":"answer"}`)})
	var down peerFrame
	peer.ReadJSON(t, &down)
	if down.T != msgMsg || string(down.D) != `{"sdp":"answer"}` {
		t.Fatalf("peer received %+v", down)
	}
}

// The payload crosses untouched: this service has no business understanding
// what it carries, and the signed fingerprint inside must survive the trip or
// MW-BIND-v1 fails at the far end.
//
// The assertion is on the DECODED value, not the bytes, and that is deliberate.
// Go's JSON encoder escapes <, > and & when it re-emits a RawMessage, so the
// wire form can legitimately differ; what must never differ is what the other
// end parses back out. A test pinned to bytes would fail on a payload containing
// an angle bracket and teach the wrong lesson.
func TestPayloadSurvivesTheRelayUnchanged(t *testing.T) {
	_, ts := testServer(t)
	host := claimAndConnect(t, ts, tokenA, idA)
	peer, _ := dialWS(t, ts.URL, "/v1/peer?id="+idA, nil)
	defer peer.Close()

	var open hostFrame
	host.ReadJSON(t, &open)
	var ready peerFrame
	peer.ReadJSON(t, &ready)

	const payload = `{"fp":"AB:CD","sig":"b64+/=","nested":{"a":[1,2,3]},"utf8":"café","lt":"a<b&c"}`
	peer.WriteJSON(t, peerFrame{T: msgMsg, D: json.RawMessage(payload)})
	var up hostFrame
	host.ReadJSON(t, &up)

	var sent, got any
	if err := json.Unmarshal([]byte(payload), &sent); err != nil {
		t.Fatalf("fixture is not valid JSON: %v", err)
	}
	if err := json.Unmarshal(up.D, &got); err != nil {
		t.Fatalf("relayed payload is not valid JSON (%s): %v", up.D, err)
	}
	if !reflect.DeepEqual(sent, got) {
		t.Fatalf("payload altered in transit:\n got %s\nwant %s", up.D, payload)
	}
}

// A browser must not be able to name a session. If it could, one visitor to a
// host could inject signalling into another visitor's negotiation.
func TestPeerCannotAddressAnotherSession(t *testing.T) {
	_, ts := testServer(t)
	host := claimAndConnect(t, ts, tokenA, idA)

	first, _ := dialWS(t, ts.URL, "/v1/peer?id="+idA, nil)
	defer first.Close()
	var openFirst hostFrame
	host.ReadJSON(t, &openFirst)
	var ready peerFrame
	first.ReadJSON(t, &ready)

	second, _ := dialWS(t, ts.URL, "/v1/peer?id="+idA, nil)
	defer second.Close()
	var openSecond hostFrame
	host.ReadJSON(t, &openSecond)
	second.ReadJSON(t, &ready)

	// Second peer forges the first peer's session id in a field the wire format
	// does not even have for browsers.
	second.WriteJSON(t, map[string]any{"t": msgMsg, "s": openFirst.S, "d": json.RawMessage(`"forged"`)})

	var got hostFrame
	host.ReadJSON(t, &got)
	if got.S != openSecond.S {
		t.Fatalf("relayed under session %q, want the sender's own %q", got.S, openSecond.S)
	}
}

// Enumeration defence: an id that was never claimed and a claimed id whose host
// is not connected must be indistinguishable — same HTTP status, same frame.
// Any difference turns this endpoint into an oracle for which ids exist.
func TestUnknownIdAndOfflineHostAreIndistinguishable(t *testing.T) {
	_, ts := testServer(t)
	postClaim(t, ts, http.MethodPost, tokenA, idA) // claimed, but never connected

	observe := func(id string) (int, peerFrame) {
		c, status := dialWS(t, ts.URL, "/v1/peer?id="+id, nil)
		if status != http.StatusSwitchingProtocols {
			return status, peerFrame{}
		}
		defer c.Close()
		var f peerFrame
		c.ReadJSON(t, &f)
		return status, f
	}

	claimedStatus, claimedFrame := observe(idA)
	unknownStatus, unknownFrame := observe(idB)

	if claimedStatus != unknownStatus {
		t.Fatalf("status differs: claimed %d, unknown %d", claimedStatus, unknownStatus)
	}
	if claimedFrame.T != unknownFrame.T || claimedFrame.Code != unknownFrame.Code ||
		len(claimedFrame.D) != len(unknownFrame.D) {
		t.Fatalf("frame differs: claimed %+v, unknown %+v", claimedFrame, unknownFrame)
	}
	if claimedFrame.Code != codeOffline {
		t.Fatalf("code = %q, want %q", claimedFrame.Code, codeOffline)
	}
}

func TestSessionCapRefusesExtraBrowsers(t *testing.T) {
	_, ts := testServer(t) // maxSessions = 2
	host := claimAndConnect(t, ts, tokenA, idA)

	for i := 0; i < 2; i++ {
		c, _ := dialWS(t, ts.URL, "/v1/peer?id="+idA, nil)
		defer c.Close()
		var open hostFrame
		host.ReadJSON(t, &open)
		var ready peerFrame
		c.ReadJSON(t, &ready)
		if ready.T != msgReady {
			t.Fatalf("peer %d got %+v", i, ready)
		}
	}

	third, _ := dialWS(t, ts.URL, "/v1/peer?id="+idA, nil)
	defer third.Close()
	var f peerFrame
	third.ReadJSON(t, &f)
	if f.T != msgError || f.Code != codeBusy {
		t.Fatalf("third peer got %+v, want a busy refusal", f)
	}
}

// A host whose connection dropped without the socket noticing reconnects long
// before the read deadline expires. The newcomer proved ownership on this very
// handshake, so it must win — otherwise the machine is locked out of its own
// identifier for up to the ping timeout, every time its address changes.
func TestHostReconnectDisplacesTheStaleLine(t *testing.T) {
	s, ts := testServer(t)
	first := claimAndConnect(t, ts, tokenA, idA)

	second, status := dialWS(t, ts.URL, "/v1/host?id="+idA, map[string]string{"X-MW-Owner": tokenA})
	if status != http.StatusSwitchingProtocols {
		t.Fatalf("reconnect: status %d", status)
	}
	defer second.Close()

	if code := first.ExpectClose(t); code != closePolicy {
		t.Fatalf("displaced line closed with %d, want %d", code, closePolicy)
	}

	// And the surviving line is the new one: a browser reaches it.
	waitFor(t, func() bool { return s.hub.line(idA) != nil })
	peer, _ := dialWS(t, ts.URL, "/v1/peer?id="+idA, nil)
	defer peer.Close()
	var open hostFrame
	second.ReadJSON(t, &open)
	if open.T != msgOpen {
		t.Fatalf("new line got %+v, want an open frame", open)
	}
}

// When the line goes, the browsers hanging off it must be told, not left
// waiting on a peer that will never answer.
func TestBrowsersAreNotifiedWhenTheLineDrops(t *testing.T) {
	_, ts := testServer(t)
	host := claimAndConnect(t, ts, tokenA, idA)

	peer, _ := dialWS(t, ts.URL, "/v1/peer?id="+idA, nil)
	defer peer.Close()
	var open hostFrame
	host.ReadJSON(t, &open)
	var ready peerFrame
	peer.ReadJSON(t, &ready)

	host.Close()

	var f peerFrame
	peer.ReadJSON(t, &f)
	if f.T != msgError || f.Code != codeHostGone {
		t.Fatalf("peer got %+v, want a host-gone error", f)
	}
}

func TestHostClosingOneSessionLeavesTheOtherAlone(t *testing.T) {
	_, ts := testServer(t)
	host := claimAndConnect(t, ts, tokenA, idA)

	a, _ := dialWS(t, ts.URL, "/v1/peer?id="+idA, nil)
	defer a.Close()
	var openA hostFrame
	host.ReadJSON(t, &openA)
	var ready peerFrame
	a.ReadJSON(t, &ready)

	b, _ := dialWS(t, ts.URL, "/v1/peer?id="+idA, nil)
	defer b.Close()
	var openB hostFrame
	host.ReadJSON(t, &openB)
	b.ReadJSON(t, &ready)

	host.WriteJSON(t, hostFrame{T: msgClose, S: openA.S})
	if code := a.ExpectClose(t); code != closeNormal {
		t.Fatalf("closed session got code %d", code)
	}

	// b is untouched and still relaying.
	host.WriteJSON(t, hostFrame{T: msgMsg, S: openB.S, D: json.RawMessage(`"still here"`)})
	var f peerFrame
	b.ReadJSON(t, &f)
	if f.T != msgMsg || string(f.D) != `"still here"` {
		t.Fatalf("surviving session got %+v", f)
	}
}

func TestMalformedIdIsRejectedBeforeTheUpgrade(t *testing.T) {
	_, ts := testServer(t)
	// Nothing of that shape could exist, so refusing early leaks nothing — and
	// it keeps junk from reaching the hub at all.
	if _, status := dialWS(t, ts.URL, "/v1/peer?id=nope", nil); status != http.StatusBadRequest {
		t.Fatalf("status = %d, want 400", status)
	}
	if _, status := dialWS(t, ts.URL, "/v1/host?id=nope", map[string]string{"X-MW-Owner": tokenA}); status != http.StatusBadRequest {
		t.Fatalf("status = %d, want 400", status)
	}
}

func TestPeerBudgetIsPerAddress(t *testing.T) {
	cfg := config{
		storePath:   filepath.Join(t.TempDir(), "s.json"),
		secret:      []byte("k"),
		maxNewPerHr: 20,
		maxPeerPerM: 2,
		maxSessions: 8,
		maxMessage:  4096,
		pingEvery:   time.Minute,
	}
	store, _ := newClaimStore(cfg.storePath, cfg.secret)
	ts := httptest.NewServer(newServer(cfg, store).routes())
	defer ts.Close()

	// The budget bounds guessing, so it must apply to ids that do not exist —
	// which is precisely what a guesser sends.
	for i := 0; i < 2; i++ {
		c, status := dialWS(t, ts.URL, "/v1/peer?id="+idB, nil)
		if status != http.StatusSwitchingProtocols {
			t.Fatalf("attempt %d: status %d", i, status)
		}
		c.Close()
	}
	if _, status := dialWS(t, ts.URL, "/v1/peer?id="+idB, nil); status != http.StatusTooManyRequests {
		t.Fatalf("status = %d, want 429", status)
	}
}

func TestClaimsSurviveARestart(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "rendezvous.json")
	cfg := config{
		storePath: path, secret: []byte("k"),
		maxNewPerHr: 20, maxPeerPerM: 30, maxSessions: 2,
		maxMessage: 4096, pingEvery: time.Minute,
	}

	store, _ := newClaimStore(path, cfg.secret)
	ts := httptest.NewServer(newServer(cfg, store).routes())
	if status, _ := postClaim(t, ts, http.MethodPost, tokenA, idA); status != http.StatusOK {
		t.Fatalf("claim: %d", status)
	}
	ts.Close()

	// A restart must not hand someone else an identifier already in a bookmark.
	reloaded, err := newClaimStore(path, cfg.secret)
	if err != nil {
		t.Fatalf("reload: %v", err)
	}
	ts2 := httptest.NewServer(newServer(cfg, reloaded).routes())
	defer ts2.Close()
	if status, _ := postClaim(t, ts2, http.MethodPost, tokenB, idA); status != http.StatusConflict {
		t.Fatalf("after restart, a stranger claimed it: status %d", status)
	}
	if status, body := postClaim(t, ts2, http.MethodPost, tokenA, idA); status != http.StatusOK || body["claimed"] != false {
		t.Fatalf("owner not recognised after restart: status %d body %v", status, body)
	}
}

func TestQuotaSweepsIdleAddresses(t *testing.T) {
	q := newQuota(5, time.Hour)
	q.hits["1.2.3.4"] = []time.Time{time.Now().Add(-2 * time.Hour)}
	q.hits["5.6.7.8"] = []time.Time{time.Now()}
	q.sweep(time.Now())
	if _, ok := q.hits["1.2.3.4"]; ok {
		t.Fatal("an address whose attempts all aged out was kept")
	}
	if _, ok := q.hits["5.6.7.8"]; !ok {
		t.Fatal("a live address was swept")
	}
}

// waitFor polls until cond holds, so a test never depends on goroutine ordering.
func waitFor(t *testing.T, cond func() bool) {
	t.Helper()
	deadline := time.Now().Add(3 * time.Second)
	for time.Now().Before(deadline) {
		if cond() {
			return
		}
		time.Sleep(5 * time.Millisecond)
	}
	t.Fatal("condition never became true")
}
