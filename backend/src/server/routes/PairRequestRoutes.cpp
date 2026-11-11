/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "server/routes/PairRequestRoutes.h"

#include "server/AppSettings.h"
#include "server/AuthManager.h"
#include "server/PhonePairing.h"
#include "server/RequestGuard.h"
#include "server/RestRouter.h"
#include "server/SessionCookie.h"
#include "common/Logger.h"
#include "common/PairingCrypto.h"

#include <QJsonDocument>
#include <QJsonObject>

#include <memory>
#include <optional>
#include <utility>

namespace {

HttpResponse refusal(int status, const char* error)
{
    QJsonObject obj;
    obj["status"] = "error";
    obj["error"] = QString::fromLatin1(error);
    return HttpResponse::json(obj, status);
}

HttpResponse lockedOut(int seconds)
{
    QJsonObject obj;
    obj["status"] = "error";
    obj["error"] = "rate_limited";
    obj["lockout_seconds"] = seconds;
    return HttpResponse::json(obj, 429);
}

/// What the routes share: the server's functions, and the book of requests.
struct Context
{
    Context(AuthManager& a, PairRequestDeps d)
        : auth(a)
        , deps(std::move(d))
    {}

    AuthManager& auth;
    PairRequestDeps deps;
    PhonePairing book;
};

/// Whether @p address sits on one of this host's own links: inside the prefix of
/// one of its interfaces, an IPv6 address inside that interface's /64. An IPv4
/// address counts only when private or link-local; a public IPv6 address counts
/// on a line only, where ICE found it.
bool onThisHostsLink(const Context& c, const QString& address, bool line)
{
    QHostAddress a(AuthManager::cleanClientAddress(address));
    bool mapped = false;
    const quint32 v4 = a.toIPv4Address(&mapped);
    if (mapped) a = QHostAddress(v4);
    if (a.isNull() || a.isLoopback() || !c.deps.localSubnets) return false;
    const bool v6 = a.protocol() == QAbstractSocket::IPv6Protocol;
    if (!v6 && !AuthManager::isLanAddress(a.toString())) return false;
    if (v6 && !line && !a.isLinkLocal() && !a.isUniqueLocalUnicast()) return false;
    const QList<QPair<QHostAddress, int>> subnets = c.deps.localSubnets();
    for (const auto& net : subnets) {
        if (net.first.protocol() != a.protocol()) continue;
        if (a.isInSubnet(net.first, v6 ? 64 : net.second)) return true;
    }
    return false;
}

/// Whether a socket request names this host by a local name
/// (RequestGuard::isLocalHostName): a Host header is required.
bool underALocalName(const HttpRequest& req)
{
    const QString host = req.headers.value(QStringLiteral("host"));
    return !host.isEmpty() && RequestGuard::isLocalHostName(host);
}

/// Whether a proxy passed this socket request along, on this machine or another
/// (RequestGuard::forwardedByProxy, the list every caller shares). Its sender
/// may stand anywhere.
bool passedAlong(const HttpRequest& req)
{
    return RequestGuard::forwardedByProxy(req.headers);
}

/// Whether @p address is this machine's own: the loopback, or what
/// PairRequestDeps::thisMachine says.
bool isThisMachine(const Context& c, const QString& address)
{
    const QString clean = AuthManager::cleanClientAddress(address);
    if (QHostAddress(clean).isLoopback()) return true;
    return c.deps.thisMachine && c.deps.thisMachine(clean);
}

/// The house, where a device asks from and a paired session answers from: this
/// LAN as AuthManager::fromThisLan judges it, on one of this host's links. On a
/// line, where ICE found the caller. On a socket, under a local name too
/// (underALocalName): the public domain is how this host is reached from
/// anywhere, and only a line says where such a caller stands. A socket request
/// a proxy passed along (passedAlong) is not the house, and neither is a peer
/// that is this machine (isThisMachine), on a socket or on a line. Anywhere
/// else, a private address included, is the internet.
bool fromThisHousesLink(const Context& c, const HttpRequest& req)
{
    if (isThisMachine(c, req.clientAddress)) return false;
    if (req.viaTunnel) return onThisHostsLink(c, req.clientAddress, true);
    if (!underALocalName(req) || passedAlong(req)) return false;
    return AuthManager::fromThisLan(req) &&
           onThisHostsLink(c, AuthManager::attemptAddress(req.clientAddress, req.headers), false);
}

/// Who may answer a device's request, and with what reach.
struct Answerer
{
    std::optional<HttpResponse> refused; ///< set: turned away, nothing else is looked at
    bool admin = false;                  ///< every request; otherwise this LAN's only
};

/// Who answers a device. The administrator, with the right that mints a PIN
/// (HttpRequest::isLocal: this machine, a host-key session, or a session that
/// unlocked the admin password), from anywhere, for every request. And, from
/// one of this host's links (fromThisHousesLink: ICE's address on a line, the
/// socket's otherwise, never a header), a paired session that signs its answers
/// with its own pairing key, for the requests made from this LAN only
/// (hiddenFromAnswerer):
/// someone in the house lets in a device of the house. A guest holds a player
/// cookie, not a session, and --noadmin closes these routes before they run.
/// Anyone else is turned away before the request is looked at: a session with
/// 403, a caller with none with 401, counted.
Answerer answererOf(Context& c, const HttpRequest& req)
{
    if (req.isLocal) return {std::nullopt, true};
    const QString token = c.deps.sessionToken(req);
    if (c.auth.validateSession(token)) {
        if (fromThisHousesLink(c, req) && !c.auth.sessionPairingKey(token).isEmpty())
            return {std::nullopt, false};
        return {refusal(403, "not_allowed"), false};
    }
    c.deps.reportAuthFailure(req.clientAddress);
    return {refusal(401, "not_authenticated"), false};
}

/// A request made from outside the house (fromThisHousesLink), or from an
/// address no longer on this host's links, is the administrator's alone: to an
/// answerer of the house it is answered as any request it may not see.
bool hiddenFromAnswerer(Context& c, const Answerer& who, const QString& id)
{
    if (who.admin) return false;
    PhonePairing::Request r;
    if (c.book.find(id, c.deps.now(), &r) != PhonePairing::Lookup::Found) return false;
    return !r.fromLan || !onThisHostsLink(c, r.address, r.viaTunnel);
}

/// Whether a session's answer is refused for being temporary: a session kept
/// without "remember me" may say no, not let a device in. This machine has no
/// session.
bool temporaryAnswerer(Context& c, const HttpRequest& req)
{
    const QString token = c.deps.sessionToken(req);
    return c.auth.validateSession(token) && c.auth.sessionForToken(token).ephemeral;
}

int secondsLeft(const PhonePairing::Request& r, qint64 now)
{
    return static_cast<int>(qMax<qint64>(0, PhonePairing::TTL_SECS - (now - r.createdAt)));
}

QString stateName(PhonePairing::State state)
{
    switch (state) {
    case PhonePairing::State::Waiting: return QStringLiteral("waiting");
    case PhonePairing::State::Approved: return QStringLiteral("approved");
    case PhonePairing::State::Refused: return QStringLiteral("refused");
    case PhonePairing::State::Paired: return QStringLiteral("paired");
    }
    return QString();
}

QByteArray base64Field(const QJsonObject& body, const char* name)
{
    return QByteArray::fromBase64(body.value(QLatin1String(name)).toString().toUtf8());
}

/// The answer's signature, by the key of the session that sends it, when it
/// has one (the host machine has no session to sign with, and needs none).
/// Empty when it holds; the refusal otherwise, counted.
std::optional<HttpResponse> badSignature(Context& c, const HttpRequest& req,
                                         const QByteArray& message, const QJsonObject& body)
{
    const QString token = c.deps.sessionToken(req);
    const QByteArray signer =
        c.auth.validateSession(token) ? c.auth.sessionPairingKey(token) : QByteArray();
    if (signer.isEmpty()) return std::nullopt;
    if (PairingCrypto::verify(signer, message, base64Field(body, "sig"))) return std::nullopt;
    c.deps.reportAuthFailure(req.clientAddress);
    return refusal(403, "bad_signature");
}

/// Whether the administrator's box lets devices ask from outside this LAN.
bool boxTicked(const Context& c)
{
    return c.deps.internetRequests && c.deps.internetRequests();
}

/// Whether @p req may ask from elsewhere: while the box is ticked, and never
/// under --noadmin, which lets no device in at all.
bool mayAskFromElsewhere(const Context& c, const HttpRequest& req)
{
    return !req.adminLocked && boxTicked(c);
}

/// Whether @p req may ask, or claim what it asked: from the house always
/// (fromThisHousesLink), from anywhere while the administrator allows it. The
/// setting is read only for a caller from elsewhere.
bool mayAsk(const Context& c, const HttpRequest& req)
{
    return fromThisHousesLink(c, req) || mayAskFromElsewhere(c, req);
}

/// Requests from elsewhere live only while the box is ticked: unticked, they
/// are gone at the next request this book sees, which frees their places and
/// ends their approval. The setting is read only while there are some.
void dropFromElsewhereOnceUnticked(Context& c)
{
    if (c.book.hasFromElsewhere() && !boxTicked(c)) {
        const int dropped = c.book.dropFromElsewhere();
        Logger::info(QStringLiteral("[Pair] %1 request(s) from outside the LAN dropped: no longer "
                                    "allowed by the administrator")
                         .arg(dropped));
    }
}

} // namespace

