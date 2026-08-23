package main

import (
	"bytes"
	"io"
	"net/http"
	"net/http/httptest"
	"testing"
)

// Framing tests run against a bare echo handler rather than the rendezvous
// routes, so a failure points at ws.go and nothing else.

func echoServer(t *testing.T, maxMessage int) *httptest.Server {
	t.Helper()
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		c, err := wsUpgrade(w, r, maxMessage)
		if err != nil {
			return
		}
		defer c.Close()
		for {
			op, payload, err := c.ReadMessage()
			if err != nil {
				if err != io.EOF {
					_ = c.WriteClose(closeCodeFor(err), "")
				}
				return
			}
			if op == opText {
				_ = c.WriteText(payload)
			}
		}
	}))
	t.Cleanup(srv.Close)
	return srv
}

func TestHandshakeAcceptsATokenListAndEchoes(t *testing.T) {
	srv := echoServer(t, 64*1024)
	c, status := dialWS(t, srv.URL, "/", nil)
	if status != http.StatusSwitchingProtocols {
		t.Fatalf("status = %d, want 101", status)
	}
	defer c.Close()

	if err := c.WriteText(`{"hello":"world"}`); err != nil {
		t.Fatalf("write: %v", err)
	}
	op, payload := c.ReadMessage(t)
	if op != opText || string(payload) != `{"hello":"world"}` {
		t.Fatalf("echo = 0x%X %q", op, payload)
	}
}

func TestHandshakeRejectsWrongVersion(t *testing.T) {
	srv := echoServer(t, 1024)
	// dialWS always sends 13, so build the request by hand.
	resp := rawHandshake(t, srv.URL, map[string]string{
		"Upgrade":               "websocket",
		"Connection":            "Upgrade",
		"Sec-WebSocket-Version": "8",
		"Sec-WebSocket-Key":     "dGhlIHNhbXBsZSBub25jZQ==",
	})
	if resp.StatusCode != http.StatusUpgradeRequired {
		t.Fatalf("status = %d, want 426", resp.StatusCode)
	}
	if got := resp.Header.Get("Sec-WebSocket-Version"); got != "13" {
		t.Fatalf("Sec-WebSocket-Version = %q, want the version we do speak", got)
	}
}

func TestHandshakeRejectsPlainRequest(t *testing.T) {
	srv := echoServer(t, 1024)
	resp := rawHandshake(t, srv.URL, map[string]string{})
	if resp.StatusCode != http.StatusBadRequest {
		t.Fatalf("status = %d, want 400", resp.StatusCode)
	}
}

func rawHandshake(t *testing.T, base string, hdr map[string]string) *http.Response {
	t.Helper()
	req, err := http.NewRequest(http.MethodGet, base+"/", nil)
	if err != nil {
		t.Fatalf("request: %v", err)
	}
	for k, v := range hdr {
		req.Header.Set(k, v)
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatalf("do: %v", err)
	}
	t.Cleanup(func() { _ = resp.Body.Close() })
	return resp
}

// RFC 6455 §5.1 — a client frame that is not masked is a protocol error. This is
// not pedantry: unmasked client traffic is what the masking rule exists to stop
// being fed through intermediaries as if it were a crafted HTTP request.
func TestUnmaskedClientFrameIsRefused(t *testing.T) {
	srv := echoServer(t, 1024)
	c, _ := dialWS(t, srv.URL, "/", nil)
	defer c.Close()

	// FIN + text, no MASK bit, 2-byte payload.
	if err := c.writeRaw([]byte{0x81, 0x02, 'h', 'i'}); err != nil {
		t.Fatalf("write: %v", err)
	}
	if code := c.ExpectClose(t); code != closeProtocolError {
		t.Fatalf("close code = %d, want %d", code, closeProtocolError)
	}
}

// No extension is ever negotiated, so a reserved bit cannot carry meaning and
// must not be quietly ignored.
func TestReservedBitIsRefused(t *testing.T) {
	srv := echoServer(t, 1024)
	c, _ := dialWS(t, srv.URL, "/", nil)
	defer c.Close()

	// FIN + RSV1 + text, masked, empty payload.
	if err := c.writeRaw([]byte{0xC1, 0x80, 0, 0, 0, 0}); err != nil {
		t.Fatalf("write: %v", err)
	}
	if code := c.ExpectClose(t); code != closeProtocolError {
		t.Fatalf("close code = %d, want %d", code, closeProtocolError)
	}
}

func TestControlFrameCannotBeFragmented(t *testing.T) {
	srv := echoServer(t, 1024)
	c, _ := dialWS(t, srv.URL, "/", nil)
	defer c.Close()

	// Ping without FIN.
	if err := c.writeFrame(false, opPing, []byte("x")); err != nil {
		t.Fatalf("write: %v", err)
	}
	if code := c.ExpectClose(t); code != closeProtocolError {
		t.Fatalf("close code = %d, want %d", code, closeProtocolError)
	}
}

