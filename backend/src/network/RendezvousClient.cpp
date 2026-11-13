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

#include "AddressRace.h"
#include "../common/Edition.h"
#include "../common/RendezvousId.h"
#include "../server/AppSettings.h"

#include <iterator>

#include <QDateTime>
#include <QHostInfo>
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
const char* const kMsgError = "error"; // server → us, before it hangs up

// Error codes from the server's 409s. Must match main.go.
const char* const kErrIdTaken = "id_taken";
const char* const kErrOwnerHasOther = "owner_has_other";
// From a 400 on the claim, or an error frame on the line. Must match hub.go.
const char* const kErrUnsupportedVersion = "unsupported_version";

// The protocol revision this build speaks INSIDE /v1/ — sent as `?v=` on the
// claim and on the line. Not the app version: it moves only when this client
// starts understanding something the introduction server can send, and every
// such change is additive (a breaking one is a /v2/). Absent means 1 on the
// server, so a build predating the field is exactly a revision-1 client. Keep
// in step with hub.go and bootstrap/v1/tunnel.js.
constexpr int kProto = 1;

// Our own ping cadence. The server pings too (Qt answers those by itself), but
// its pings only prove the line is alive to the SERVER. This one is how WE find
// out, which is what decides when to reconnect. With the pong wait below, a
// line cut in silence is noticed within 35 s at worst; it was 65 s at 45 + 20.
// At a thousand hosts this is 50 pings a second for the server, a few
// kilobytes each way, and for a host on Wi-Fi one radio wake-up every 20 s for
// a frame of a hundred bytes. The server, for its part, closes a line that has
// sent nothing for 105 s: every ping of ours re-arms that, so a live line is
// never mistaken for a mute one.
constexpr int kKeepAliveMs = 20 * 1000;

// A pong must come back within this. Generous on purpose: a home connection
// stalling for a few seconds is ordinary, and tearing down a working line costs
// more than waiting. 15 s of queue on the uplink is more than any stream in
// progress produces.
constexpr int kPongTimeoutMs = 15 * 1000;

// The clock watch: a coarse tick, and how far ahead the wall clock must have
// jumped between two ticks for the process to have been asleep. A machine
// that sleeps, or an app the OS naps, stops its timers with it; on waking,
// the line may be long gone (the server closes a mute line after 105 s) while
// the keep-alive timer, resumed where it stopped, is still up to 20 s away
// from its next ping. A tick that finds the clock far ahead pings at once. The
// wall clock rather than a monotonic one: the monotonic clocks of Linux and
// macOS stand still during a suspend, the wall clock is set again from the
// RTC on resume.
constexpr int kClockTickMs = 5 * 1000;
constexpr int kClockJumpMs = 30 * 1000;

// One attempt to bring the line up, openLine() to connected, may take this
// long: name lookup, the address race, TLS and the upgrade. A healthy one takes
// under a second. Nothing used to bound this phase (the pong watchdog only
// starts once the line is up), and on 20/10/2026 a line sat in it for two hours
// while its machine was unreachable. 15 s lets two SYN retransmissions through
// on Windows (at 3 s and 9 s) and three on Linux, and still beats the 20 s a
// missing pong is given.
constexpr int kOpenTimeoutMs = 15 * 1000;

