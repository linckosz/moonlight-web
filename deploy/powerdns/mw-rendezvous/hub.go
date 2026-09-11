// The hub: held host lines, and the sessions bridged onto them.
//
// A host keeps one WebSocket open permanently (§2.2 of the architecture plan):
// its public address is a socket, not a DNS record, because the reflexive
// candidate a browser needs is a port that only exists while a connection is
// being made. Nothing here is published or polled — the line IS the reachability.
//
// A browser arrives on /v1/peer with an id, the hub opens a session against that
// host's line, and copies signalling both ways until either side goes. It reads
// nothing it copies and stores none of it: the only durable state is the id →
// owner-fingerprint claim in store.go.
//
// Backpressure, deliberately simple
// ---------------------------------
// Every socket write carries a 5 s deadline (wsWriteTimeout) and there are no
// queues. A browser too wedged to absorb a few kilobytes of SDP within that
// budget has its socket killed, which is the correct outcome; what must not
// happen is the host's read loop blocking on it forever, and the deadline is
// what bounds that. Signalling is a handful of messages per session, so a queue
// would add failure modes to buy throughput nobody needs. If the traffic shape
// ever changes, this is the assumption to revisit first.
package main

import (
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"log"
	"strconv"
	"sync"
	"time"
)

// Envelope types on the host line. The host multiplexes every browser session
// over its single socket, so each frame names the session it belongs to.
const (
	msgOpen  = "open"  // server → host: a browser arrived, here is its session id
	msgMsg   = "msg"   // both ways: one opaque signalling payload
	msgClose = "close" // both ways: this session is finished
	msgReady = "ready" // server → browser: the host is on the line
	msgError = "error" // server → browser: why this is not going to work
)

// Refusal codes handed to a browser.
//
// codeOffline covers BOTH "no such id" and "that host is not connected", and
// must keep covering both: telling them apart would turn this endpoint into an
// oracle confirming which identifiers exist, defeating the 128-bit id and the
// per-IP budget in one step.
//
// codeUnsupportedVersion is the one code a HOST can receive too, on its own
// line, and the one code /v1/claim answers with in a 400. Keep in step with
// RendezvousClient.cpp and bootstrap/v1/tunnel.js.
const (
	codeOffline            = "offline"
	codeBusy               = "busy"
	codeHostGone           = "host-gone" // the line dropped while this session was live
	codeUnsupportedVersion = "unsupported_version"
)

// ── Protocol revision ───────────────────────────────────────────────────────
//
// Two numbers answer two different questions, and neither is the app version.
//
// The path prefix (/v1/) is the SHAPE of the API. A change that would break
// every existing client gets /v2/ beside it, and the production box serves both
// until the version census says nobody is left on the old one.
//
// The `v` query parameter is what a client understands INSIDE that shape: one
// integer, sent by hosts on /v1/claim and /v1/host and by browsers on /v1/peer.
// It is absent from every client shipped before it existed, and absent means 1,
// so those pass exactly as they always did. It buys two things: a client too
// old to be served is told so in a code it can show instead of failing in a way
// that looks like a network problem; and an addition only some clients
// understand can be sent to those and withheld from the rest, because each side
// is told the other's revision (`v` on the open and ready frames).
//
// Bumping protoCurrent is an ADDITIVE change by definition — anything else is a
// /v2/. Keep in step with RendezvousClient.cpp and bootstrap/v1/tunnel.js.
const protoCurrent = 1

// protoOf reads the revision a client claims. Absent means 1, the revision every
// client that predates the field speaks. Anything that is not a positive integer
// is 0, which no accepted range includes.
func protoOf(query string) int {
	if query == "" {
		return 1
	}
	n, err := strconv.Atoi(query)
	if err != nil || n < 1 {
		return 0
	}
	return n
}

// hostFrame is what travels on the host line, in both directions.
//
// V rides only on the open frame: the revision of the browser that just
// arrived, so the host knows what that session's peer understands.
type hostFrame struct {
	T string          `json:"t"`
	S string          `json:"s"`
	D json.RawMessage `json:"d,omitempty"`
	V int             `json:"v,omitempty"`
}

// peerFrame is what travels on a browser socket. The session id is implicit —
// a browser has exactly one — so it never appears on the wire, and a browser
// therefore cannot address a session that is not its own.
//
// V rides only on the ready frame: the revision of the host on the line.
type peerFrame struct {
	T    string          `json:"t"`
	D    json.RawMessage `json:"d,omitempty"`
	Code string          `json:"code,omitempty"`
	V    int             `json:"v,omitempty"`
}

// ── Sessions ────────────────────────────────────────────────────────────────

type session struct {
	id   string
	conn *wsConn
}

// ── Host lines ──────────────────────────────────────────────────────────────

