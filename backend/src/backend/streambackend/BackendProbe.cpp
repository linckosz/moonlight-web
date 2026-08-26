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

#include "backend/streambackend/BackendProbe.h"

#include "backend/NvHTTP.h"
#include "backend/WolfApiClient.h"
#include "common/Logger.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QPointer>
#include <QTimer>
#include <QUrl>

namespace {

/// Short on purpose. This runs alongside host polling and its answer is a
/// nicety, never something a user waits on: a machine that is not MultiSeat
/// must cost the poll almost nothing.
constexpr int kProbeTimeoutMs = 1500;

/// Longer, because the answer this probe most wants is a REFUSAL, and a refusal
/// is not always instant. Measured against a live Wolf host on the LAN, three
/// runs: its open ports connect in under 5 ms, while the closed one takes a
/// steady ~2040 ms to come back ConnectionRefused. At 1500 ms that refusal is
/// aborted and reads as silence — turning the one case this exists to catch into
/// "cannot tell". Nothing waits on this; it rides along with host polling.
constexpr int kRefusalTimeoutMs = 5000;

} // namespace

namespace BackendProbe {

QString toString(Detected d)
{
    switch (d) {
    case Detected::GameStream: return QStringLiteral("gamestream");
    case Detected::NvidiaGfe: return QStringLiteral("gfe");
    case Detected::Unknown: break;
    }
    return QStringLiteral("unknown");
}

Detected classifyServerInfo(const QString& serverInfo)
{
    if (serverInfo.isEmpty()) return Detected::Unknown;

    const QString state = NvHTTP::getXmlString(serverInfo, QStringLiteral("state"));

    // The original NVIDIA software is the one thing that names itself.
    if (state.contains(QStringLiteral("MJOLNIR"))) return Detected::NvidiaGfe;

    // Everything else in the family says SUNSHINE_SERVER_{FREE,BUSY} — Sunshine,
    // Apollo and Wolf alike. See the header: this is as far as serverinfo goes.
    if (state.startsWith(QStringLiteral("SUNSHINE_SERVER"))) return Detected::GameStream;

    // No state, but a document that carries the fields a GameStream host must
    // have. Some forks answer a reduced serverinfo before pairing; treating that
    // as "speaks GameStream" is right, and it is also the safe direction — the
    // caller only uses this to decide whether to look further.
    if (!NvHTTP::getXmlString(serverInfo, QStringLiteral("uniqueid")).isEmpty() &&
        !NvHTTP::getXmlString(serverInfo, QStringLiteral("appversion")).isEmpty()) {
        return Detected::GameStream;
    }

    return Detected::Unknown;
}

bool looksLikeMultiSeatAuth(const QByteArray& body)
{
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return false;

    // The endpoint answers {"authEnabled": true|false}. The key's presence is
    // the signal; its value only says whether a credential will be needed next,
    // which is a separate question from "is MultiSeat there".
    const QJsonObject obj = doc.object();
    return obj.contains(QStringLiteral("authEnabled")) &&
           obj.value(QStringLiteral("authEnabled")).isBool();
}

void probeMultiSeat(QNetworkAccessManager* nam, const QString& address,
                    std::function<void(bool present)> cb)
{
    if (!nam || address.isEmpty()) {
        if (cb) cb(false);
        return;
    }

    QUrl url;
    url.setScheme(QStringLiteral("http"));
    url.setHost(address);
    url.setPort(kMultiSeatApiPort);
    url.setPath(QStringLiteral("/api/system/auth"));

    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::ManualRedirectPolicy);
    req.setTransferTimeout(kProbeTimeoutMs);

    QNetworkReply* reply = nam->get(req);

    // Belt and braces alongside setTransferTimeout: a peer that accepts the
    // connection and then says nothing is exactly the shape a stalled probe
    // takes, and this one must never outlive the poll that started it.
    QPointer<QNetworkReply> guard(reply);
    QTimer::singleShot(kProbeTimeoutMs, reply, [guard]() {
        if (guard && !guard->isFinished()) guard->abort();
    });

    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, cb]() {
        reply->deleteLater();
        const bool ok = reply->error() == QNetworkReply::NoError &&
                        looksLikeMultiSeatAuth(reply->readAll());
        if (cb) cb(ok);
    });
}

SunshineRest classifySunshineRest(Reach reach, int httpStatus,
                                  const QByteArray& wwwAuthenticate)
{
    // Actively refused: something is up at that address and nothing is listening
    // on that port. That is a definite answer, and it is the one a host without
    // this API actually gives.
    if (reach == Reach::Refused) return SunshineRest::Absent;

    // Silence is not. Could be a firewall in front of a host that has the API
    // after all — and a negative is what makes us offer a setup, so guessing
    // here would offer it to a Sunshine host. Refuse to guess.
    if (reach == Reach::NoAnswer) return SunshineRest::Unknown;

    // The signal: the API names itself in the challenge it sends to a caller
    // with no credentials. Matching the realm rather than the bare 401 is what
    // keeps some other password-protected service on that port from passing.
    if (httpStatus == 401 &&
        wwwAuthenticate.contains(QByteArrayLiteral("Sunshine Gamestream Host"))) {
        return SunshineRest::Present;
    }

    // A 401 from something else entirely: it answered, it is protected, and it
    // is not this API — but naming what IS there is beyond us, and a machine
    // running an unrelated service on that port should not be handed a setup
    // dialog. Unknown is the honest, harmless answer.
    if (httpStatus == 401) return SunshineRest::Unknown;

    // Anything else that completed a request — a 404, a plain page, a redirect —
    // is a definite "this is not the Sunshine REST API".
    return SunshineRest::Absent;
}