// The same bound for a line that goes by name, on a Qt that cannot open it on
// an address (canPinLine()). Qt then walks the addresses itself, one after the
// other, and gives a silent one 21 s on Windows and 30 s on Linux before it
// tries the next. With the 15 s above, an IPv6 that drops packets would cut
// every attempt before the IPv4 was reached, and the line would never come up.
constexpr int kNamedOpenTimeoutMs = 45 * 1000;

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
    , m_Race(new AddressRace(this))
    , m_OpenTimeoutMs(kOpenTimeoutMs)
    , m_KeepAliveMs(kKeepAliveMs)
    , m_PongTimeoutMs(kPongTimeoutMs)
    , m_ClockJumpMs(kClockJumpMs)
    , m_PinLine(canPinLine(QVersionNumber::fromString(QLatin1String(qVersion()))))
{
    m_RetryTimer.setSingleShot(true);
    m_KeepAliveTimer.setInterval(m_KeepAliveMs);
    m_Watchdog.setSingleShot(true);
    m_Watchdog.setInterval(m_PongTimeoutMs);
    m_ClockTick.setInterval(kClockTickMs);
    m_OpenTimer.setSingleShot(true);

    connect(&m_RetryTimer, &QTimer::timeout, this, [this]() {
        if (!m_Running) return;
        if (m_Settings->rendezvousClaimed())
            openLine();
        else
            claim();
    });
    connect(&m_KeepAliveTimer, &QTimer::timeout, this, &RendezvousClient::onKeepAlive);
    connect(&m_Watchdog, &QTimer::timeout, this, &RendezvousClient::onWatchdog);
    connect(&m_ClockTick, &QTimer::timeout, this, &RendezvousClient::onClockTick);
    connect(&m_OpenTimer, &QTimer::timeout, this, &RendezvousClient::onOpenTimeout);
    connect(m_Race, &AddressRace::won, this, &RendezvousClient::onRaceWon);
    connect(m_Race, &AddressRace::lost, this, &RendezvousClient::onRaceLost);

    // Bounded: Qt takes a message of up to 2 GB by default, and buffers all of
    // it before anyone sees a byte. 16 MiB is far past anything this socket
    // legitimately carries: the rendezvous server relays 64 KiB at most.
    m_Socket.setMaxAllowedIncomingMessageSize(16 * 1024 * 1024);
    m_Socket.setMaxAllowedIncomingFrameSize(16 * 1024 * 1024);
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
    // A LAN-only instance has no address outside its LAN, whatever it once claimed.
    if (mw::edition::lanOnly() || !m_Settings->rendezvousClaimed()) return {};
    const QString id = m_Settings->rendezvousId();
    if (id.isEmpty()) return {};
    return baseUrl() + QLatin1Char('/') + id;
}

void RendezvousClient::start()
{
    // Every caller is already gated on Internet Access, which a LAN-only
    // instance never has; this keeps the line shut if one ever is not.
    if (m_Running || mw::edition::lanOnly()) return;
    m_Running = true;
    m_RetryCount = 0;
    // A halt belongs to the previous run. Nothing about this build changed, so
    // it will most likely halt again — but with a fresh log line, which is what
    // whoever toggled the switch is looking for.
    m_Halted = false;
    m_LastError.clear();

    // Said once: it is the Qt loaded at run time that decides, and it decides
    // how long an IPv6 that drops packets holds every attempt.
    if (m_PinLine)
        qInfo() << "[RDV] Qt" << qVersion() << "- the line opens on the address that wins the race";
    else
        qInfo() << "[RDV] Qt" << qVersion()
                << "cannot open the line on an address (QTBUG-141396) - it goes by name";

    m_LastTickMs = QDateTime::currentMSecsSinceEpoch();
    m_ClockTick.start();

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
    m_ClockTick.stop();
    // A deliberate stop is not an attempt that failed: nothing is counted.
    abandonAttempt();
    m_Socket.close(QWebSocketProtocol::CloseCodeNormal, QStringLiteral("shutting down"));
    setOnline(false);
}

void RendezvousClient::setOpenTimeoutMs(int ms)
{
    m_OpenTimeoutMs = qMax(1, ms);
}

bool RendezvousClient::canPinLine(const QVersionNumber& qt)
{
    // QTBUG-141396. Until its fix, QWebSocket ignores the request's
    // peerVerifyName, and adds the request's raw Host header after the one it
    // takes from the URL instead of replacing it. On a line opened on an
    // address, the TLS handshake then names no server, and Caddy ends it
    // ("tlsv1 alert internal error"). Given the name some other way, the
    // request still carries two Host headers, and Go answers 400. The fix is in
    // 6.10.3, 6.11.1 and 6.12.0; 6.11.0 and everything up to 6.10.2 lack it.
    if (qt >= QVersionNumber(6, 11, 1)) return true;
    return qt >= QVersionNumber(6, 10, 3) && qt < QVersionNumber(6, 11, 0);
}

