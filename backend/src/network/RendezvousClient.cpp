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

#include "RendezvousClient.h"

#include "../common/RendezvousId.h"
#include "../server/AppSettings.h"

#include <iterator>

#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QUrl>
#include <QUrlQuery>

namespace {

// Envelope types on the line. Must match hub.go.
const char* const kMsgOpen = "open";
const char* const kMsgMsg = "msg";
const char* const kMsgClose = "close";

// Error codes from the server's 409s. Must match main.go.
const char* const kErrIdTaken = "id_taken";
const char* const kErrOwnerHasOther = "owner_has_other";

// Our own ping cadence. The server pings too (Qt answers those by itself), but
// its pings only prove the line is alive to the SERVER. This one is how WE find
// out, which is what decides when to reconnect.
constexpr int kKeepAliveMs = 45 * 1000;

// A pong must come back within this. Generous on purpose: a home connection
// stalling for a few seconds is ordinary, and tearing down a working line costs
// more than waiting.
constexpr int kPongTimeoutMs = 20 * 1000;

// Backoff bounds. Capped so a machine left running overnight still recovers
// promptly once the server returns.
constexpr int kRetryBaseMs = 2 * 1000;
constexpr int kRetryMaxMs = 60 * 1000;

// Redraws allowed per process. See the header.
constexpr int kMaxIdentityRedraws = 2;

// Consecutive failures to bring the line up before we stop trusting our own
// claim and go through the claim endpoint again. Two, because one is ordinary
// (the server restarting under us) and three would leave a machine unreachable
// for the best part of a minute longer for no gain.
constexpr int kReclaimAfterFailures = 2;

/// 256 bits, hex. This is the credential that proves ownership of the
/// identifier, so it comes from the system CSPRNG, never the global generator.
QString generateOwnerToken()
{
    // Filled as quint32 words rather than through a cast over a byte buffer:
    // generate() needs a properly aligned range, and a QByteArray only happens
    // to be aligned.
    quint32 words[8];
    QRandomGenerator::system()->generate(std::begin(words), std::end(words));
    const QByteArray raw(reinterpret_cast<const char*>(words), sizeof(words));
    return QString::fromLatin1(raw.toHex());
}

} // namespace

RendezvousClient::RendezvousClient(AppSettings* settings, QObject* parent)
    : QObject(parent)
    , m_Settings(settings)
    , m_Net(new QNetworkAccessManager(this))
{
    m_RetryTimer.setSingleShot(true);
    m_KeepAliveTimer.setInterval(kKeepAliveMs);
    m_Watchdog.setSingleShot(true);
    m_Watchdog.setInterval(kPongTimeoutMs);

    connect(&m_RetryTimer, &QTimer::timeout, this, [this]() {
        if (!m_Running) return;
        if (m_Settings->rendezvousClaimed())
            openLine();
        else
            claim();
    });
    connect(&m_KeepAliveTimer, &QTimer::timeout, this, &RendezvousClient::onKeepAlive);
    connect(&m_Watchdog, &QTimer::timeout, this, &RendezvousClient::onWatchdog);

    connect(&m_Socket, &QWebSocket::connected, this, &RendezvousClient::onConnected);
    connect(&m_Socket, &QWebSocket::disconnected, this, &RendezvousClient::onDisconnected);
    connect(&m_Socket, &QWebSocket::textMessageReceived, this, &RendezvousClient::onTextMessage);
    connect(&m_Socket, &QWebSocket::pong, this, &RendezvousClient::onPong);
    connect(&m_Socket, &QWebSocket::errorOccurred, this, &RendezvousClient::onSocketError);
}

RendezvousClient::~RendezvousClient()
{
    // Close without waiting: the destructor runs during shutdown and the server
    // notices a dropped socket perfectly well on its own.
    m_Socket.abort();
}

