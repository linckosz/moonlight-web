package main

import (
	"bufio"
	"crypto/rand"
	"encoding/binary"
	"encoding/json"
	"io"
	"net"
	"net/http"
	"strings"
	"testing"
	"time"
)

// A minimal WebSocket CLIENT, for tests only.
//
// The server half lives in ws.go; driving it needs the other half, including the
// masking every client frame must carry. Writing it here rather than importing
// one keeps the module dependency-free and, more usefully, lets a test emit
// deliberately illegal frames — unmasked, reserved bits set, a fragmented ping —
// which a well-behaved library would refuse to produce.

type testClient struct {
	conn net.Conn
	br   *bufio.Reader
}

// dialWS performs the opening handshake against an httptest server. It returns
// the HTTP status; anything other than 101 means the handshake was refused and
// the client is nil.
func dialWS(t *testing.T, base, path string, hdr map[string]string) (*testClient, int) {
	t.Helper()
	addr := strings.TrimPrefix(base, "http://")
	conn, err := net.Dial("tcp", addr)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	_ = conn.SetDeadline(time.Now().Add(10 * time.Second))

	var b strings.Builder
	b.WriteString("GET " + path + " HTTP/1.1\r\n")
	b.WriteString("Host: " + addr + "\r\n")
	b.WriteString("Upgrade: websocket\r\n")
	// Deliberately a token LIST: browsers send "keep-alive, Upgrade", and a
	// server comparing the whole value would reject them.
	b.WriteString("Connection: keep-alive, Upgrade\r\n")
	b.WriteString("Sec-WebSocket-Version: 13\r\n")
	b.WriteString("Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n")
	for k, v := range hdr {
		b.WriteString(k + ": " + v + "\r\n")
	}
	b.WriteString("\r\n")
	if _, err := conn.Write([]byte(b.String())); err != nil {
		t.Fatalf("write handshake: %v", err)
	}

	br := bufio.NewReader(conn)
	resp, err := http.ReadResponse(br, nil)
	if err != nil {
		_ = conn.Close()
		t.Fatalf("read handshake response: %v", err)
	}
	if resp.StatusCode != http.StatusSwitchingProtocols {
		_, _ = io.Copy(io.Discard, resp.Body)
		_ = resp.Body.Close()
		_ = conn.Close()
		return nil, resp.StatusCode
	}
	if got := resp.Header.Get("Sec-WebSocket-Accept"); got != "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=" {
		t.Fatalf("Sec-WebSocket-Accept = %q, want the RFC 6455 §1.3 example value", got)
	}
	return &testClient{conn: conn, br: br}, resp.StatusCode
}

func (c *testClient) Close() { _ = c.conn.Close() }

// writeFrame emits one properly masked client frame.
func (c *testClient) writeFrame(fin bool, op byte, payload []byte) error {
	var mask [4]byte
	if _, err := rand.Read(mask[:]); err != nil {
		return err
	}
	n := len(payload)
	hdr := make([]byte, 0, 14)
	first := op
	if fin {
		first |= 0x80
	}
	hdr = append(hdr, first)
	switch {
	case n <= 125:
		hdr = append(hdr, 0x80|byte(n))
	case n <= 0xFFFF:
		var ext [2]byte
		binary.BigEndian.PutUint16(ext[:], uint16(n))
		hdr = append(hdr, 0x80|126, ext[0], ext[1])
	default:
		var ext [8]byte
		binary.BigEndian.PutUint64(ext[:], uint64(n))
		hdr = append(hdr, 0x80|127)
		hdr = append(hdr, ext[:]...)
	}
	hdr = append(hdr, mask[:]...)
	masked := make([]byte, n)
	for i := range payload {
		masked[i] = payload[i] ^ mask[i&3]
	}
	_, err := c.conn.Write(append(hdr, masked...))
	return err
}

func (c *testClient) WriteText(s string) error { return c.writeFrame(true, opText, []byte(s)) }

func (c *testClient) WriteJSON(t *testing.T, v any) {
	t.Helper()
	body, err := json.Marshal(v)
	if err != nil {
		t.Fatalf("marshal: %v", err)
	}
	if err := c.WriteText(string(body)); err != nil {
		t.Fatalf("write: %v", err)
	}
}

// writeRaw sends bytes verbatim, so a test can build an illegal frame by hand.
func (c *testClient) writeRaw(b []byte) error {
	_, err := c.conn.Write(b)
	return err
}

// readFrame reads one server frame. Server frames are never masked.
func (c *testClient) readFrame() (fin bool, op byte, payload []byte, err error) {
	_ = c.conn.SetReadDeadline(time.Now().Add(10 * time.Second))
	var hdr [2]byte
	if _, err = io.ReadFull(c.br, hdr[:]); err != nil {
		return
	}
	fin = hdr[0]&0x80 != 0
	op = hdr[0] & 0x0F
	if hdr[1]&0x80 != 0 {
		err = errProto{"server frame must not be masked"}
		return
	}
	n := int64(hdr[1] & 0x7F)
	switch n {
	case 126:
		var ext [2]byte
		if _, err = io.ReadFull(c.br, ext[:]); err != nil {
			return
		}
		n = int64(binary.BigEndian.Uint16(ext[:]))
	case 127:
		var ext [8]byte
		if _, err = io.ReadFull(c.br, ext[:]); err != nil {
			return
		}
		n = int64(binary.BigEndian.Uint64(ext[:]))
	}
	payload = make([]byte, n)
	_, err = io.ReadFull(c.br, payload)
	return
}

// ReadMessage returns the next DATA message. Control frames are handled here so
// they never surface as results: a ping is answered, and a pong is dropped —
// including the one the server sends back when a test pings mid-message, which
// otherwise arrives ahead of the data and looks like the message itself.
// A close frame is returned as-is so tests can read the code out of it.
func (c *testClient) ReadMessage(t *testing.T) (op byte, payload []byte) {
	t.Helper()
	for {
		_, o, p, err := c.readFrame()
		if err != nil {
			t.Fatalf("read frame: %v", err)
		}
		switch o {
		case opPing:
			if err := c.writeFrame(true, opPong, p); err != nil {
				t.Fatalf("pong: %v", err)
			}
		case opPong:
			// Liveness only; never a result.
		default:
			return o, p
		}
	}
}

// ReadJSON reads one text message and decodes it.
func (c *testClient) ReadJSON(t *testing.T, v any) {
	t.Helper()
	op, payload := c.ReadMessage(t)
	if op != opText {
		t.Fatalf("expected a text frame, got opcode 0x%X (%q)", op, payload)
	}
	if err := json.Unmarshal(payload, v); err != nil {
		t.Fatalf("decode %q: %v", payload, err)
	}
}

// ExpectClose reads until a close frame and returns its code.
func (c *testClient) ExpectClose(t *testing.T) int {
	t.Helper()
	for {
		_, op, payload, err := c.readFrame()
		if err != nil {
			t.Fatalf("expected a close frame, got error: %v", err)
		}
		if op != opClose {
			continue
		}
		if len(payload) < 2 {
			return closeNormal
		}
		return int(binary.BigEndian.Uint16(payload[:2]))
	}
}