void RendezvousClient::forceLineByName()
{
    m_PinLine = false;
}

void RendezvousClient::setKeepAliveMs(int ms)
{
    m_KeepAliveMs = qMax(1, ms);
    m_KeepAliveTimer.setInterval(m_KeepAliveMs);
}

void RendezvousClient::setPongTimeoutMs(int ms)
{
    m_PongTimeoutMs = qMax(1, ms);
    m_Watchdog.setInterval(m_PongTimeoutMs);
}

void RendezvousClient::setClockWatch(int tickMs, int jumpMs)
{
    m_ClockTick.setInterval(qMax(1, tickMs));
    m_ClockJumpMs = qMax(1, jumpMs);
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
    if (m_Claiming || !m_Running || m_Halted) return;
    m_Claiming = true;

    const QString id = m_Settings->rendezvousId();
    const QString token = m_Settings->rendezvousToken();

    QUrl claimUrl(baseUrl() + QStringLiteral("/v1/claim"));
    claimUrl.setQuery(protoQuery());
    QNetworkRequest req{claimUrl};
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("X-MW-Owner", token.toLatin1());
    // The claim goes by name through Qt's own address race, and it is bounded
    // like the line: a claim that hangs would hold every retry behind it.
    AddressRace::enableOn(req);
    req.setTransferTimeout(m_OpenTimeoutMs);

    QJsonObject body;
    body["id"] = id;
    QNetworkReply* reply = m_Net->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));

    connect(reply, &QNetworkReply::finished, this, [this, reply, id]() {
        reply->deleteLater();
        m_Claiming = false;
        if (!m_Running) return;

        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        // A claim cut by its transfer timeout is already closed: reading it
        // would only earn Qt's "device not open" warning.
        const QByteArray payload = reply->isOpen() ? reply->readAll() : QByteArray();
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

        if (status == 400 && code == QLatin1String(kErrUnsupportedVersion)) {
            // Retrying cannot help: nothing about this build changes between
            // attempts. Stop, say so where the owner will read it, and leave
            // the identifier alone — it is still ours, just not served.
            halt(QStringLiteral("this build speaks protocol revision %1, the server serves %2..%3")
                     .arg(kProto)
                     .arg(answer.value("min").toInt())
                     .arg(answer.value("max").toInt()));
            return;
        }

        // Everything else — no network, server down, 5xx — is transient by
        // assumption. There is no state to repair locally, only a wait.
        qWarning() << "[RDV] claim failed:"
                   << (status ? QString::number(status) : reply->errorString());
        scheduleRetry("claim failed");
    });
}

QUrlQuery RendezvousClient::protoQuery()
{
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("v"), QString::number(kProto));
    return query;
}

