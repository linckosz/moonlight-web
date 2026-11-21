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

#include "LanHttpRoutes.h"

#include "common/Logger.h"
#include "common/PairingCrypto.h"
#include "server/AuthManager.h"
#include "server/HouseLink.h"
#include "server/LanHttp.h"
#include "server/LanHttpPairing.h"
#include "server/PhonePairing.h"
#include "server/RestRouter.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <memory>
#include <optional>

namespace {

struct Context
{
    AuthManager& auth;
    LanHttpPairing& book;
    LanHttpDeps deps;
};

HttpResponse refusal(int status, const char* error)
{
    QJsonObject obj;
    obj["error"] = QString::fromLatin1(error);
    return HttpResponse::json(obj, status);
}

/// The 404 every route of the other side answers: as if it were not there.
HttpResponse notHere()
{
    return HttpResponse::error(404, QStringLiteral("Not Found"));
}

/// The approval page the QR code opens: under this machine's public link while
/// its line is up, as MW-PAIR-v1's; otherwise on this LAN, over HTTPS, under
/// the very name the TV reached this machine by.
QString approvalUrl(const Context& c, const HttpRequest& req, const QString& id)
{
    const QString path = QStringLiteral("/tv-pair-") + id;
    QString base = c.deps.rendezvousUrl ? c.deps.rendezvousUrl() : QString();
    while (base.endsWith(QLatin1Char('/')))
        base.chop(1);
    if (!base.isEmpty()) return base + path;
    const quint16 port = c.deps.httpsPort ? c.deps.httpsPort() : 0;
    const QString host = req.lanHttpHost.section(QLatin1Char(':'), 0, 0);
    if (port == 0 || host.isEmpty()) return {};
    return port == 443 ? QStringLiteral("https://%1%2").arg(host, path)
                       : QStringLiteral("https://%1:%2%3").arg(host).arg(port).arg(path);
}

/// The one answer a paired device gets for anything it may not do here, a
/// request it may not see included: the same for an unknown id, one that ran
/// out and a session that may not let a TV in (S4, R2). The page shows one
/// text for it.
HttpResponse notAllowedShort()
{
    return refusal(403, "not_allowed_short");
}

/// Who answers a TV, and with what reach.
struct Approver
{
    std::optional<HttpResponse> refused; ///< set: turned away, nothing else is looked at
    bool admin = false;                  ///< the administrator: for remember_me_days
    QString token;                       ///< otherwise: the paired device's raw cookie
    /// A device that could let the TV in, but was paired before v0.4.4
    /// (AuthManager::ShortApproval::ReappairNeeded). It sees the request as
    /// any other would; its signed answer is met by reappairNeeded() alone.
    bool reappair = false;
};

/// What a device paired before v0.4.4 is told once its answer is signed
/// (TVNAO1 A): pair again. The request is never looked at for it, so no try
/// is spent and the answer is the same whether the id exists or not.
HttpResponse reappairNeeded()
{
    return refusal(403, "reappair_needed");
}

/// The administrator's side, as before: never on the TV's path, and with the
/// right that mints a PIN (isLocal) for remember_me_days. Then a paired device
/// of the house for a short while (TVNONADM, security review S1): a request
/// from the house (HouseLink, as for MW-PAIR-v1), neither on the local channel
/// nor in a booth, from a session that may (AuthManager::mayLetInShortly).
/// Without a live HTTPS session at all, turned away and counted as before;
/// with one that may not, turned away with notAllowedShort() and not counted,
/// since that is only a way it may not take. A device that may in all but
/// its origin, unknown since it was paired before v0.4.4, goes on with
/// `reappair` set (TVNAO1 A).
Approver approverOf(Context& c, const HttpRequest& req)
{
    if (req.lanHttp) return {notHere()};
    if (req.isLocal) return {std::nullopt, true, {}};
    const QString token = c.deps.sessionToken ? c.deps.sessionToken(req) : QString();
    if (!c.auth.validateSession(token)) {
        c.deps.reportAuthFailure(req.clientAddress);
        return {refusal(403, "not_allowed")};
    }
    const HouseLink::Links links{c.deps.localSubnets, c.deps.thisMachine};
    if (req.localChannel || req.adminLocked || req.boothShare || req.boothPin ||
        !HouseLink::fromThisHousesLink(links, req))
        return {notAllowedShort()};
    switch (c.auth.shortApprovalState(token)) {
    case AuthManager::ShortApproval::Allowed: return {std::nullopt, false, token};
    case AuthManager::ShortApproval::ReappairNeeded: return {std::nullopt, false, token, true};
    case AuthManager::ShortApproval::Refused: break;
    }
    return {notAllowedShort()};
}

/// This host's key id, what a paired device signs with the request (R1).
/// Empty without a host key: no device may then.
QString hostIdOf(const Context& c)
{
    const QByteArray key = c.deps.hostKey ? c.deps.hostKey() : QByteArray();
    return key.isEmpty() ? QString() : PairingCrypto::keyId(key);
}

/// Whether @p message carries the signature of the paired device's own key:
/// the one bound to its session, never one sent with the request (R1). A
/// signature that does not hold is counted.
bool signedByTheDevice(Context& c, const HttpRequest& req, const QString& token,
                       const QByteArray& message, const QJsonObject& body)
{
    const QByteArray key = c.auth.sessionPairingKey(token);
    const QByteArray sig =
        QByteArray::fromBase64(body.value(QStringLiteral("sig")).toString().toUtf8());
    if (!key.isEmpty() && PairingCrypto::verify(key, message, sig)) return true;
    c.deps.reportAuthFailure(req.clientAddress);
    return false;
}

/// The longest code a device may send: anything longer is not looked at, and
/// spends none of a request's tries (R1b).
constexpr int MAX_SENT_CODE_BYTES = 16;

} // namespace