bool writePairRequestInternet(const HttpRequest& req, const QJsonObject& body,
                              AppSettings& settings, QJsonObject& reply)
{
    const QString key = QStringLiteral("pair_request_internet");
    if (!req.isLocal || !body.contains(key)) return false;
    const bool enabled = body.value(key).toBool();
    settings.setPairRequestInternet(enabled);
    reply[key] = settings.pairRequestInternet();
    Logger::info(QStringLiteral("[Pair] QR code requests from the internet %1 by the administrator")
                     .arg(enabled ? QStringLiteral("allowed") : QStringLiteral("refused")));
    return true;
}

void registerPairRequestRoutes(RestRouter& router, AuthManager& authManager, PairRequestDeps deps)
{
    const auto c = std::make_shared<Context>(authManager, std::move(deps));

    // POST /api/auth/pair-request — a device asks to be let in, with no PIN.
    //
    // From the house (fromThisHousesLink: this LAN, on this host's links), or
    // from anywhere while the administrator allows it
    // (AppSettings::pairRequestInternet, off by default); before any session
    // like the PIN, and under the same lockout.
    // The body names the device's MW-BIND-v1 key and commits to its nonce. On a
    // line, the line has already proved a key, and the request must be for that
    // one.
    //
    // The answer carries the host's identity (the code is computed over it) and,
    // while the line is up, this machine's public link: the page the QR code
    // opens. Every caller let this far is on this LAN, came in by that link, or
    // was let in from elsewhere by the administrator's own setting. The request
    // keeps where it came from (GET says it), and those from elsewhere hold
    // MAX_FROM_ELSEWHERE of the book's places at most.
    router.post("/api/auth/pair-request", [c](const HttpRequest& req) {
        dropFromElsewhereOnceUnticked(*c);
        // The caller as the PIN counts it (AuthManager::attemptAddress): its
        // lockout, its failures and its share of the book are the PIN's.
        const QString who = AuthManager::attemptAddress(req.clientAddress, req.headers);
        const int locked = c->auth.lockoutSeconds(who);
        if (locked > 0) return lockedOut(locked);
        const bool fromLan = fromThisHousesLink(*c, req);
        if (!fromLan && !mayAskFromElsewhere(*c, req)) return refusal(403, "lan_only");

        const QJsonObject body = QJsonDocument::fromJson(req.body).object();
        const QByteArray key = base64Field(body, "public_key");
        const QByteArray commit = base64Field(body, "commit");
        if (!PairingCrypto::isValidP256Spki(key)) return refusal(400, "invalid_key");
        if (commit.size() != PhonePairing::COMMITMENT_BYTES) return refusal(400, "invalid_commit");
        if (req.viaTunnel && req.tunnelKey != key) return refusal(400, "not_the_line_key");

        const PhonePairing::OpenResult opened =
            c->book.open(key, body.value(QStringLiteral("machine_name")).toString(), commit, who,
                         AuthManager::rateLimitKey(who), req.viaTunnel, fromLan, c->deps.now());
        if (opened.outcome == PhonePairing::Open::Full) return refusal(429, "busy");
        // More requests than one address holds: its oldest made room, and the
        // excess counts like a wrong PIN, so that keeping the book full ends in
        // the PIN's lockout.
        if (opened.outcome == PhonePairing::Open::Crowded) c->auth.countPairingFailure(who);

        const QByteArray hostKey = c->deps.hostKey();
        QJsonObject obj;
        obj["status"] = "ok";
        obj["request"] = opened.request.id;
        obj["expires_in"] = static_cast<int>(PhonePairing::TTL_SECS);
        obj["host_public_key"] = QString::fromLatin1(hostKey.toBase64());
        obj["host_id"] = PairingCrypto::keyId(hostKey);
        const QString url = c->deps.rendezvousUrl ? c->deps.rendezvousUrl() : QString();
        if (!url.isEmpty()) obj["rendezvous_url"] = url;
        Logger::info(QStringLiteral("[Pair] '%1' asks to pair from %2")
                         .arg(opened.request.machineName, AuthManager::cleanClientAddress(who)));
        return HttpResponse::json(obj);
    });

    // POST /api/auth/pair-request/:id/claim — the device asks where its request
    // stands, signed by its key; the first claim reveals its nonce. Once the
    // administrator has approved, this is where the session is made: the one a
    // PIN would have opened, bound to the device's key and to the session that
    // approved. A proof that fails counts like a wrong PIN. From where it may
    // ask, as a request: from elsewhere only while the setting allows it.
    router.post("/api/auth/pair-request/:id/claim", [c](const HttpRequest& req) {
        dropFromElsewhereOnceUnticked(*c);
        const QString who = AuthManager::attemptAddress(req.clientAddress, req.headers);
        const int locked = c->auth.lockoutSeconds(who);
        if (locked > 0) return lockedOut(locked);

        // Unknown, and not counted: ids are 96 random bits and every claim is
        // signed, so this is a device whose request a restart, the purge or a
        // newer request of its own forgot. It shows a new code.
        const QString id = req.pathParams.value(QStringLiteral("id"));
        if (!PhonePairing::isRequestId(id)) return refusal(404, "unknown_request");
        // Not counted either, and the request is left as it is: the device goes
        // back to its PIN.
        if (!mayAsk(*c, req)) return refusal(403, "lan_only");

        const QJsonObject body = QJsonDocument::fromJson(req.body).object();
        const QByteArray hostKey = c->deps.hostKey();
        const QString hostId = PairingCrypto::keyId(hostKey);
        const qint64 now = c->deps.now();
        const PhonePairing::ClaimResult r =
            c->book.claim(id, base64Field(body, "nonce"), base64Field(body, "sig"), hostId, hostKey,
                          req.viaTunnel, req.tunnelKey, now);
        switch (r.outcome) {
        case PhonePairing::Claim::Waiting: {
            QJsonObject obj;
            obj["state"] = "waiting";
            obj["expires_in"] = secondsLeft(r.request, now);
            return HttpResponse::json(obj);
        }
        case PhonePairing::Claim::Expired: return refusal(410, "expired");
        case PhonePairing::Claim::Refused: return refusal(403, "refused");
        case PhonePairing::Claim::Unknown: return refusal(404, "unknown_request");
        case PhonePairing::Claim::BadProof:
            c->auth.countPairingFailure(who);
            c->deps.reportAuthFailure(req.clientAddress);
            Logger::warning(QStringLiteral("[Pair] A claim without the device's proof from %1")
                                .arg(AuthManager::cleanClientAddress(who)));
            return refusal(403, "bad_proof");
        case PhonePairing::Claim::Paired: break;
        }

        const PhonePairing::Request& p = r.request;
        // The session that approved has been revoked since, or the one made at
        // an earlier claim has: the approval went with it.
        if ((!p.approver.isEmpty() && !c->auth.hasLiveSession(p.approver)) ||
            (!p.sessionId.isEmpty() && !c->auth.hasLiveSession(p.sessionId))) {
            c->book.drop(id);
            return refusal(403, "refused");
        }
        // Paired already, and claimed again: the answer was lost on the way. A
        // new session replaces that one, so a request still opens one session.
        if (!p.sessionId.isEmpty()) c->auth.destroySession(p.sessionId);

        const QString token = c->auth.createSession(req.clientAddress, p.machineName);
        if (!c->auth.bindSessionKey(token, p.deviceKey)) {
            c->auth.destroySession(c->auth.sessionIdForToken(token));
            return HttpResponse::error(500, "Internal error");
        }
        if (!p.approver.isEmpty()) c->auth.setSessionApprover(token, p.approver);
        c->book.markPaired(id, c->auth.sessionIdForToken(token), now);
        if (c->deps.locate) c->deps.locate(token, req.clientAddress);
        Logger::info(QStringLiteral("[Pair] '%1' paired by its QR code, approved on the page")
                         .arg(p.machineName));

        QJsonObject obj;
        obj["state"] = "paired";
        obj["status"] = "ok";
        obj["remembered"] = true;
        obj["pairing_key"] = "bound";
        obj["host_public_key"] = QString::fromLatin1(hostKey.toBase64());
        obj["host_id"] = hostId;
        HttpResponse resp = HttpResponse::json(obj);
        resp.headers["Set-Cookie"] = sessionCookie(token, true);
        return resp;
    });

    // GET /api/auth/pair-request/:id — the request behind a QR code, for the
    // page to name the device and ask for its code. The administrator's, or a
    // paired session's on this LAN for a request made from this LAN (answererOf).
    // The code itself is never sent: it is read from the device's screen. Where
    // the request came from is: this LAN or elsewhere, and the address it came
    // from. For a request from elsewhere, also the city of that address, while
    // the administrator lets the server look up where an address is
    // (`session_location_enabled`): the lookup starts here, at the
    // administrator's first look, never at the device's request, and the city
    // comes in a later answer once it is known.
    router.get("/api/auth/pair-request/:id", [c](const HttpRequest& req) {
        dropFromElsewhereOnceUnticked(*c);
        const Answerer who = answererOf(*c, req);
        if (who.refused) return *who.refused;
        const QString id = req.pathParams.value(QStringLiteral("id"));
        if (!PhonePairing::isRequestId(id)) return refusal(404, "unknown_request");
        if (hiddenFromAnswerer(*c, who, id)) return refusal(403, "not_allowed");

        PhonePairing::Request r;
        const qint64 now = c->deps.now();
        switch (c->book.find(id, now, &r)) {
        case PhonePairing::Lookup::Unknown: return refusal(404, "unknown_request");
        case PhonePairing::Lookup::Expired: return refusal(410, "expired");
        case PhonePairing::Lookup::Found: break;
        }
        QJsonObject obj;
        obj["machine_name"] = r.machineName;
        obj["state"] = stateName(r.state);
        // Whether the device has sent its half of the code, a moment after it
        // asked: until then no code can be approved.
        obj["ready"] = !r.code.isEmpty();
        obj["origin"] = r.fromLan ? QStringLiteral("lan") : QStringLiteral("internet");
        const QString address = AuthManager::cleanClientAddress(r.address);
        obj["address"] = address;
        if (!r.fromLan && c->deps.cityOf && c->deps.locationAllowed && c->deps.locationAllowed()) {
            const QString city = c->deps.cityOf(address);
            if (!city.isEmpty()) obj["city"] = city;
        }
        obj["host_id"] = PairingCrypto::keyId(c->deps.hostKey());
        obj["expires_in"] = secondsLeft(r, now);
        return HttpResponse::json(obj);
    });

    // POST /api/auth/pair-request/:id/approve — the administrator, or a paired
    // session on this LAN for a request from this LAN (answererOf), says yes,
    // with the code typed from the device's screen. A session signs it with its
    // own pairing key, when it has one: the host machine itself has no session
    // to sign with, and needs none. A temporary session does not answer yes.
    // The session that approves is the new one's approver, and takes it along
    // when it is revoked.
    router.post("/api/auth/pair-request/:id/approve", [c](const HttpRequest& req) {
        dropFromElsewhereOnceUnticked(*c);
        const Answerer who = answererOf(*c, req);
        if (who.refused) return *who.refused;
        const QString id = req.pathParams.value(QStringLiteral("id"));
        if (!PhonePairing::isRequestId(id)) return refusal(404, "unknown_request");
        if (hiddenFromAnswerer(*c, who, id)) return refusal(403, "not_allowed");
        if (temporaryAnswerer(*c, req)) return refusal(403, "temporary_session");

        const QJsonObject body = QJsonDocument::fromJson(req.body).object();
        const QString code = body.value(QStringLiteral("code")).toString();
        const QString hostId = PairingCrypto::keyId(c->deps.hostKey());
        if (auto no = badSignature(*c, req, PhonePairing::approveMessage(hostId, id, code), body))
            return *no;
        const QString token = c->deps.sessionToken(req);
        const bool signedIn = c->auth.validateSession(token);

        const QString approver = signedIn ? c->auth.sessionIdForToken(token) : QString();
        const PhonePairing::AnswerResult a = c->book.approve(id, code, approver, c->deps.now());
        switch (a.outcome) {
        case PhonePairing::Answer::Done: break;
        case PhonePairing::Answer::Unknown: return refusal(404, "unknown_request");
        case PhonePairing::Answer::Expired: return refusal(410, "expired");
        case PhonePairing::Answer::NotReady: return refusal(409, "not_ready");
        case PhonePairing::Answer::Decided: return refusal(409, "decided");
        case PhonePairing::Answer::CodeMismatch: {
            QJsonObject obj;
            obj["status"] = "error";
            obj["error"] = "code_mismatch";
            obj["tries_left"] = a.triesLeft;
            return HttpResponse::json(obj, 409);
        }
        case PhonePairing::Answer::TooManyTries:
            // Refused, as if the administrator had said no: the device is told
            // at its next claim, and the refusal counts against its address.
            c->auth.countPairingFailure(a.address);
            return refusal(409, "too_many_tries");
        }

        const SessionInfo by = c->auth.sessionForToken(token);
        Logger::info(
            QStringLiteral("[Pair] A device's request approved by '%1'")
                .arg(by.token.isEmpty() ? QStringLiteral("this machine") : by.machineName));
        QJsonObject obj;
        obj["status"] = "ok";
        return HttpResponse::json(obj);
    });

    // POST /api/auth/pair-request/:id/refuse — the administrator, or a paired
    // session on this LAN for a request from this LAN, says no, signed as a yes
    // is. The request is spent at the device's next claim, and the refusal counts
    // against the address it came from like a wrong PIN: a device turned away
    // does not get to ask again and again.
    router.post("/api/auth/pair-request/:id/refuse", [c](const HttpRequest& req) {
        dropFromElsewhereOnceUnticked(*c);
        const Answerer who = answererOf(*c, req);
        if (who.refused) return *who.refused;
        const QString id = req.pathParams.value(QStringLiteral("id"));
        if (!PhonePairing::isRequestId(id)) return refusal(404, "unknown_request");
        if (hiddenFromAnswerer(*c, who, id)) return refusal(403, "not_allowed");
        const QJsonObject body = QJsonDocument::fromJson(req.body).object();
        const QString hostId = PairingCrypto::keyId(c->deps.hostKey());
        if (auto no = badSignature(*c, req, PhonePairing::refuseMessage(hostId, id), body))
            return *no;

        const PhonePairing::AnswerResult a = c->book.refuse(id, c->deps.now());
        switch (a.outcome) {
        case PhonePairing::Answer::Done: break;
        case PhonePairing::Answer::Unknown: return refusal(404, "unknown_request");
        case PhonePairing::Answer::Expired: return refusal(410, "expired");
        case PhonePairing::Answer::NotReady:
        case PhonePairing::Answer::CodeMismatch:
        case PhonePairing::Answer::TooManyTries:
        case PhonePairing::Answer::Decided: return refusal(409, "decided");
        }
        c->auth.countPairingFailure(a.address);
        Logger::info(QStringLiteral("[Pair] A device's request refused on the page"));
        QJsonObject obj;
        obj["status"] = "ok";
        return HttpResponse::json(obj);
    });
}