QString RendezvousClient::baseUrl()
{
    QString url = QString::fromUtf8(qgetenv("MW_RENDEZVOUS_URL"));
    if (!url.isEmpty()) {
        while (url.endsWith(QLatin1Char('/')))
            url.chop(1);
        return url;
    }
    QString domain = QString::fromUtf8(qgetenv("MW_DOMAIN"));
    if (domain.isEmpty()) domain = QStringLiteral("moonlightweb.top");
    return QStringLiteral("https://stream.") + domain;
}

QString RendezvousClient::entryUrl() const
{
    // Deliberately empty until the server has accepted the claim. An identifier
    // we drew but nobody acknowledged is an address that answers to nobody, and
    // handing it to the tray menu or a desktop shortcut would look like success.
    if (!m_Settings->rendezvousClaimed()) return {};
    const QString id = m_Settings->rendezvousId();
    if (id.isEmpty()) return {};
    return baseUrl() + QLatin1Char('/') + id;
}

void RendezvousClient::start()
{
    if (m_Running) return;
    m_Running = true;
    m_RetryCount = 0;

    ensureIdentity();
    if (m_Settings->rendezvousClaimed())
        openLine();
    else
        claim();
}

void RendezvousClient::stop()
{
    m_Running = false;
    m_RetryTimer.stop();
    m_KeepAliveTimer.stop();
    m_Watchdog.stop();
    m_Socket.close(QWebSocketProtocol::CloseCodeNormal, QStringLiteral("shutting down"));
    setOnline(false);
}

void RendezvousClient::ensureIdentity()
{
    if (m_Settings->rendezvousToken().isEmpty()) {
        m_Settings->setRendezvousToken(generateOwnerToken());
    }

    QString id = m_Settings->rendezvousId();
    if (RendezvousId::isValid(id)) return;

    // Either we have never had one, or something wrote a malformed value. Fold
    // first — a value that only differs by case or hyphens is the same
    // identifier, and redrawing it would abandon a claim we still hold.
    const QString folded = RendezvousId::normalise(id);
    if (RendezvousId::isValid(folded)) {
        qInfo() << "[RDV] normalising stored identifier";
        m_Settings->setRendezvousId(folded);
        return;
    }

    id = RendezvousId::generate();
    m_Settings->setRendezvousId(id); // also clears the claimed flag
    qInfo() << "[RDV] drew a new identifier";
    emit identityChanged(id);
}