void registerLanHttpRoutes(RestRouter& router, AuthManager& authManager, LanHttpPairing& book,
                           LanHttpDeps deps)
{
    auto c = std::make_shared<Context>(Context{authManager, book, std::move(deps)});

    // GET /api/lan-http/status — whether this TV is signed in. A good cookie is
    // given its time again (sliding), up to the day it must be approved anew.
    router.get("/api/lan-http/status", [c](const HttpRequest& req) {
        if (!req.lanHttp) return notHere();
        const QString token = LanHttp::tokenFromCookieHeader(req.headers.value("cookie"));
        QJsonObject obj;
        if (c->auth.validateLanHttpSession(token, req.clientAddress)) {
            c->auth.touchLanHttpSession(token, req.clientAddress);
            obj["signed_in"] = true;
            HttpResponse resp = HttpResponse::json(obj);
            resp.headers["Set-Cookie"] = LanHttp::cookie(token, c->auth.lanHttpCookieMaxAge(token));
            return resp;
        }
        obj["signed_in"] = false;
        HttpResponse resp = HttpResponse::json(obj);
        if (!token.isEmpty()) resp.headers["Set-Cookie"] = LanHttp::clearedCookie();
        return resp;
    });

    // POST /api/lan-http/pair — the TV asks to be let in. What it gets: the id
    // of its request, the code to show, the claim to ask with, and where the
    // QR code leads. A page reloaded asks again and replaces its request.
    router.post("/api/lan-http/pair", [c](const HttpRequest& req) {
        if (!req.lanHttp) return notHere();
        const QString address = LanHttp::normalizePeer(req.clientAddress);
        if (address.isEmpty()) return notHere();
        // Its last request's codes all wrong (countPairingFailure): it waits,
        // for the time the PIN's lockout lasts, 10 minutes at most. A paired
        // device can keep a TV waiting that long, never for good (R3).
        if (c->auth.lockoutSeconds(address) > 0) return refusal(429, "busy");
        const LanHttpPairing::OpenResult opened = c->book.open(address, c->deps.now());
        if (opened.outcome == LanHttpPairing::Open::Full) return refusal(429, "busy");
        if (opened.outcome != LanHttpPairing::Open::Opened) return refusal(503, "unavailable");

        QJsonObject obj;
        obj["request"] = opened.request.id;
        obj["code"] = opened.request.code;
        obj["claim"] = opened.request.claim;
        obj["expires_in"] = static_cast<int>(LanHttpPairing::TTL_SECS);
        obj["approve_url"] = approvalUrl(*c, req, opened.request.id);
        Logger::info(QStringLiteral("[LanHttp] %1 asks to be let in").arg(address));
        return HttpResponse::json(obj);
    });

    // GET /api/lan-http/pair/:id — where the TV's request stands, asked with
    // the claim it was given (X-MW-Claim), from its address. Approved: its
    // session is made here, bound to that address, and its cookie set. Who
    // approved is never said.
    router.get("/api/lan-http/pair/:id", [c](const HttpRequest& req) {
        if (!req.lanHttp) return notHere();
        const QString id = req.pathParams.value(QStringLiteral("id"));
        const QString address = LanHttp::normalizePeer(req.clientAddress);
        if (!PhonePairing::isRequestId(id) || address.isEmpty()) return notHere();
        const LanHttpPairing::PollResult r = c->book.poll(
            id, req.headers.value(QStringLiteral("x-mw-claim")), address, c->deps.now());
        QJsonObject obj;
        switch (r.state) {
        case LanHttpPairing::Poll::Pending:
            obj["state"] = QStringLiteral("pending");
            return HttpResponse::json(obj);
        case LanHttpPairing::Poll::Refused:
            obj["state"] = QStringLiteral("refused");
            return HttpResponse::json(obj);
        case LanHttpPairing::Poll::Gone:
            // Not counted as a failure: a request that ran out of time ends
            // here too, and a claim of 32 random bytes is not guessed by
            // asking again.
            obj["state"] = QStringLiteral("gone");
            return HttpResponse::json(obj, 404);
        case LanHttpPairing::Poll::Approved: break;
        }
        // Named after the model its browser gives, else its address, until the
        // administrator renames it among the devices.
        const QString name =
            LanHttp::deviceName(req.headers.value(QStringLiteral("user-agent")), address);
        // Let in for a short while by a paired device: until its end, and with
        // that device's session. Nothing left of it by now (that session gone,
        // or no time to give): no session, no cookie (R8).
        if (r.shortApproval) {
            const QString token = c->auth.createShortLanHttpSession(address, name, r.approverId);
            if (token.isEmpty()) {
                obj["state"] = QStringLiteral("gone");
                return HttpResponse::json(obj, 404);
            }
            obj["state"] = QStringLiteral("approved");
            HttpResponse resp = HttpResponse::json(obj);
            resp.headers["Set-Cookie"] = LanHttp::cookie(token, c->auth.lanHttpCookieMaxAge(token));
            return resp;
        }
        const QString token = c->auth.createLanHttpSession(address, name, r.approverId);
        if (token.isEmpty()) return refusal(503, "unavailable");
        obj["state"] = QStringLiteral("approved");
        HttpResponse resp = HttpResponse::json(obj);
        resp.headers["Set-Cookie"] = LanHttp::cookie(token, c->auth.lanHttpCookieMaxAge(token));
        return resp;
    });

    // GET /api/lan-http/approve/:id — what the approval page shows: the TV's
    // address and the time left. Never the code: whoever approves reads it on
    // the TV.
    router.get("/api/lan-http/approve/:id", [c](const HttpRequest& req) {
        const Approver who = approverOf(*c, req);
        if (who.refused) return *who.refused;
        const QString id = req.pathParams.value(QStringLiteral("id"));
        if (!who.admin && !LanHttpPairing::isRequestId(id)) return notAllowedShort();
        const auto shown = c->book.find(id, c->deps.now());
        if (!shown) return who.admin ? refusal(404, "gone") : notAllowedShort();
        QJsonObject obj;
        obj["address"] = shown->address;
        obj["expires_in"] = static_cast<int>(shown->expiresIn);
        if (!who.admin) {
            // For how long, as the page says it: never past this device's own
            // session, which the TV goes with.
            const QString approverId = c->auth.sessionIdForToken(who.token);
            obj["short"] = true;
            // What the device signs its answer for (R1), as MW-PAIR-v1's page.
            obj["host_id"] = hostIdOf(*c);
            obj["ends_in"] = static_cast<qint64>(std::max<qint64>(
                0, c->auth.shortApprovalEndsAt(approverId) - QDateTime::currentSecsSinceEpoch()));
        }
        return HttpResponse::json(obj);
    });

    // POST /api/lan-http/approve/:id {code[, sig]} — the TV is let in. The
    // approving session is kept on the TV's (approvedBy): revoking it revokes
    // the TV. The host machine has no session: nothing is kept then. A paired
    // device signs {code}, and lets it in for a short while only (TVNONADM).
    router.post("/api/lan-http/approve/:id", [c](const HttpRequest& req) {
        const Approver who = approverOf(*c, req);
        if (who.refused) return *who.refused;
        const QJsonObject body = QJsonDocument::fromJson(req.body).object();
        const QString id = req.pathParams.value(QStringLiteral("id"));
        const QString code = body.value(QStringLiteral("code")).toString();
        const qint64 now = c->deps.now();
        const auto tv = c->book.find(id, now);

        if (who.admin) {
            const QString token = c->deps.sessionToken ? c->deps.sessionToken(req) : QString();
            const QString approver =
                c->auth.validateSession(token) ? c->auth.sessionIdForToken(token) : QString();
            switch (c->book.approve(id, code, approver, now)) {
            case LanHttpPairing::Answer::Approved: {
                QJsonObject obj;
                obj["status"] = QStringLiteral("approved");
                return HttpResponse::json(obj);
            }
            case LanHttpPairing::Answer::WrongCode: return refusal(400, "wrong_code");
            // The administrator's way, as it always was (R3): its last try
            // spent, the request is gone, and nothing else is counted.
            case LanHttpPairing::Answer::TooManyTries:
            case LanHttpPairing::Answer::Gone: break;
            }
            return refusal(404, "gone");
        }

        // A paired device, in this order (security review, R1b): it may (done
        // above), the shapes, which spend nothing; the signature on what was
        // sent, counted when it fails; only then the code, each wrong one
        // counted against the request, the device and, at the last try, the TV.
        if (!LanHttpPairing::isRequestId(id) || code.toUtf8().size() > MAX_SENT_CODE_BYTES)
            return notAllowedShort();
        const QString hostId = hostIdOf(*c);
        if (hostId.isEmpty()) return notAllowedShort();
        if (!signedByTheDevice(*c, req, who.token, LanHttpPairing::approveMessage(hostId, id, code),
                               body))
            return notAllowedShort();
        // Signed by a device paired before v0.4.4: told to pair again, the
        // request left untouched (no try spent), whether it exists or not.
        if (who.reappair) return reappairNeeded();
        const QString approverId = c->auth.sessionIdForToken(who.token);
        // Out of the alphabet: a wrong code, as the comparison would find it,
        // counted the same way.
        const QString typed = LanHttpPairing::isCodeShape(code) ? code : QString();
        switch (c->book.approve(id, typed, approverId, now, true)) {
        case LanHttpPairing::Answer::Approved: {
            QJsonObject obj;
            obj["status"] = QStringLiteral("approved");
            return HttpResponse::json(obj);
        }
        case LanHttpPairing::Answer::WrongCode:
            if (c->auth.countShortApproverWrongCode(approverId)) return notAllowedShort();
            return refusal(400, "wrong_code");
        case LanHttpPairing::Answer::TooManyTries:
            c->auth.countShortApproverWrongCode(approverId);
            if (tv) c->auth.countPairingFailure(tv->address);
            break;
        case LanHttpPairing::Answer::Gone: break;
        }
        return notAllowedShort();
    });

    // POST /api/lan-http/refuse/:id[ {sig}] — the TV is turned away: by the
    // administrator, or by a paired device that signs it.
    router.post("/api/lan-http/refuse/:id", [c](const HttpRequest& req) {
        const Approver who = approverOf(*c, req);
        if (who.refused) return *who.refused;
        const QString id = req.pathParams.value(QStringLiteral("id"));
        if (!who.admin) {
            const QJsonObject body = QJsonDocument::fromJson(req.body).object();
            const QString hostId = hostIdOf(*c);
            if (!LanHttpPairing::isRequestId(id) || hostId.isEmpty()) return notAllowedShort();
            if (!signedByTheDevice(*c, req, who.token, LanHttpPairing::refuseMessage(hostId, id),
                                   body))
                return notAllowedShort();
            // As for a yes: the request is not looked at, nor refused.
            if (who.reappair) return reappairNeeded();
        }
        if (!c->book.refuse(id, c->deps.now()))
            return who.admin ? refusal(404, "gone") : notAllowedShort();
        QJsonObject obj;
        obj["status"] = QStringLiteral("refused");
        return HttpResponse::json(obj);
    });
}