void RendezvousClient::openLine()
{
    if (!m_Running || m_Halted || m_Opening) return;
    if (m_Socket.state() != QAbstractSocket::UnconnectedState) return;

    const QString id = m_Settings->rendezvousId();
    const QString token = m_Settings->rendezvousToken();
    if (id.isEmpty() || token.isEmpty()) {
        qCritical() << "[RDV] cannot open the line without an identity";
        return;
    }

    m_LineEverUp = false;
    m_RaceWinner = QAbstractSocket::UnknownNetworkLayerProtocol;
    QUrl url(baseUrl() + QStringLiteral("/v1/host"));
    url.setScheme(url.scheme() == QLatin1String("http") ? QStringLiteral("ws")
                                                        : QStringLiteral("wss"));
    QUrlQuery query = protoQuery();
    query.addQueryItem(QStringLiteral("id"), id);
    url.setQuery(query);

    m_LineRequest = QNetworkRequest(url);
    m_LineRequest.setRawHeader("X-MW-Owner", token.toLatin1());

    // One attempt starts here and ends in onConnected() or attemptFailed(),
    // whichever the open timeout lets happen first.
    ++m_Attempt;
    m_Opening = true;

    // A URL that names an address has nothing to race: the test stand-in, or a
    // server pinned by hand. A name this Qt cannot carry to an address goes by
    // name, and Qt walks its addresses itself, on the longer bound.
    const bool literal = !QHostAddress(url.host()).isNull();
    if (literal || !m_PinLine) {
        m_OpenTimer.start(literal ? m_OpenTimeoutMs : kNamedOpenTimeoutMs);
        m_Socket.open(m_LineRequest);
        return;
    }

    m_OpenTimer.start(m_OpenTimeoutMs);
    const int attempt = m_Attempt;
    m_Lookup = QHostInfo::lookupHost(
        url.host(), this, [this, attempt](const QHostInfo& info) { onResolved(attempt, info); });
}

void RendezvousClient::onResolved(int attempt, const QHostInfo& info)
{
    // A lookup answers once, and only the attempt that asked is listening.
    if (attempt != m_Attempt || !m_Opening) return;
    m_Lookup = -1;

    QList<QHostAddress> addresses;
    for (const QHostAddress& address : info.addresses()) {
        const auto family = address.protocol();
        if (family == QAbstractSocket::IPv4Protocol || family == QAbstractSocket::IPv6Protocol)
            addresses.append(address);
    }
    if (addresses.isEmpty()) {
        qWarning() << "[RDV] cannot resolve" << m_LineRequest.url().host() << "-"
                   << info.errorString();
        attemptFailed("name lookup failed");
        return;
    }

    const QUrl url = m_LineRequest.url();
    const quint16 port = quint16(url.port(url.scheme() == QLatin1String("wss") ? 443 : 80));
    m_Race->start(addresses, port, m_LeadFamily);
}

void RendezvousClient::onRaceWon(const QHostAddress& address, qint64 elapsedMs)
{
    if (!m_Opening) return;
    m_RaceWinner = address.protocol();
    qInfo() << "[RDV] line via" << (m_RaceWinner == QAbstractSocket::IPv6Protocol ? "IPv6" : "IPv4")
            << "(race" << elapsedMs << "ms)";
    openLineOn(address);
}

void RendezvousClient::onRaceLost(qint64 elapsedMs)
{
    if (!m_Opening) return;
    qWarning() << "[RDV] no address of" << m_LineRequest.url().host() << "answered (race"
               << elapsedMs << "ms)";
    attemptFailed("no address answered");
}

void RendezvousClient::openLineOn(const QHostAddress& address)
{
    // The line goes to the address that answered, and the name goes with it.
    // The server routes on the Host header and the certificate is checked
    // against the name, and Qt would take both from the URL, which now names an
    // address. Both are set to exactly what the URL by name would have sent:
    // the authority as written, with the port only where the URL spells one.
    // Only a Qt with the fix for QTBUG-141396 passes them on (canPinLine()).
    const QUrl named = m_LineRequest.url();
    const quint16 port = quint16(named.port(named.scheme() == QLatin1String("wss") ? 443 : 80));
    const QString literal = address.protocol() == QAbstractSocket::IPv6Protocol
                                ? QLatin1Char('[') + address.toString() + QLatin1Char(']')
                                : address.toString();
    QUrl pinned = named;
    pinned.setAuthority(literal + QLatin1Char(':') + QString::number(port));

    const QString hostHeader =
        named
            .toString(QUrl::RemoveScheme | QUrl::RemoveUserInfo | QUrl::RemovePath |
                      QUrl::RemoveQuery | QUrl::RemoveFragment)
            .mid(2);

    QNetworkRequest req = m_LineRequest;
    req.setUrl(pinned);
    req.setRawHeader("Host", hostHeader.toLatin1());
    req.setPeerVerifyName(named.host());
    m_Socket.open(req);
}

