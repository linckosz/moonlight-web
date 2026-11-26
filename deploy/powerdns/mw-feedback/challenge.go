package main

// The challenge: a minimum delay and a little work, checked here, not trusted
// from the client.
//
// GET /v1/feedback/challenge hands out a token signed with a key that exists
// only in this process's memory (drawn at startup, never in the environment, so
// a restart simply invalidates the tokens in flight). To send, the instance must
// present that token no sooner than challengeMinAge after it was issued and no
// later than challengeMaxAge, together with a counter whose SHA-256 over
// "token:counter" starts with powBits zero bits — about a second of one core at
// 20 bits. Each token is accepted once: its nonce is remembered until it would
// have expired anyway, in a set bounded in size.
//
// Nothing is stored when a token is issued, so asking for tokens costs this
// process nothing but the signature; Caddy rate-limits that path as well.

import (
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/binary"
	"errors"
	"math/bits"
	"sync"
	"time"
)

const (
	challengeVersion = 1
	challengeMinAge  = 5 * time.Second
	challengeMaxAge  = 10 * time.Minute
	nonceLen         = 16
	// version(1) | issued, unix seconds (8) | nonce (16) | HMAC-SHA256 (32)
	challengeRawLen  = 1 + 8 + nonceLen + sha256.Size
	maxPowLen       = 32
)

var (
	errChallengeInvalid = errors.New("challenge_invalid")
	errChallengeEarly   = errors.New("challenge_early")
	errChallengeExpired = errors.New("challenge_expired")
	errChallengeReplay  = errors.New("challenge_replayed")
	errPowInvalid       = errors.New("pow_invalid")
	errNoncesFull       = errors.New("busy")
)

type challenger struct {
	key       []byte
	bits      int
	maxNonces int

	mu   sync.Mutex
	used map[[nonceLen]byte]time.Time // nonce → when it stops mattering
}

func newChallenger(powBits, maxNonces int) (*challenger, error) {
	key := make([]byte, 32)
	if _, err := rand.Read(key); err != nil {
		return nil, err
	}
	return &challenger{key: key, bits: powBits, maxNonces: maxNonces,
		used: make(map[[nonceLen]byte]time.Time)}, nil
}

func (c *challenger) mac(msg []byte) []byte {
	m := hmac.New(sha256.New, c.key)
	m.Write(msg)
	return m.Sum(nil)
}

// issue returns a fresh token, base64url without padding.
func (c *challenger) issue(now time.Time) (string, error) {
	raw := make([]byte, challengeRawLen)
	raw[0] = challengeVersion
	binary.BigEndian.PutUint64(raw[1:9], uint64(now.Unix()))
	if _, err := rand.Read(raw[9 : 9+nonceLen]); err != nil {
		return "", err
	}
	copy(raw[9+nonceLen:], c.mac(raw[:9+nonceLen]))
	return base64.RawURLEncoding.EncodeToString(raw), nil
}

// verify checks the token and the work, then spends the token. The order is
// deliberate: nothing is remembered for a token that fails any check, so only
// a request that paid for its work can take a place in the bounded set.
func (c *challenger) verify(token, pow string, now time.Time) error {
	if len(token) != base64.RawURLEncoding.EncodedLen(challengeRawLen) {
		return errChallengeInvalid
	}
	raw, err := base64.RawURLEncoding.DecodeString(token)
	if err != nil || len(raw) != challengeRawLen || raw[0] != challengeVersion {
		return errChallengeInvalid
	}
	if !hmac.Equal(raw[9+nonceLen:], c.mac(raw[:9+nonceLen])) { // constant time
		return errChallengeInvalid
	}
	issued := time.Unix(int64(binary.BigEndian.Uint64(raw[1:9])), 0)
	age := now.Sub(issued)
	if age < challengeMinAge {
		return errChallengeEarly
	}
	if age > challengeMaxAge {
		return errChallengeExpired
	}
	if !validPow(pow) || leadingZeroBits(token, pow) < c.bits {
		return errPowInvalid
	}

	var nonce [nonceLen]byte
	copy(nonce[:], raw[9:9+nonceLen])
	c.mu.Lock()
	defer c.mu.Unlock()
	if _, seen := c.used[nonce]; seen {
		return errChallengeReplay
	}
	if len(c.used) >= c.maxNonces {
		for n, until := range c.used {
			if !now.Before(until) {
				delete(c.used, n)
			}
		}
		if len(c.used) >= c.maxNonces {
			return errNoncesFull // fail closed rather than forget a nonce
		}
	}
	c.used[nonce] = issued.Add(challengeMaxAge)
	return nil
}

// validPow: the counter is short and plain, so it can never carry anything.
func validPow(pow string) bool {
	if pow == "" || len(pow) > maxPowLen {
		return false
	}
	for i := 0; i < len(pow); i++ {
		ch := pow[i]
		if !(ch >= '0' && ch <= '9' || ch >= 'a' && ch <= 'z' || ch >= 'A' && ch <= 'Z') {
			return false
		}
	}
	return true
}

// leadingZeroBits of SHA-256("token:pow") — what the instance searches for.
func leadingZeroBits(token, pow string) int {
	sum := sha256.Sum256([]byte(token + ":" + pow))
	n := 0
	for _, b := range sum {
		if b == 0 {
			n += 8
			continue
		}
		return n + bits.LeadingZeros8(b)
	}
	return n
}