void RendezvousClient::claim()
{
    if (m_Claiming || !m_Running) return;
    m_Claiming = true;

    const QString id = m_Settings->rendezvousId();
    const QString token = m_Settings->rendezvousToken();

    QNetworkRequest req{QUrl(baseUrl() + QStringLiteral("/v1/claim"))};
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("X-MW-Owner", token.toLatin1());

    QJsonObject body;
    body["id"] = id;
    QNetworkReply* reply = m_Net->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));

    connect(reply, &QNetworkReply::finished, this, [this, reply, id]() {
        reply->deleteLater();
        m_Claiming = false;
        if (!m_Running) return;

        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray payload = reply->readAll();
        const QJsonObject answer = QJsonDocument::fromJson(payload).object();

        if (status == 200) {
            m_Settings->setRendezvousClaimed(true);
            m_RetryCount = 0;
            qInfo() << "[RDV] identifier claimed"
                    << (answer.value("claimed").toBool() ? "(new)" : "(already ours)");
            emit identityChanged(id);
            openLine();
            return;
        }

        const QString code = answer.value("code").toString();

        if (status == 409 && code == QLatin1String(kErrIdTaken)) {
            // 128 bits: this is not a collision, it is someone else holding the
            // value we stored — a restored backup, a cloned disk. Only a fresh
            // identifier gets us out, and the old one was never ours to keep.
            if (m_IdentityRedraws < kMaxIdentityRedraws) {
                ++m_IdentityRedraws;
                const QString fresh = RendezvousId::generate();
                m_Settings->setRendezvousId(fresh);
                qWarning() << "[RDV] identifier already claimed by another owner — redrawing";
                emit identityChanged(fresh);
                scheduleRetry("redraw after conflict");
                return;
            }
            qCritical() << "[RDV] identifier conflict persists after" << kMaxIdentityRedraws
                        << "redraws — giving up until restart";
            return;
        }

        if (status == 409 && code == QLatin1String(kErrOwnerHasOther)) {
            // Our credential holds a DIFFERENT identifier and we no longer know
            // which, so we cannot release it. Redraw both: the stale entry is a
            // single row keyed by an HMAC nobody holds any more, and its
            // identifier is 128 random bits that nobody will ever guess.
            if (m_IdentityRedraws < kMaxIdentityRedraws) {
                ++m_IdentityRedraws;
                m_Settings->setRendezvousToken(generateOwnerToken());
                const QString fresh = RendezvousId::generate();
                m_Settings->setRendezvousId(fresh);
                qWarning() << "[RDV] credential holds a different identifier — "
                              "drawing a fresh pair";
                emit identityChanged(fresh);
                scheduleRetry("fresh identity after ownership mismatch");
                return;
            }
            qCritical() << "[RDV] ownership mismatch persists — giving up until restart";
            return;
        }

        if (status == 429) {
            qWarning() << "[RDV] claim rate-limited by the server, backing off";
            scheduleRetry("rate limited");
            return;
        }

        // Everything else — no network, server down, 5xx — is transient by
        // assumption. There is no state to repair locally, only a wait.
        qWarning() << "[RDV] claim failed:"
                   << (status ? QString::number(status) : reply->errorString());
        scheduleRetry("claim failed");
    });
}

