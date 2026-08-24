#!/bin/sh
# Acceptance check for a DEPLOYED rendezvous server.
#
#   ./smoke.sh                                  # https://stream.moonlightweb.top
#   ./smoke.sh https://stream.example.top
#   ./smoke.sh https://stream.mw.local:8443 -k  # lab, self-signed
#
# The unit tests cover the logic; this covers the deployment — DNS, TLS, the
# Caddy routing in front, and the container actually being up. It is safe to run
# against production: it claims one identifier drawn at random, then releases it,
# and touches nothing else.
#
# Exits non-zero on the first failed expectation.
set -eu

BASE="${1:-https://stream.moonlightweb.top}"
shift 2>/dev/null || true
CURL_EXTRA="$*"

# shellcheck disable=SC2086
curl_() { curl -s --max-time 15 $CURL_EXTRA "$@"; }

pass=0
fail=0

check() { # check <label> <expected> <actual>
    if [ "$2" = "$3" ]; then
        printf '  ok    %-46s %s\n' "$1" "$3"
        pass=$((pass + 1))
    else
        printf '  FAIL  %-46s got %s, want %s\n' "$1" "$3" "$2"
        fail=$((fail + 1))
    fi
}

contains() { # contains <label> <needle> <haystack>
    case "$3" in
    *"$2"*)
        printf '  ok    %-46s %s\n' "$1" "$2"
        pass=$((pass + 1))
        ;;
    *)
        printf '  FAIL  %-46s %s\n' "$1" "$3"
        fail=$((fail + 1))
        ;;
    esac
}

status() { curl_ -o /dev/null -w '%{http_code}' "$@"; }

# A fresh identifier per run, so repeated runs never collide with each other and
# a crashed run leaves nothing that blocks the next one.
alphabet=0123456789abcdefghjkmnpqrstvwxyz
ID=""
i=0
while [ "$i" -lt 26 ]; do
    n=$(( $(od -An -N2 -tu2 < /dev/urandom | tr -d ' ') % 32 + 1 ))
    ID="$ID$(printf %s "$alphabet" | cut -c"$n")"
    i=$((i + 1))
done
# Two credentials: one is us, the other stands in for a stranger.
TOKEN=$(od -An -N32 -tx1 < /dev/urandom | tr -d ' \n')
OTHER=$(od -An -N32 -tx1 < /dev/urandom | tr -d ' \n')

echo "rendezvous smoke — $BASE"
echo "identifier $ID"
echo

echo "entry page (no redirects — the address people see is the one they stay on)"
check "GET /" 200 "$(status "$BASE/")"
check "GET /{id}" 200 "$(status "$BASE/$ID")"
check "GET /bootstrap.js" 200 "$(status "$BASE/bootstrap.js")"
check "GET /CNAME is hidden" 404 "$(status "$BASE/CNAME")"
check "GET / does not redirect" "" "$(curl_ -o /dev/null -w '%{redirect_url}' "$BASE/")"
contains "entry page is the bootstrap" "MoonlightWeb" "$(curl_ "$BASE/$ID")"

echo
echo "claim lifecycle"
body=$(curl_ -X POST "$BASE/v1/claim" -H "X-MW-Owner: $TOKEN" -d "{\"id\":\"$ID\"}")
contains "first claim is granted" '"claimed":true' "$body"

# The spelling a person would write down must resolve to the same claim. If the
# folding rules ever drift from the host's, this is the check that notices.
human=$(printf %s "$ID" | sed 's/\(.....\)\(.....\)\(.....\)\(.....\)\(......\)/\1-\2-\3-\4-\5/' | tr 'a-z' 'A-Z')
body=$(curl_ -X POST "$BASE/v1/claim" -H "X-MW-Owner: $TOKEN" -d "{\"id\":\"$human\"}")
contains "re-claim in hyphenated upper case" '"claimed":false' "$body"

body=$(curl_ -X POST "$BASE/v1/claim" -H "X-MW-Owner: $OTHER" -d "{\"id\":\"$ID\"}")
contains "a stranger cannot take it" '"code":"id_taken"' "$body"

body=$(curl_ -X POST "$BASE/v1/claim" -H "X-MW-Owner: $TOKEN" -d '{"id":"0123456789abcdefghjkmnpqrs"}')
contains "one line per owner" '"code":"owner_has_other"' "$body"

check "short credential refused" 401 \
    "$(status -X POST "$BASE/v1/claim" -H 'X-MW-Owner: short' -d "{\"id\":\"$ID\"}")"
check "malformed id refused" 400 \
    "$(status -X POST "$BASE/v1/claim" -H "X-MW-Owner: $TOKEN" -d '{"id":"nope"}')"

echo
echo "held line and signalling sockets"
# A browser sends its WebSocket handshake over HTTP/1.1 whatever else it speaks,
# so --http1.1 here is the browser's behaviour, not a workaround.
ws() {
    curl_ -i -N --http1.1 \
        -H 'Connection: Upgrade' -H 'Upgrade: websocket' \
        -H 'Sec-WebSocket-Version: 13' -H 'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==' \
        "$@" 2>/dev/null | head -1 | tr -d '\r'
}
contains "host line accepts the owner" "101" \
    "$(ws -H "X-MW-Owner: $TOKEN" "$BASE/v1/host?id=$ID")"
check "host line rejects a stranger" 401 \
    "$(status -H "X-MW-Owner: $OTHER" "$BASE/v1/host?id=$ID")"
contains "browser socket upgrades" "101" "$(ws "$BASE/v1/peer?id=$ID")"

# Enumeration defence: an identifier nobody ever claimed must be answered
# exactly like a claimed one whose host is offline. A difference here would make
# this endpoint an oracle for which identifiers exist.
unknown=$(ws "$BASE/v1/peer?id=0123456789abcdefghjkmnpqrs")
claimed_offline=$(ws "$BASE/v1/peer?id=$ID")
check "unknown and offline are indistinguishable" "$claimed_offline" "$unknown"

echo
echo "cleanup"
check "release returns the identifier" 200 \
    "$(status -X DELETE "$BASE/v1/claim" -H "X-MW-Owner: $TOKEN" -d "{\"id\":\"$ID\"}")"
check "released id is claimable again" 200 \
    "$(status -X POST "$BASE/v1/claim" -H "X-MW-Owner: $OTHER" -d "{\"id\":\"$ID\"}")"
curl_ -o /dev/null -X DELETE "$BASE/v1/claim" -H "X-MW-Owner: $OTHER" -d "{\"id\":\"$ID\"}"

echo
echo "──────────────────────────────────────────"
printf '%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ] || exit 1
