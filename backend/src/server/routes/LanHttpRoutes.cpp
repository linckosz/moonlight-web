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
#include "server/AuthManager.h"
#include "server/LanHttp.h"
#include "server/LanHttpPairing.h"
#include "server/PhonePairing.h"
#include "server/RestRouter.h"

#include <QJsonDocument>
#include <QJsonObject>

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

/// The administrator's side: never on the TV's path, and only with the right
/// that mints a PIN. Empty when the caller may answer.
std::optional<HttpResponse> notAnApprover(Context& c, const HttpRequest& req)
{
    if (req.lanHttp) return notHere();
    if (req.isLocal) return std::nullopt;
    c.deps.reportAuthFailure(req.clientAddress);
    return refusal(403, "not_allowed");
}

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
        if (auto refused = notAnApprover(*c, req)) return *refused;
        const auto shown = c->book.find(req.pathParams.value(QStringLiteral("id")), c->deps.now());
        if (!shown) return refusal(404, "gone");
        QJsonObject obj;
        obj["address"] = shown->address;
        obj["expires_in"] = static_cast<int>(shown->expiresIn);
        return HttpResponse::json(obj);
    });

    // POST /api/lan-http/approve/:id {code} — the administrator lets the TV in.
    // The approving session is kept on the TV's (approvedBy): revoking it
    // revokes the TV. The host machine has no session: nothing is kept then.
    router.post("/api/lan-http/approve/:id", [c](const HttpRequest& req) {
        if (auto refused = notAnApprover(*c, req)) return *refused;
        const QJsonObject body = QJsonDocument::fromJson(req.body).object();
        const QString token = c->deps.sessionToken ? c->deps.sessionToken(req) : QString();
        const QString approver =
            c->auth.validateSession(token) ? c->auth.sessionIdForToken(token) : QString();
        switch (c->book.approve(req.pathParams.value(QStringLiteral("id")),
                                body.value(QStringLiteral("code")).toString(), approver,
                                c->deps.now())) {
        case LanHttpPairing::Answer::Approved: {
            QJsonObject obj;
            obj["status"] = QStringLiteral("approved");
            return HttpResponse::json(obj);
        }
        case LanHttpPairing::Answer::WrongCode: return refusal(400, "wrong_code");
        case LanHttpPairing::Answer::Gone: break;
        }
        return refusal(404, "gone");
    });

    // POST /api/lan-http/refuse/:id — the administrator says no.
    router.post("/api/lan-http/refuse/:id", [c](const HttpRequest& req) {
        if (auto refused = notAnApprover(*c, req)) return *refused;
        if (!c->book.refuse(req.pathParams.value(QStringLiteral("id")), c->deps.now()))
            return refusal(404, "gone");
        QJsonObject obj;
        obj["status"] = QStringLiteral("refused");
        return HttpResponse::json(obj);
    });
}