// The declared length is attacker-controlled, so the cap has to be enforced from
// the header — before anything is allocated for it.
func TestOversizedMessageIsRefusedWithoutAllocating(t *testing.T) {
	srv := echoServer(t, 1024)
	c, _ := dialWS(t, srv.URL, "/", nil)
	defer c.Close()

	// A header claiming 4 GiB, and not one byte of payload behind it. If the
	// server sized a buffer from this it would either die or hang here.
	hdr := []byte{0x81, 0xFF, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0}
	if err := c.writeRaw(hdr); err != nil {
		t.Fatalf("write: %v", err)
	}
	if code := c.ExpectClose(t); code != closeTooBig {
		t.Fatalf("close code = %d, want %d", code, closeTooBig)
	}
}

func TestFragmentedMessageIsReassembled(t *testing.T) {
	srv := echoServer(t, 64*1024)
	c, _ := dialWS(t, srv.URL, "/", nil)
	defer c.Close()

	if err := c.writeFrame(false, opText, []byte(`{"a":`)); err != nil {
		t.Fatalf("write: %v", err)
	}
	if err := c.writeFrame(false, opContinuation, []byte(`1,"b":`)); err != nil {
		t.Fatalf("write: %v", err)
	}
	// A ping in the middle of a fragmented message is legal and must not break
	// reassembly — this is the case a naive accumulator gets wrong.
	if err := c.writeFrame(true, opPing, []byte("mid")); err != nil {
		t.Fatalf("write: %v", err)
	}
	if err := c.writeFrame(true, opContinuation, []byte(`2}`)); err != nil {
		t.Fatalf("write: %v", err)
	}

	// The pong for that ping arrives first; ReadMessage skips control frames.
	op, payload := c.ReadMessage(t)
	if op != opText || string(payload) != `{"a":1,"b":2}` {
		t.Fatalf("reassembled = 0x%X %q", op, payload)
	}
}

func TestFragmentedMessageOverCapIsRefused(t *testing.T) {
	const limit = 4096
	srv := echoServer(t, limit)
	c, _ := dialWS(t, srv.URL, "/", nil)
	defer c.Close()

	chunk := bytes.Repeat([]byte("x"), 3000)
	if err := c.writeFrame(false, opText, chunk); err != nil {
		t.Fatalf("write: %v", err)
	}
	// Each fragment fits the cap; together they do not. Checking only per-frame
	// would let a peer stream unbounded memory in legal-looking pieces.
	if err := c.writeFrame(true, opContinuation, chunk); err != nil {
		t.Fatalf("write: %v", err)
	}
	if code := c.ExpectClose(t); code != closeTooBig {
		t.Fatalf("close code = %d, want %d", code, closeTooBig)
	}
}

func TestPingIsAnsweredWithPong(t *testing.T) {
	srv := echoServer(t, 1024)
	c, _ := dialWS(t, srv.URL, "/", nil)
	defer c.Close()

	if err := c.writeFrame(true, opPing, []byte("keepalive")); err != nil {
		t.Fatalf("write: %v", err)
	}
	_, op, payload, err := c.readFrame()
	if err != nil {
		t.Fatalf("read: %v", err)
	}
	if op != opPong || string(payload) != "keepalive" {
		t.Fatalf("got opcode 0x%X %q, want a pong echoing the payload", op, payload)
	}
}

func TestAcceptValueMatchesTheSpecExample(t *testing.T) {
	// RFC 6455 §1.3 worked example — if this drifts, every browser refuses us.
	if got := wsAccept("dGhlIHNhbXBsZSBub25jZQ=="); got != "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=" {
		t.Fatalf("wsAccept = %q", got)
	}
}

func TestHeaderTokenMatchingIsCaseInsensitiveAndListAware(t *testing.T) {
	h := http.Header{}
	h.Add("Connection", "keep-alive, Upgrade")
	if !headerHasToken(h, "Connection", "upgrade") {
		t.Fatal("did not find the token in a comma list")
	}
	h2 := http.Header{}
	h2.Add("Connection", "close")
	if headerHasToken(h2, "Connection", "upgrade") {
		t.Fatal("matched a token that is not there")
	}
	// A substring must not count: "upgrade-insecure-requests" is not "upgrade".
	h3 := http.Header{}
	h3.Add("Connection", "upgrade-insecure-requests")
	if headerHasToken(h3, "Connection", "upgrade") {
		t.Fatal("matched a substring rather than a whole token")
	}
}
