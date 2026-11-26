package main

// What reaches Discord, and how nothing in it can do more than be read.
//
//   - allowed_mentions {parse: []} on every message: no @everyone, @here, user
//     or role can ping, whatever the text says;
//   - flags 4 (SUPPRESS_EMBEDS): no link unfurls, so a link cannot fetch
//     anything or dress itself up as a preview;
//   - everything the person wrote — message, e-mail, version line — sits inside
//     one code block, where Discord renders no markdown, no masked link and no
//     mention. The block cannot be closed from inside: every backtick of the
//     text becomes U+02CB, and the e-mail and version are restricted to
//     characters that contain none;
//   - control characters and invisible format characters (bidi overrides and
//     isolates among them) are removed, so the moderators read what was sent;
//   - the whole content, fixed text included, stays within Discord's 2000
//     characters: maxMessageUnits is what is left once the longest possible
//     e-mail and version line are counted (TestContentWorstCaseFitsDiscord).
//
// Discord counts the length in UTF-16 code units, as JavaScript does — never
// fewer than the code points — so that is the unit here too.

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"net/url"
	"regexp"
	"strings"
	"time"
	"unicode"
)

const (
	discordMaxContent = 2000
	// The UI counter shows this number: it is the real limit.
	maxMessageUnits   = 1600
	minMessageRunes   = 10
	maxEmailLen       = 254
	maxVersionLen     = 32
	discordTimeout    = 10 * time.Second
	flagSuppressEmb   = 4
	contentHead       = "**New feedback**\n```text\n"
	contentTail       = "\n```"
)

var (
	// Deliberately narrower than RFC 5322: ASCII only, no quoting, no comments.
	// What it refuses is rare in practice, and none of what it accepts can
	// close a code block, mention anyone or reorder text.
	emailRe   = regexp.MustCompile(`^[A-Za-z0-9._%+-]{1,64}@[A-Za-z0-9-]{1,63}(\.[A-Za-z0-9-]{1,63})*\.[A-Za-z]{2,63}$`)
	versionRe = regexp.MustCompile(`^[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}(-[0-9A-Za-z.]{1,20})?$`)
	editions  = map[string]bool{"release": true, "dev": true}
	osFamily  = map[string]bool{"windows": true, "linux": true, "macos": true}

	errBadWebhook = errors.New("MW_FEEDBACK_WEBHOOK is not a Discord webhook URL")
)

// utf16Len is the length Discord applies its limit to.
func utf16Len(s string) int {
	n := 0
	for _, r := range s {
		if r >= 0x10000 {
			n += 2 // a surrogate pair
		} else {
			n++
		}
	}
	return n
}

// cleanMessage returns the text as the moderators will read it.
func cleanMessage(s string) string {
	s = strings.ReplaceAll(s, "\r\n", "\n")
	var b strings.Builder
	b.Grow(len(s))
	for _, r := range s {
		switch {
		case r == '\r' || r == ' ' || r == ' ':
			b.WriteByte('\n')
		case r == '\n' || r == '\t':
			b.WriteRune(r)
		case r == '`':
			b.WriteRune('ˋ') // same width, cannot open or close a block
		case r == '‍':
			b.WriteRune(r) // ZWJ: joins emoji, reorders nothing
		case unicode.IsControl(r), unicode.Is(unicode.Cf, r):
			// dropped: C0/C1 controls, bidi overrides/isolates, ZWSP, BOM…
		case r == unicode.ReplacementChar:
			// what invalid UTF-8 decoded to; nothing to read there
		default:
			b.WriteRune(r)
		}
	}
	return strings.TrimSpace(b.String())
}

type feedback struct {
	message string
	email   string // "" = no reply wanted
	version string // each of the three may be "", dropped when not on its list
	edition string
	os      string
}

// sentFrom is the version line, "" when nothing was attached.
func (f feedback) sentFrom() string {
	var parts []string
	for _, p := range []string{f.version, f.edition, f.os} {
		if p != "" {
			parts = append(parts, p)
		}
	}
	if len(parts) == 0 {
		return ""
	}
	return "Sent from: " + strings.Join(parts, ", ")
}

func (f feedback) content() string {
	var b strings.Builder
	b.WriteString(contentHead)
	if f.email != "" {
		b.WriteString("Email: " + f.email + "\n")
	}
	if line := f.sentFrom(); line != "" {
		b.WriteString(line + "\n")
	}
	if f.email != "" || f.sentFrom() != "" {
		b.WriteString("\n")
	}
	b.WriteString(f.message)
	b.WriteString(contentTail)
	return b.String()
}

type discordPayload struct {
	Content         string          `json:"content"`
	Username        string          `json:"username"`
	AllowedMentions allowedMentions `json:"allowed_mentions"`
	Flags           int             `json:"flags"`
}

// Parse is a slice, never nil, so it always serialises as [] — an absent or
// null parse list would mean "Discord's defaults", which ping.
type allowedMentions struct {
	Parse []string `json:"parse"`
}

func (f feedback) payload() ([]byte, error) {
	return json.Marshal(discordPayload{
		Content:         f.content(),
		Username:        "MoonlightWeb feedback",
		AllowedMentions: allowedMentions{Parse: []string{}},
		Flags:           flagSuppressEmb,
	})
}

// parseWebhook accepts https://discord.com/api/webhooks/… and nothing else,
// keeping any query it already has (thread_id) and adding wait=true so that
// Discord's answer says whether the message was really posted.
func parseWebhook(raw string) (*url.URL, error) {
	raw = strings.TrimSpace(raw)
	if !strings.HasPrefix(raw, "https://discord.com/api/webhooks/") {
		return nil, errBadWebhook
	}
	u, err := url.Parse(raw)
	if err != nil || u.Scheme != "https" || u.Host != "discord.com" ||
		u.User != nil || u.Fragment != "" ||
		!strings.HasPrefix(u.Path, "/api/webhooks/") ||
		len(strings.Split(strings.TrimPrefix(u.Path, "/api/webhooks/"), "/")) != 2 {
		return nil, errBadWebhook
	}
	q := u.Query()
	q.Set("wait", "true")
	u.RawQuery = q.Encode()
	return u, nil
}

// newDiscordClient follows no redirect: the only place a message may go is
// the URL that was configured.
func newDiscordClient() *http.Client {
	return &http.Client{
		Timeout: discordTimeout,
		CheckRedirect: func(*http.Request, []*http.Request) error {
			return errors.New("redirect refused")
		},
	}
}

// post returns Discord's status code, or 0 when there was no answer. The error
// itself is never returned: a *url.Error carries the full webhook URL, token
// included, and the only safe way to keep it out of every log is to drop it here.
func post(ctx context.Context, c *http.Client, hook *url.URL, body []byte) int {
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, hook.String(), bytes.NewReader(body))
	if err != nil {
		return 0
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("User-Agent", "mw-feedback")
	resp, err := c.Do(req)
	if err != nil {
		return 0
	}
	defer resp.Body.Close()
	return resp.StatusCode
}