void probeSunshineRest(const QString& address, quint16 httpPort,
                       std::function<void(SunshineRest result)> cb)
{
    if (address.isEmpty() || httpPort == 0) {
        if (cb) cb(SunshineRest::Unknown);
        return;
    }

    // A socket rather than QNetworkAccessManager, deliberately.
    //
    // Through QNAM this probe was unusable: every outcome came back as
    // OperationCanceled at the timeout, including a plain TCP refusal that a raw
    // socket reports in ~2 s and a Sunshine host that answers curl in 6 ms.
    // Neither switching verification off on the request nor acknowledging
    // sslErrors changed it. QNAM is built to fetch a resource; what is wanted
    // here is the three-way distinction between refused, silent, and answered —
    // and only the transport can tell those apart. Talking to the socket keeps
    // that answer exact, at the cost of writing one request line by hand.
    const quint16 port = sunshineRestPort(httpPort);
    auto* sock = new QSslSocket();
    // No verification at all: this reads a status line and one header, trusts
    // nothing, and sends no credential. On a socket this really does cover the
    // host-name check too, which is what it did not do through QNAM.
    sock->setPeerVerifyMode(QSslSocket::VerifyNone);

    // One answer only, whichever of the three racing outcomes gets there first.
    auto answered = std::make_shared<bool>(false);
    auto buffer = std::make_shared<QByteArray>();
    auto settle = [sock, cb, answered, address, port](Reach reach, int status,
                                                      const QByteArray& auth) {
        if (*answered) return;
        *answered = true;
        const SunshineRest verdict = classifySunshineRest(reach, status, auth);
        // The three outcomes are indistinguishable from outside, and which one
        // it is decides whether the host is offered a control-API setup. Without
        // this line, "why does my host not offer it" has no answer.
        Logger::debug(QStringLiteral("[Backend] Sunshine REST probe %1:%2 -> %3 (status %4)")
                          .arg(address)
                          .arg(port)
                          .arg(reach == Reach::Refused    ? QStringLiteral("refused")
                               : reach == Reach::Answered ? QStringLiteral("answered")
                                                          : QStringLiteral("no answer"))
                          .arg(status));
        sock->disconnectFromHost();
        sock->deleteLater();
        if (cb) cb(verdict);
    };

    QObject::connect(sock, &QSslSocket::encrypted, sock, [sock, address, port]() {
        // Minimal and complete: HTTP/1.1 needs a Host header, and Connection:
        // close stops us holding open a server that handles one client at a time.
        const QByteArray crlf = "\r\n";
        const QByteArray req = "GET /api/apps HTTP/1.1" + crlf + "Host: " + address.toUtf8() +
                               ':' + QByteArray::number(port) + crlf + "Connection: close" + crlf +
                               "User-Agent: MoonlightWeb" + crlf + crlf;
        sock->write(req);
    });

    QObject::connect(sock, &QIODevice::readyRead, sock, [sock, buffer, settle]() {
        buffer->append(sock->readAll());
        // Headers end at the blank line. Nothing past it is ever read: the body
        // is somebody else's data and none of our business.
        const int headEnd = buffer->indexOf(QByteArray("\r\n\r\n"));
        if (headEnd < 0) {
            // A peer that dribbles headers forever must not keep us here.
            if (buffer->size() > 8192) settle(Reach::Answered, 0, QByteArray());
            return;
        }
        const QByteArray head = buffer->left(headEnd);
        int status = 0;
        QByteArray auth;
        const QList<QByteArray> lines = head.split('\n');
        for (int i = 0; i < lines.size(); ++i) {
            const QByteArray line = lines[i].trimmed();
            if (i == 0) {
                // "HTTP/1.1 401 Unauthorized"
                const QList<QByteArray> parts = line.split(' ');
                if (parts.size() >= 2) status = parts[1].toInt();
                continue;
            }
            if (line.startsWith("WWW-Authenticate:") || line.startsWith("www-authenticate:"))
                auth = line.mid(line.indexOf(':') + 1).trimmed();
        }
        settle(Reach::Answered, status, auth);
    });

    QObject::connect(sock, &QAbstractSocket::errorOccurred, sock,
                     [settle](QAbstractSocket::SocketError err) {
                         // A refusal is an ANSWER — nothing listens there — and it
                         // is the answer a host without this API gives. Everything
                         // else (timeout, unreachable, a handshake that failed for
                         // its own reasons) leaves the question open.
                         settle(err == QAbstractSocket::ConnectionRefusedError ? Reach::Refused
                                                                              : Reach::NoAnswer,
                                0, QByteArray());
                     });

    QTimer::singleShot(kRefusalTimeoutMs, sock,
                       [settle]() { settle(Reach::NoAnswer, 0, QByteArray()); });

    sock->connectToHostEncrypted(address, port);
}

void identifyControlApi(QNetworkAccessManager* nam, const QString& apiUrl, const QString& apiToken,
                        std::function<void(QString type)> cb)
{
    if (!nam || apiUrl.isEmpty()) {
        if (cb) cb(QString());
        return;
    }

    // Only Wolf is asked, because only Wolf needs asking: MultiSeat is found on
    // its own port without anyone typing anything, and a plain GameStream host
    // has no control API to point at. Adding a provider here means adding one
    // request, not a list for the user to choose from.
    auto* wolf = new WolfApiClient(apiUrl, apiToken, nam);
    wolf->pairedClients([wolf, cb](bool ok, const WolfApiError&, const QVector<WolfPairedClient>&) {
        wolf->deleteLater();
        // A successful, well-formed answer to a Wolf-shaped request is the
        // identification. A refusal is not: a wrong token, or something else
        // entirely at that address, both mean we cannot name what is there.
        if (cb) cb(ok ? QStringLiteral("wolf") : QString());
    });
}

} // namespace BackendProbe
