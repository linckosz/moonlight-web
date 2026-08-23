// RFC 6455 server subset — just enough WebSocket to relay signalling.
//
// Why hand-written rather than a library
// --------------------------------------
// mw-proxy is deliberately stdlib-only so it stays auditable, and this service
// sits in the same trust position: a poisoned dependency is named as a realistic
// compromise vector for exactly this box. The server half of RFC 6455 is a small,
// closed protocol — this file implements the parts a signalling relay needs and
// rejects everything else, rather than pulling a general-purpose library that
// implements extensions, compression and a client we would never use.
//
// What is implemented
//   - the version 13 opening handshake (server side);
//   - text/binary messages, including continuation frames, with a hard byte cap;
//   - ping (answered automatically), pong (recorded as liveness), close.
//
// What is deliberately NOT implemented
//   - extensions: the handshake never negotiates any, so a frame carrying an RSV
//     bit is a protocol error rather than something to interpret;
//   - permessage-deflate: it would let a peer spend our CPU and memory for a few
//     bytes on the wire, on messages that are already tiny;
//   - the client half.
//
// UTF-8 validity of text payloads is not checked. Every payload this service
// carries is handed straight to encoding/json, which rejects invalid UTF-8 on its
// own, and the relayed bodies are opaque blobs forwarded byte for byte.
package main

import (
	"bufio"
	"crypto/sha1"
	"encoding/base64"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"math"
	"net"
	"net/http"
	"strings"
	"sync"
	"time"
)

// wsGUID is the constant RFC 6455 §1.3 mixes into the handshake accept value.
const wsGUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

const (
	opContinuation = 0x0
	opText         = 0x1
	opBinary       = 0x2
	opClose        = 0x8
	opPing         = 0x9
	opPong         = 0xA
)

// Close codes this server sends. RFC 6455 §7.4.1.
const (
	closeNormal        = 1000
	closeProtocolError = 1002
	closePolicy        = 1008
	closeTooBig        = 1009
	closeInternal      = 1011
)

// How long a single write may block. A relayed message is a few kilobytes, so a
// peer that cannot absorb one within this budget is wedged, and the host's read
// loop must not wait on it — see the note on backpressure in hub.go.
const wsWriteTimeout = 5 * time.Second

// errProto marks a peer that broke framing rules; the caller closes with 1002.
type errProto struct{ why string }

func (e errProto) Error() string { return "websocket protocol error: " + e.why }

// errTooBig marks a message above the configured cap; the caller closes with 1009.
type errTooBig struct{}

func (errTooBig) Error() string { return "websocket message too large" }

var errWSClosed = errors.New("websocket already closed")

// ── Handshake ───────────────────────────────────────────────────────────────

// headerHasToken reports whether a comma-separated header lists token, matched
// case-insensitively. Both Connection and Upgrade are token lists, and browsers
// do send "Connection: keep-alive, Upgrade" — comparing the whole header value
// would reject perfectly ordinary clients.
func headerHasToken(h http.Header, name, token string) bool {
	for _, v := range h.Values(name) {
		for _, part := range strings.Split(v, ",") {
			if strings.EqualFold(strings.TrimSpace(part), token) {
				return true
			}
		}
	}
	return false
}

func wsAccept(key string) string {
	sum := sha1.Sum([]byte(key + wsGUID))
	return base64.StdEncoding.EncodeToString(sum[:])
}