void RendezvousClient::openLine()
{
    if (!m_Running) return;
    if (m_Socket.state() != QAbstractSocket::UnconnectedState) return;

    const QString id = m_Settings->rendezvousId();
    const QString token = m_Settings->rendezvousToken();
    if (id.isEmpty() || token.isEmpty()) {
        qCritical() << "[RDV] cannot open the line without an identity";
        return;
    }

    m_LineEverUp = false;
    QUrl url(baseUrl() + QStringLiteral("/v1/host"));
    url.setScheme(url.scheme() == QLatin1String("http") ? QStringLiteral("ws")
                                                        : QStringLiteral("wss"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("id"), id);
    url.setQuery(query);

    QNetworkRequest req(url);
    req.setRawHeader("X-MW-Owner", token.toLatin1());
    m_Socket.open(req);
}

void RendezvousClient::onConnected()
{
    m_RetryCount = 0;
    m_LineFailures = 0;
    m_LineEverUp = true;
    m_KeepAliveTimer.start();
    setOnline(true);
    qInfo() << "[RDV] line up —" << entryUrl();
}

void RendezvousClient::onDisconnected()
{
    m_KeepAliveTimer.stop();
    m_Watchdog.stop();
    setOnline(false);

    // An attempt that never came up is a different animal from a line that was
    // working and dropped. Count only the former, and after a few of them go
    // back through the claim.
    //
    // This exists because of a real failure that a live test found and no unit
    // test could: the server had lost its claim store, so it answered the
    // handshake with 401 forever, while this client — believing it still owned
    // the identifier — retried the line and never once re-claimed. It backed off
    // to a minute and stayed there, permanently unreachable.
    //
    // The first fix was to recognise the 401 in the error string. That was the
    // wrong instinct: Qt reports it as "Unsupported WWW-Authenticate challenges
    // encountered", with neither "401" nor "Unauthorized" anywhere in it, and
    // any other wording would have failed the same way on the next Qt release.
    // Counting failures needs no error message at all. Re-claiming when we do
    // still own the identifier costs nothing — the server answers
    // "claimed": false and charges no budget for it — so the repair is safe to
    // attempt even when the real cause was something else entirely.
    if (!m_LineEverUp) {
        if (++m_LineFailures >= kReclaimAfterFailures) {
            m_LineFailures = 0;
            qWarning() << "[RDV] line refused repeatedly — re-claiming the identifier";
            m_Settings->setRendezvousClaimed(false);
        }
    } else {
        m_LineFailures = 0;
    }

    if (m_Running) scheduleRetry("line dropped");
}

void RendezvousClient::onSocketError(QAbstractSocket::SocketError error)
{
    Q_UNUSED(error)
    qWarning() << "[RDV] line error:" << m_Socket.errorString();
}

void RendezvousClient::onKeepAlive()
{
    // Two jobs at once: keep the router's NAT entry warm (home boxes forget an
    // idle session somewhere between 30 s and 2 minutes), and give the watchdog
    // something to wait for.
    m_Socket.ping();
    m_Watchdog.start();
}

void RendezvousClient::onPong(quint64 elapsed, const QByteArray& payload)
{
    Q_UNUSED(elapsed)
    Q_UNUSED(payload)
    m_Watchdog.stop();
}

void RendezvousClient::onWatchdog()
{
    // No pong came back. The socket may still look connected — a NAT entry
    // dropped upstream leaves exactly this state — so close it and rebuild
    // rather than trust it.
    qWarning() << "[RDV] no pong within" << (kPongTimeoutMs / 1000) << "s — rebuilding the line";
    m_Socket.abort();
}

void RendezvousClient::scheduleRetry(const char* why)
{
    if (!m_Running || m_RetryTimer.isActive()) return;

    // Capped exponential, with jitter. The jitter is not cosmetic: without it
    // every instance that was connected when the server restarted comes back at
    // the same instant, and the thundering herd is what keeps it down.
    int delay = kRetryBaseMs << qMin(m_RetryCount, 5);
    delay = qMin(delay, kRetryMaxMs);
    const int jitter = QRandomGenerator::global()->bounded(delay / 2 + 1);
    delay = delay / 2 + jitter;

    ++m_RetryCount;
    qInfo() << "[RDV] retry in" << delay << "ms —" << why;
    m_RetryTimer.start(delay);
}

void RendezvousClient::setOnline(bool online)
{
    if (m_Online == online) return;
    m_Online = online;
    emit onlineChanged(online);
}

void RendezvousClient::sendFrame(const QJsonObject& frame)
{
    if (m_Socket.state() != QAbstractSocket::ConnectedState) return;
    m_Socket.sendTextMessage(
        QString::fromUtf8(QJsonDocument(frame).toJson(QJsonDocument::Compact)));
}

void RendezvousClient::sendSignal(const QString& sessionId, const QJsonObject& payload)
{
    QJsonObject frame;
    frame["t"] = QLatin1String(kMsgMsg);
    frame["s"] = sessionId;
    frame["d"] = payload;
    sendFrame(frame);
}

void RendezvousClient::closeSession(const QString& sessionId)
{
    QJsonObject frame;
    frame["t"] = QLatin1String(kMsgClose);
    frame["s"] = sessionId;
    sendFrame(frame);
}

void RendezvousClient::onTextMessage(const QString& text)
{
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8());
    if (!doc.isObject()) return;
    const QJsonObject frame = doc.object();

    const QString type = frame.value("t").toString();
    const QString session = frame.value("s").toString();
    if (session.isEmpty()) return;

    if (type == QLatin1String(kMsgOpen)) {
        emit sessionOpened(session);
    } else if (type == QLatin1String(kMsgMsg)) {
        // The payload is whatever the browser sent. It is NOT trusted here and
        // is not inspected here — validation belongs with the signalling code
        // that understands it, and this class must stay ignorant of its shape.
        emit signalReceived(session, frame.value("d").toObject());
    } else if (type == QLatin1String(kMsgClose)) {
        emit sessionClosed(session);
    }
}