void RendezvousClient::onOpenTimeout()
{
    if (!m_Opening) return;
    qWarning() << "[RDV] the line did not come up within" << m_OpenTimer.interval()
               << "ms - abandoning this attempt";

    // A family that won the race and then stalled is not the one to lead with
    // next time: its TCP went through but TLS or the upgrade never completed,
    // which is what a path MTU hole in IPv6 looks like from here. The race
    // alone cannot see that; this is the only place that can.
    if (m_RaceWinner != QAbstractSocket::UnknownNetworkLayerProtocol) {
        m_LeadFamily = m_RaceWinner == QAbstractSocket::IPv6Protocol
                           ? QAbstractSocket::IPv4Protocol
                           : QAbstractSocket::IPv6Protocol;
    }

    // The failure first, the abort second: abort() on a connecting socket
    // emits disconnected() from inside the call, and that path must find the
    // attempt already counted rather than count it again.
    attemptFailed("open timeout");
    if (m_Socket.state() != QAbstractSocket::UnconnectedState) m_Socket.abort();
}

void RendezvousClient::attemptFailed(const char* why)
{
    // Every way an attempt can end without the line coming up lands here: the
    // socket's disconnected (a refusal), its error without a disconnected (a
    // URL Qt will not open), a race nobody won, the open timeout. Whichever
    // reports first closes the attempt; the others find it closed and do
    // nothing, so a failure counts once.
    if (!m_Opening) return;
    abandonAttempt();

    // An attempt that never came up is a different animal from a line that was
    // working and dropped. Only the former is counted, and after a few of them
    // the client goes back through the claim.
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
    if (++m_LineFailures >= kReclaimAfterFailures) {
        m_LineFailures = 0;
        qWarning() << "[RDV] line refused repeatedly — re-claiming the identifier";
        m_Settings->setRendezvousClaimed(false);
    }

    if (m_Running) scheduleRetry(why);
}

void RendezvousClient::abandonAttempt()
{
    m_Opening = false;
    m_OpenTimer.stop();
    if (m_Lookup != -1) {
        QHostInfo::abortHostLookup(m_Lookup);
        m_Lookup = -1;
    }
    m_Race->cancel();
}

void RendezvousClient::onConnected()
{
    m_Opening = false;
    m_OpenTimer.stop();
    m_LeadFamily = QAbstractSocket::UnknownNetworkLayerProtocol;
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

    // A halt is the server having said, in words, that no retry will be
    // served. It is neither a refusal nor a drop and must not feed either: the
    // re-claim would only be refused with the same code.
    if (m_Halted) return;

    if (!m_LineEverUp) {
        attemptFailed("line refused");
        return;
    }

    m_LineFailures = 0;
    if (m_Running) scheduleRetry("line dropped");
}

void RendezvousClient::halt(const QString& why)
{
    // Stays m_Running: the switch is still on, the owner still consents. What
    // stops is the retrying, which only a new build — or a server change — can
    // make worthwhile, and stop() followed by start() is how either gets tried.
    m_Halted = true;
    m_LastError = why;
    m_RetryTimer.stop();
    qCritical() << "[RDV] giving up until restart —" << why;
}