// wsUpgrade completes the opening handshake and takes over the connection.
//
// On failure it has already written an HTTP error response and the caller must
// not touch w again. On success the ResponseWriter is spent: the connection is
// hijacked and belongs to the returned wsConn.
func wsUpgrade(w http.ResponseWriter, r *http.Request, maxMessage int) (*wsConn, error) {
	if r.Method != http.MethodGet {
		http.Error(w, "websocket handshake requires GET", http.StatusMethodNotAllowed)
		return nil, errProto{"non-GET handshake"}
	}
	if !headerHasToken(r.Header, "Connection", "upgrade") ||
		!headerHasToken(r.Header, "Upgrade", "websocket") {
		http.Error(w, "not a websocket upgrade", http.StatusBadRequest)
		return nil, errProto{"missing upgrade headers"}
	}
	if r.Header.Get("Sec-WebSocket-Version") != "13" {
		// RFC 6455 §4.2.2: advertise what we do speak, so a mismatched client
		// learns why instead of guessing.
		w.Header().Set("Sec-WebSocket-Version", "13")
		http.Error(w, "unsupported websocket version", http.StatusUpgradeRequired)
		return nil, errProto{"bad version"}
	}
	key := r.Header.Get("Sec-WebSocket-Key")
	if key == "" {
		http.Error(w, "missing Sec-WebSocket-Key", http.StatusBadRequest)
		return nil, errProto{"missing key"}
	}

	hj, ok := w.(http.Hijacker)
	if !ok {
		http.Error(w, "connection cannot be upgraded", http.StatusInternalServerError)
		return nil, errors.New("ResponseWriter does not support hijacking")
	}
	conn, brw, err := hj.Hijack()
	if err != nil {
		http.Error(w, "connection cannot be upgraded", http.StatusInternalServerError)
		return nil, err
	}

	// Answer by hand: after Hijack the normal response machinery is gone. No
	// Sec-WebSocket-Protocol is echoed — this service negotiates no subprotocol,
	// and echoing one a client asked for would claim support we do not have.
	resp := "HTTP/1.1 101 Switching Protocols\r\n" +
		"Upgrade: websocket\r\n" +
		"Connection: Upgrade\r\n" +
		"Sec-WebSocket-Accept: " + wsAccept(key) + "\r\n\r\n"
	_ = conn.SetWriteDeadline(time.Now().Add(wsWriteTimeout))
	if _, err := conn.Write([]byte(resp)); err != nil {
		_ = conn.Close()
		return nil, err
	}
	_ = conn.SetWriteDeadline(time.Time{})

	// brw.Reader, not a fresh one: a client may legitimately send frames straight
	// after its handshake request, and those bytes are already buffered here.
	return &wsConn{conn: conn, br: brw.Reader, maxMessage: maxMessage}, nil
}

// ── Connection ──────────────────────────────────────────────────────────────

type wsConn struct {
	conn       net.Conn
	br         *bufio.Reader
	maxMessage int
	readIdle   time.Duration

	wmu  sync.Mutex
	dead bool
}

// RemoteAddr exposes the socket peer for logging only. Behind Caddy this is the
// proxy, so it is never used to decide anything — see clientIP in main.go.
func (c *wsConn) RemoteAddr() net.Addr { return c.conn.RemoteAddr() }

// SetReadIdle bounds the silence tolerated between two frames, re-armed on each
// one. It has to live here rather than in the read loop: a pong is consumed
// inside ReadMessage and never surfaces to the caller, so a deadline the caller
// re-armed would expire on a peer that is answering pings perfectly well —
// killing exactly the healthy connections the ping was meant to prove.
func (c *wsConn) SetReadIdle(d time.Duration) { c.readIdle = d }

func (c *wsConn) Close() error {
	c.wmu.Lock()
	c.dead = true
	c.wmu.Unlock()
	return c.conn.Close()
}

// readFrame reads exactly one frame and unmasks it.
func (c *wsConn) readFrame() (fin bool, opcode byte, payload []byte, err error) {
	if c.readIdle > 0 {
		_ = c.conn.SetReadDeadline(time.Now().Add(c.readIdle))
	}
	var hdr [2]byte
	if _, err = io.ReadFull(c.br, hdr[:]); err != nil {
		return
	}
	fin = hdr[0]&0x80 != 0
	if hdr[0]&0x70 != 0 {
		// No extension was negotiated, so a reserved bit cannot mean anything.
		err = errProto{"reserved bit set"}
		return
	}
	opcode = hdr[0] & 0x0F

	if hdr[1]&0x80 == 0 {
		// RFC 6455 §5.1: client-to-server frames MUST be masked.
		err = errProto{"client frame not masked"}
		return
	}
	length := int64(hdr[1] & 0x7F)
	switch length {
	case 126:
		var ext [2]byte
		if _, err = io.ReadFull(c.br, ext[:]); err != nil {
			return
		}
		length = int64(binary.BigEndian.Uint16(ext[:]))
	case 127:
		var ext [8]byte
		if _, err = io.ReadFull(c.br, ext[:]); err != nil {
			return
		}
		v := binary.BigEndian.Uint64(ext[:])
		if v > math.MaxInt64 {
			err = errProto{"absurd frame length"}
			return
		}
		length = int64(v)
	}

	if opcode&0x08 != 0 {
		// Control frames: never fragmented, never longer than 125 bytes.
		if !fin {
			err = errProto{"fragmented control frame"}
			return
		}
		if length > 125 {
			err = errProto{"oversized control frame"}
			return
		}
	} else if length > int64(c.maxMessage) {
		// Refuse before allocating: the declared length is attacker-controlled,
		// and honouring it would let one frame header ask for gigabytes.
		err = errTooBig{}
		return
	}

	var mask [4]byte
	if _, err = io.ReadFull(c.br, mask[:]); err != nil {
		return
	}
	payload = make([]byte, length)
	if _, err = io.ReadFull(c.br, payload); err != nil {
		payload = nil
		return
	}
	for i := range payload {
		payload[i] ^= mask[i&3]
	}
	return
}