type hostLine struct {
	id    string
	conn  *wsConn
	proto int // the host's protocol revision, handed to each browser on ready

	mu       sync.Mutex
	sessions map[string]*session
	gone     bool
}

// addSession registers a browser against this line, or returns the refusal code
// to hand back. It reports "offline" for a line that shut down between the hub
// lookup and here — the same code an unknown id gets, so the distinction never
// reaches the caller.
func (h *hostLine) addSession(s *session, max int) (code string) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if h.gone {
		return codeOffline
	}
	if len(h.sessions) >= max {
		return codeBusy
	}
	h.sessions[s.id] = s
	return ""
}

func (h *hostLine) session(id string) *session {
	h.mu.Lock()
	defer h.mu.Unlock()
	return h.sessions[id]
}

// dropSession removes a session and reports whether it was still registered, so
// the caller can avoid sending a duplicate close toward the host.
func (h *hostLine) dropSession(id string) bool {
	h.mu.Lock()
	defer h.mu.Unlock()
	if _, ok := h.sessions[id]; !ok {
		return false
	}
	delete(h.sessions, id)
	return true
}

// shutdown marks the line dead and returns the sessions to hang up.
func (h *hostLine) shutdown() []*session {
	h.mu.Lock()
	defer h.mu.Unlock()
	h.gone = true
	out := make([]*session, 0, len(h.sessions))
	for _, s := range h.sessions {
		out = append(out, s)
	}
	h.sessions = map[string]*session{}
	return out
}

// ── Hub ─────────────────────────────────────────────────────────────────────

type hub struct {
	mu    sync.Mutex
	lines map[string]*hostLine

	maxSessions int
}

func newHub(maxSessions int) *hub {
	return &hub{lines: map[string]*hostLine{}, maxSessions: maxSessions}
}

// register puts a freshly authenticated host line in place, displacing any
// earlier line for the same id.
//
// The new connection wins on purpose. A host whose address changed, or whose
// box dropped the session, reconnects long before the old socket's read deadline
// notices — and if the stale line kept the id, the host would be locked out of
// its own identifier for up to the ping timeout, every time. Ownership was
// already proven on this handshake, so the newcomer is the same machine.
func (h *hub) register(line *hostLine) (displaced *hostLine) {
	h.mu.Lock()
	defer h.mu.Unlock()
	displaced = h.lines[line.id]
	h.lines[line.id] = line
	return displaced
}

// unregister removes a line, but only if it is still the current one — a line
// that was displaced by a reconnect must not evict its successor on the way out.
func (h *hub) unregister(line *hostLine) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if h.lines[line.id] == line {
		delete(h.lines, line.id)
	}
}

func (h *hub) line(id string) *hostLine {
	h.mu.Lock()
	defer h.mu.Unlock()
	return h.lines[id]
}

func (h *hub) onlineCount() int {
	h.mu.Lock()
	defer h.mu.Unlock()
	return len(h.lines)
}

// ── Frame helpers ───────────────────────────────────────────────────────────

func newSessionID() string {
	var b [8]byte
	if _, err := rand.Read(b[:]); err != nil {
		// crypto/rand failing is not survivable, and a predictable session id
		// would let one browser address another's session on the host line.
		log.Fatalf("[mw-rendezvous] crypto/rand unavailable: %v", err)
	}
	return hex.EncodeToString(b[:])
}

func sendHost(c *wsConn, f hostFrame) error {
	body, err := json.Marshal(f)
	if err != nil {
		return err
	}
	return c.WriteText(body)
}

func sendPeer(c *wsConn, f peerFrame) error {
	body, err := json.Marshal(f)
	if err != nil {
		return err
	}
	return c.WriteText(body)
}

// refuse tells a browser why no session is coming, then hangs up. A host line
// refused for its revision gets the same frame: it has no session field, so a
// host reads it exactly as a browser does.
//
// The caller must use the SAME code for "no such id" and "that host is offline".
// Distinguishing them would turn this endpoint into an oracle that confirms
// which identifiers exist, which is exactly what the 128-bit id and the per-IP
// budget are there to prevent.
func refuse(c *wsConn, code string) {
	_ = sendPeer(c, peerFrame{T: msgError, Code: code})
	_ = c.WriteClose(closePolicy, code)
	_ = c.Close()
}

// pinger keeps a line warm and reaps dead ones.
//
// Home routers forget an idle NAT session somewhere between 30 s and 2 minutes,
// so silence would cost the host its reachability without either end noticing.
// The pong the peer sends back is what pushes the read deadline forward; a peer
// that stops answering trips it and the read loop ends.
func pinger(c *wsConn, every time.Duration, done <-chan struct{}) {
	t := time.NewTicker(every)
	defer t.Stop()
	for {
		select {
		case <-done:
			return
		case <-t.C:
			if err := c.WritePing(); err != nil {
				return
			}
		}
	}
}