void RendezvousClient::onSocketError(QAbstractSocket::SocketError error)
{
    Q_UNUSED(error)
    qWarning() << "[RDV] line error:" << m_Socket.errorString();

    // An attempt that errors out before connecting normally gets a disconnected
    // right after, and that is its exit. Not always: a URL Qt will not open is
    // an error and nothing else, and the attempt would otherwise wait out the
    // open timeout for a line that was never tried. Looked at on the next turn
    // of the loop, once Qt has emitted whatever else it was going to.
    if (m_Opening && m_Socket.state() == QAbstractSocket::UnconnectedState) {
        QTimer::singleShot(0, this, [this]() {
            if (m_Opening && m_Socket.state() == QAbstractSocket::UnconnectedState)
                attemptFailed("line error");
        });
        return;
    }

    // An error on a line that is still open means the line is gone, whatever Qt
    // says next — and sometimes it says nothing. Behind production's Caddy, a
    // restart of the introduction server reaches us as a TLS close_notify on a
    // TCP connection that stays open: Qt reports "The TLS/SSL connection has
    // been closed" and never emits disconnected. The line then looked up until
    // the next ping and its missing pong, 45 + 20 s at worst at the time —
    // 31 s and 63 s on the staging bench of 16/10/2026, where every machine on
    // it was unreachable for new visitors the whole time.
    //
    // Aborted on the next turn of the loop rather than here, inside the
    // socket's own error emission. Only a CONNECTED socket is touched: one
    // still connecting may not emit disconnected on abort, and then no retry
    // would ever be scheduled. Where Qt does emit disconnected after the error
    // (a plain FIN, a reset), it has done so by then and this does nothing.
    if (m_Socket.state() != QAbstractSocket::ConnectedState) return;
    QTimer::singleShot(0, this, [this]() {
        if (m_Socket.state() != QAbstractSocket::ConnectedState) return;
        qInfo() << "[RDV] the line failed but is still open — closing it to rebuild it now";
        m_Socket.abort();
    });
}

void RendezvousClient::onKeepAlive()
{
    // Two jobs at once: keep the router's NAT entry warm (home boxes forget an
    // idle session somewhere between 30 s and 2 minutes), and give the watchdog
    // something to wait for.
    m_Socket.ping();
    ++m_PingsSent;
    // A ping still unanswered keeps its own deadline: restarting the watchdog
    // here would push it back at every keep-alive, and with a pong wait as long
    // as the keep-alive (the tests' 100/100 ms) it would never fire at all.
    if (!m_Watchdog.isActive()) m_Watchdog.start();
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
    qWarning() << "[RDV] no pong within" << (m_PongTimeoutMs / 1000) << "s — rebuilding the line";
    m_Socket.abort();
}

void RendezvousClient::onClockTick()
{
    // The gap is what the wall clock advanced beyond the tick itself. Timers do
    // not run while the machine sleeps, so the first tick after a wake-up
    // carries the whole sleep in it; a timer that merely fired late carries a
    // few milliseconds.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 gap = now - m_LastTickMs - m_ClockTick.interval();
    m_LastTickMs = now;
    if (gap < m_ClockJumpMs) return;
    qInfo() << "[RDV] the clock jumped" << (gap / 1000) << "s ahead (sleep?)";

    // Only a line that believes itself up needs the proof: an attempt in
    // progress has its own bound, a pending retry fires on its own, and a ping
    // already waiting for its pong will be judged by the watchdog, late or not.
    if (m_Socket.state() != QAbstractSocket::ConnectedState || m_Watchdog.isActive()) return;
    qInfo() << "[RDV] pinging the line now rather than at the next keep-alive";
    onKeepAlive();
    m_KeepAliveTimer.start();
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

    // The one frame without a session: the server explaining, just before it
    // hangs up, why this line is not going to be held. Read it before the
    // session check below discards it as malformed.
    if (type == QLatin1String(kMsgError)) {
        const QString code = frame.value("code").toString();
        if (code == QLatin1String(kErrUnsupportedVersion)) {
            halt(QStringLiteral("this build speaks protocol revision %1, which the server "
                                "no longer serves")
                     .arg(kProto));
        } else {
            qWarning() << "[RDV] server refused the line:" << code;
        }
        return;
    }

    const QString session = frame.value("s").toString();
    if (session.isEmpty()) return;

    if (type == QLatin1String(kMsgOpen)) {
        // `v` names the revision of the browser that just arrived; absent
        // means 1. Nothing reads it yet — it is there so an addition can be
        // sent only to browsers that understand it, the day there is one.
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