// ReadMessage returns the next complete application message, reassembling
// continuation frames. Ping is answered here and pong is swallowed, so callers
// only ever see data. A close frame is answered and reported as io.EOF.
func (c *wsConn) ReadMessage() (opcode byte, payload []byte, err error) {
	var (
		msgOp byte
		buf   []byte
		frag  bool
	)
	for {
		fin, op, data, err := c.readFrame()
		if err != nil {
			return 0, nil, err
		}
		switch op {
		case opPing:
			if err := c.write(opPong, data); err != nil {
				return 0, nil, err
			}
		case opPong:
			// Liveness only: the read deadline moving is the whole point.
		case opClose:
			code := closeNormal
			if len(data) >= 2 {
				code = int(binary.BigEndian.Uint16(data[:2]))
			}
			_ = c.WriteClose(code, "")
			return 0, nil, io.EOF
		case opText, opBinary:
			if frag {
				return 0, nil, errProto{"data frame inside a fragmented message"}
			}
			if fin {
				return op, data, nil
			}
			msgOp, buf, frag = op, data, true
		case opContinuation:
			if !frag {
				return 0, nil, errProto{"continuation without a started message"}
			}
			if len(buf)+len(data) > c.maxMessage {
				return 0, nil, errTooBig{}
			}
			buf = append(buf, data...)
			if fin {
				return msgOp, buf, nil
			}
		default:
			return 0, nil, errProto{fmt.Sprintf("unknown opcode 0x%X", op)}
		}
	}
}

// write emits one unmasked server frame. Server-to-client frames are never
// masked (RFC 6455 §5.1).
func (c *wsConn) write(opcode byte, payload []byte) error {
	c.wmu.Lock()
	defer c.wmu.Unlock()
	if c.dead {
		return errWSClosed
	}

	n := len(payload)
	var hdr [10]byte
	hdr[0] = 0x80 | opcode
	var hl int
	switch {
	case n <= 125:
		hdr[1] = byte(n)
		hl = 2
	case n <= math.MaxUint16:
		hdr[1] = 126
		binary.BigEndian.PutUint16(hdr[2:4], uint16(n))
		hl = 4
	default:
		hdr[1] = 127
		binary.BigEndian.PutUint64(hdr[2:10], uint64(n))
		hl = 10
	}

	// One Write call: two would let a short write on the header leave a
	// half-framed stream that no peer can resynchronise.
	frame := make([]byte, hl+n)
	copy(frame, hdr[:hl])
	copy(frame[hl:], payload)

	_ = c.conn.SetWriteDeadline(time.Now().Add(wsWriteTimeout))
	if _, err := c.conn.Write(frame); err != nil {
		c.dead = true
		return err
	}
	return nil
}

// WriteText sends one text message.
func (c *wsConn) WriteText(payload []byte) error { return c.write(opText, payload) }

// WritePing sends a ping; the peer's pong is what moves the read deadline.
func (c *wsConn) WritePing() error { return c.write(opPing, nil) }

// WriteClose sends a close frame. It never returns an error the caller needs to
// act on: the connection is going away either way.
func (c *wsConn) WriteClose(code int, reason string) error {
	if code == 0 {
		code = closeNormal
	}
	if len(reason) > 123 {
		reason = reason[:123] // 125-byte control payload minus the 2-byte code
	}
	body := make([]byte, 2+len(reason))
	binary.BigEndian.PutUint16(body[:2], uint16(code))
	copy(body[2:], reason)
	return c.write(opClose, body)
}

// closeCodeFor maps a read error onto the close code the peer should receive.
func closeCodeFor(err error) int {
	var proto errProto
	if errors.As(err, &proto) {
		return closeProtocolError
	}
	var big errTooBig
	if errors.As(err, &big) {
		return closeTooBig
	}
	return closeNormal
}
